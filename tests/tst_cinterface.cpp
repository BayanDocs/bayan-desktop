// Tests of the engine's C interface (bayan_ffi.h) itself, against the engine the build links: the stub (BAYAN_ENGINE=stub) or the real
// engine of bayan-core's C SDK (BAYAN_ENGINE=sdk). They call the C functions directly, with a callback of their own, to check what
// bayan_ffi.h documents, which the stub must do as the real engine does: bayan_render_tile returns BAYAN_STATUS_WRONG_THREAD inside any
// engine's callback, renders the tile after the messages posted before it, and an engine freed from inside its own callback delivers
// nothing more. They also check every message the engine sends in a conversation that reaches every kind of message it has against the
// protocol's JSON Schema: for the stub, that it sends only messages the real engine could send; for the real engine, that the tests'
// checker accepts what the engine really sends.

#include "bayan_ffi.h"
#include "protocolschema.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

bool isStub() { return QString::fromUtf8(bayan_version()).endsWith(u"-stub"_s); }

// Whether the run must have the real engine's mock document (see tst_documentsession.cpp).
bool mockDocumentRequired() { return qEnvironmentVariableIsSet("BAYAN_REQUIRE_MOCK_DOCUMENT"); }

const std::uint8_t *bytesOf(const QByteArray &data) {
  return reinterpret_cast<const std::uint8_t *>(data.constData()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): C passes bytes.
}

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// A render.tile request for the top-left square inch of the first page of document 1, the first document an engine opens.
QJsonObject tileRequest() {
  constexpr qint64 kInch = 1'828'800;
  return QJsonObject{{u"doc_id"_s, 1},
                     {u"page"_s, 0},
                     {u"rect"_s, QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, kInch}, {u"height"_s, kInch}}},
                     {u"zoom"_s, 1.0},
                     {u"device_scale"_s, 1.0},
                     {u"mode"_s, u"interactive"_s}};
}

// Renders the tile of tileRequest(), 8 × 8 pixels, and returns the status.
BayanStatus renderTile(BayanEngine *engine) {
  constexpr std::uint32_t kSide = 8;
  std::vector<std::uint8_t> pixels(std::size_t{kSide} * kSide * 4);
  const QByteArray request = compact(tileRequest());
  return bayan_render_tile(engine, bytesOf(request), static_cast<std::size_t>(request.size()), pixels.data(), pixels.size(), kSide, kSide,
                           std::size_t{kSide} * 4);
}

QJsonObject helloPayload() { return QJsonObject{{u"protocol_versions"_s, QJsonArray{0}}}; }

// An engine created through the C interface, with a callback that keeps every message it receives. A test can also run code of its own
// inside the callback (onMessage), on the engine thread.
class RawEngine {
public:
  explicit RawEngine(const QJsonObject &config = {}) {
    const QByteArray json = compact(config);
    engine_ = bayan_engine_new(bytesOf(json), static_cast<std::size_t>(json.size()));
    if (engine_ != nullptr && bayan_engine_set_callback(engine_, &RawEngine::callback, this) != BAYAN_STATUS_OK) {
      bayan_engine_free(engine_);
      engine_ = nullptr;
    }
  }
  ~RawEngine() {
    if (!freedInCallback_) {
      bayan_engine_free(engine_);
    }
  }
  RawEngine(const RawEngine &) = delete;
  RawEngine &operator=(const RawEngine &) = delete;
  RawEngine(RawEngine &&) = delete;
  RawEngine &operator=(RawEngine &&) = delete;

  [[nodiscard]] BayanEngine *get() const { return engine_; }

  // Runs on the engine thread, inside the callback, for every message, after the message is kept.
  std::function<void(BayanEngine *, const QJsonObject &)> onMessage;

  // Posts a request and returns its identifier, or 0 if the engine refused it; remembers its type, so that its reply can be checked.
  qint64 request(const QString &type, const QJsonObject &payload = {}) {
    const qint64 id = nextId_++;
    if (!postBytes(compact(QJsonObject{{u"v"_s, 0}, {u"id"_s, id}, {u"type"_s, type}, {u"payload"_s, payload}}))) {
      return 0;
    }
    const std::scoped_lock lock(mutex_);
    requestTypes_[id] = type;
    return id;
  }
  // Posts a request whose identifier the caller chose, as given.
  qint64 rawRequest(const QJsonObject &envelope) {
    const qint64 id = envelope.value(u"id"_s).toInteger(0);
    return postBytes(compact(envelope)) ? id : 0;
  }
  bool post(const QString &type, const QJsonObject &payload = {}) {
    return postBytes(compact(QJsonObject{{u"v"_s, 0}, {u"type"_s, type}, {u"payload"_s, payload}}));
  }
  bool postBytes(const QByteArray &json) {
    return bayan_engine_post(engine_, bytesOf(json), static_cast<std::size_t>(json.size())) == BAYAN_STATUS_OK;
  }
  // Takes the next identifier, for a request posted with rawRequest().
  qint64 takeId() { return nextId_++; }

  // Waits until a message satisfies the predicate; returns it, or an empty object after 10 seconds.
  QJsonObject waitFor(const std::function<bool(const QJsonObject &)> &predicate) {
    std::unique_lock lock(mutex_);
    QJsonObject found;
    arrived_.wait_for(lock, 10s, [&] {
      for (const QJsonObject &message : std::as_const(received_)) {
        if (predicate(message)) {
          found = message;
          return true;
        }
      }
      return false;
    });
    return found;
  }
  QJsonObject waitForReply(qint64 id) {
    return waitFor([id](const QJsonObject &message) { return message.value(u"re"_s).toInteger(0) == id; });
  }

  [[nodiscard]] QList<QJsonObject> received() const {
    const std::scoped_lock lock(mutex_);
    return received_;
  }
  [[nodiscard]] QString requestType(qint64 id) const {
    const std::scoped_lock lock(mutex_);
    const auto found = requestTypes_.find(id);
    return found == requestTypes_.end() ? QString() : found->second;
  }
  // Called by a test's onMessage when it frees the engine inside the callback, so that the destructor does not free it again.
  void markFreedInCallback() { freedInCallback_ = true; }

private:
  static void callback(void *userData, const std::uint8_t *json, std::size_t length) {
    auto *self = static_cast<RawEngine *>(userData);
    const QJsonObject message =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the C interface passes UTF-8 as bytes.
        QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(json), static_cast<qsizetype>(length))).object();
    {
      const std::scoped_lock lock(self->mutex_);
      self->received_.append(message);
    }
    if (self->onMessage) {
      self->onMessage(self->engine_, message);
    }
    self->arrived_.notify_all();
  }

  BayanEngine *engine_ = nullptr;
  std::atomic<bool> freedInCallback_{false};
  qint64 nextId_ = 1;
  mutable std::mutex mutex_;
  std::condition_variable arrived_;
  QList<QJsonObject> received_;
  std::map<qint64, QString> requestTypes_;
};

// Waits for the reply to a request and returns whether it succeeded.
bool succeeds(RawEngine &engine, qint64 id) { return id > 0 && engine.waitForReply(id).value(u"ok"_s).toBool(false); }

} // namespace

class TestCInterface final : public QObject {
  Q_OBJECT

private slots:
  void theHeaderHasTheStatusesOfTheRealEngine();
  void renderTileReturnsWrongThreadInsideAnyCallback();
  void tilesAreRenderedAfterThePostedMessages();
  void freeingInsideTheCallbackDeliversNothingMore();
  void everyMessageOfTheEngineMatchesTheSchema();
};

void TestCInterface::theHeaderHasTheStatusesOfTheRealEngine() {
  // The values of the generated header that the shell relies on; the header itself is compared with the SDK's by engine.sdk_copies.
  static_assert(BAYAN_STATUS_OK == 0 && BAYAN_STATUS_INVALID_ARGUMENT == 1 && BAYAN_STATUS_NOT_FOUND == 2 &&
                BAYAN_STATUS_BUFFER_TOO_SMALL == 3 && BAYAN_STATUS_INTERNAL_ERROR == 4 && BAYAN_STATUS_WRONG_THREAD == 5);
  QVERIFY(bayan_version() != nullptr);
}

void TestCInterface::renderTileReturnsWrongThreadInsideAnyCallback() {
  // Inside its own callback, waiting for the engine thread would wait for itself; inside another engine's callback it could wait for an
  // engine that waits for this one. Both get BAYAN_STATUS_WRONG_THREAD at once (engine protocol §8), while posting from the callback works.
  RawEngine other;
  QVERIFY(other.get() != nullptr);
  RawEngine engine;
  QVERIFY(engine.get() != nullptr);
  std::atomic<int> ownStatus{-1};
  std::atomic<int> otherStatus{-1};
  std::atomic<int> postStatus{-1};
  engine.onMessage = [&](BayanEngine *self, const QJsonObject &message) {
    if (message.value(u"re"_s).toInteger(0) == 1) {
      ownStatus = renderTile(self);
      otherStatus = renderTile(other.get());
      const QByteArray json = compact(QJsonObject{{u"v"_s, 0}, {u"type"_s, u"no.such.message"_s}});
      postStatus = bayan_engine_post(self, bytesOf(json), static_cast<std::size_t>(json.size()));
    }
  };
  QVERIFY(succeeds(engine, engine.request(u"hello"_s, helloPayload())));
  QCOMPARE(ownStatus.load(), BAYAN_STATUS_WRONG_THREAD);
  QCOMPARE(otherStatus.load(), BAYAN_STATUS_WRONG_THREAD);
  QCOMPARE(postStatus.load(), BAYAN_STATUS_OK);
  // The message posted from inside the callback is handled after the current one: it has no identifier, so the engine reports its error as
  // engine.error.
  QVERIFY(!engine.waitFor([](const QJsonObject &message) { return message.value(u"type"_s) == u"engine.error"_s; }).isEmpty());
  // Outside any callback the same call renders the tile.
  QVERIFY(succeeds(engine,
                   engine.request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(bayan_blob_put(engine.get(), nullptr, 0))}})));
  QCOMPARE(renderTile(engine.get()), BAYAN_STATUS_OK);
}

void TestCInterface::tilesAreRenderedAfterThePostedMessages() {
  // bayan_render_tile waits for the messages posted before it (engine protocol §3.1, §8): a tile requested right after doc.open is posted,
  // without waiting for its reply, shows the document, and one requested right after doc.close is posted finds no document.
  RawEngine engine;
  QVERIFY(engine.get() != nullptr);
  const BayanBlobId blob = bayan_blob_put(engine.get(), nullptr, 0);
  QVERIFY(blob != 0);
  const qint64 hello = engine.request(u"hello"_s, helloPayload());
  const qint64 open = engine.request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(blob)}});
  QVERIFY(hello > 0 && open > 0);
  QCOMPARE(renderTile(engine.get()), BAYAN_STATUS_OK);
  // The tile above assumed that the document is number 1.
  QCOMPARE(engine.waitForReply(open).value(u"payload"_s).toObject().value(u"doc_id"_s).toInteger(), 1);
  QVERIFY(engine.request(u"doc.close"_s, QJsonObject{{u"doc_id"_s, 1}}) > 0);
  QCOMPARE(renderTile(engine.get()), BAYAN_STATUS_NOT_FOUND);
  QCOMPARE(bayan_blob_release(engine.get(), blob), BAYAN_STATUS_OK);
}

void TestCInterface::freeingInsideTheCallbackDeliversNothingMore() {
  // view.set is answered with an empty reply followed by four events (engine protocol §6). Freeing the engine inside the callback that
  // receives the reply returns at once, and the four events are never delivered: the callback is never called again (bayan_ffi.h).
  auto engine = std::make_unique<RawEngine>();
  QVERIFY(engine->get() != nullptr);
  QVERIFY(succeeds(*engine, engine->request(u"hello"_s, helloPayload())));
  const auto blob = static_cast<qint64>(bayan_blob_put(engine->get(), nullptr, 0));
  QVERIFY(succeeds(*engine, engine->request(u"doc.open"_s, QJsonObject{{u"blob"_s, blob}})));
  std::atomic<bool> freed{false};
  std::atomic<bool> calledAfterFree{false};
  const qint64 view = 3;
  RawEngine *raw = engine.get();
  engine->onMessage = [&freed, &calledAfterFree, raw, view](BayanEngine *self, const QJsonObject &message) {
    if (freed) {
      calledAfterFree = true;
      return;
    }
    if (message.value(u"re"_s).toInteger(0) == view) {
      raw->markFreedInCallback();
      bayan_engine_free(self);
      freed = true;
    }
  };
  const QJsonObject viewport{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 800}, {u"height"_s, 600}};
  QCOMPARE(engine->request(u"view.set"_s, QJsonObject{{u"doc_id"_s, 1},
                                                      {u"viewport"_s, viewport},
                                                      {u"zoom"_s, 1.0},
                                                      {u"device_scale"_s, 1.0},
                                                      {u"mode"_s, u"print"_s},
                                                      {u"show_formatting_marks"_s, false}}),
           view);
  QTRY_VERIFY(freed);
  QTest::qWait(300);
  QVERIFY(!calledAfterFree);
  QCOMPARE(engine->received().last().value(u"re"_s).toInteger(), view);
}

void TestCInterface::everyMessageOfTheEngineMatchesTheSchema() {
  // A conversation that makes the engine send every kind of message it has: replies, events and errors. Every one of them must be a message
  // the protocol's schema allows, and the conversation must have produced the kinds expected of this engine.
  RawEngine engine;
  QVERIFY(engine.get() != nullptr);
  const auto waitedFor = [&engine](qint64 id) { return id > 0 && !engine.waitForReply(id).isEmpty(); };

  // Errors before the handshake: a hello without a common version, a message for another protocol version, a request before hello, and
  // bytes that are not JSON, which have no identifier to reply to.
  QVERIFY(waitedFor(engine.request(u"hello"_s, QJsonObject{{u"protocol_versions"_s, QJsonArray{99}}})));
  QVERIFY(waitedFor(engine.rawRequest(QJsonObject{{u"v"_s, 1}, {u"id"_s, engine.takeId()}, {u"type"_s, u"hello"_s}})));
  QVERIFY(waitedFor(engine.request(u"doc.open"_s, QJsonObject{{u"blob"_s, 1}})));
  QVERIFY(engine.postBytes("not json"_ba));

  // The handshake and the document.
  QVERIFY(waitedFor(engine.request(
      u"hello"_s,
      QJsonObject{{u"protocol_versions"_s, QJsonArray{0}},
                  {u"shell"_s, QJsonObject{{u"name"_s, u"tst_cinterface"_s}, {u"version"_s, u"0"_s}, {u"platform"_s, u"test"_s}}},
                  {u"capabilities"_s, QJsonObject{{u"clipboard_formats"_s, QJsonArray{}}, {u"ime"_s, true}, {u"accessibility"_s, true}}},
                  {u"locale"_s, u"en"_s},
                  {u"theme"_s, u"light"_s}})));
  const auto blob = static_cast<qint64>(bayan_blob_put(engine.get(), nullptr, 0));
  QVERIFY(waitedFor(engine.request(u"doc.open"_s, QJsonObject{{u"blob"_s, blob}})));
  QCOMPARE(bayan_blob_release(engine.get(), static_cast<BayanBlobId>(blob)), BAYAN_STATUS_OK);
  const QJsonObject viewport{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 1024}, {u"height"_s, 768}};
  QVERIFY(waitedFor(engine.request(u"view.set"_s, QJsonObject{{u"doc_id"_s, 1},
                                                              {u"viewport"_s, viewport},
                                                              {u"zoom"_s, 1.0},
                                                              {u"device_scale"_s, 1.0},
                                                              {u"mode"_s, u"print"_s},
                                                              {u"show_formatting_marks"_s, false}})));
  QVERIFY(waitedFor(engine.request(u"query.a11y"_s, QJsonObject{{u"doc_id"_s, 1}})));

  // Input, which the mock document of the real engine answers with changes to its editable line; the stub answers unsupported_message.
  const QJsonObject noModifiers{{u"shift"_s, false}, {u"ctrl"_s, false}, {u"alt"_s, false}, {u"meta"_s, false}};
  QVERIFY(waitedFor(engine.request(u"input.text"_s, QJsonObject{{u"doc_id"_s, 1}, {u"text"_s, u"abc"_s}})));
  QVERIFY(waitedFor(engine.request(u"input.key"_s, QJsonObject{{u"doc_id"_s, 1},
                                                               {u"key"_s, u"ArrowLeft"_s},
                                                               {u"code"_s, u"ArrowLeft"_s},
                                                               {u"modifiers"_s, noModifiers},
                                                               {u"repeat"_s, false},
                                                               {u"composing"_s, false}})));
  for (const QString &phase : {u"start"_s, u"update"_s, u"end"_s}) {
    QVERIFY(waitedFor(engine.request(
        u"input.composition"_s, QJsonObject{{u"doc_id"_s, 1},
                                            {u"phase"_s, phase},
                                            {u"text"_s, phase == u"update"_s ? u"か"_s : QString()},
                                            {u"selection"_s, QJsonObject{{u"start"_s, 0}, {u"end"_s, phase == u"update"_s ? 1 : 0}}}})));
  }
  QVERIFY(waitedFor(engine.request(u"input.pointer"_s, QJsonObject{{u"doc_id"_s, 1},
                                                                   {u"kind"_s, u"down"_s},
                                                                   {u"x"_s, 150},
                                                                   {u"y"_s, 140},
                                                                   {u"button"_s, 0},
                                                                   {u"clicks"_s, 1},
                                                                   {u"modifiers"_s, noModifiers},
                                                                   {u"pointer_type"_s, u"mouse"_s}})));
  QVERIFY(waitedFor(engine.request(u"ui.manifest"_s, QJsonObject{{u"locale"_s, u"en"_s}})));

  // A tile through the message, a recording and its replay; the blobs the engine creates belong to the test, which releases them.
  QJsonObject tile = tileRequest();
  tile.insert(u"width"_s, 8);
  tile.insert(u"height"_s, 8);
  const QJsonObject tileReply = engine.waitForReply(engine.request(u"render.tile"_s, tile));
  QVERIFY(!tileReply.isEmpty());
  const QJsonObject recordStart = engine.waitForReply(engine.request(u"diag.record.start"_s));
  QVERIFY(!recordStart.isEmpty());
  QVERIFY(waitedFor(engine.request(u"input.text"_s, QJsonObject{{u"doc_id"_s, 1}, {u"text"_s, u"d"_s}})));
  const QJsonObject recording = engine.waitForReply(engine.request(u"diag.record.stop"_s));
  QVERIFY(!recording.isEmpty());
  const qint64 recordingBlob = recording.value(u"payload"_s).toObject().value(u"blob"_s).toInteger(0);
  QVERIFY(waitedFor(engine.request(u"diag.replay"_s, QJsonObject{{u"blob"_s, recordingBlob}})));
  for (const qint64 created : {tileReply.value(u"payload"_s).toObject().value(u"blob"_s).toInteger(0), recordingBlob}) {
    if (created != 0) {
      QCOMPARE(bayan_blob_release(engine.get(), static_cast<BayanBlobId>(created)), BAYAN_STATUS_OK);
    }
  }

  // Errors in the session: a document that is not open, an unknown message with and without an identifier, diag.panic without its switch.
  QVERIFY(waitedFor(engine.request(u"doc.close"_s, QJsonObject{{u"doc_id"_s, 1}})));
  QVERIFY(waitedFor(engine.request(u"doc.close"_s, QJsonObject{{u"doc_id"_s, 1}})));
  QVERIFY(waitedFor(engine.request(u"no.such.message"_s)));
  QVERIFY(engine.post(u"no.such.message"_s));
  QVERIFY(waitedFor(engine.request(u"diag.panic"_s)));

  // A panic, from an engine created with the test switch that allows it.
  RawEngine panicking(QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}}}});
  QVERIFY(panicking.get() != nullptr);
  QVERIFY(waitedFor(panicking.request(u"hello"_s, helloPayload())));
  QVERIFY(waitedFor(panicking.request(u"diag.panic"_s)));
  QVERIFY(!panicking
               .waitFor([](const QJsonObject &message) {
                 return message.value(u"type"_s) == u"engine.error"_s &&
                        message.value(u"payload"_s).toObject().value(u"recoverable"_s) == QJsonValue(false);
               })
               .isEmpty());

  // The stub's own test switches (src/engine/stub/bayan_engine_stub.cpp) send only messages the real engine could send too: a refused
  // doc.open, and a welcome with another engine version. The real engine refuses to start with switches it does not know.
  std::vector<std::unique_ptr<RawEngine>> switched;
  if (isStub()) {
    switched.push_back(std::make_unique<RawEngine>(
        QJsonObject{{u"test"_s, QJsonObject{{u"doc_open"_s, u"fail"_s}, {u"engine_version"_s, u"<b>9.9.9</b>"_s}}}}));
    RawEngine &stub = *switched.back();
    QVERIFY(stub.get() != nullptr);
    QVERIFY(waitedFor(stub.request(u"hello"_s, helloPayload())));
    const auto stubBlob = static_cast<qint64>(bayan_blob_put(stub.get(), nullptr, 0));
    QVERIFY(!stub.waitForReply(stub.request(u"doc.open"_s, QJsonObject{{u"blob"_s, stubBlob}})).isEmpty());
  } else {
    QVERIFY(RawEngine(QJsonObject{{u"test"_s, QJsonObject{{u"doc_open"_s, u"fail"_s}}}}).get() == nullptr);
  }

  // Every message checks out, and the kinds of messages are those this engine sends.
  QSet<QString> kinds;
  std::vector<RawEngine *> sources{&engine, &panicking};
  for (const auto &stub : switched) {
    sources.push_back(stub.get());
  }
  for (RawEngine *source : sources) {
    for (const QJsonObject &message : source->received()) {
      const QString requestType = source->requestType(message.value(u"re"_s).toInteger(0));
      const QStringList problems = bayan::test::ProtocolSchema::engineProtocol().problemsOfMessageFromEngine(message, requestType);
      QVERIFY2(problems.isEmpty(),
               qPrintable(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)) + u"\n"_s + problems.join(u'\n')));
      if (message.contains(u"re"_s)) {
        kinds.insert(message.value(u"ok"_s).toBool() ? requestType
                                                     : u"error "_s + message.value(u"error"_s).toObject().value(u"code"_s).toString());
      } else {
        const QJsonObject payload = message.value(u"payload"_s).toObject();
        kinds.insert(message.value(u"type"_s).toString() +
                     (payload.contains(u"code"_s) ? u" "_s + payload.value(u"code"_s).toString() : QString()));
      }
    }
  }
  QSet<QString> expected{u"hello"_s,
                         u"doc.open"_s,
                         u"view.set"_s,
                         u"doc.close"_s,
                         u"view.pages"_s,
                         u"render.invalidate"_s,
                         u"view.layout.progress"_s,
                         u"overlay.update"_s,
                         u"error unsupported_protocol_version"_s,
                         u"error handshake_required"_s,
                         u"error not_found"_s,
                         u"error unsupported_message"_s,
                         u"error not_allowed"_s,
                         u"error panic"_s,
                         u"engine.error invalid_message"_s,
                         u"engine.error unsupported_message"_s,
                         u"engine.error panic"_s};
  if (isStub()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    expected.insert(u"error limit_exceeded"_s);
  } else {
    // The real engine implements what the stub refuses: the mock document's accessibility tree and input, the UI manifest, tiles through
    // messages, recordings and replays.
    expected.unite(QSet<QString>{u"query.a11y"_s, u"a11y.update"_s, u"input.text"_s, u"input.key"_s, u"input.composition"_s,
                                 u"input.pointer"_s, u"ui.manifest"_s, u"render.tile"_s, u"diag.record.start"_s, u"diag.record.stop"_s,
                                 u"diag.replay"_s});
  }
  const QSet<QString> missing = expected - kinds;
  QVERIFY2(missing.isEmpty(), qPrintable(u"no message of these kinds: "_s + QStringList(missing.cbegin(), missing.cend()).join(u", "_s)));
}

QTEST_GUILESS_MAIN(TestCInterface)
#include "tst_cinterface.moc"
