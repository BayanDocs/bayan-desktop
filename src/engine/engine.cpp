#include "engine.h"

#include "bayan_ffi.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaObject>

#include <cmath>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// The largest identifier, sequence number and revision of the protocol: below 2^53, so that JavaScript numbers hold them exactly (§4).
constexpr qint64 kMaxExactInteger = static_cast<qint64>(quint64{1} << 53U) - 1;

QByteArray toCompactJson(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// Whether a JSON value is a number with an exact integer value from minimum to maximum.
bool isInteger(const QJsonValue &value, qint64 minimum, qint64 maximum) {
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > static_cast<double>(kMaxExactInteger)) {
    return false;
  }
  const qint64 integer = value.toInteger(std::numeric_limits<qint64>::min());
  return integer >= minimum && integer <= maximum;
}

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
  if (bayan_engine_post(handle_.get(), bytesOf(json), static_cast<std::size_t>(json.size())) != BAYAN_STATUS_OK) {
    return false;
  }
  if (sentObserver_) {
    sentObserver_(envelope);
  }
  return true;
}

bool Engine::post(const QString &type, const QJsonObject &payload) {
  return send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"type"_s, type}, {u"payload"_s, payload}});
}

qint64 Engine::request(const QString &type, const QJsonObject &payload) {
  const qint64 id = nextRequestId_++;
  const bool panic = !panicOnRequestType_.isEmpty() && type == panicOnRequestType_;
  // Recorded before posting, because the reply may arrive before send() returns (it is queued for this thread, so it is never handled
  // before), and forgotten again if the engine refused the message.
  pending_.emplace(id, !panic && type == u"hello"_s);
  if (!send(QJsonObject{
          {u"v"_s, kProtocolVersion},
          {u"id"_s, id},
          {u"type"_s, panic ? u"diag.panic"_s : type},
          {u"payload"_s, panic ? QJsonObject() : payload},
      })) {
    pending_.erase(id);
    return 0;
  }
  // Published only after the engine has queued the message, so a thread that reads it and then renders a tile is ordered after it.
  lastPostedRequest_.store(id, std::memory_order_release);
  return id;
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

Engine::Tile Engine::renderTile(const QJsonObject &request, QSize pixelSize) {
  if (pixelSize.width() <= 0 || pixelSize.height() <= 0 || pixelSize.width() > kMaxTileSide || pixelSize.height() > kMaxTileSide) {
    return {};
  }
  QImage image(pixelSize, QImage::Format_RGBA8888_Premultiplied);
  if (image.isNull()) {
    return {};
  }
  const QByteArray json = toCompactJson(request);
  std::uint64_t revision = 0;
  const BayanStatus status =
      bayan_render_tile(handle_.get(), bytesOf(json), static_cast<std::size_t>(json.size()), image.bits(),
                        static_cast<std::size_t>(image.sizeInBytes()), static_cast<std::uint32_t>(pixelSize.width()),
                        static_cast<std::uint32_t>(pixelSize.height()), static_cast<std::size_t>(image.bytesPerLine()), &revision);
  if (status != BAYAN_STATUS_OK) {
    return {};
  }
  // A render revision is a positive integer below 2^53 (engine protocol §14.6). This may run on any thread, so the violation is reported to
  // this object's thread, which stops using the engine.
  if (revision == 0 || revision > static_cast<std::uint64_t>(kMaxExactInteger)) {
    QMetaObject::invokeMethod(
        this, [this] { emit protocolError(u"The engine rendered a tile with a revision out of range."_s); }, Qt::QueuedConnection);
    return {};
  }
  return {.image = image, .revision = revision};
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
  // The envelope (engine protocol §4, §14.1 item 5). Every field the shell reads must have the type, and the range, that the protocol gives
  // it; a message without v, or with a payload that is not an object, is no protocol message at all.
  const QJsonObject message = document.object();
  const QJsonValue version = message.value(u"v"_s);
  const QJsonValue type = message.value(u"type"_s);
  const QJsonValue payload = message.value(u"payload"_s);
  if (!isInteger(version, 0, kMaxExactInteger) || (!type.isUndefined() && !type.isString()) ||
      (!payload.isUndefined() && !payload.isObject())) {
    emit protocolError(u"The engine sent a message with a malformed envelope."_s);
    return;
  }
  const bool currentVersion = version.toInteger() == kProtocolVersion;
  const QJsonValue replyTo = message.value(u"re"_s);
  if (!replyTo.isUndefined()) {
    const QJsonValue ok = message.value(u"ok"_s);
    const QJsonValue error = message.value(u"error"_s);
    if (!isInteger(replyTo, 1, kMaxExactInteger) || !ok.isBool() ||
        (!ok.toBool() && (!error.isObject() || !error.toObject().value(u"code"_s).isString()))) {
      emit protocolError(u"The engine sent a malformed reply."_s);
      return;
    }
    // Each request is answered once. A reply that answers no request waiting for one breaks the protocol; one to a request of an earlier
    // session still answers a request that was posted, and the session ignores it (engine protocol §12.1 step 4).
    const qint64 requestId = replyTo.toInteger();
    const auto request = pending_.find(requestId);
    if (request == pending_.end()) {
      emit protocolError(u"The engine sent a reply to a request it was not answering."_s);
      return;
    }
    // Only the reply to a hello may carry another version: that is how an engine that speaks other versions refuses it (§14.1 item 1).
    const bool hello = request->second;
    pending_.erase(request);
    if (!currentVersion && !hello) {
      emit protocolError(u"The engine sent a message for another protocol version."_s);
      return;
    }
    emit replyReceived(requestId, ok.toBool(), payload.toObject(), error.toObject());
    return;
  }
  if (!currentVersion) {
    emit protocolError(u"The engine sent a message for another protocol version."_s);
    return;
  }
  if (type.toString().isEmpty() || !isInteger(message.value(u"seq"_s), 1, kMaxExactInteger)) {
    emit protocolError(u"The engine sent an event without a type or a sequence number."_s);
    return;
  }
  emit eventReceived(type.toString(), payload.toObject());
}

} // namespace bayan
