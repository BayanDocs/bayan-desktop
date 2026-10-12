// Unit tests for the document session (src/app/documentsession.h), against the engine the build links: the stub engine (BAYAN_ENGINE=stub)
// or the real engine of bayan-core's C SDK (BAYAN_ENGINE=sdk). The error paths of opening a document need the stub's test switches, and the
// accessibility tree and text input need the real engine's mock document; each of those tests skips with the other engine.

#include "documentsession.h"
#include "engine.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <cstdint>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

using Status = bayan::DocumentSession::Status;

// Whether the linked engine is the stub, which reports a version ending in -stub (also as the stand-in SDK of scripts/test-engine-sdk.sh).
bool isStub() { return bayan::Engine::version().endsWith(u"-stub"_s); }

// Whether the run must have the real engine's mock document: CI sets BAYAN_REQUIRE_MOCK_DOCUMENT when it tests the real SDK, so that the
// tests that need it fail rather than skip if it is missing.
bool mockDocumentRequired() { return qEnvironmentVariableIsSet("BAYAN_REQUIRE_MOCK_DOCUMENT"); }

// An engine whose stub answers doc.open as <mode> says ("normal", "fail" or "ignore"; see src/engine/stub/bayan_engine_stub.cpp). The real
// engine knows no such switch and refuses to start.
std::unique_ptr<bayan::Engine> engineWithDocOpen(const QString &mode) {
  return bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"doc_open"_s, mode}}}});
}

// How many blobs the engine still holds. Both engines number the shell's blobs from 1 (the real engine 1, 3, 5 and so on), and these tests
// create at most a few, so a leaked blob shows up among the first identifiers.
int liveBlobs(bayan::Engine &engine) {
  int count = 0;
  for (bayan::BlobId id = 1; id <= 16; ++id) {
    if (engine.blob(id).has_value()) {
      ++count;
    }
  }
  return count;
}

// Hands a message to the engine wrapper as if the engine had sent it, to see what the session makes of messages the engine never sends.
void deliver(bayan::Engine &engine, const QJsonObject &message) {
  const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the C interface passes UTF-8 as bytes.
  engine.deliverFromEngineThread(reinterpret_cast<const std::uint8_t *>(json.constData()), static_cast<std::size_t>(json.size()));
}

QJsonObject engineEvent(const QString &type, const QJsonObject &payload) {
  return QJsonObject{{u"v"_s, bayan::Engine::kProtocolVersion}, {u"type"_s, type}, {u"seq"_s, 1'000'000}, {u"payload"_s, payload}};
}

// The editable node of the accessibility tree (the mock document's editable line), or nullptr.
const bayan::protocol::AccessibleNode *editableNode(const std::optional<bayan::protocol::AccessibleTree> &tree) {
  if (!tree) {
    return nullptr;
  }
  for (const bayan::protocol::AccessibleNode *node : tree->readingOrder()) {
    if (node->editable) {
      return node;
    }
  }
  return nullptr;
}

QString editableText(const bayan::DocumentSession &session) {
  const auto *node = editableNode(session.accessibilityTree());
  return node != nullptr ? node->text : u"<no editable node>"_s;
}

// A session that has opened its document and received its accessibility tree (or learnt that the engine has none).
struct OpenSession {
  std::unique_ptr<bayan::Engine> engine;
  std::unique_ptr<bayan::DocumentSession> session;
};

std::unique_ptr<OpenSession> openSession() {
  auto open = std::make_unique<OpenSession>();
  open->engine = bayan::Engine::create();
  if (!open->engine) {
    return nullptr;
  }
  open->session = std::make_unique<bayan::DocumentSession>(*open->engine);
  QSignalSpy accessibility(open->session.get(), &bayan::DocumentSession::accessibilityChanged);
  open->session->start();
  if (!QTest::qWaitFor([&open, &accessibility] { return open->session->status() == Status::Ready && accessibility.count() > 0; })) {
    return nullptr;
  }
  return open;
}

} // namespace

class TestDocumentSession final : public QObject {
  Q_OBJECT

private slots:
  void opensTheDocumentAndReleasesTheBlob();
  void releasesTheBlobWhenOpeningFails();
  void releasesTheBlobWhenDestroyedWhileOpening();
  void unknownTestSwitchesAreRefused();
  void inputBeforeTheDocumentIsOpenSendsNothing();
  void changingTheViewRedrawsThePages();
  void sendsTheAccessibilityTreeOrNone();
  void textInputChangesTheLineTheOverlayAndTheTree();
  void refusedInputIsReported();
  void unreadableAccessibilityUpdatesFetchTheTreeAgain();
  void malformedEventsFailTheSession();
  void eventsForAnotherDocumentAreIgnored();
  void onlyUnrecoverableEngineErrorsFailTheSession();
};

void TestDocumentSession::opensTheDocumentAndReleasesTheBlob() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  session.start();
  QTRY_COMPARE(session.status(), Status::Ready);
  QCOMPARE(session.engineVersion(), bayan::Engine::version());
  QVERIFY(session.documentId() > 0);
  // The stub has one test page; the real engine's mock document has three, one below the other in the view.
  QCOMPARE(session.pageCount(), isStub() ? 1 : 3);
  for (std::size_t i = 0; i < session.pages().size(); ++i) {
    const auto &page = session.pages().at(i);
    QCOMPARE(page.index, static_cast<int>(i));
    QVERIFY(page.width > 0 && page.height > 0);
    if (i > 0) {
      const auto &previous = session.pages().at(i - 1);
      QVERIFY(page.y >= previous.y + previous.height);
    }
  }
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::releasesTheBlobWhenOpeningFails() {
  if (!isStub()) {
    QSKIP("Needs the stub's doc_open test switch.");
  }
  const auto engine = engineWithDocOpen(u"fail"_s);
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  QSignalSpy statusChanges(&session, &bayan::DocumentSession::statusChanged);
  session.start();
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(liveBlobs(*engine), 0);
  QVERIFY(statusChanges.count() >= 1);
}

void TestDocumentSession::releasesTheBlobWhenDestroyedWhileOpening() {
  if (!isStub()) {
    QSKIP("Needs the stub's doc_open test switch.");
  }
  const auto engine = engineWithDocOpen(u"ignore"_s);
  QVERIFY(engine);
  auto session = std::make_unique<bayan::DocumentSession>(*engine);
  session->start();
  // The session creates the placeholder blob after the handshake and then waits for doc.open, which this engine never answers.
  QTRY_COMPARE(liveBlobs(*engine), 1);
  QCOMPARE(session->status(), Status::Starting);
  session.reset();
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::unknownTestSwitchesAreRefused() {
  // Both engines refuse a test switch they do not know, so a test can never silently run the normal path.
  QVERIFY(!engineWithDocOpen(u"sometimes"_s));
  QVERIFY(!bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"unknown_switch"_s, true}}}}));
}

void TestDocumentSession::inputBeforeTheDocumentIsOpenSendsNothing() {
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  QCOMPARE(session.sendText(u"early"_s), 0);
  QCOMPARE(session.sendKey(bayan::protocol::KeyInput{.key = u"a"_s}), 0);
  QCOMPARE(session.sendComposition(bayan::protocol::CompositionPhase::Start), 0);
  QCOMPARE(session.sendPointer(bayan::protocol::PointerInput{}), 0);
}

void TestDocumentSession::changingTheViewRedrawsThePages() {
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  const qint64 before = open->engine->lastPostedRequest();
  bayan::DocumentSession::View view = session.view();
  view.zoom = 2.0;
  session.setView(view);
  // In protocol v0 a new view redraws every page; the cause is the view.set request, which the session sent after `before`.
  QTRY_VERIFY(!invalidated.isEmpty());
  const auto regions = invalidated.at(0).at(0).value<std::vector<bayan::protocol::PageRect>>();
  QCOMPARE(static_cast<int>(regions.size()), session.pageCount());
  QVERIFY(invalidated.at(0).at(1).toLongLong() > before);
  // The same view again sends nothing.
  invalidated.clear();
  session.setView(view);
  QTest::qWait(200);
  QCOMPARE(invalidated.count(), 0);
}

void TestDocumentSession::sendsTheAccessibilityTreeOrNone() {
  const auto open = openSession();
  QVERIFY(open);
  const bayan::DocumentSession &session = *open->session;
  if (isStub()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    // The stub has no accessibility tree; the session says so instead of failing.
    QVERIFY(!session.accessibilityTree());
    QCOMPARE(session.status(), Status::Ready);
    return;
  }
  const auto &tree = session.accessibilityTree();
  QVERIFY(tree && tree->root().role == bayan::protocol::Role::Document);
  const auto *line = editableNode(tree);
  QVERIFY(line != nullptr && line->text.isEmpty() && line->bounds);
  const auto caret = session.accessibilityCaret();
  QVERIFY(caret && caret->node == line->id && caret->offset == 0);
}

void TestDocumentSession::textInputChangesTheLineTheOverlayAndTheTree() {
  const auto open = openSession();
  QVERIFY(open);
  if (isStub()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  QSignalSpy overlay(&session, &bayan::DocumentSession::overlayChanged);
  QSignalSpy accessibility(&session, &bayan::DocumentSession::accessibilityChanged);
  const qint64 request = session.sendText(u"abc"_s);
  QVERIFY(request > 0);
  QTRY_COMPARE(editableText(session), u"abc"_s);
  // The engine redrew the editable line, moved the caret and replaced the line's accessible node, all caused by this request.
  QVERIFY(!invalidated.isEmpty());
  QCOMPARE(invalidated.at(0).at(1).toLongLong(), request);
  QVERIFY(!overlay.isEmpty() && session.overlay().caret);
  const auto changed = accessibility.last().at(0).value<QList<quint64>>();
  const auto *line = editableNode(session.accessibilityTree());
  QVERIFY(line != nullptr);
  QCOMPARE(changed, QList<quint64>{line->id});
  QCOMPARE(session.accessibilityCaret().value_or(bayan::protocol::AccessibleCaret{}).offset, 3);
}

void TestDocumentSession::refusedInputIsReported() {
  const auto open = openSession();
  QVERIFY(open);
  if (isStub()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentSession &session = *open->session;
  QSignalSpy refused(&session, &bayan::DocumentSession::inputRefused);
  // The mock document's editable line holds at most 1,024 UTF-16 code units.
  QVERIFY(session.sendText(QString(1025, u'x')) > 0);
  QTRY_COMPARE(refused.count(), 1);
  QCOMPARE(refused.at(0).at(0).toString(), u"limit_exceeded"_s);
  QCOMPARE(session.status(), Status::Ready);
  QCOMPARE(editableText(session), QString());
}

void TestDocumentSession::unreadableAccessibilityUpdatesFetchTheTreeAgain() {
  const auto open = openSession();
  QVERIFY(open);
  if (isStub()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the accessibility tree of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentSession &session = *open->session;
  QVERIFY(session.accessibilityTree());
  // A change the session cannot apply (a node that is not in the tree): it drops the tree and asks for the whole of it again.
  const QJsonObject stranger{
      {u"id"_s, 999'999}, {u"role"_s, u"paragraph"_s}, {u"name"_s, QString()}, {u"text"_s, u"stranger"_s}, {u"children"_s, QJsonArray{}}};
  deliver(*open->engine, engineEvent(u"a11y.update"_s,
                                     QJsonObject{{u"doc_id"_s, session.documentId()},
                                                 {u"changes"_s, QJsonArray{QJsonObject{{u"kind"_s, u"replace"_s}, {u"node"_s, stranger}}}},
                                                 {u"caret"_s, QJsonValue::Null}}));
  QTRY_VERIFY(!session.accessibilityTree());
  QTRY_VERIFY(session.accessibilityTree());
  QVERIFY(editableNode(session.accessibilityTree()) != nullptr);
  QCOMPARE(session.status(), Status::Ready);
}

void TestDocumentSession::malformedEventsFailTheSession() {
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  // A page list that is not a list: the session cannot trust the engine any more.
  deliver(*open->engine, engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, session.documentId()}, {u"pages"_s, u"three"_s}}));
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.sendText(u"late"_s), 0);
}

void TestDocumentSession::eventsForAnotherDocumentAreIgnored() {
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  deliver(*open->engine, engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, session.documentId() + 1}, {u"pages"_s, u"three"_s}}));
  deliver(*open->engine,
          engineEvent(u"render.invalidate"_s, QJsonObject{{u"doc_id"_s, session.documentId() + 1}, {u"regions"_s, QJsonArray{}}}));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  QCOMPARE(invalidated.count(), 0);
}

void TestDocumentSession::onlyUnrecoverableEngineErrorsFailTheSession() {
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  const QJsonObject error{{u"code"_s, u"internal"_s}, {u"message_id"_s, u"engine-error-internal"_s}};
  deliver(*open->engine, engineEvent(u"engine.error"_s, QJsonObject{{u"error"_s, error}, {u"recoverable"_s, true}}));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  deliver(*open->engine, engineEvent(u"engine.error"_s, QJsonObject{{u"error"_s, error}, {u"recoverable"_s, false}}));
  QTRY_COMPARE(session.status(), Status::Failed);
}

QTEST_MAIN(TestDocumentSession)
#include "tst_documentsession.moc"
