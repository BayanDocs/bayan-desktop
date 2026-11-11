// Unit tests for the engine wrapper (src/engine/engine.h), against the engine the build links: the stub engine (BAYAN_ENGINE=stub) or the
// real engine of bayan-core's C SDK (BAYAN_ENGINE=sdk). The checks of particular pixels and pages know the stub's test page and skip with
// the real engine, whose pixels tst_canvas compares with the engine's own rendering.

#include "engine.h"
#include "enginemessages.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

// Whether the linked engine is the stub, which reports a version ending in -stub (also as the stand-in SDK of scripts/test-engine-sdk.sh).
bool isStub() { return bayan::Engine::version().endsWith(u"-stub"_s); }

// Geometry of a US Letter page in BLU (the stub's test page and the pages of the real engine's mock document), and the factor from BLU to
// device-independent pixels at 100 %.
constexpr qint64 kPageWidth = 15'544'800;
constexpr qint64 kPageHeight = 20'116'800;
constexpr qint64 kBluPerDip = 19'050;

QJsonObject helloPayload() { return QJsonObject{{u"protocol_versions"_s, QJsonArray{bayan::Engine::kProtocolVersion}}}; }

QJsonObject pageRequest(qint64 x, qint64 y, qint64 width, qint64 height) {
  return QJsonObject{{u"doc_id"_s, 1},
                     {u"page"_s, 0},
                     {u"rect"_s, QJsonObject{{u"x"_s, x}, {u"y"_s, y}, {u"width"_s, width}, {u"height"_s, height}}},
                     {u"zoom"_s, 1.0},
                     {u"device_scale"_s, 1.0},
                     {u"mode"_s, u"interactive"_s}};
}

// An event the engine sends, for the tests that hand the wrapper messages as if from the engine (tests/support/enginemessages.h).
QJsonObject progressEvent() {
  return bayan::test::engineEvent(
      u"view.layout.progress"_s,
      QJsonObject{{u"doc_id"_s, 1}, {u"pages_laid_out"_s, 1}, {u"page_count_estimate"_s, 1}, {u"complete"_s, true}});
}

// What the engine answers a request of the type no.such.message, which the tests below post.
QJsonObject unsupportedReply(qint64 requestId) {
  return QJsonObject{{u"v"_s, bayan::Engine::kProtocolVersion},
                     {u"re"_s, requestId},
                     {u"ok"_s, false},
                     {u"error"_s, QJsonObject{{u"code"_s, u"unsupported_message"_s},
                                              {u"message_id"_s, u"engine-error-unsupported-message"_s},
                                              {u"args"_s, QJsonObject{{u"type"_s, u"no.such.message"_s}}}}}};
}

// Waits for the reply to one request and returns its arguments (requestId, ok, payload, error).
QList<QVariant> waitForReply(QSignalSpy &spy, qint64 requestId) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    for (const QList<QVariant> &reply : std::as_const(spy)) {
      if (reply.at(0).toLongLong() == requestId) {
        return reply;
      }
    }
    if (!spy.wait(100)) {
      continue;
    }
  }
  return {};
}

} // namespace

class TestEngine final : public QObject {
  Q_OBJECT

private slots:
  void versionIsReported();
  void handshakeRepliesOnTheMainThread();
  void unsupportedProtocolVersionIsRefused();
  void messagesBeforeTheHandshakeAreRefused();
  void unknownRequestsGetAnErrorReply();
  void openingADocumentAnnouncesItsPages();
  void blobsRoundTrip();
  void tileShowsTheTestPage();
  void tileOutsideThePageIsTransparent();
  void invalidTileRequestsAreRefused();
  void deliveriesFromAnotherThreadArriveOnTheMainThread();
  void malformedEngineMessagesAreProtocolErrors();
  void repliesMustAnswerPostedRequestsInOrder();
  void oversizedEngineMessagesAreRejected();
  void destroyingWhileBusyIsSafe();
  void diagPanicNeedsTheTestSwitch();
  void aPanicDiscardsTheSession();
  void requestsOfOneTypeCanBeSentAsPanics();

private:
  // Starts an engine with a configuration, completes the handshake and opens its document (the stub's test page or the real engine's mock
  // document).
  static std::unique_ptr<bayan::Engine> openedEngine(const QJsonObject &config = {});
};

std::unique_ptr<bayan::Engine> TestEngine::openedEngine(const QJsonObject &config) {
  auto engine = bayan::Engine::create(config);
  if (!engine) {
    return nullptr;
  }
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  const qint64 hello = engine->request(u"hello"_s, helloPayload());
  if (!waitForReply(replies, hello).value(1).toBool()) {
    return nullptr;
  }
  const bayan::BlobId blob = engine->putBlob(QByteArray());
  const qint64 open = engine->request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(blob)}});
  if (!waitForReply(replies, open).value(1).toBool()) {
    return nullptr;
  }
  return engine;
}

void TestEngine::versionIsReported() {
  const QString version = bayan::Engine::version();
  QVERIFY(!version.isEmpty());
  if (isStub()) {
    QCOMPARE(version, u"0.0.0-stub"_s);
  }
}

void TestEngine::handshakeRepliesOnTheMainThread() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QThread *replyThread = nullptr;
  connect(engine.get(), &bayan::Engine::replyReceived, this, [&replyThread] { replyThread = QThread::currentThread(); });
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);

  const qint64 hello = engine->request(u"hello"_s, helloPayload());
  QVERIFY(hello > 0);
  const QList<QVariant> reply = waitForReply(replies, hello);
  QVERIFY(!reply.isEmpty());
  QVERIFY(reply.at(1).toBool());
  const auto payload = reply.at(2).toJsonObject();
  QCOMPARE(payload.value(u"protocol_version"_s).toInt(-1), bayan::Engine::kProtocolVersion);
  QCOMPARE(payload.value(u"engine_version"_s).toString(), bayan::Engine::version());
  QCOMPARE(replyThread, QThread::currentThread());
}

void TestEngine::unsupportedProtocolVersionIsRefused() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  const qint64 hello = engine->request(u"hello"_s, QJsonObject{{u"protocol_versions"_s, QJsonArray{99}}});
  const QList<QVariant> reply = waitForReply(replies, hello);
  QVERIFY(!reply.isEmpty());
  QVERIFY(!reply.at(1).toBool());
  QCOMPARE(reply.at(3).toJsonObject().value(u"code"_s).toString(), u"unsupported_protocol_version"_s);
}

void TestEngine::messagesBeforeTheHandshakeAreRefused() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  const qint64 open = engine->request(u"doc.open"_s, QJsonObject{{u"blob"_s, 1}});
  const QList<QVariant> reply = waitForReply(replies, open);
  QVERIFY(!reply.isEmpty());
  QVERIFY(!reply.at(1).toBool());
  QCOMPARE(reply.at(3).toJsonObject().value(u"code"_s).toString(), u"handshake_required"_s);
}

void TestEngine::unknownRequestsGetAnErrorReply() {
  const auto engine = openedEngine();
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  const qint64 request = engine->request(u"no.such.message"_s);
  const QList<QVariant> reply = waitForReply(replies, request);
  QVERIFY(!reply.isEmpty());
  QVERIFY(!reply.at(1).toBool());
  const auto error = reply.at(3).toJsonObject();
  QCOMPARE(error.value(u"code"_s).toString(), u"unsupported_message"_s);
  QCOMPARE(error.value(u"message_id"_s).toString(), u"engine-error-unsupported-message"_s);
}

void TestEngine::openingADocumentAnnouncesItsPages() {
  const auto engine = openedEngine();
  QVERIFY(engine);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  const QJsonObject viewport{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 1024}, {u"height"_s, 768}};
  QVERIFY(engine->post(
      u"view.set"_s,
      QJsonObject{{u"doc_id"_s, 1}, {u"viewport"_s, viewport}, {u"zoom"_s, 1.0}, {u"device_scale"_s, 1.0}, {u"mode"_s, u"print"_s}}));
  QTRY_VERIFY(events.count() >= 3);
  // First the pages, then what to draw again (engine protocol §6.2, §6.3).
  QCOMPARE(events.at(0).at(0).toString(), u"view.pages"_s);
  const QJsonArray pages = events.at(0).at(1).toJsonObject().value(u"pages"_s).toArray();
  // The stub has one test page; the real engine's mock document has three. Both are US Letter.
  QCOMPARE(pages.size(), isStub() ? 1 : 3);
  for (const auto &page : pages) {
    QCOMPARE(page.toObject().value(u"width"_s).toInteger(), kPageWidth);
    QCOMPARE(page.toObject().value(u"height"_s).toInteger(), kPageHeight);
  }
  QCOMPARE(events.at(1).at(0).toString(), u"render.invalidate"_s);
  bool progress = false;
  for (const auto &event : std::as_const(events)) {
    progress = progress || event.at(0).toString() == u"view.layout.progress"_s;
  }
  QVERIFY(progress);
}

void TestEngine::blobsRoundTrip() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  const QByteArray bytes = "PK\x03\x04 not really a document"_ba;
  const bayan::BlobId id = engine->putBlob(bytes);
  QVERIFY(id != 0);
  const std::optional<QByteArray> copy = engine->blob(id);
  QVERIFY(copy.has_value());
  QCOMPARE(copy.value_or(QByteArray()), bytes);

  const bayan::BlobId empty = engine->putBlob(QByteArray());
  QVERIFY(empty != 0 && empty != id);
  const std::optional<QByteArray> emptyCopy = engine->blob(empty);
  QVERIFY(emptyCopy.has_value());
  QVERIFY(emptyCopy.value_or("not empty"_ba).isEmpty());

  QVERIFY(engine->releaseBlob(id));
  QVERIFY(!engine->releaseBlob(id));
  QVERIFY(!engine->blob(id).has_value());
  QVERIFY(!engine->blob(12345).has_value());
}

void TestEngine::tileShowsTheTestPage() {
  if (!isStub()) {
    QSKIP("Checks the stub's test page; tst_canvas compares the real engine's pixels with its own rendering.");
  }
  const auto engine = openedEngine();
  QVERIFY(engine);
  // The whole page at 100 %: 816 x 1056 device-independent pixels.
  const QSize size(static_cast<int>(kPageWidth / kBluPerDip), static_cast<int>(kPageHeight / kBluPerDip));
  QCOMPARE(size, QSize(816, 1056));
  const QImage tile = engine->renderTile(pageRequest(0, 0, kPageWidth, kPageHeight), size);
  QVERIFY(!tile.isNull());
  QCOMPARE(tile.size(), size);
  QCOMPARE(tile.format(), QImage::Format_RGBA8888_Premultiplied);
  QCOMPARE(tile.pixelColor(48, 48), QColor(255, 255, 255));   // margin
  QCOMPARE(tile.pixelColor(408, 120), QColor(31, 95, 191));   // heading bar
  QCOMPARE(tile.pixelColor(200, 198), QColor(154, 160, 166)); // first text line
  QCOMPARE(tile.pixelColor(200, 220), QColor(255, 255, 255)); // between lines

  // The same tile again gives exactly the same pixels (the engine is deterministic).
  QCOMPARE(engine->renderTile(pageRequest(0, 0, kPageWidth, kPageHeight), size), tile);
}

void TestEngine::tileOutsideThePageIsTransparent() {
  if (!isStub()) {
    QSKIP("Checks the stub's test page.");
  }
  const auto engine = openedEngine();
  QVERIFY(engine);
  // A tile whose left half lies beyond the page's left edge.
  const QImage tile = engine->renderTile(pageRequest(-kPageWidth / 2, 0, kPageWidth, kPageHeight), QSize(200, 100));
  QVERIFY(!tile.isNull());
  QCOMPARE(tile.pixelColor(10, 10).alpha(), 0);              // beyond the page
  QCOMPARE(tile.pixelColor(110, 10), QColor(255, 255, 255)); // the page's left margin
  QCOMPARE(tile.pixelColor(190, 10), QColor(31, 95, 191));   // the heading bar
}

void TestEngine::invalidTileRequestsAreRefused() {
  const auto engine = openedEngine();
  QVERIFY(engine);
  const QJsonObject valid = pageRequest(0, 0, kPageWidth, kPageHeight);
  QVERIFY(engine->renderTile(valid, QSize(0, 10)).isNull());
  QVERIFY(engine->renderTile(valid, QSize(10, -1)).isNull());
  QVERIFY(engine->renderTile(valid, QSize(bayan::Engine::kMaxTileSide + 1, 10)).isNull());
  QVERIFY(engine->renderTile(pageRequest(0, 0, 0, kPageHeight), QSize(10, 10)).isNull()); // empty rectangle
  QVERIFY(engine->renderTile(pageRequest(0, 0, kPageWidth, -5), QSize(10, 10)).isNull()); // negative height
  QJsonObject otherPage = valid;
  otherPage.insert(u"page"_s, 7);
  QVERIFY(engine->renderTile(otherPage, QSize(10, 10)).isNull());
  QJsonObject otherDocument = valid;
  otherDocument.insert(u"doc_id"_s, 2);
  QVERIFY(engine->renderTile(otherDocument, QSize(10, 10)).isNull());

  // Before any document is open, nothing can be rendered.
  const auto fresh = bayan::Engine::create();
  QVERIFY(fresh);
  QVERIFY(fresh->renderTile(valid, QSize(10, 10)).isNull());
}

void TestEngine::deliveriesFromAnotherThreadArriveOnTheMainThread() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QThread *eventThread = nullptr;
  connect(engine.get(), &bayan::Engine::eventReceived, this, [&eventThread] { eventThread = QThread::currentThread(); });
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);

  // Play the engine thread: deliver a message from a different thread, as the engine's callback does.
  bool delivered = false;
  std::thread engineThread([&engine, &delivered] { delivered = bayan::test::deliver(*engine, progressEvent()); });
  engineThread.join();
  QVERIFY(delivered);
  QCOMPARE(events.count(), 0); // Nothing is emitted on the engine thread itself.
  QTRY_COMPARE(events.count(), 1);
  QCOMPARE(events.at(0).at(0).toString(), u"view.layout.progress"_s);
  QCOMPARE(eventThread, QThread::currentThread());
}

void TestEngine::malformedEngineMessagesAreProtocolErrors() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy errors(engine.get(), &bayan::Engine::protocolError);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  // Each is wrong in the one way it names, and otherwise has the shape of a message the engine sends.
  const QList<QPair<QByteArray, QString>> malformed{
      {"not json"_ba, u"not JSON"_s},
      {"[1, 2, 3]"_ba, u"not an object"_s},
      {R"({"v":5,"type":"view.layout.progress","seq":1,"payload":{"doc_id":1,"pages_laid_out":1,"page_count_estimate":1,"complete":true}})"_ba,
       u"another protocol version"_s},
      {R"({"v":0,"seq":1,"payload":{}})"_ba, u"neither a type nor a reply"_s},
      {R"({"v":0,"re":0,"ok":true,"payload":{}})"_ba, u"an invalid request identifier"_s},
      {R"({"v":0,"re":3,"ok":"yes","payload":{}})"_ba, u"ok is not a boolean"_s},
  };
  for (const auto &[json, why] : malformed) {
    QVERIFY(bayan::test::deliverMalformed(*engine, json, why));
  }
  QTRY_COMPARE(errors.count(), malformed.size());
  QCOMPARE(events.count(), 0);
  QCOMPARE(replies.count(), 0);
}

void TestEngine::repliesMustAnswerPostedRequestsInOrder() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy errors(engine.get(), &bayan::Engine::protocolError);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  // A reply to a request that was never posted.
  QVERIFY(bayan::test::deliver(*engine, unsupportedReply(1)));
  QTRY_COMPARE(errors.count(), 1);
  QCOMPARE(replies.count(), 0);
  // The engine's own reply is accepted; a second reply to the same request, or one to a request answered already, is not.
  const qint64 first = engine->request(u"no.such.message"_s);
  QVERIFY(!waitForReply(replies, first).isEmpty());
  const qint64 second = engine->request(u"no.such.message"_s);
  QVERIFY(!waitForReply(replies, second).isEmpty());
  const int answered = static_cast<int>(replies.count());
  for (const qint64 id : {second, first}) {
    QVERIFY(bayan::test::deliver(*engine, unsupportedReply(id)));
  }
  QTRY_COMPARE(errors.count(), 3);
  QCOMPARE(replies.count(), answered);
}

void TestEngine::oversizedEngineMessagesAreRejected() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy errors(engine.get(), &bayan::Engine::protocolError);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  // The length is checked before the bytes are read, so the claimed size is all that matters.
  QVERIFY(bayan::test::deliverOversized(*engine, progressEvent(), static_cast<std::size_t>(bayan::Engine::kMaxMessageBytes) + 1));
  bayan::test::deliverNothing(*engine);
  QTRY_COMPARE(errors.count(), 2);
  QCOMPARE(events.count(), 0);
}

void TestEngine::destroyingWhileBusyIsSafe() {
  // Queue many requests and destroy the engine at once: the destructor waits for the engine thread, and queued messages are dropped
  // without reaching the destroyed object. Sanitizer builds turn any mistake here into a failure.
  for (int round = 0; round < 20; ++round) {
    auto engine = bayan::Engine::create();
    QVERIFY(engine);
    engine->request(u"hello"_s, helloPayload());
    for (int i = 0; i < 100; ++i) {
      engine->request(u"no.such.message"_s);
    }
    engine.reset();
    QCoreApplication::processEvents();
  }
}

namespace {

bool isUnrecoverableError(const QList<QVariant> &event) {
  return event.at(0).toString() == u"engine.error"_s && event.at(1).toJsonObject().value(u"recoverable"_s) == QJsonValue(false);
}

} // namespace

void TestEngine::diagPanicNeedsTheTestSwitch() {
  // Without test.allow_panic, which the application never sets, diag.panic is refused and the session goes on (engine protocol §6.7).
  const auto engine = openedEngine();
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  const QList<QVariant> refused = waitForReply(replies, engine->request(u"diag.panic"_s));
  QVERIFY(!refused.isEmpty());
  QVERIFY(!refused.at(1).toBool());
  QCOMPARE(refused.at(3).toJsonObject().value(u"code"_s).toString(), u"not_allowed"_s);
  const QList<QVariant> next = waitForReply(replies, engine->request(u"no.such.message"_s));
  QVERIFY(!next.isEmpty());
  QCOMPARE(next.at(3).toJsonObject().value(u"code"_s).toString(), u"unsupported_message"_s);
  QVERIFY(std::ranges::none_of(events, isUnrecoverableError));
}

void TestEngine::aPanicDiscardsTheSession() {
  // What the shell's recovery relies on, from the stub as from the real engine (engine protocol §12): the message that panicked is answered
  // with the error panic, then engine.error says that the session is gone, and the next message other than hello needs a new handshake.
  const auto engine = openedEngine(QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}}}});
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  const QList<QVariant> reply = waitForReply(replies, engine->request(u"diag.panic"_s));
  QVERIFY(!reply.isEmpty());
  QVERIFY(!reply.at(1).toBool());
  QCOMPARE(reply.at(3).toJsonObject().value(u"code"_s).toString(), u"panic"_s);
  QTRY_VERIFY(std::ranges::any_of(events, isUnrecoverableError));
  const QList<QVariant> after = waitForReply(replies, engine->request(u"doc.open"_s, QJsonObject{{u"blob"_s, 1}}));
  QVERIFY(!after.isEmpty());
  QCOMPARE(after.at(3).toJsonObject().value(u"code"_s).toString(), u"handshake_required"_s);
  // Without an identifier, only the event says so.
  QVERIFY(waitForReply(replies, engine->request(u"hello"_s, helloPayload())).value(1).toBool());
  events.clear();
  QVERIFY(engine->post(u"diag.panic"_s));
  QTRY_VERIFY(std::ranges::any_of(events, isUnrecoverableError));
}

void TestEngine::requestsOfOneTypeCanBeSentAsPanics() {
  // The recovery tests' hook: a request of the chosen type goes out as diag.panic with its own identifier, so the engine answers that very
  // request with the error panic.
  const auto engine = bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}}}});
  QVERIFY(engine);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  QVERIFY(waitForReply(replies, engine->request(u"hello"_s, helloPayload())).value(1).toBool());
  engine->setPanicOnRequestTypeForTesting(u"doc.open"_s);
  const bayan::BlobId blob = engine->putBlob(QByteArray());
  QVERIFY(blob != 0);
  const QJsonObject open{{u"blob"_s, static_cast<qint64>(blob)}};
  const QList<QVariant> panicked = waitForReply(replies, engine->request(u"doc.open"_s, open));
  QVERIFY(!panicked.isEmpty());
  QCOMPARE(panicked.at(3).toJsonObject().value(u"code"_s).toString(), u"panic"_s);
  // Turned off, doc.open opens the document again, in a new session.
  engine->setPanicOnRequestTypeForTesting({});
  QVERIFY(waitForReply(replies, engine->request(u"hello"_s, helloPayload())).value(1).toBool());
  QVERIFY(waitForReply(replies, engine->request(u"doc.open"_s, open)).value(1).toBool());
  QVERIFY(engine->releaseBlob(blob));
}

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
