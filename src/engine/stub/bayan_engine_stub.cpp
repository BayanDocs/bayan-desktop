// The stub engine: implements the engine's C interface with a fixed one-page test document, so that the desktop shell can be built and
// tested without Rust. include/bayan_ffi.h is a byte-for-byte copy of the header that bayan-core generates and ships in its C SDK, from the
// commit pinned in deps/bayan-core.json (the test engine.sdk_copies compares it with the SDK's), and the stub does what that header
// documents, as bayan-core's engine does:
// - messages and tiles are handled in order on the engine's own thread: bayan_render_tile queues its request behind the messages posted
//   before it and waits until the engine thread has rendered it, and reports the render revision the tile shows (engine protocol §14.6);
// - the message callback runs on that thread, one message at a time, and nothing reaches it once the engine is stopping;
// - on any engine's thread, that is inside any engine's callback, bayan_render_tile returns BAYAN_STATUS_WRONG_THREAD at once;
// - bayan_engine_free called from inside the engine's own callback returns at once, and the engine thread finishes by itself;
// - once an engine.error has said restart: null (§14.2), the engine has stopped: bayan_engine_post and bayan_render_tile return
//   BAYAN_STATUS_INTERNAL_ERROR, and the callback is not called again;
// - no exception ever crosses the C interface.
// Its messages have the shapes of the protocol's JSON Schema (schema/engine-protocol.schema.json, a copy of the SDK's), and its errors the
// codes, message identifiers and arguments that bayan-core's engine gives them (engine protocol §12).
//
// It speaks engine protocol v1 as far as the shell's tests use it (§14): it reads a hello whatever its v and answers every other message
// whose v is not 1 with unsupported_protocol_version; it learns the shell's platform and runtime from hello; its ui.manifest is the v1 stub
// manifest with the key data that bayan-core's engine resolves for that platform and runtime, which it takes from the copy of the
// conformance vectors (schema/conformance/keys.json, the expected manifest of each platform and runtime), so that no rule of §14.3 is
// written here; the test page has a render revision, 1 when it opens, which a view.set that changes the mode or the formatting marks raises
// (and then invalidates the page), while a new viewport, zoom or device scale invalidates nothing; and it never counts restarts.
//
// For tests of the shell's error paths, the configuration JSON may contain these test switches in "test":
// - "hello": "ignore" (hello is never answered, so that a test can answer it itself, for example with a refusal);
// - "doc_open": "fail" (doc.open is refused with limit_exceeded, as the real engine refuses a document beyond its limit of open documents,
//   keeping its session) or "ignore" (doc.open is never answered);
// - "query_a11y": "ignore" (query.a11y is never answered, so that a test can answer it with an accessibility tree of its own through
//   tests/support/enginemessages.h); otherwise the stub, which has no accessibility tree, refuses it with unsupported_message;
// - "allow_panic": true, which lets diag.panic make the engine fail as the real engine's does (engine protocol §3.3, §6.7, §12): the
//   message is answered with the error "panic" if it has an identifier, then engine.error says that the session was discarded
//   (recoverable: false) and carries restart (§14.2), and the engine continues with a fresh session that needs a new hello. Without it,
//   diag.panic is refused with "not_allowed", as by the real engine;
// - "restarts": the restart value of each successive engine.error with recoverable false, in order: { "number": n, "limit": l }, or null,
//   after which the engine has stopped. The stub counts nothing: the real engine's binding counts the restarts (§14.2), and a test gives
//   the stub the values that the binding would give. A failure after the list is used up carries null, as when the allowance is used up;
// - "engine_version": a string that welcome reports as the engine's version instead of the stub's own, so that tests can hand the shell
//   an engine string with markup in it (bayan_version() still returns the stub's own version).
// Anything else in "test", or a value of the wrong type, makes bayan_engine_new fail, as the real engine's configuration does.
//
// It understands only what the shell needs to show a page: the hello/welcome handshake, ui.manifest, doc.open (always opens the test page),
// view.set (answered with the events engine protocol §6.2 lists: view.pages, render.invalidate when the revision rose, view.layout.progress
// and overlay.update), doc.close and diag.panic. Everything else, input and accessibility included, is answered with an
// "unsupported_message" error: the real engine from bayan-core (BAYAN_ENGINE=sdk) is the one to test those against.

#include "bayan_ffi.h"

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QStringView>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

constexpr int kProtocolVersion = 1;
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
constexpr qint64 kMaxCoordinate = static_cast<qint64>(quint64{1} << 40U);
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

// An error as the engine reports it (engine protocol §12): its code, the Fluent message identifier that bayan-core's engine derives from
// the code (engine-error- and the code with hyphens for underscores), and arguments that hold only codes and numbers.
QJsonObject errorObject(const QString &code, const QJsonObject &args = {}) {
  QString messageId = u"engine-error-"_s + code;
  messageId.replace(u'_', u'-');
  return QJsonObject{{u"code"_s, code}, {u"message_id"_s, messageId}, {u"args"_s, args}};
}

// The arguments of an error about a message of this type: the type itself, but only if it looks like one (1 to 64 lower-case letters,
// digits, _ and .), so that an error never carries other text of the message back (engine protocol §12).
QJsonObject typeArgs(const QString &type) {
  constexpr qsizetype kMaxTypeLength = 64;
  const bool looksLikeAType = !type.isEmpty() && type.size() <= kMaxTypeLength && std::ranges::all_of(type, [](QChar character) {
    return (character >= u'a' && character <= u'z') || (character >= u'0' && character <= u'9') || character == u'_' || character == u'.';
  });
  return looksLikeAType ? QJsonObject{{u"type"_s, type}} : QJsonObject{};
}

// unsupported_protocol_version, which lists the versions the engine speaks in args.supported (engine protocol §12).
QJsonObject unsupportedVersion() {
  return errorObject(u"unsupported_protocol_version"_s, QJsonObject{{u"supported"_s, QJsonArray{kProtocolVersion}}});
}

// The platforms of engine protocol §14.1 item 2. A missing or unknown value counts as "other", as in bayan-core's engine.
QString platformOf(const QJsonValue &value) {
  constexpr std::array kPlatforms{u"windows", u"macos", u"linux", u"ios", u"android", u"chromeos"};
  const QString name = value.toString();
  return std::ranges::any_of(kPlatforms, [&name](const char16_t *platform) { return name == QStringView(platform); }) ? name : u"other"_s;
}

// The key data of the v1 stub manifest that bayan-core's engine resolves for a platform and runtime (engine protocol §14.3): the expected
// commands and keys of the case for that platform and runtime in the conformance vectors' keys.json (§15), which bayan-core's tests check
// against the engine's own manifest. The build compiles the copy in src/engine/stub/schema/conformance into the stub as a resource.
std::optional<QJsonObject> manifestKeyData(const QString &platform, const QString &runtime) {
  QFile file(u":/bayan/stub/keys.json"_s);
  QJsonObject vectors;
  if (!file.open(QIODevice::ReadOnly) || !parseObject(file.readAll(), vectors)) {
    return std::nullopt;
  }
  for (const auto &value : vectors.value(u"cases"_s).toArray()) {
    const QJsonObject candidate = value.toObject();
    QJsonObject expected = candidate.value(u"expect"_s).toObject();
    if (candidate.value(u"platform"_s).toString() == platform && candidate.value(u"runtime"_s).toString() == runtime &&
        expected.value(u"commands"_s).isArray() && expected.value(u"keys"_s).isObject()) {
      return expected;
    }
  }
  return std::nullopt;
}

QJsonObject ribbonGroup(const QString &id, const QString &label, const QStringList &commands) {
  return QJsonObject{{u"id"_s, id}, {u"label"_s, label}, {u"commands"_s, QJsonArray::fromStringList(commands)}};
}

// The stub manifest's ribbon, as bayan-core's engine has it (its manifest.rs).
QJsonObject stubRibbon() {
  const QJsonObject file{
      {u"id"_s, u"file"_s},
      {u"label"_s, u"File"_s},
      {u"groups"_s, QJsonArray{ribbonGroup(u"document"_s, u"Document"_s, {u"file.open"_s, u"file.save"_s})}},
  };
  const QJsonObject home{
      {u"id"_s, u"home"_s},
      {u"label"_s, u"Home"_s},
      {
          u"groups"_s,
          QJsonArray{
              ribbonGroup(u"history"_s, u"Undo"_s, {u"edit.undo"_s, u"edit.redo"_s}),
              ribbonGroup(u"clipboard"_s, u"Clipboard"_s, {u"edit.paste"_s, u"edit.cut"_s, u"edit.copy"_s}),
              ribbonGroup(u"font"_s, u"Font"_s, {u"format.bold.toggle"_s, u"format.italic.toggle"_s}),
          },
      },
  };
  return QJsonObject{{u"tabs"_s, QJsonArray{file, home}}};
}

// The view modes of view.set (engine protocol §6.2).
bool isViewMode(const QString &mode) { return mode == u"print"_s || mode == u"web"_s || mode == u"draft"_s || mode == u"focus"_s; }

// Whether the calling thread is a stub engine's thread, set when that thread starts. Code on an engine's thread is either the engine's own
// or a shell's callback, so bayan_render_tile, which would wait for an engine thread, returns BAYAN_STATUS_WRONG_THREAD there (spec §8).
bool &onEngineThread() {
  thread_local bool engineThread = false;
  return engineThread;
}

// How the stub answers doc.open and query.a11y; anything but Normal is only for tests (see the top of this file).
enum class DocOpenMode : std::uint8_t { Normal, Fail, Ignore };
enum class QueryA11yMode : std::uint8_t { Normal, Ignore };

// The engine thread's answer to a tile request: the status, and for BAYAN_STATUS_OK the pixels, height rows of 4 × width bytes each.
struct TileResult {
  BayanStatus status = BAYAN_STATUS_INTERNAL_ERROR;
  std::vector<std::uint8_t> pixels;
  // The render revision that the tile shows (engine protocol §14.6).
  std::uint64_t revision = 0;
};

// The answer to a tile request that the engine refuses.
TileResult refused(BayanStatus status) { return TileResult{.status = status, .pixels = {}, .revision = 0}; }

// A tile request that bayan_render_tile queues for the engine thread and then waits for. If the engine stops before it gets to the request,
// the request is destroyed unanswered, and the waiting call gets BAYAN_STATUS_INTERNAL_ERROR from the broken promise.
struct TileJob {
  QByteArray request;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::promise<TileResult> answer;
};

// What the engine thread handles, in the order the shell's calls queued it: a posted message or a tile request.
using Job = std::variant<QByteArray, TileJob>;

} // namespace

// The engine instance behind the opaque pointer of the C interface.
struct BayanEngine {
  // Shared between the shell's threads and the engine thread; guarded by mutex.
  std::mutex mutex;
  std::condition_variable wakeUp;
  std::deque<Job> inbox;
  bool stopping = false;
  // An engine.error said restart: null (engine protocol §14.2): the engine has stopped and answers nothing more.
  bool stopped = false;
  // A message was posted or a tile requested: the callback can no longer change.
  bool posted = false;
  BayanMessageCallback callback = nullptr;
  void *userData = nullptr;

  // Blobs; guarded by blobMutex.
  std::mutex blobMutex;
  std::map<BayanBlobId, std::vector<std::uint8_t>> blobs;
  BayanBlobId nextBlob = 1;

  // Used only on the engine thread.
  bool handshakeDone = false;
  bool documentOpen = false;
  // The test page's render revision (engine protocol §14.6), and the view's mode and formatting marks, which a document opens with.
  std::uint64_t revision = 1;
  QString mode = u"print"_s;
  bool showFormattingMarks = false;
  // The session's platform and runtime, from its hello (§14.1 item 2), for the manifest's key data.
  QString platform = u"other"_s;
  QString runtime = u"native"_s;
  // Set when bayan_engine_free was called from inside the callback: the engine thread then destroys the engine when it has stopped.
  bool destroyWhenStopped = false;
  // Test switches, set before the engine thread starts (see the top of this file).
  DocOpenMode docOpenMode = DocOpenMode::Normal;
  QueryA11yMode queryA11yMode = QueryA11yMode::Normal;
  bool ignoreHello = false;
  bool allowPanic = false;
  // The restart values of the test switch "restarts", and how many of them failures have used.
  std::vector<QJsonValue> restarts;
  std::size_t restartsUsed = 0;
  QString engineVersion = QString::fromLatin1(kStubVersion);
  qint64 nextSequence = 1;

  std::thread thread;

  void run();
  void handle(const QByteArray &bytes);
  void handleMessage(qint64 id, const QString &type, const QJsonObject &payload);
  void handleHello(qint64 id, const QJsonObject &payload);
  void handleManifest(qint64 id);
  void handleOpen(qint64 id, const QJsonObject &payload);
  void handleViewSet(qint64 id, const QJsonObject &payload);
  void handlePanic(qint64 id);
  [[nodiscard]] TileResult renderTile(const QByteArray &requestBytes, std::uint32_t width, std::uint32_t height) const;
  void send(const QJsonObject &message);
  void reply(qint64 id, const QString &type, const QJsonObject &payload);
  void fail(qint64 id, const QJsonObject &error);
  void event(const QString &type, const QJsonObject &payload);
  [[nodiscard]] bool blobExists(BayanBlobId blob);
};

void BayanEngine::run() {
  onEngineThread() = true;
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex);
      wakeUp.wait(lock, [this] { return stopping || !inbox.empty(); });
      if (stopping) {
        break;
      }
      job = std::move(inbox.front());
      inbox.pop_front();
    }
    // Once the engine has stopped after using up its restarts (engine protocol §14.2), it answers nothing more: queued messages are
    // dropped, and queued tile requests fail.
    if (stopped) {
      if (auto *tile = std::get_if<TileJob>(&job)) {
        tile->answer.set_value(refused(BAYAN_STATUS_INTERNAL_ERROR));
      }
      continue;
    }
    if (auto *tile = std::get_if<TileJob>(&job)) {
      TileResult result;
      try {
        result = renderTile(tile->request, tile->width, tile->height);
      } catch (...) {
        // Out of memory while rendering: the waiting call returns BAYAN_STATUS_INTERNAL_ERROR, as for a failure inside the real engine.
        result = TileResult{};
      }
      tile->answer.set_value(std::move(result));
      continue;
    }
    try {
      handle(std::get<QByteArray>(job));
    } catch (...) {
      // Like a caught panic in the real engine: report it and keep running.
      try {
        QJsonObject failure = errorObject(u"internal"_s);
        failure.insert(u"recoverable"_s, true);
        event(u"engine.error"_s, failure);
      } catch (...) { // NOLINT(bugprone-empty-catch): out of memory while reporting an error; nothing more can be done for this message.
      }
    }
  }
  // Freed from inside the callback: bayan_engine_free has returned without waiting, and nothing else may use the engine any more, so its
  // thread destroys it. Tile requests still queued are destroyed with it, unanswered.
  if (destroyWhenStopped) {
    delete this; // NOLINT(cppcoreguidelines-owning-memory): ownership crosses the C interface as a raw pointer.
  }
}

void BayanEngine::send(const QJsonObject &message) {
  BayanMessageCallback target = nullptr;
  void *targetData = nullptr;
  {
    const std::scoped_lock lock(mutex);
    // Once the engine is stopping, nothing more reaches the callback, also when the callback itself has just freed the engine; nor once it
    // has stopped after using up its restarts.
    if (stopping || stopped) {
      return;
    }
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
    fail(0, errorObject(u"invalid_message"_s));
    return;
  }
  const QJsonValue idValue = message.value(u"id"_s);
  const qint64 id = idValue.isDouble() ? idValue.toInteger(0) : 0;
  const QString type = message.value(u"type"_s).toString();
  // The engine reads a hello whatever its v, so that a shell of another version learns which versions it speaks; every other message must
  // carry the session's version (engine protocol §14.1 item 1).
  if (type != u"hello"_s && message.value(u"v"_s).toInteger(-1) != kProtocolVersion) {
    fail(id, unsupportedVersion());
    return;
  }
  handleMessage(id, type, message.value(u"payload"_s).toObject());
}

void BayanEngine::handleMessage(qint64 id, const QString &type, const QJsonObject &payload) {
  if (type == u"hello"_s) {
    handleHello(id, payload);
    return;
  }
  if (!handshakeDone) {
    fail(id, errorObject(u"handshake_required"_s));
    return;
  }
  if (type == u"ui.manifest"_s) {
    handleManifest(id);
  } else if (type == u"doc.open"_s) {
    handleOpen(id, payload);
  } else if (type == u"view.set"_s || type == u"doc.close"_s) {
    if (!documentOpen || payload.value(u"doc_id"_s).toInteger(0) != kDocumentId) {
      fail(id, errorObject(u"not_found"_s, QJsonObject{{u"what"_s, u"document"_s}}));
    } else if (type == u"view.set"_s) {
      handleViewSet(id, payload);
    } else {
      documentOpen = false;
      reply(id, {}, {});
    }
  } else if (type == u"diag.panic"_s) {
    handlePanic(id);
  } else if (type != u"query.a11y"_s || queryA11yMode != QueryA11yMode::Ignore) {
    // Only the type is echoed back, and only if it looks like one: never other text of the message.
    fail(id, errorObject(u"unsupported_message"_s, typeArgs(type)));
  }
}

void BayanEngine::handleHello(qint64 id, const QJsonObject &payload) {
  if (ignoreHello) {
    return;
  }
  const QJsonArray versions = payload.value(u"protocol_versions"_s).toArray();
  if (!versions.contains(QJsonValue(kProtocolVersion))) {
    fail(id, unsupportedVersion());
    return;
  }
  // A new session: the engine closes every document first (engine protocol §5). The platform and the runtime decide only the manifest's
  // key data (§14.1 item 2); a missing or unknown value counts as "other" and "native".
  documentOpen = false;
  const QJsonObject shell = payload.value(u"shell"_s).toObject();
  platform = platformOf(shell.value(u"platform"_s));
  runtime = shell.value(u"runtime"_s).toString() == u"browser"_s ? u"browser"_s : u"native"_s;
  handshakeDone = true;
  reply(id, u"welcome"_s,
        QJsonObject{
            {u"protocol_version"_s, kProtocolVersion},
            {u"engine_version"_s, engineVersion},
            {u"layout_epoch"_s, 0},
            {u"features"_s, QJsonArray{u"stub"_s}},
        });
}

void BayanEngine::handleManifest(qint64 id) {
  // The v1 stub manifest, in English whatever locale is asked for, as bayan-core's engine answers it (engine protocol §6.8, §14.3).
  const auto keyData = manifestKeyData(platform, runtime);
  if (!keyData) {
    fail(id, errorObject(u"internal"_s));
    return;
  }
  reply(id, {},
        QJsonObject{
            {u"manifest_version"_s, 0},
            {u"locale"_s, u"en-US"_s},
            {u"commands"_s, keyData->value(u"commands"_s)},
            {u"keys"_s, keyData->value(u"keys"_s)},
            {u"ribbon"_s, stubRibbon()},
        });
}

void BayanEngine::handleOpen(qint64 id, const QJsonObject &payload) {
  if (docOpenMode == DocOpenMode::Ignore) {
    return;
  }
  if (docOpenMode == DocOpenMode::Fail) {
    fail(id, errorObject(u"limit_exceeded"_s, QJsonObject{{u"limit"_s, u"documents"_s}}));
    return;
  }
  // The stub opens its test page whatever the blob contains, but the blob must exist.
  const auto blob = static_cast<BayanBlobId>(payload.value(u"blob"_s).toInteger(0));
  if (blob == 0 || !blobExists(blob)) {
    fail(id, errorObject(u"not_found"_s, QJsonObject{{u"what"_s, u"blob"_s}}));
    return;
  }
  // A document opens with revision 1, in print mode without formatting marks (engine protocol §14.6 item 2).
  documentOpen = true;
  revision = 1;
  mode = u"print"_s;
  showFormattingMarks = false;
  reply(id, u"doc.opened"_s,
        QJsonObject{
            {u"doc_id"_s, kDocumentId},
            {u"revision"_s, static_cast<qint64>(revision)},
            {u"page_count_estimate"_s, 1},
            {u"warnings"_s, QJsonArray{}},
            {u"fonts"_s, QJsonObject{{u"missing"_s, QJsonArray{}}, {u"machine_dependent"_s, false}}},
        });
}

void BayanEngine::handleViewSet(qint64 id, const QJsonObject &payload) {
  const QString newMode = payload.value(u"mode"_s).toString();
  const QJsonValue marks = payload.value(u"show_formatting_marks"_s);
  if (!isViewMode(newMode) || (!marks.isUndefined() && !marks.isBool())) {
    fail(id, errorObject(u"invalid_request"_s, typeArgs(u"view.set"_s)));
    return;
  }
  // Only a new mode or a change of the formatting marks changes what the tiles show: then the revision rises and the page is
  // invalidated. A new viewport, zoom or device scale changes nothing that tiles requested for a scale show (engine protocol §14.6).
  const bool everyPage = newMode != mode || marks.toBool(false) != showFormattingMarks;
  mode = newMode;
  showFormattingMarks = marks.toBool(false);
  if (everyPage) {
    ++revision;
  }
  reply(id, {}, {});
  const QJsonObject pageRect{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, kPageWidth}, {u"height"_s, kPageHeight}};
  event(u"view.pages"_s, QJsonObject{
                             {u"doc_id"_s, kDocumentId},
                             {
                                 u"pages"_s,
                                 QJsonArray{
                                     QJsonObject{
                                         {u"index"_s, 0},
                                         {u"width"_s, kPageWidth},
                                         {u"height"_s, kPageHeight},
                                         {u"section"_s, 0},
                                         {u"x"_s, 0},
                                         {u"y"_s, 0},
                                     },
                                 },
                             },
                         });
  if (everyPage) {
    event(u"render.invalidate"_s, QJsonObject{
                                      {u"doc_id"_s, kDocumentId},
                                      {u"revision"_s, static_cast<qint64>(revision)},
                                      {u"regions"_s, QJsonArray{QJsonObject{{u"page"_s, 0}, {u"rect"_s, pageRect}}}},
                                  });
  }
  event(u"view.layout.progress"_s,
        QJsonObject{{u"doc_id"_s, kDocumentId}, {u"pages_laid_out"_s, 1}, {u"page_count_estimate"_s, 1}, {u"complete"_s, true}});
  // The test page has no caret and no selection.
  event(u"overlay.update"_s, QJsonObject{
                                 {u"doc_id"_s, kDocumentId},
                                 {u"revision"_s, static_cast<qint64>(revision)},
                                 {u"caret"_s, QJsonValue::Null},
                                 {u"selection"_s, QJsonArray{}},
                                 {u"remote"_s, QJsonArray{}},
                                 {u"highlights"_s, QJsonArray{}},
                             });
}

void BayanEngine::handlePanic(qint64 id) {
  if (!allowPanic) {
    fail(id, errorObject(u"not_allowed"_s, QJsonObject{{u"reason"_s, u"test_switch_off"_s}}));
    return;
  }
  // What the real engine does when a message's handler panics: the session is discarded with its document, the message is answered with
  // the error panic, and engine.error says that the session is gone and what follows (engine protocol §14.2): restart { number, limit },
  // after which the next message other than hello gets handshake_required, or null, after which the engine has stopped. The value is the
  // next one of the test switch "restarts"; the stub counts nothing.
  handshakeDone = false;
  documentOpen = false;
  const QJsonObject panic = errorObject(u"panic"_s);
  if (id > 0) {
    send(QJsonObject{{u"v"_s, kProtocolVersion}, {u"re"_s, id}, {u"ok"_s, false}, {u"error"_s, panic}});
  }
  const QJsonValue restart = restartsUsed < restarts.size() ? restarts.at(restartsUsed++) : QJsonValue(QJsonValue::Null);
  QJsonObject failure = panic;
  failure.insert(u"recoverable"_s, false);
  failure.insert(u"restart"_s, restart);
  event(u"engine.error"_s, failure);
  if (restart.isNull()) {
    const std::scoped_lock lock(mutex);
    stopped = true;
  }
}

TileResult BayanEngine::renderTile(const QByteArray &requestBytes, std::uint32_t width, std::uint32_t height) const {
  QJsonObject request;
  if (!parseObject(requestBytes, request)) {
    return refused(BAYAN_STATUS_INVALID_ARGUMENT);
  }
  // The payload's own width and height may be omitted; if present, they must equal the parameters (engine protocol §3.1).
  for (const auto &[key, size] : {std::pair{u"width"_s, width}, std::pair{u"height"_s, height}}) {
    const QJsonValue given = request.value(key);
    if (!given.isUndefined() && given.toInteger(-1) != qint64{size}) {
      return refused(BAYAN_STATUS_INVALID_ARGUMENT);
    }
  }
  if (!documentOpen || request.value(u"doc_id"_s).toInteger(0) != kDocumentId || request.value(u"page"_s).toInteger(-1) != 0) {
    return refused(BAYAN_STATUS_NOT_FOUND);
  }
  const QJsonObject rect = request.value(u"rect"_s).toObject();
  const qint64 rectX = rect.value(u"x"_s).toInteger(kMaxCoordinate + 1);
  const qint64 rectY = rect.value(u"y"_s).toInteger(kMaxCoordinate + 1);
  const qint64 rectWidth = rect.value(u"width"_s).toInteger(0);
  const qint64 rectHeight = rect.value(u"height"_s).toInteger(0);
  if (rectX < -kMaxCoordinate || rectX > kMaxCoordinate || rectY < -kMaxCoordinate || rectY > kMaxCoordinate || rectWidth <= 0 ||
      rectWidth > kMaxCoordinate || rectHeight <= 0 || rectHeight > kMaxCoordinate) {
    return refused(BAYAN_STATUS_INVALID_ARGUMENT);
  }

  // Sample the page at the centre of every pixel in integer arithmetic, as the real engine does, into rows without padding.
  const std::size_t rowBytes = std::size_t{width} * kBytesPerPixel;
  TileResult result{.status = BAYAN_STATUS_OK, .pixels = std::vector<std::uint8_t>(rowBytes * height), .revision = revision};
  const std::span<std::uint8_t> output(result.pixels);
  for (std::uint32_t row = 0; row < height; ++row) {
    const qint64 y = pixelCentre(rectY, rectHeight, row, height);
    const auto pixels = output.subspan(std::size_t{row} * rowBytes, rowBytes);
    for (std::uint32_t column = 0; column < width; ++column) {
      const qint64 x = pixelCentre(rectX, rectWidth, column, width);
      const Rgba color = testPageColor(x, y);
      std::ranges::copy(std::array{color.r, color.g, color.b, color.a}, pixels.subspan(std::size_t{column} * kBytesPerPixel).begin());
    }
  }
  return result;
}

namespace {

// Reads the values of the test switch "restarts": null, or an object with exactly the integers number and limit, each from 0 to 2^32 - 1
// as the protocol's schema allows them, so that a test can also give the shell values that it must refuse.
bool readRestarts(const QJsonArray &values, std::vector<QJsonValue> &restarts) {
  constexpr qint64 kMaxValue = std::numeric_limits<std::uint32_t>::max();
  for (const auto &value : values) {
    const QJsonObject restart = value.toObject();
    const auto valid = [&restart](const QString &key) {
      const QJsonValue field = restart.value(key);
      return field.isDouble() && field.toInteger(-1) >= 0 && field.toInteger(-1) <= kMaxValue;
    };
    if (!value.isNull() && (!value.isObject() || restart.size() != 2 || !valid(u"number"_s) || !valid(u"limit"_s))) {
      return false;
    }
    restarts.push_back(value);
  }
  return true;
}

// Reads a test switch that names a mode: "normal", which leaves the default, or one of the names given, which sets target.
template <typename Mode> bool readMode(const QJsonValue &setting, std::initializer_list<std::pair<QStringView, Mode>> modes, Mode &target) {
  const QString name = setting.toString();
  if (!setting.isString()) {
    return false;
  }
  if (name == u"normal"_s) {
    return true;
  }
  for (const auto &[modeName, mode] : modes) {
    if (name == modeName) {
      target = mode;
      return true;
    }
  }
  return false;
}

// Reads the test switches described at the top of this file into the engine. Returns false for anything else in "test" and for a value of
// the wrong type, so that a misspelt switch cannot silently test the normal path.
bool configureTestSwitches(BayanEngine &engine, const QJsonValue &value) {
  if (value.isUndefined()) {
    return true;
  }
  if (!value.isObject()) {
    return false;
  }
  const QJsonObject test = value.toObject();
  for (auto it = test.begin(); it != test.end(); ++it) {
    const QString &key = it.key();
    const QJsonValue setting = it.value();
    bool read = false;
    if (key == u"doc_open"_s) {
      read = readMode(setting, {{u"fail", DocOpenMode::Fail}, {u"ignore", DocOpenMode::Ignore}}, engine.docOpenMode);
    } else if (key == u"hello"_s) {
      read = readMode(setting, {{u"ignore", true}}, engine.ignoreHello);
    } else if (key == u"query_a11y"_s) {
      read = readMode(setting, {{u"ignore", QueryA11yMode::Ignore}}, engine.queryA11yMode);
    } else if (key == u"allow_panic"_s) {
      read = setting.isBool();
      engine.allowPanic = setting.toBool();
    } else if (key == u"restarts"_s) {
      read = setting.isArray() && readRestarts(setting.toArray(), engine.restarts);
    } else if (key == u"engine_version"_s) {
      read = setting.isString();
      engine.engineVersion = setting.toString();
    }
    if (!read) {
      return false;
    }
  }
  return true;
}

} // namespace

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
      if (!configureTestSwitches(*engine, config.value(u"test"_s))) {
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
  // From now on nothing more reaches the callback, and posting fails.
  {
    const std::scoped_lock lock(engine->mutex);
    engine->stopping = true;
  }
  engine->wakeUp.notify_all();
  if (engine->thread.get_id() == std::this_thread::get_id()) {
    // From inside the engine's own callback, waiting for the engine thread would wait for this very call. Return at once: the engine thread
    // stops when the callback returns and destroys the engine itself (BayanEngine::run).
    engine->destroyWhenStopped = true;
    engine->thread.detach();
    return;
  }
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
      if (engine->stopping || engine->stopped) {
        return BAYAN_STATUS_INTERNAL_ERROR;
      }
      engine->posted = true;
      engine->inbox.emplace_back(std::move(copy));
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
                              uint32_t width, uint32_t height, size_t stride, uint64_t *revision_out) {
  // The checks, and their order, are those of bayan-core's bayan_render_tile.
  if (engine == nullptr || rgba_out == nullptr || request_len == 0 || width > kMaxTileSide || height > kMaxTileSide) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  const std::size_t rowBytes = std::size_t{width} * kBytesPerPixel;
  // The rows need stride × (height − 1) + rowBytes bytes, which must not overflow.
  if (width == 0 || height == 0 || stride < rowBytes ||
      (height > 1 && stride > (std::numeric_limits<std::size_t>::max() - rowBytes) / (height - 1))) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  if (out_capacity < (stride * (height - 1)) + rowBytes) {
    return BAYAN_STATUS_BUFFER_TOO_SMALL;
  }
  const auto request = messageBytes(request_json, request_len);
  if (!request) {
    return BAYAN_STATUS_INVALID_ARGUMENT;
  }
  // On an engine thread, that is from inside any engine's callback, waiting for an engine thread could wait forever: for this very thread,
  // or for another engine whose callback may be waiting for this one (engine protocol §8).
  if (onEngineThread()) {
    return BAYAN_STATUS_WRONG_THREAD;
  }
  try {
    std::future<TileResult> answer;
    {
      const std::scoped_lock lock(engine->mutex);
      if (engine->stopping || engine->stopped) {
        return BAYAN_STATUS_INTERNAL_ERROR;
      }
      // As in bayan-core's engine, a tile request counts as posting: the callback can no longer change.
      engine->posted = true;
      TileJob job{.request = request->toByteArray(), .width = width, .height = height, .answer = {}};
      answer = job.answer.get_future();
      engine->inbox.emplace_back(std::move(job));
    }
    engine->wakeUp.notify_one();
    // The engine thread renders the tile after every message posted before it. If the engine stops first, the request is destroyed
    // unanswered, and get() throws.
    const TileResult result = answer.get();
    if (result.status != BAYAN_STATUS_OK) {
      return result.status;
    }
    if (result.pixels.size() != rowBytes * height) {
      return BAYAN_STATUS_INTERNAL_ERROR;
    }
    // This thread writes the rows into the caller's buffer, leaving the bytes between them untouched, as bayan-core's engine does.
    const std::span<const std::uint8_t> pixels(result.pixels);
    const std::span<std::uint8_t> output(rgba_out, out_capacity);
    for (std::uint32_t row = 0; row < height; ++row) {
      std::ranges::copy(pixels.subspan(std::size_t{row} * rowBytes, rowBytes), output.subspan(std::size_t{row} * stride).begin());
    }
    if (revision_out != nullptr) {
      *revision_out = result.revision;
    }
    return BAYAN_STATUS_OK;
  } catch (...) {
    return BAYAN_STATUS_INTERNAL_ERROR;
  }
}

} // extern "C"
