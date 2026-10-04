#include "engine.h"

#include "bayan_ffi.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaObject>

#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

QByteArray toCompactJson(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// The C interface passes UTF-8 as bytes; Qt stores it as char.
const std::uint8_t *bytesOf(const QByteArray &data) {
  return reinterpret_cast<const std::uint8_t *>(data.constData()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

// The engine's message callback. Runs on the engine thread.
void onEngineMessage(void *userData, const std::uint8_t *json, std::size_t length) {
  static_cast<Engine *>(userData)->deliverFromEngineThread(json, length);
}

} // namespace

void Engine::HandleDeleter::operator()(BayanEngine *handle) const noexcept { bayan_engine_free(handle); }

Engine::Engine(std::unique_ptr<BayanEngine, HandleDeleter> handle) : handle_(std::move(handle)) {}

Engine::~Engine() {
  // Stop the engine before anything else: bayan_engine_free waits for the engine thread, so no callback can reach this object afterwards.
  // Messages already queued for this object are discarded by ~QObject.
  handle_.reset();
}

std::unique_ptr<Engine> Engine::create(const QJsonObject &config) {
  const QByteArray json = toCompactJson(config);
  std::unique_ptr<BayanEngine, HandleDeleter> handle(bayan_engine_new(bytesOf(json), static_cast<std::size_t>(json.size())));
  if (!handle) {
    return nullptr;
  }
  BayanEngine *raw = handle.get();
  std::unique_ptr<Engine> engine(new Engine(std::move(handle)));
  if (bayan_engine_set_callback(raw, &onEngineMessage, engine.get()) != BAYAN_STATUS_OK) {
    return nullptr;
  }
  return engine;
}

QString Engine::version() { return QString::fromUtf8(bayan_version()); }

bool Engine::send(const QJsonObject &envelope) {
  const QByteArray json = toCompactJson(envelope);
  return bayan_engine_post(handle_.get(), bytesOf(json), static_cast<std::size_t>(json.size())) == BAYAN_STATUS_OK;
}

bool Engine::post(const QString &type, const QJsonObject &payload) {
  return send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"type"_s, type}, {u"payload"_s, payload}});
}

qint64 Engine::request(const QString &type, const QJsonObject &payload) {
  const qint64 id = nextRequestId_++;
  return send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"id"_s, id}, {u"type"_s, type}, {u"payload"_s, payload}}) ? id : 0;
}

BlobId Engine::putBlob(const QByteArray &bytes) {
  return bayan_blob_put(handle_.get(), bytesOf(bytes), static_cast<std::size_t>(bytes.size()));
}

std::optional<QByteArray> Engine::blob(BlobId id) {
  // Ask for the size first, then copy.
  std::size_t length = 0;
  BayanStatus status = bayan_blob_get(handle_.get(), id, nullptr, 0, &length);
  if (status == BAYAN_STATUS_OK && length == 0) {
    return QByteArray();
  }
  if (status != BAYAN_STATUS_BUFFER_TOO_SMALL || length > static_cast<std::size_t>(QByteArray::maxSize())) {
    return std::nullopt;
  }
  QByteArray bytes(static_cast<qsizetype>(length), Qt::Uninitialized);
  std::size_t copied = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see bytesOf.
  status = bayan_blob_get(handle_.get(), id, reinterpret_cast<std::uint8_t *>(bytes.data()), length, &copied);
  if (status != BAYAN_STATUS_OK || copied != length) {
    return std::nullopt;
  }
  return bytes;
}

bool Engine::releaseBlob(BlobId id) { return bayan_blob_release(handle_.get(), id) == BAYAN_STATUS_OK; }

QImage Engine::renderTile(const QJsonObject &request, QSize pixelSize) {
  if (pixelSize.width() <= 0 || pixelSize.height() <= 0 || pixelSize.width() > kMaxTileSide || pixelSize.height() > kMaxTileSide) {
    return {};
  }
  QImage image(pixelSize, QImage::Format_RGBA8888_Premultiplied);
  if (image.isNull()) {
    return {};
  }
  const QByteArray json = toCompactJson(request);
  const BayanStatus status =
      bayan_render_tile(handle_.get(), bytesOf(json), static_cast<std::size_t>(json.size()), image.bits(),
                        static_cast<std::size_t>(image.sizeInBytes()), static_cast<std::uint32_t>(pixelSize.width()),
                        static_cast<std::uint32_t>(pixelSize.height()), static_cast<std::size_t>(image.bytesPerLine()));
  return status == BAYAN_STATUS_OK ? image : QImage();
}

void Engine::deliverFromEngineThread(const std::uint8_t *json, std::size_t length) noexcept {
  // Runs on the engine thread: copy the bytes (they are valid only during this call) and queue them for this object's thread. Nothing may
  // throw back into the engine.
  try {
    if (json == nullptr || length > static_cast<std::size_t>(kMaxMessageBytes)) {
      QMetaObject::invokeMethod(
          this, [this] { emit protocolError(u"The engine sent an empty or oversized message."_s); }, Qt::QueuedConnection);
      return;
    }
    QByteArray copy = QByteArrayView(json, static_cast<qsizetype>(length)).toByteArray();
    QMetaObject::invokeMethod(this, [this, message = std::move(copy)] { dispatch(message); }, Qt::QueuedConnection);
  } catch (...) {
    // Out of memory: the message is lost. Count it, so that the next message that does arrive reports the loss.
    lostMessages_.fetch_add(1, std::memory_order_relaxed);
  }
}

void Engine::dispatch(const QByteArray &json) {
  if (lostMessages_.exchange(0, std::memory_order_relaxed) > 0) {
    emit protocolError(u"Messages from the engine were lost because memory ran out."_s);
  }
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    emit protocolError(u"The engine sent a message that is not a JSON object."_s);
    return;
  }
  const QJsonObject message = document.object();
  if (message.value(u"v"_s).toInteger(-1) != kProtocolVersion) {
    emit protocolError(u"The engine sent a message for another protocol version."_s);
    return;
  }
  const QJsonValue replyTo = message.value(u"re"_s);
  if (!replyTo.isUndefined()) {
    const qint64 requestId = replyTo.toInteger(0);
    const QJsonValue ok = message.value(u"ok"_s);
    if (requestId <= 0 || !ok.isBool()) {
      emit protocolError(u"The engine sent a malformed reply."_s);
      return;
    }
    emit replyReceived(requestId, ok.toBool(), message.value(u"payload"_s).toObject(), message.value(u"error"_s).toObject());
    return;
  }
  const QString type = message.value(u"type"_s).toString();
  if (type.isEmpty()) {
    emit protocolError(u"The engine sent a message without a type."_s);
    return;
  }
  emit eventReceived(type, message.value(u"payload"_s).toObject());
}

} // namespace bayan
