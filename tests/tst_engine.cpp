// Unit tests for the engine wrapper (src/engine/engine.h), run against the stub engine.

#include "engine.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

// Geometry of the stub's test page (US Letter) in BLU, and the factor from BLU to device-independent pixels at 100 %.
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

// Hands JSON to the wrapper the way the engine's callback does. <claimedLength> lets a test lie about the size.
void deliver(bayan::Engine &engine, const QByteArray &json, std::size_t claimedLength) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the C interface passes UTF-8 as bytes.
  engine.deliverFromEngineThread(reinterpret_cast<const std::uint8_t *>(json.constData()), claimedLength);
}

void deliver(bayan::Engine &engine, const QByteArray &json) { deliver(engine, json, static_cast<std::size_t>(json.size())); }

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
  void oversizedEngineMessagesAreRejected();
  void destroyingWhileBusyIsSafe();

private:
  // Starts an engine, completes the handshake and opens the stub document.
  static std::unique_ptr<bayan::Engine> openedEngine();
};

std::unique_ptr<bayan::Engine> TestEngine::openedEngine() {
  auto engine = bayan::Engine::create();
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

void TestEngine::versionIsReported() { QCOMPARE(bayan::Engine::version(), u"0.0.0-stub"_s); }

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
  QCOMPARE(payload.value(u"engine_version"_s).toString(), u"0.0.0-stub"_s);
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
  QVERIFY(engine->post(u"view.set"_s, QJsonObject{{u"doc_id"_s, 1}, {u"zoom"_s, 1.0}, {u"mode"_s, u"print"_s}}));
  QTRY_VERIFY(events.count() >= 3);
  QCOMPARE(events.at(0).at(0).toString(), u"view.pages"_s);
  const QJsonArray pages = events.at(0).at(1).toJsonObject().value(u"pages"_s).toArray();
  QCOMPARE(pages.size(), 1);
  QCOMPARE(pages.at(0).toObject().value(u"width"_s).toInteger(), kPageWidth);
  QCOMPARE(pages.at(0).toObject().value(u"height"_s).toInteger(), kPageHeight);
  QCOMPARE(events.at(1).at(0).toString(), u"render.invalidate"_s);
  QCOMPARE(events.at(2).at(0).toString(), u"view.layout.progress"_s);
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
  const QByteArray json = R"({"v":0,"type":"diag.log","seq":1,"payload":{"level":"info"}})"_ba;
  std::thread engineThread([&engine, &json] { deliver(*engine, json); });
  engineThread.join();
  QCOMPARE(events.count(), 0); // Nothing is emitted on the engine thread itself.
  QTRY_COMPARE(events.count(), 1);
  QCOMPARE(events.at(0).at(0).toString(), u"diag.log"_s);
  QCOMPARE(eventThread, QThread::currentThread());
}

void TestEngine::malformedEngineMessagesAreProtocolErrors() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy errors(engine.get(), &bayan::Engine::protocolError);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  QSignalSpy replies(engine.get(), &bayan::Engine::replyReceived);
  const QList<QByteArray> malformed{
      "not json"_ba,
      "[1, 2, 3]"_ba,
      R"({"v":5,"type":"view.pages","payload":{}})"_ba, // another protocol version
      R"({"v":0,"payload":{}})"_ba,                     // neither a type nor a reply
      R"({"v":0,"re":0,"ok":true})"_ba,                 // invalid request identifier
      R"({"v":0,"re":3,"ok":"yes"})"_ba,                // ok is not a boolean
  };
  for (const QByteArray &json : malformed) {
    deliver(*engine, json);
  }
  QTRY_COMPARE(errors.count(), malformed.size());
  QCOMPARE(events.count(), 0);
  QCOMPARE(replies.count(), 0);
}

void TestEngine::oversizedEngineMessagesAreRejected() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  QSignalSpy errors(engine.get(), &bayan::Engine::protocolError);
  QSignalSpy events(engine.get(), &bayan::Engine::eventReceived);
  const QByteArray json = R"({"v":0,"type":"diag.log","payload":{}})"_ba;
  // The length is checked before the bytes are read, so the claimed size is all that matters.
  deliver(*engine, json, static_cast<std::size_t>(bayan::Engine::kMaxMessageBytes) + 1);
  engine->deliverFromEngineThread(nullptr, 0);
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

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
