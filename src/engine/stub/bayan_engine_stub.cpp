// The stub engine: implements bayan_ffi.h with a fixed one-page test document, so that the desktop shell can be built and tested before the
// real engine from bayan-core (CORE-007) exists. It follows the same rules as the real engine: messages are processed in order on the
// engine's own thread, the message callback runs on that thread, and no exception ever crosses the C interface.
//
// For tests of the shell's error paths, the configuration JSON may contain {"test": {"doc_open": "fail"}} (doc.open is refused) or
// {"test": {"doc_open": "ignore"}} (doc.open is never answered). Anything else in "test" makes bayan_engine_new fail.
//
// It understands only what the shell scaffold needs: the hello/welcome handshake, doc.open (always opens the test page), view.set and
// doc.close. Everything else is answered with an "unsupported_message" error.

#include "bayan_ffi.h"

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

constexpr int kProtocolVersion = 0;
constexpr const char *kStubVersion = "0.0.0-stub";

// Limits. Messages from the shell are trusted more than documents, but the stub still refuses absurd sizes.
constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;
constexpr std::size_t kMaxBlobBytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kMaxBlobCount = 1024;
constexpr std::uint32_t kMaxTileSide = 4096;
constexpr std::size_t kBytesPerPixel = 4;

// Geometry of the test page in BLU (ADR-0005: 1 inch = 1,828,800 BLU). The page is US Letter.
constexpr qint64 kBluPerInch = 1'828'800;
constexpr qint64 kPageWidth = kBluPerInch * 17 / 2;
constexpr qint64 kPageHeight = kBluPerInch * 11;
// Tile rectangles are limited so that the integer arithmetic in renderTile cannot overflow.
constexpr qint64 kMaxCoordinate = qint64{1} << 40;
constexpr qint64 kDocumentId = 1;

struct Rgba {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
  std::uint8_t a;
};

// Premultiplied colours of the test page; every opaque colour is its own premultiplied form.
constexpr Rgba kTransparent{.r = 0, .g = 0, .b = 0, .a = 0};
constexpr Rgba kPaper{.r = 255, .g = 255, .b = 255, .a = 255};
constexpr Rgba kHeading{.r = 31, .g = 95, .b = 191, .a = 255};
constexpr Rgba kText{.r = 154, .g = 160, .b = 166, .a = 255};

// The colour of the test page at a point given in BLU from the page's top-left corner: a heading bar and grey lines standing in for text.
Rgba testPageColor(qint64 x, qint64 y) {
  if (x < 0 || y < 0 || x >= kPageWidth || y >= kPageHeight) {
    return kTransparent;
  }
  constexpr qint64 kLeft = kBluPerInch;
  constexpr qint64 kRight = kPageWidth - kBluPerInch;
  if (x < kLeft || x >= kRight) {
    return kPaper;
  }
  if (y >= kBluPerInch && y < kBluPerInch * 3 / 2) {
    return kHeading;
  }
  constexpr qint64 kFirstLine = kBluPerInch * 2;
  constexpr qint64 kLastLine = kBluPerInch * 10;
  constexpr qint64 kLineSpacing = kBluPerInch * 3 / 10;
  constexpr qint64 kLineHeight = kBluPerInch * 12 / 100;
  if (y >= kFirstLine && y < kLastLine) {
    const qint64 line = (y - kFirstLine) / kLineSpacing;
    const qint64 offset = (y - kFirstLine) % kLineSpacing;
    const qint64 shortening = (line * 7 % 5) * kBluPerInch / 2;
    if (offset < kLineHeight && x < kRight - shortening) {
      return kText;
    }
  }
  return kPaper;
}

// The page coordinate at the centre of one pixel of a tile that covers <extent> BLU from <origin> with <pixelsAcross> pixels. Integer
// arithmetic only.
qint64 pixelCentre(qint64 origin, qint64 extent, std::uint32_t pixel, std::uint32_t pixelsAcross) {
  return origin + ((((2 * qint64{pixel}) + 1) * extent) / (2 * qint64{pixelsAcross}));
}

QByteArray toCompactJson(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// The C interface passes UTF-8 as bytes; Qt stores it as char.
const std::uint8_t *bytesOf(const QByteArray &data) {
  return reinterpret_cast<const std::uint8_t *>(data.constData()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

// Views length bytes at data, or returns nothing if they exceed kMaxMessageBytes (or are missing).
std::optional<QByteArrayView> messageBytes(const std::uint8_t *data, std::size_t length) {
  if (data == nullptr || length == 0 || length > kMaxMessageBytes) {
    return std::nullopt;
  }
  return QByteArrayView(data, static_cast<qsizetype>(length));
}

// Parses a JSON object of at most kMaxMessageBytes. Returns false if the bytes are not one.
bool parseObject(QByteArrayView bytes, QJsonObject &out) {
  if (bytes.empty() || std::cmp_greater(bytes.size(), kMaxMessageBytes)) {
    return false;
  }
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(bytes.toByteArray(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return false;
  }
  out = document.object();
  return true;
}

QJsonObject errorObject(const QString &code, const QString &messageId, const QJsonObject &args = {}) {
  return QJsonObject{{u"code"_s, code}, {u"message_id"_s, messageId}, {u"args"_s, args}};
}

// How the stub answers doc.open; anything but Normal is only for tests (see the top of this file).
enum class DocOpenMode : std::uint8_t { Normal, Fail, Ignore };

} // namespace

// The engine instance behind the opaque pointer of the C interface.
struct BayanEngine {
  // Shared between the shell's threads and the engine thread; guarded by mutex.
  std::mutex mutex;
  std::condition_variable wakeUp;
  std::deque<QByteArray> inbox;
  bool stopping = false;
  bool posted = false;
  BayanMessageCallback callback = nullptr;
  void *userData = nullptr;

  // Blobs; guarded by blobMutex.
  std::mutex blobMutex;
  std::map<BayanBlobId, std::vector<std::uint8_t>> blobs;
  BayanBlobId nextBlob = 1;

  // Set by the engine thread, read by bayan_render_tile on the shell's thread.
  std::atomic<bool> documentOpen{false};

  // Used only on the engine thread.
  bool handshakeDone = false;
  DocOpenMode docOpenMode = DocOpenMode::Normal; // set before the engine thread starts
  qint64 nextSequence = 1;

  std::thread thread;

  void run();
  void handle(const QByteArray &bytes);
  void handleMessage(qint64 id, const QString &type, const QJsonObject &payload);
  void send(const QJsonObject &message);
  void reply(qint64 id, const QString &type, const QJsonObject &payload);
  void fail(qint64 id, const QJsonObject &error);
  void event(const QString &type, const QJsonObject &payload);
  [[nodiscard]] bool blobExists(BayanBlobId blob);
};

void BayanEngine::run() {
  for (;;) {
    QByteArray message;
    {
      std::unique_lock lock(mutex);
      wakeUp.wait(lock, [this] { return stopping || !inbox.empty(); });
      if (stopping) {
        return;
      }
      message = std::move(inbox.front());
      inbox.pop_front();
    }
    try {
      handle(message);
    } catch (...) {
      // Like a caught panic in the real engine: report it and keep running.
      try {
        event(u"engine.error"_s,
              QJsonObject{{u"code"_s, u"internal"_s}, {u"message_id"_s, u"engine-error-internal"_s}, {u"recoverable"_s, true}});
      } catch (...) { // NOLINT(bugprone-empty-catch): out of memory while reporting an error; nothing more can be done for this message.
      }
    }
  }
}

void BayanEngine::send(const QJsonObject &message) {
  BayanMessageCallback target = nullptr;
  void *targetData = nullptr;
  {
    const std::scoped_lock lock(mutex);
    target = callback;
    targetData = userData;
  }
  if (target == nullptr) {
    return;
  }
  const QByteArray json = toCompactJson(message);
  target(targetData, bytesOf(json), static_cast<std::size_t>(json.size()));
}

void BayanEngine::reply(qint64 id, const QString &type, const QJsonObject &payload) {
  if (id <= 0) {
    return;
  }
  QJsonObject message{{u"v"_s, kProtocolVersion}, {u"re"_s, id}, {u"ok"_s, true}, {u"payload"_s, payload}};
  if (!type.isEmpty()) {
    message.insert(u"type"_s, type);
  }
  send(message);
}

void BayanEngine::fail(qint64 id, const QJsonObject &error) {
  if (id > 0) {
    send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"re"_s, id}, {u"ok"_s, false}, {u"error"_s, error}});
    return;
  }
  // Fire-and-forget messages have nobody to reply to; report the problem as an event instead.
  QJsonObject payload = error;
  payload.insert(u"recoverable"_s, true);
  event(u"engine.error"_s, payload);
}

void BayanEngine::event(const QString &type, const QJsonObject &payload) {
  send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"type"_s, type}, {u"seq"_s, nextSequence++}, {u"payload"_s, payload}});
}

bool BayanEngine::blobExists(BayanBlobId blob) {
  const std::scoped_lock lock(blobMutex);
  return blobs.contains(blob);
}

void BayanEngine::handle(const QByteArray &bytes) {
  QJsonObject message;
  if (!parseObject(bytes, message)) {
    fail(0, errorObject(u"invalid_message"_s, u"engine-error-invalid-message"_s));
    return;
  }
  const QJsonValue idValue = message.value(u"id"_s);
  const qint64 id = idValue.isDouble() ? idValue.toInteger(0) : 0;
  if (message.value(u"v"_s).toInteger(-1) != kProtocolVersion) {
    fail(id, errorObject(u"unsupported_protocol_version"_s, u"engine-error-unsupported-protocol-version"_s));
    return;
  }
  handleMessage(id, message.value(u"type"_s).toString(), message.value(u"payload"_s).toObject());
}

void BayanEngine::handleMessage(qint64 id, const QString &type, const QJsonObject &payload) {
  if (type == u"hello"_s) {
    const QJsonArray versions = payload.value(u"protocol_versions"_s).toArray();
    if (!versions.contains(QJsonValue(kProtocolVersion))) {
      fail(id, errorObject(u"unsupported_protocol_version"_s, u"engine-error-unsupported-protocol-version"_s));
      return;
    }
    handshakeDone = true;
    reply(id, u"welcome"_s,
          QJsonObject{{u"protocol_version"_s, kProtocolVersion},
                      {u"engine_version"_s, QString::fromLatin1(kStubVersion)},
                      {u"layout_epoch"_s, 0},
                      {u"features"_s, QJsonArray{u"stub"_s}}});
    return;
  }
  if (!handshakeDone) {
    fail(id, errorObject(u"handshake_required"_s, u"engine-error-handshake-required"_s));
    return;
  }
  if (type == u"doc.open"_s) {
    if (docOpenMode == DocOpenMode::Ignore) {
      return;
    }
    if (docOpenMode == DocOpenMode::Fail) {
      fail(id, errorObject(u"open_failed"_s, u"engine-error-open-failed"_s));
      return;
    }
    // The stub opens its test page whatever the blob contains, but the blob must exist.
    const auto blob = static_cast<BayanBlobId>(payload.value(u"blob"_s).toInteger(0));
    if (blob == 0 || !blobExists(blob)) {
      fail(id, errorObject(u"not_found"_s, u"engine-error-blob-not-found"_s));
      return;
    }
    documentOpen = true;
    reply(id, u"doc.opened"_s,
          QJsonObject{{u"doc_id"_s, kDocumentId},
                      {u"page_count_estimate"_s, 1},
                      {u"warnings"_s, QJsonArray{}},
                      {u"fonts"_s, QJsonObject{{u"missing"_s, QJsonArray{}}, {u"machine_dependent"_s, false}}}});
    return;
  }
  const bool knownDocument = documentOpen && payload.value(u"doc_id"_s).toInteger(0) == kDocumentId;
  if (type == u"view.set"_s) {
    if (!knownDocument) {
      fail(id, errorObject(u"not_found"_s, u"engine-error-document-not-found"_s));
      return;
    }
    reply(id, {}, {});
    const QJsonObject pageRect{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, kPageWidth}, {u"height"_s, kPageHeight}};
    event(u"view.pages"_s,
          QJsonObject{{u"doc_id"_s, kDocumentId},
                      {u"pages"_s,
                       QJsonArray{QJsonObject{{u"index"_s, 0}, {u"width"_s, kPageWidth}, {u"height"_s, kPageHeight}, {u"section"_s, 0}}}}});
    event(u"render.invalidate"_s,
          QJsonObject{{u"doc_id"_s, kDocumentId}, {u"regions"_s, QJsonArray{QJsonObject{{u"page"_s, 0}, {u"rect"_s, pageRect}}}}});
    event(u"view.layout.progress"_s,
          QJsonObject{{u"doc_id"_s, kDocumentId}, {u"pages_laid_out"_s, 1}, {u"page_count_estimate"_s, 1}, {u"complete"_s, true}});
    return;
  }
  if (type == u"doc.close"_s) {
    if (!knownDocument) {
      fail(id, errorObject(u"not_found"_s, u"engine-error-document-not-found"_s));
      return;
    }
    documentOpen = false;
    reply(id, {}, {});
    return;
  }
  // Only the type is echoed back (shortened), never message content.
  fail(id, errorObject(u"unsupported_message"_s, u"engine-error-unsupported-message"_s, QJsonObject{{u"type"_s, type.left(64)}}));
}

extern "C" {

const char *bayan_version(void) { return kStubVersion; }

BayanEngine *bayan_engine_new(const uint8_t *config_json, size_t config_len) {
  try {
    auto engine = std::make_unique<BayanEngine>();
    if (config_len != 0) {
      QJsonObject config;
      const auto bytes = messageBytes(config_json, config_len);
      if (!bytes || !parseObject(*bytes, config)) {
        return nullptr;
      }
      // The real engine reads its settings here; the stub only knows the test switches described at the top of this file.
      const QJsonObject test = config.value(u"test"_s).toObject();
      const QString docOpen = test.value(u"doc_open"_s).toString(u"normal"_s);
      if (docOpen == u"fail"_s) {
        engine->docOpenMode = DocOpenMode::Fail;
      } else if (docOpen == u"ignore"_s) {
        engine->docOpenMode = DocOpenMode::Ignore;
      } else if (docOpen != u"normal"_s || test.size() > (test.contains(u"doc_open"_s) ? 1 : 0)) {
        return nullptr;
      }
    }
    engine->thread = std::thread([raw = engine.get()] { raw->run(); });
    return engine.release();
  } catch (...) {
    return nullptr;
  }
}

void bayan_engine_free(BayanEngine *engine) {
  if (engine == nullptr) {
    return;
  }
  // Freeing the engine from inside its own callback would make the thread wait for itself. That breaks the rules in bayan_ffi.h; the engine
  // is then leaked rather than destroyed while running.
  if (engine->thread.joinable() && engine->thread.get_id() == std::this_thread::get_id()) {
    return;
  }
  {
    const std::scoped_lock lock(engine->mutex);
    engine->stopping = true;
  }
  engine->wakeUp.notify_all();
  if (engine->thread.joinable()) {
    engine->thread.join();
  }
  delete engine; // NOLINT(cppcoreguidelines-owning-memory): ownership crosses the C interface as a raw pointer.
}

BayanStatus bayan_engine_set_callback(BayanEngine *engine, BayanMessageCallback callback, void *user_data) {
  if (engine == nullptr || callback == nullptr) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  const std::scoped_lock lock(engine->mutex);
  if (engine->posted) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  engine->callback = callback;
  engine->userData = user_data;
  return BAYAN_STATUS_OK;
}

BayanStatus bayan_engine_post(BayanEngine *engine, const uint8_t *json, size_t json_len) {
  if (engine == nullptr || json == nullptr || json_len == 0 || json_len > kMaxMessageBytes) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  try {
    QByteArray copy = QByteArrayView(json, static_cast<qsizetype>(json_len)).toByteArray();
    {
      const std::scoped_lock lock(engine->mutex);
      if (engine->stopping) {
        return BAYAN_STATUS_INTERNAL_ERROR;
      }
      engine->posted = true;
      engine->inbox.push_back(std::move(copy));
    }
    engine->wakeUp.notify_one();
    return BAYAN_STATUS_OK;
  } catch (...) {
    return BAYAN_STATUS_INTERNAL_ERROR;
  }
}

BayanBlobId bayan_blob_put(BayanEngine *engine, const uint8_t *bytes, size_t len) {
  if (engine == nullptr || (bytes == nullptr && len != 0) || len > kMaxBlobBytes) {
    return 0;
  }
  try {
    const auto source = bytes == nullptr ? std::span<const std::uint8_t>() : std::span(bytes, len);
    std::vector<std::uint8_t> copy(source.begin(), source.end());
    const std::scoped_lock lock(engine->blobMutex);
    if (engine->blobs.size() >= kMaxBlobCount) {
      return 0;
    }
    const BayanBlobId id = engine->nextBlob++;
    engine->blobs.emplace(id, std::move(copy));
    return id;
  } catch (...) {
    return 0;
  }
}

BayanStatus bayan_blob_get(BayanEngine *engine, BayanBlobId blob, uint8_t *out, size_t out_capacity, size_t *out_len) {
  if (engine == nullptr || out_len == nullptr) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  const std::scoped_lock lock(engine->blobMutex);
  const auto found = engine->blobs.find(blob);
  if (found == engine->blobs.end()) {
    return BAYAN_STATUS_NOT_FOUND;
  }
  const std::vector<std::uint8_t> &bytes = found->second;
  *out_len = bytes.size();
  if (bytes.empty()) {
    return BAYAN_STATUS_OK;
  }
  if (out == nullptr || out_capacity < bytes.size()) {
    return BAYAN_STATUS_BUFFER_TOO_SMALL;
  }
  std::ranges::copy(bytes, std::span(out, out_capacity).begin());
  return BAYAN_STATUS_OK;
}

BayanStatus bayan_blob_release(BayanEngine *engine, BayanBlobId blob) {
  if (engine == nullptr) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  const std::scoped_lock lock(engine->blobMutex);
  return engine->blobs.erase(blob) == 1 ? BAYAN_STATUS_OK : BAYAN_STATUS_NOT_FOUND;
}

BayanStatus bayan_render_tile(BayanEngine *engine, const uint8_t *request_json, size_t request_len, uint8_t *rgba_out, size_t out_capacity,
                              uint32_t width, uint32_t height, size_t stride) {
  if (engine == nullptr || request_json == nullptr || rgba_out == nullptr) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  if (width == 0 || height == 0 || width > kMaxTileSide || height > kMaxTileSide) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  const std::size_t rowBytes = std::size_t{width} * kBytesPerPixel;
  if (stride < rowBytes || stride > (std::numeric_limits<std::size_t>::max() - rowBytes) / height) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  if (out_capacity < (stride * (height - 1)) + rowBytes) {
    return BAYAN_STATUS_BUFFER_TOO_SMALL;
  }
  try {
    QJsonObject request;
    const auto bytes = messageBytes(request_json, request_len);
    if (!bytes || !parseObject(*bytes, request)) {
      return BAYAN_STATUS_INVALID_ARGUMENT;
    }
    if (!engine->documentOpen || request.value(u"doc_id"_s).toInteger(0) != kDocumentId || request.value(u"page"_s).toInteger(-1) != 0) {
      return BAYAN_STATUS_NOT_FOUND;
    }
    const QJsonObject rect = request.value(u"rect"_s).toObject();
    const qint64 rectX = rect.value(u"x"_s).toInteger(kMaxCoordinate + 1);
    const qint64 rectY = rect.value(u"y"_s).toInteger(kMaxCoordinate + 1);
    const qint64 rectWidth = rect.value(u"width"_s).toInteger(0);
    const qint64 rectHeight = rect.value(u"height"_s).toInteger(0);
    if (rectX < -kMaxCoordinate || rectX > kMaxCoordinate || rectY < -kMaxCoordinate || rectY > kMaxCoordinate || rectWidth <= 0 ||
        rectWidth > kMaxCoordinate || rectHeight <= 0 || rectHeight > kMaxCoordinate) {
      return BAYAN_STATUS_INVALID_ARGUMENT;
    }

    // Sample the page at the centre of every pixel, in integer arithmetic (no floating point, as in the real engine).
    const std::span<std::uint8_t> output(rgba_out, out_capacity);
    for (std::uint32_t row = 0; row < height; ++row) {
      const qint64 y = pixelCentre(rectY, rectHeight, row, height);
      const auto pixels = output.subspan(std::size_t{row} * stride, rowBytes);
      for (std::uint32_t column = 0; column < width; ++column) {
        const qint64 x = pixelCentre(rectX, rectWidth, column, width);
        const Rgba color = testPageColor(x, y);
        std::ranges::copy(std::array{color.r, color.g, color.b, color.a}, pixels.subspan(std::size_t{column} * kBytesPerPixel).begin());
      }
    }
    return BAYAN_STATUS_OK;
  } catch (...) {
    return BAYAN_STATUS_INTERNAL_ERROR;
  }
}

} // extern "C"
