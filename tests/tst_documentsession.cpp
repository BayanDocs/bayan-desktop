// Unit tests for the document session (src/app/documentsession.h), against the engine the build links: the stub engine (BAYAN_ENGINE=stub)
// or the real engine of bayan-core's C SDK (BAYAN_ENGINE=sdk). The error paths of opening a document need the stub's test switches, and the
// accessibility tree and text input need the real engine's mock document; each of those tests skips with the other engine. The recovery
// after engine failures (engine protocol §12.1) runs against both: both engines panic on diag.panic when created with test.allow_panic.

#include "documentsession.h"
#include "engine.h"
#include "enginemessages.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
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

// The tests hand the session messages as if from the engine only through tests/support/enginemessages.h, which checks each against the
// protocol's JSON Schema: deliver() and emitEvent() for messages the engine could send, deliverMalformed() for input it could never send.
using bayan::test::deliver;
using bayan::test::deliverMalformed;
using bayan::test::emitEvent;
using bayan::test::engineEvent;

QByteArray compact(const QJsonObject &message) { return QJsonDocument(message).toJson(QJsonDocument::Compact); }

// A US Letter page as view.pages describes it, at the top of the view.
QJsonObject letterPage(int index) {
  return QJsonObject{{u"index"_s, index}, {u"width"_s, 15'544'800}, {u"height"_s, 20'116'800}, {u"section"_s, 0}, {u"x"_s, 0}, {u"y"_s, 0}};
}

// The configuration that lets diag.panic make the engine fail (engine protocol §3.3); the application never sets it.
QJsonObject allowPanic() { return QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}}}}; }

// The session's messages after a failure (engine protocol §12.1, point 2), as DocumentSession words them.
QString restarted(int restart, const QString &document) {
  return u"The engine stopped working and was restarted (%1 of 3). %2"_s.arg(restart).arg(document);
}
const QString closedNow = u"The document was closed and can be reopened."_s;
const QString stillClosed = u"The document is still closed and can be reopened."_s;
const QString notOpenedYet = u"The document can be opened now."_s;
const QString stoppedAgain =
    u"The engine stopped working again, so it was not restarted. Close BayanDocs and open it again to restart the engine."_s;
const QString couldNotOpen = u"The document could not be opened."_s;

// Records every message the session says, in order, also one that repeats the last.
class MessageLog final : public QObject {
public:
  explicit MessageLog(const bayan::DocumentSession &session) {
    connect(&session, &bayan::DocumentSession::messageChanged, this, [this, &session] { messages_.append(session.message()); });
  }
  [[nodiscard]] const QStringList &messages() const { return messages_; }

private:
  QStringList messages_;
};

// Records the identifier of every request the engine answers, from its creation on, so that a test can wait for a reply that may already
// have arrived, such as a new session's welcome.
class ReplyLog final : public QObject {
public:
  explicit ReplyLog(const bayan::Engine &engine) {
    connect(&engine, &bayan::Engine::replyReceived, this,
            [this](qint64 requestId, bool, const QJsonObject &, const QJsonObject &) { answered_.append(requestId); });
  }
  [[nodiscard]] bool answered(qint64 requestId) const { return answered_.contains(requestId); }

private:
  QList<qint64> answered_;
};

// Waits for the session to have restarted after a failure: closed, with the message given, and the new session's hello answered. Returns
// the identifier of that hello, the last request the session sent, or 0.
qint64 waitForRestart(const ReplyLog &replies, const bayan::Engine &engine, const bayan::DocumentSession &session, const QString &message) {
  if (!QTest::qWaitFor(
          [&session, &message] { return session.status() == bayan::DocumentSession::Status::Closed && session.message() == message; })) {
    qWarning("The session did not restart with the expected message; it says: %s", qUtf8Printable(session.message()));
    return 0;
  }
  const qint64 hello = engine.lastPostedRequest();
  return QTest::qWaitFor([&replies, hello] { return replies.answered(hello); }) ? hello : 0;
}

// Makes the engine fail: diag.panic with an identifier, whose reply is the error panic, or without, so that only engine.error says so.
void panic(bayan::Engine &engine, bool withIdentifier) {
  if (withIdentifier) {
    QVERIFY(engine.request(u"diag.panic"_s) > 0);
  } else {
    QVERIFY(engine.post(u"diag.panic"_s));
  }
}

// Checks that the session sends nothing more: neither input nor a view, nor doc.open when asked to reopen.
void expectNothingSent(bayan::Engine &engine, bayan::DocumentSession &session) {
  const qint64 before = engine.lastPostedRequest();
  session.reopen();
  QCOMPARE(session.sendText(u"late"_s), 0);
  QCOMPARE(session.sendKey(bayan::protocol::KeyInput{.key = u"a"_s}), 0);
  bayan::DocumentSession::View view = session.view();
  view.zoom = 3.0;
  session.setView(view);
  QTest::qWait(200);
  QCOMPARE(engine.lastPostedRequest(), before);
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
  std::unique_ptr<ReplyLog> replies;
  std::unique_ptr<bayan::DocumentSession> session;
};

std::unique_ptr<OpenSession> openSession(const QJsonObject &config = {}) {
  auto open = std::make_unique<OpenSession>();
  open->engine = bayan::Engine::create(config);
  if (!open->engine) {
    return nullptr;
  }
  open->replies = std::make_unique<ReplyLog>(*open->engine);
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
  void onlyUnrecoverableEngineErrorsRestartTheSession();
  void failuresRestartTheSessionThreeTimesAndReopenOnlyWhenAsked();
  void aFailureWhileTheDocumentOpensClosesIt();
  void theOfferStaysThroughFurtherFailuresAndGoesAtTheStop();
  void aFailureBeforeTheDocumentWasEverOpenOffersToOpenIt();
  void aDocumentThatMakesTheEngineFailWheneverItOpensIsReopenedThreeTimes();
  void leftoversOfTheFailedSessionAreIgnored();
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
  // The engine refused the document and kept its session: the session says so, and neither stops nor restarts the engine.
  QTRY_COMPARE(session.status(), Status::Closed);
  QCOMPARE(session.message(), couldNotOpen);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
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
  QVERIFY(deliver(
      *open->engine,
      engineEvent(u"a11y.update"_s, QJsonObject{{u"doc_id"_s, session.documentId()},
                                                {u"changes"_s, QJsonArray{QJsonObject{{u"kind"_s, u"replace"_s}, {u"node"_s, stranger}}}},
                                                {u"caret"_s, QJsonValue::Null}})));
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
  QVERIFY(deliverMalformed(
      *open->engine, compact(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, session.documentId()}, {u"pages"_s, u"three"_s}})),
      u"pages is not a list"_s));
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.sendText(u"late"_s), 0);
}

void TestDocumentSession::eventsForAnotherDocumentAreIgnored() {
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  QSignalSpy pagesChanged(&session, &bayan::DocumentSession::pagesChanged);
  const int pages = session.pageCount();
  const qint64 other = session.documentId() + 1;
  // The pages and redrawn regions of another document change nothing here.
  QVERIFY(deliver(*open->engine,
                  engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, other}, {u"pages"_s, QJsonArray{letterPage(0), letterPage(1)}}})));
  const QJsonObject region{{u"page"_s, 0}, {u"rect"_s, QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 100}, {u"height"_s, 100}}}};
  QVERIFY(
      deliver(*open->engine, engineEvent(u"render.invalidate"_s, QJsonObject{{u"doc_id"_s, other}, {u"regions"_s, QJsonArray{region}}})));
  // The session does not even read an event about another document: one it could not read does not stop it.
  QVERIFY(deliverMalformed(*open->engine,
                           compact(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, other}, {u"pages"_s, u"three"_s}})),
                           u"pages is not a list"_s));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  QCOMPARE(invalidated.count(), 0);
  QCOMPARE(pagesChanged.count(), 0);
  QCOMPARE(session.pageCount(), pages);
}

void TestDocumentSession::onlyUnrecoverableEngineErrorsRestartTheSession() {
  // A fake engine.error, from either engine: only recoverable decides, anything but true counts as false (engine protocol §12.1), and
  // nothing of the error reaches the message, here markup that a document might have made the engine repeat. The schema lets the engine
  // send any text as the message identifier and as an argument, but only its own codes.
  const auto open = openSession();
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  const QJsonObject hostile{{u"code"_s, u"internal"_s},
                            {u"message_id"_s, u"engine-error-<img src=x>"_s},
                            {u"args"_s, QJsonObject{{u"type"_s, u"<b>marker</b>"_s}}}};
  QJsonObject recoverable = hostile;
  recoverable.insert(u"recoverable"_s, true);
  QVERIFY(deliver(engine, engineEvent(u"engine.error"_s, recoverable)));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  QVERIFY(session.message().isEmpty());

  // The string "true" is not true: the engine discarded its session. The document closes, and is not reopened. The engine never sends
  // either of these two errors, but the shell must not trust that it cannot.
  QJsonObject notTrue = hostile;
  notTrue.insert(u"recoverable"_s, u"true"_s);
  QVERIFY(deliverMalformed(engine, compact(engineEvent(u"engine.error"_s, notTrue)), u"recoverable is a string"_s));
  const qint64 hello = waitForRestart(*open->replies, engine, session, restarted(1, closedNow));
  QVERIFY(hello > 0);
  QCOMPARE(closed.count(), 1);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
  QCOMPARE(session.documentId(), 0);
  QCOMPARE(session.pageCount(), 0);
  QVERIFY(!session.accessibilityTree());
  QVERIFY(!session.message().contains(u"marker"_s) && !session.message().contains(u"<"_s));
  QTest::qWait(200);
  QCOMPARE(engine.lastPostedRequest(), hello);

  // Without recoverable at all: the same, and the document is still closed.
  QVERIFY(deliverMalformed(engine, compact(engineEvent(u"engine.error"_s, hostile)), u"recoverable is missing"_s));
  QVERIFY(waitForRestart(*open->replies, engine, session, restarted(2, stillClosed)) > 0);
  QCOMPARE(closed.count(), 1);
  QCOMPARE(liveBlobs(engine), 0);
}

void TestDocumentSession::failuresRestartTheSessionThreeTimesAndReopenOnlyWhenAsked() {
  const auto open = openSession(allowPanic());
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  QSignalSpy refused(&session, &bayan::DocumentSession::inputRefused);
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  for (int restart = 1; restart <= 3; ++restart) {
    // With an identifier the engine also answers diag.panic with the error panic, which must change nothing by itself.
    panic(engine, restart != 2);
    const qint64 hello = waitForRestart(*open->replies, engine, session, restarted(restart, closedNow));
    QVERIFY2(hello > 0, qPrintable(u"restart %1"_s.arg(restart)));
    QCOMPARE(closed.count(), restart);
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
    QCOMPARE(session.documentId(), 0);
    QCOMPARE(session.pageCount(), 0);
    // A new session, without the document: nothing but the hello was sent.
    QTest::qWait(200);
    QCOMPARE(engine.lastPostedRequest(), hello);
    // Only when the user asks.
    session.reopen();
    QCOMPARE(session.message(), u"Reopening the document…"_s);
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
    QTRY_COMPARE(session.status(), Status::Ready);
    QCOMPARE(session.message(), u"The document was reopened."_s);
    QVERIFY(session.documentId() > 0);
    QVERIFY(session.pageCount() > 0);
    if (!isStub()) {
      // The real engine's mock document has an accessibility tree, which comes back with the document.
      QTRY_VERIFY(session.accessibilityTree().has_value());
    }
  }
  // The fourth failure within ten minutes: no new session; the session says so, offers nothing and sends nothing more.
  panic(engine, true);
  const qint64 before = engine.lastPostedRequest();
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.message(), stoppedAgain);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
  QCOMPARE(closed.count(), 4);
  QCOMPARE(engine.lastPostedRequest(), before);
  expectNothingSent(engine, session);
  QCOMPARE(session.status(), Status::Failed);
  QCOMPARE(liveBlobs(engine), 0);
  // The error panic was never taken for refused input.
  for (const QList<QVariant> &code : std::as_const(refused)) {
    QVERIFY(code.at(0).toString() != u"panic"_s && code.at(0).toString() != u"handshake_required"_s);
  }
}

void TestDocumentSession::aFailureWhileTheDocumentOpensClosesIt() {
  const auto open = openSession(allowPanic());
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  panic(engine, true);
  QVERIFY(waitForRestart(*open->replies, engine, session, restarted(1, closedNow)) > 0);
  // diag.panic just before the doc.open of the reopening: the engine fails while the document is on its way in, and answers that doc.open
  // with handshake_required from its next session. The document counts as closed, and is not reopened again.
  panic(engine, false);
  session.reopen();
  const qint64 hello = waitForRestart(*open->replies, engine, session, restarted(2, closedNow));
  QVERIFY(hello > 0);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
  QTest::qWait(200);
  QCOMPARE(engine.lastPostedRequest(), hello);
  QCOMPARE(session.status(), Status::Closed);
  QCOMPARE(liveBlobs(engine), 0);
}

void TestDocumentSession::theOfferStaysThroughFurtherFailuresAndGoesAtTheStop() {
  const auto open = openSession(allowPanic());
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  panic(engine, true);
  QVERIFY(waitForRestart(*open->replies, engine, session, restarted(1, closedNow)) > 0);
  // Failures while the document is closed: it is still closed, and the offer stays.
  for (int restart = 2; restart <= 3; ++restart) {
    panic(engine, restart == 2);
    const qint64 hello = waitForRestart(*open->replies, engine, session, restarted(restart, stillClosed));
    QVERIFY(hello > 0);
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
    QCOMPARE(engine.lastPostedRequest(), hello);
  }
  panic(engine, true);
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.message(), stoppedAgain);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
  QCOMPARE(closed.count(), 1);
  expectNothingSent(engine, session);
}

void TestDocumentSession::aFailureBeforeTheDocumentWasEverOpenOffersToOpenIt() {
  const auto engine = bayan::Engine::create(allowPanic());
  QVERIFY(engine);
  // Plays an engine that fails before the session has seen the welcome to its first hello: this slot, connected before the session's own,
  // hands the session an unrecoverable engine.error just before the welcome reaches it, when it has not sent doc.open yet. The welcome
  // then answers a hello of the discarded session, and must be ignored.
  bool failed = false;
  bool emitted = false;
  QObject::connect(engine.get(), &bayan::Engine::replyReceived, engine.get(),
                   [&failed, &emitted, &engine](qint64, bool, const QJsonObject &, const QJsonObject &) {
                     if (!failed) {
                       failed = true;
                       emitted = emitEvent(*engine, u"engine.error"_s,
                                           QJsonObject{{u"code"_s, u"panic"_s},
                                                       {u"message_id"_s, u"engine-error-panic"_s},
                                                       {u"recoverable"_s, false},
                                                       {u"args"_s, QJsonObject{}}});
                     }
                   });
  const ReplyLog replies(*engine);
  bayan::DocumentSession session(*engine);
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  session.start();
  const qint64 hello = waitForRestart(replies, *engine, session, restarted(1, notOpenedYet));
  QVERIFY(emitted);
  QVERIFY(hello > 0);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Open);
  QCOMPARE(closed.count(), 0);
  // No doc.open, not for the old session's welcome either.
  QTest::qWait(200);
  QCOMPARE(engine->lastPostedRequest(), hello);
  QCOMPARE(liveBlobs(*engine), 0);
  session.reopen();
  QCOMPARE(session.message(), u"Opening the document…"_s);
  QTRY_COMPARE(session.status(), Status::Ready);
  QCOMPARE(session.message(), u"The document was opened."_s);
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::aDocumentThatMakesTheEngineFailWheneverItOpensIsReopenedThreeTimes() {
  // Review finding M6's own case: every doc.open of the session goes out as diag.panic with the same identifier, so the engine fails while
  // opening the document and answers the session's own doc.open with the error panic, as for a document that makes it fail whenever it
  // opens. The document is opened once and reopened three times, and the fourth failure stops the session; it is never "could not be
  // opened".
  const auto engine = bayan::Engine::create(allowPanic());
  QVERIFY(engine);
  engine->setPanicOnRequestTypeForTesting(u"doc.open"_s);
  const ReplyLog replies(*engine);
  bayan::DocumentSession session(*engine);
  const MessageLog log(session);
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  session.start();
  for (int restart = 1; restart <= 3; ++restart) {
    QVERIFY2(waitForRestart(replies, *engine, session, restarted(restart, closedNow)) > 0, qPrintable(u"restart %1"_s.arg(restart)));
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
    // Each reopening is the user's, and counts as one more restart when it fails too.
    session.reopen();
  }
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
  const QString reopening = u"Reopening the document…"_s;
  QCOMPARE(log.messages(), (QStringList{restarted(1, closedNow), reopening, restarted(2, closedNow), reopening, restarted(3, closedNow),
                                        reopening, stoppedAgain}));
  QVERIFY(!log.messages().contains(couldNotOpen));
  QCOMPARE(closed.count(), 4);
  // Four sessions, each with its hello and its doc.open, and nothing else: requests 1 to 8.
  QCOMPARE(engine->lastPostedRequest(), 8);
  expectNothingSent(*engine, session);
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::leftoversOfTheFailedSessionAreIgnored() {
  const auto open = openSession(allowPanic());
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  QSignalSpy refused(&session, &bayan::DocumentSession::inputRefused);
  // Requests that reach the engine after the one that panics, before the session has learnt of the failure: the new session answers them
  // with handshake_required, and a refused view.set would otherwise stop the session for good.
  panic(engine, true);
  QVERIFY(session.sendText(u"lost"_s) > 0);
  bayan::DocumentSession::View view = session.view();
  view.zoom = 2.0;
  session.setView(view);
  QVERIFY(waitForRestart(*open->replies, engine, session, restarted(1, closedNow)) > 0);
  QTest::qWait(200);
  QCOMPARE(session.status(), Status::Closed);
  QCOMPARE(refused.count(), 0);
  // Late events about the old session's document change nothing either, whether the session could read them or not.
  QVERIFY(deliver(engine, engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, 1}, {u"pages"_s, QJsonArray{letterPage(0)}}})));
  QVERIFY(deliverMalformed(engine, compact(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, 1}, {u"pages"_s, u"three"_s}})),
                           u"pages is not a list"_s));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Closed);
  QCOMPARE(session.pageCount(), 0);
  session.reopen();
  QTRY_COMPARE(session.status(), Status::Ready);
}

QTEST_MAIN(TestDocumentSession)
#include "tst_documentsession.moc"
