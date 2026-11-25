// Unit tests for the document session (src/app/documentsession.h), against the engine the build links: the stub engine (BAYAN_ENGINE=stub)
// or the real engine of bayan-core's C SDK (BAYAN_ENGINE=sdk). The error paths of opening a document and of the handshake need the stub's
// test switches, and the accessibility tree and text input need the real engine's mock document; each of those tests skips with the other
// engine. The recovery after engine failures (engine protocol §12.1, §14.2) runs against both: both engines panic on diag.panic when
// created with test.allow_panic; the real engine's binding counts the restarts, and the stub carries the restart values of its test switch,
// which gives it those that the real engine gives, so that the rule is written only in bayan-core.

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

QJsonObject restartValue(int number, int limit = 3) { return QJsonObject{{u"number"_s, number}, {u"limit"_s, limit}}; }

// The configuration that lets diag.panic make the engine fail (engine protocol §3.3), which the application never sets; for the stub, with
// the restart values that the real engine's binding gives four failures within ten minutes (§14.2).
QJsonObject allowPanic() {
  QJsonObject test{{u"allow_panic"_s, true}};
  if (isStub()) {
    test.insert(u"restarts"_s, QJsonArray{restartValue(1), restartValue(2), restartValue(3), QJsonValue::Null});
  }
  return QJsonObject{{u"test"_s, test}};
}

// A stub that leaves hello, or query.a11y, unanswered, so that a test can answer them itself. The real engine knows no such switches.
std::unique_ptr<bayan::Engine> stubWith(const QString &name, const QString &mode) {
  return bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{name, mode}}}});
}

// Records every envelope the engine accepted from the session (Engine::setSentObserverForTesting).
class SentLog {
public:
  explicit SentLog(bayan::Engine &engine) {
    engine.setSentObserverForTesting([this](const QJsonObject &envelope) { sent_.append(envelope); });
  }
  [[nodiscard]] QList<QJsonObject> ofType(const QString &type) const {
    QList<QJsonObject> found;
    for (const QJsonObject &envelope : sent_) {
      if (envelope.value(u"type"_s) == type) {
        found.append(envelope);
      }
    }
    return found;
  }
  [[nodiscard]] qint64 lastIdOf(const QString &type) const {
    const QList<QJsonObject> found = ofType(type);
    return found.isEmpty() ? 0 : found.last().value(u"id"_s).toInteger();
  }

private:
  QList<QJsonObject> sent_;
};

// A successful reply to a request.
QJsonObject replyTo(qint64 requestId, const QJsonObject &payload, const QString &type = {}) {
  QJsonObject reply{{u"v"_s, 1}, {u"re"_s, requestId}, {u"ok"_s, true}, {u"payload"_s, payload}};
  if (!type.isEmpty()) {
    reply.insert(u"type"_s, type);
  }
  return reply;
}

// A refusal, as the engine words it (engine protocol §12).
QJsonObject refusal(qint64 requestId, const QString &code, const QJsonObject &args = {}) {
  QString messageId = u"engine-error-"_s + code;
  messageId.replace(u'_', u'-');
  return QJsonObject{
      {u"v"_s, 1},
      {u"re"_s, requestId},
      {u"ok"_s, false},
      {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message_id"_s, messageId}, {u"args"_s, args}}},
  };
}

QJsonObject welcome(int protocolVersion = 1, const QString &engineVersion = u"1.0.0"_s) {
  return QJsonObject{
      {u"protocol_version"_s, protocolVersion},
      {u"engine_version"_s, engineVersion},
      {u"layout_epoch"_s, 0},
      {u"features"_s, QJsonArray{u"mock_document"_s}},
  };
}

// The engine.error of a failure, with what follows: a restart, or null once the engine has stopped (engine protocol §14.2).
QJsonObject failure(const QJsonValue &restartValue) {
  return QJsonObject{
      {u"code"_s, u"panic"_s},      {u"message_id"_s, u"engine-error-panic"_s}, {u"recoverable"_s, false}, {u"args"_s, QJsonObject{}},
      {u"restart"_s, restartValue},
  };
}

// The session's messages after a failure (engine protocol §12.1, point 2), as DocumentSession words them.
QString restarted(int number, const QString &document, int limit = 3) {
  return u"The engine stopped working and was restarted (%1 of %2). %3"_s.arg(number).arg(limit).arg(document);
}
const QString closedNow = u"The document was closed and can be reopened."_s;
const QString stillClosed = u"The document is still closed and can be reopened."_s;
const QString notOpenedYet = u"The document can be opened now."_s;
// The stop message: after the last restart, after a protocol violation and after a refused hello (§12.1, §14.1 items 3 and 5).
const QString stopped = u"The engine stopped. Close BayanDocs and open it again to restart the engine."_s;
// The update prompt after unsupported_protocol_version (§14.1 item 3).
const QString updatePrompt =
    u"This installation of BayanDocs is incomplete: the app and its document engine do not fit together. Update BayanDocs, or install it again."_s;
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

// Waits for the session to have restarted after a failure: closed, with the message given, and the new session started: its hello answered,
// and the request for the UI manifest that follows the welcome answered too. Returns the identifier of that last request, the last that the
// session sent, or 0.
qint64 waitForRestart(const ReplyLog &replies, const bayan::Engine &engine, const bayan::DocumentSession &session, const QString &message) {
  if (!QTest::qWaitFor(
          [&session, &message] { return session.status() == bayan::DocumentSession::Status::Closed && session.message() == message; })) {
    qWarning("The session did not restart with the expected message; it says: %s", qUtf8Printable(session.message()));
    return 0;
  }
  qint64 last = 0;
  const bool started = QTest::qWaitFor([&replies, &engine, &last] {
    last = engine.lastPostedRequest();
    return replies.answered(last) && replies.answered(last - 1);
  });
  return started ? last : 0;
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
  void changingTheViewInvalidatesNothing();
  void invalidationsCarryTheRevision();
  void helloNamesTheOperatingSystemAndTheRuntime();
  void theManifestDecidesWhichKeysGoToTheEngine();
  void aRefusedHelloEndsTheEnginesUse();
  void aRefusedHelloAfterAPanicIsAFailure();
  void aWelcomeForAnotherVersionBreaksTheProtocol();
  void violationsEndTheEnginesUse();
  void aLongEngineVersionBreaksTheProtocol();
  void theRestartNumberComesFromTheEngine();
  void aNodeWithManyChildrenIsLoadedInParts();
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

void TestDocumentSession::changingTheViewInvalidatesNothing() {
  // In print mode a new viewport, zoom or device scale changes nothing that tiles show (engine protocol §14.6 item 2): the canvas asks for
  // the tiles of a new scale by itself.
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  QSignalSpy pages(&session, &bayan::DocumentSession::pagesChanged);
  bayan::DocumentSession::View view = session.view();
  view.zoom = 2.0;
  view.viewport.translate(0, 300);
  session.setView(view);
  const qint64 request = open->engine->lastPostedRequest();
  QTRY_VERIFY(open->replies->answered(request));
  QTest::qWait(100);
  QCOMPARE(invalidated.count(), 0);
  QCOMPARE(session.status(), Status::Ready);
  // The same view again sends nothing.
  session.setView(view);
  QTest::qWait(100);
  QCOMPARE(open->engine->lastPostedRequest(), request);
}

void TestDocumentSession::invalidationsCarryTheRevision() {
  // The regions of render.invalidate come with the document's render revision after the change (§14.6), which the canvas compares with each
  // tile's.
  const auto open = openSession();
  QVERIFY(open);
  bayan::DocumentSession &session = *open->session;
  QSignalSpy invalidated(&session, &bayan::DocumentSession::invalidated);
  const QJsonObject region{{u"page"_s, 0}, {u"rect"_s, QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 100}, {u"height"_s, 100}}}};
  QVERIFY(deliver(*open->engine,
                  engineEvent(u"render.invalidate"_s,
                              QJsonObject{{u"doc_id"_s, session.documentId()}, {u"revision"_s, 41}, {u"regions"_s, QJsonArray{region}}})));
  QTRY_COMPARE(invalidated.count(), 1);
  QCOMPARE(invalidated.at(0).at(1).toULongLong(), quint64{41});
  QCOMPARE(invalidated.at(0).at(0).value<std::vector<bayan::protocol::PageRect>>().size(), std::size_t{1});
}

void TestDocumentSession::helloNamesTheOperatingSystemAndTheRuntime() {
  // hello offers version 1 and names the operating system from the protocol's fixed list, from Qt's knowledge of the system the app was
  // built for, and the runtime (engine protocol §14.1 item 2).
  using System = bayan::DocumentSession::OperatingSystem;
  QCOMPARE(bayan::DocumentSession::platformName(System::Windows), u"windows"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::MacOS), u"macos"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::Linux), u"linux"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::IOS), u"ios"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::Android), u"android"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::ChromeOS), u"chromeos"_s);
  QCOMPARE(bayan::DocumentSession::platformName(System::Other), u"other"_s);
#ifdef Q_OS_WIN
  const QString platform = u"windows"_s;
#elif defined(Q_OS_MACOS)
  const QString platform = u"macos"_s;
#elif defined(Q_OS_LINUX)
  const QString platform = u"linux"_s;
#else
  const QString platform = u"other"_s;
#endif
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  const SentLog sent(*engine);
  bayan::DocumentSession session(*engine);
  session.start();
  QTRY_COMPARE(session.status(), Status::Ready);
  const QList<QJsonObject> hellos = sent.ofType(u"hello"_s);
  QCOMPARE(hellos.size(), 1);
  QCOMPARE(hellos.first().value(u"v"_s).toInt(), 1);
  const QJsonObject payload = hellos.first().value(u"payload"_s).toObject();
  QCOMPARE(payload.value(u"protocol_versions"_s).toArray(), QJsonArray{1});
  QCOMPARE(payload.value(u"shell"_s).toObject().value(u"platform"_s).toString(), platform);
  QCOMPARE(payload.value(u"shell"_s).toObject().value(u"runtime"_s).toString(), u"native"_s);
  // After the welcome, the session asks for the manifest's key data.
  QCOMPARE(sent.ofType(u"ui.manifest"_s).size(), 1);
}

void TestDocumentSession::theManifestDecidesWhichKeysGoToTheEngine() {
  // The key data that both engines resolve for this platform (engine protocol §14.3): Control+B (Command+B on macOS) is bold, Backspace a
  // document key, Tab and Control+C (Command+C) the shell's.
  const auto open = openSession();
  QVERIFY(open);
  const bayan::KeyBindings &keys = open->session->keyBindings();
  QTRY_VERIFY(!keys.manifest().commands.empty());
#ifdef Q_OS_MACOS
  const bayan::protocol::Modifiers mod{.shift = false, .ctrl = false, .alt = false, .meta = true};
  const QString bold = u"⌘B"_s;
#else
  const bayan::protocol::Modifiers mod{.shift = false, .ctrl = true, .alt = false, .meta = false};
  const QString bold = u"Ctrl+B"_s;
#endif
  const auto pressOf = [](const QString &key, const QString &code, bayan::protocol::Modifiers modifiers) {
    return bayan::protocol::KeyInput{.key = key, .code = code, .modifiers = modifiers, .repeat = false, .composing = false};
  };
  QVERIFY(keys.sends(pressOf(u"b"_s, u"KeyB"_s, mod)));
  QVERIFY(keys.sends(pressOf(u"Backspace"_s, u"Backspace"_s, {})));
  QVERIFY(!keys.sends(pressOf(u"Tab"_s, u"Tab"_s, {})));
  QVERIFY(!keys.sends(pressOf(u"c"_s, u"KeyC"_s, mod)));
  QVERIFY(!keys.sends(pressOf(u"b"_s, u"KeyB"_s, {})));
  // The interface shows a shortcut in the engine's words.
  QCOMPARE(open->session->shortcutLabels(u"format.bold.toggle"_s), QStringList{bold});
}

void TestDocumentSession::aRefusedHelloEndsTheEnginesUse() {
  if (!isStub()) {
    QSKIP("Needs the stub's hello test switch, so that the test answers hello itself.");
  }
  // A refused hello ends the engine's use without another hello (engine protocol §14.1 item 3): after unsupported_protocol_version, which
  // the user can fix by updating, with the update prompt; after any other error, with the stop message. The prompt is the session's
  // message, which the interface shows and announces to screen readers (tst_interface).
  struct Refusal {
    QString code;
    QString message;
    bool update;
    int version;
  };
  for (const Refusal &item : {
           Refusal{.code = u"unsupported_protocol_version"_s, .message = updatePrompt, .update = true, .version = 1},
           Refusal{.code = u"unsupported_protocol_version"_s, .message = updatePrompt, .update = true, .version = 0},
           Refusal{.code = u"invalid_request"_s, .message = stopped, .update = false, .version = 1},
       }) {
    const auto engine = stubWith(u"hello"_s, u"ignore"_s);
    QVERIFY(engine);
    const SentLog sent(*engine);
    bayan::DocumentSession session(*engine);
    session.start();
    const qint64 hello = sent.lastIdOf(u"hello"_s);
    QVERIFY(hello > 0);
    QJsonObject reply = refusal(hello, item.code,
                                item.code == u"unsupported_protocol_version"_s ? QJsonObject{{u"supported"_s, QJsonArray{item.version}}}
                                                                               : QJsonObject{{u"type"_s, u"hello"_s}});
    if (item.version == 1) {
      QVERIFY(deliver(*engine, reply));
    } else {
      // An engine of version 0 refuses in its own version: the one message whose v may differ (§14.1 item 1).
      reply.insert(u"v"_s, item.version);
      QVERIFY(deliverMalformed(*engine, compact(reply), u"/v"_s, u"the refusal of an engine of version 0"_s));
    }
    QTRY_COMPARE(session.status(), Status::Failed);
    QCOMPARE(session.message(), item.message);
    QCOMPARE(session.updateRequired(), item.update);
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
    expectNothingSent(*engine, session);
    QCOMPARE(sent.ofType(u"hello"_s).size(), 1);
  }
}

void TestDocumentSession::aRefusedHelloAfterAPanicIsAFailure() {
  if (!isStub()) {
    QSKIP("Needs the stub's hello test switch, so that the test answers hello itself.");
  }
  // A panic in reply to hello is not a refusal: the engine.error that follows says what follows (§14.1 item 3, §14.2).
  const auto engine = stubWith(u"hello"_s, u"ignore"_s);
  QVERIFY(engine);
  const SentLog sent(*engine);
  bayan::DocumentSession session(*engine);
  session.start();
  QVERIFY(deliver(*engine, refusal(sent.lastIdOf(u"hello"_s), u"panic"_s)));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Starting);
  QVERIFY(deliver(*engine, engineEvent(u"engine.error"_s, failure(restartValue(1)))));
  QTRY_COMPARE(session.message(), restarted(1, notOpenedYet));
  QCOMPARE(sent.ofType(u"hello"_s).size(), 2);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Open);
}

void TestDocumentSession::aWelcomeForAnotherVersionBreaksTheProtocol() {
  if (!isStub()) {
    QSKIP("Needs the stub's hello test switch, so that the test answers hello itself.");
  }
  // The session's version is the one the shell offered: a welcome that names another breaks the protocol (engine protocol §14.1 item 3),
  // and one that names it starts the session.
  for (const int version : {2, 1}) {
    const auto engine = stubWith(u"hello"_s, u"ignore"_s);
    QVERIFY(engine);
    const SentLog sent(*engine);
    bayan::DocumentSession session(*engine);
    session.start();
    QVERIFY(deliver(*engine, replyTo(sent.lastIdOf(u"hello"_s), welcome(version), u"welcome"_s), u"hello"_s));
    if (version == 1) {
      QTRY_COMPARE(session.engineVersion(), u"1.0.0"_s);
      QTRY_VERIFY(sent.lastIdOf(u"doc.open"_s) > 0);
      QCOMPARE(session.status(), Status::Starting);
    } else {
      QTRY_COMPARE(session.status(), Status::Failed);
      QCOMPARE(session.message(), stopped);
      QCOMPARE(sent.ofType(u"ui.manifest"_s).size(), 0);
    }
  }
}

void TestDocumentSession::violationsEndTheEnginesUse() {
  // A message that breaks the protocol ends the engine's use with the stop message, without a new session (engine protocol §14.1 item 5),
  // whether the schema could tell (deliverMalformed) or only the protocol's ranges and limits do (deliver).
  struct Violation {
    QString why;
    QJsonObject message;
    QString brokenAt; // empty: the schema allows the message
  };
  const auto documentEvent = [](const QString &type, QJsonObject payload) {
    payload.insert(u"doc_id"_s, 1);
    return engineEvent(type, payload);
  };
  const QJsonObject region{{u"page"_s, 0}, {u"rect"_s, QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 10}, {u"height"_s, 10}}}};
  QJsonObject otherVersion = documentEvent(u"render.invalidate"_s, QJsonObject{{u"revision"_s, 5}, {u"regions"_s, QJsonArray{region}}});
  otherVersion.insert(u"v"_s, 2);
  QJsonObject noSeq = documentEvent(u"render.invalidate"_s, QJsonObject{{u"revision"_s, 5}, {u"regions"_s, QJsonArray{region}}});
  noSeq.remove(u"seq"_s);
  // A chain 65 levels deep: the schema allows it, the limits of §14.7 do not.
  QJsonObject deep{{u"id"_s, 65}, {u"role"_s, u"paragraph"_s}, {u"text"_s, u"65"_s}};
  for (int level = 64; level >= 2; --level) {
    deep =
        QJsonObject{{u"id"_s, level}, {u"role"_s, u"paragraph"_s}, {u"text"_s, QString::number(level)}, {u"children"_s, QJsonArray{deep}}};
  }
  const QJsonObject deepUpdate =
      documentEvent(u"a11y.update"_s,
                    QJsonObject{
                        {
                            u"changes"_s,
                            QJsonArray{
                                QJsonObject{
                                    {u"kind"_s, u"replace"_s},
                                    {u"node"_s, QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"children"_s, QJsonArray{deep}}}},
                                },
                            },
                        },
                        {u"caret"_s, QJsonValue::Null},
                    });
  QJsonObject failureWithoutRestart = failure(restartValue(1));
  failureWithoutRestart.remove(u"restart"_s);
  QJsonObject recoverableString = failure(restartValue(1));
  recoverableString.insert(u"recoverable"_s, u"false"_s);
  const QList<Violation> violations{
      {.why = u"another protocol version"_s, .message = otherVersion, .brokenAt = u"/v"_s},
      {.why = u"an event without a sequence number"_s, .message = noSeq, .brokenAt = u"/seq"_s},
      {
          .why = u"render.invalidate without its revision"_s,
          .message = documentEvent(u"render.invalidate"_s, QJsonObject{{u"regions"_s, QJsonArray{region}}}),
          .brokenAt = u"/payload/revision"_s,
      },
      {
          .why = u"overlay.update with a revision that is a string"_s,
          .message = documentEvent(u"overlay.update"_s,
                                   QJsonObject{
                                       {u"revision"_s, u"5"_s},
                                       {u"caret"_s, QJsonValue::Null},
                                       {u"selection"_s, QJsonArray{}},
                                       {u"remote"_s, QJsonArray{}},
                                       {u"highlights"_s, QJsonArray{}},
                                   }),
          .brokenAt = u"/payload/revision"_s,
      },
      {
          .why = u"an unrecoverable engine.error without restart"_s,
          .message = engineEvent(u"engine.error"_s, failureWithoutRestart),
          .brokenAt = {},
      },
      {
          .why = u"recoverable that is a string"_s,
          .message = engineEvent(u"engine.error"_s, recoverableString),
          .brokenAt = u"/payload/recoverable"_s,
      },
      {.why = u"a restart beyond its limit"_s, .message = engineEvent(u"engine.error"_s, failure(restartValue(4))), .brokenAt = {}},
      {
          .why = u"a revision of 0"_s,
          .message = documentEvent(u"render.invalidate"_s, QJsonObject{{u"revision"_s, 0}, {u"regions"_s, QJsonArray{region}}}),
          .brokenAt = {},
      },
      {.why = u"an accessibility tree 65 levels deep"_s, .message = deepUpdate, .brokenAt = {}},
  };
  for (const Violation &violation : violations) {
    const auto open = openSession();
    QVERIFY(open);
    bayan::DocumentSession &session = *open->session;
    QCOMPARE(session.documentId(), 1);
    if (!session.accessibilityTree() && violation.message.value(u"type"_s) == u"a11y.update"_s) {
      // The stub has no accessibility tree, so a change to one is not read; the reply to query.a11y is tested below instead.
      continue;
    }
    const qint64 before = open->engine->lastPostedRequest();
    if (violation.brokenAt.isEmpty()) {
      QVERIFY2(deliver(*open->engine, violation.message), qPrintable(violation.why));
    } else {
      QVERIFY2(deliverMalformed(*open->engine, compact(violation.message), violation.brokenAt, violation.why), qPrintable(violation.why));
    }
    QTRY_VERIFY2(session.status() == Status::Failed, qPrintable(violation.why));
    QCOMPARE(session.message(), stopped);
    QCOMPARE(session.offer(), bayan::DocumentSession::Offer::None);
    QCOMPARE(open->engine->lastPostedRequest(), before);
  }
}

void TestDocumentSession::aLongEngineVersionBreaksTheProtocol() {
  if (!isStub()) {
    QSKIP("Needs the stub's engine_version test switch.");
  }
  // engine_version is at most 64 ASCII characters (engine protocol §14.1 item 6); the status bar shows it.
  for (const auto &[version, accepted] : {
           std::pair{QString(64, u'9'), true},
           std::pair{QString(65, u'9'), false},
           std::pair{u"1.0.0-ß"_s, false},
           std::pair{u"<b>9.9.9</b>"_s, true},
       }) {
    const auto engine = bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"engine_version"_s, version}}}});
    QVERIFY(engine);
    bayan::DocumentSession session(*engine);
    session.start();
    QTRY_VERIFY(session.status() == Status::Ready || session.status() == Status::Failed);
    QCOMPARE(session.status() == Status::Ready, accepted);
    QCOMPARE(session.engineVersion(), accepted ? version : QString());
  }
}

void TestDocumentSession::theRestartNumberComesFromTheEngine() {
  if (!isStub()) {
    QSKIP("Needs the stub's restarts test switch; the real engine's numbers are tested with its failures below.");
  }
  // The session counts nothing: it says what the engine's restart says (engine protocol §14.2).
  const auto open =
      openSession(QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}, {u"restarts"_s, QJsonArray{restartValue(2, 5)}}}}});
  QVERIFY(open);
  panic(*open->engine, true);
  QVERIFY(waitForRestart(*open->replies, *open->engine, *open->session, restarted(2, closedNow, 5)) > 0);
}

void TestDocumentSession::aNodeWithManyChildrenIsLoadedInParts() {
  if (!isStub()) {
    QSKIP("Needs the stub's query_a11y test switch, so that the test answers query.a11y itself.");
  }
  // A node whose children do not all fit into one message comes with child_count and a part of them; the session loads the rest with
  // query.a11y and children ranges at once, so that screen readers get every child (engine protocol §14.7 item 1).
  const auto engine = stubWith(u"query_a11y"_s, u"ignore"_s);
  QVERIFY(engine);
  const SentLog sent(*engine);
  bayan::DocumentSession session(*engine);
  QSignalSpy accessibility(&session, &bayan::DocumentSession::accessibilityChanged);
  session.start();
  QTRY_COMPARE(session.status(), Status::Ready);
  QTRY_VERIFY(sent.lastIdOf(u"query.a11y"_s) > 0);
  const auto children = [](int from, int to) {
    QJsonArray list;
    for (int index = from; index < to; ++index) {
      list.append(QJsonObject{{u"id"_s, index + 2}, {u"role"_s, u"paragraph"_s}, {u"text"_s, u"Paragraph %1"_s.arg(index)}});
    }
    return list;
  };
  // The document node holds children 2 to 4 of 7, the part with the caret.
  const QJsonObject partial{
      {u"id"_s, 1},          {u"role"_s, u"document"_s}, {u"name"_s, u"Test"_s},
      {u"child_count"_s, 7}, {u"children_start"_s, 2},   {u"children"_s, children(2, 5)},
  };
  QVERIFY(deliver(*engine,
                  replyTo(sent.lastIdOf(u"query.a11y"_s),
                          QJsonObject{{u"doc_id"_s, 1}, {u"root"_s, partial}, {u"caret"_s, QJsonObject{{u"node"_s, 4}, {u"offset"_s, 0}}}}),
                  u"query.a11y"_s));
  QTRY_COMPARE(sent.ofType(u"query.a11y"_s).size(), 3);
  // The two parts it lacks, before and after.
  const auto rangeOf = [](const QJsonObject &query) { return query.value(u"payload"_s).toObject().value(u"children"_s).toObject(); };
  const QList<QJsonObject> queries = sent.ofType(u"query.a11y"_s);
  QCOMPARE(queries.at(1).value(u"payload"_s).toObject().value(u"node"_s).toInteger(), 1);
  QCOMPARE(rangeOf(queries.at(1)), (QJsonObject{{u"start"_s, 0}, {u"count"_s, 2}}));
  QCOMPARE(rangeOf(queries.at(2)), (QJsonObject{{u"start"_s, 5}, {u"count"_s, 2}}));
  QVERIFY(session.loadingAccessibilityParts());
  // The engine answers the first with both children; the second with one of the two, as one message may hold no more, so the session asks
  // for the last one again.
  const QJsonObject caret{{u"node"_s, 4}, {u"offset"_s, 0}};
  QVERIFY(deliver(*engine,
                  replyTo(queries.at(1).value(u"id"_s).toInteger(), QJsonObject{{u"doc_id"_s, 1},
                                                                                {u"root"_s, QJsonObject{{u"id"_s, 1},
                                                                                                        {u"role"_s, u"document"_s},
                                                                                                        {u"name"_s, u"Test"_s},
                                                                                                        {u"child_count"_s, 7},
                                                                                                        {u"children_start"_s, 0},
                                                                                                        {u"children"_s, children(0, 2)}}},
                                                                                {u"caret"_s, caret}}),
                  u"query.a11y"_s));
  QVERIFY(deliver(*engine,
                  replyTo(queries.at(2).value(u"id"_s).toInteger(), QJsonObject{{u"doc_id"_s, 1},
                                                                                {u"root"_s, QJsonObject{{u"id"_s, 1},
                                                                                                        {u"role"_s, u"document"_s},
                                                                                                        {u"name"_s, u"Test"_s},
                                                                                                        {u"child_count"_s, 7},
                                                                                                        {u"children_start"_s, 5},
                                                                                                        {u"children"_s, children(5, 6)}}},
                                                                                {u"caret"_s, caret}}),
                  u"query.a11y"_s));
  QTRY_COMPARE(sent.ofType(u"query.a11y"_s).size(), 4);
  const QJsonObject last = sent.ofType(u"query.a11y"_s).last();
  QCOMPARE(rangeOf(last), (QJsonObject{{u"start"_s, 6}, {u"count"_s, 1}}));
  QVERIFY(deliver(*engine,
                  replyTo(last.value(u"id"_s).toInteger(), QJsonObject{{u"doc_id"_s, 1},
                                                                       {u"root"_s, QJsonObject{{u"id"_s, 1},
                                                                                               {u"role"_s, u"document"_s},
                                                                                               {u"name"_s, u"Test"_s},
                                                                                               {u"child_count"_s, 7},
                                                                                               {u"children_start"_s, 6},
                                                                                               {u"children"_s, children(6, 7)}}},
                                                                       {u"caret"_s, caret}}),
                  u"query.a11y"_s));
  // Every child, in reading order, and nothing more is asked for.
  QTRY_VERIFY(!session.loadingAccessibilityParts());
  const auto &tree = session.accessibilityTree();
  if (!tree) {
    QFAIL("the tree was refused");
  }
  QVERIFY(!tree->root().childCount.has_value());
  QCOMPARE(tree->root().children, (std::vector<quint64>{2, 3, 4, 5, 6, 7, 8}));
  QStringList texts;
  for (const bayan::protocol::AccessibleNode *node : tree->readingOrder()) {
    if (node->hasText) {
      texts.append(node->text);
    }
  }
  QCOMPARE(texts.size(), 7);
  QCOMPARE(texts.first(), u"Paragraph 0"_s);
  QCOMPARE(texts.last(), u"Paragraph 6"_s);
  QTest::qWait(100);
  QCOMPARE(sent.ofType(u"query.a11y"_s).size(), 4);
  QCOMPARE(session.status(), Status::Ready);
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
  // The engine redrew the editable line in a new render revision, moved the caret and replaced the line's accessible node.
  QVERIFY(!invalidated.isEmpty());
  QVERIFY(invalidated.at(0).at(1).toULongLong() > 1);
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
      {u"id"_s, 999'999}, {u"role"_s, u"paragraph"_s}, {u"name"_s, QString()}, {u"text"_s, u"stranger"_s}, {u"children"_s, QJsonArray{}},
  };
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
      u"/payload/pages"_s, u"pages is not a list"_s));
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
  QVERIFY(deliver(*open->engine, engineEvent(u"render.invalidate"_s,
                                             QJsonObject{{u"doc_id"_s, other}, {u"revision"_s, 2}, {u"regions"_s, QJsonArray{region}}})));
  // The session does not even read an event about another document: one it could not read does not stop it.
  QVERIFY(deliverMalformed(*open->engine,
                           compact(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, other}, {u"pages"_s, u"three"_s}})),
                           u"/payload/pages"_s, u"pages is not a list"_s));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  QCOMPARE(invalidated.count(), 0);
  QCOMPARE(pagesChanged.count(), 0);
  QCOMPARE(session.pageCount(), pages);
}

void TestDocumentSession::onlyUnrecoverableEngineErrorsRestartTheSession() {
  // A fake engine.error, from either engine: recoverable true ends nothing (engine protocol §14.1 item 4); recoverable false with restart
  // starts a new session. Nothing of the error reaches the message, here markup that a document might have made the engine repeat. The
  // schema lets the engine send any text as the message identifier and as an argument, but only its own codes.
  const auto open = openSession();
  QVERIFY(open);
  bayan::Engine &engine = *open->engine;
  bayan::DocumentSession &session = *open->session;
  QSignalSpy closed(&session, &bayan::DocumentSession::documentClosed);
  const QJsonObject hostile{
      {u"code"_s, u"internal"_s},
      {u"message_id"_s, u"engine-error-<img src=x>"_s},
      {u"args"_s, QJsonObject{{u"type"_s, u"<b>marker</b>"_s}}},
  };
  QJsonObject recoverable = hostile;
  recoverable.insert(u"recoverable"_s, true);
  QVERIFY(deliver(engine, engineEvent(u"engine.error"_s, recoverable)));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Ready);
  QVERIFY(session.message().isEmpty());

  // recoverable false: the engine discarded its session, and restart says that this is restart 1 of 3. The document closes, and is not
  // reopened.
  QJsonObject unrecoverable = hostile;
  unrecoverable.insert(u"recoverable"_s, false);
  unrecoverable.insert(u"restart"_s, restartValue(1));
  QVERIFY(deliver(engine, engineEvent(u"engine.error"_s, unrecoverable)));
  const qint64 hello = waitForRestart(*open->replies, engine, session, restarted(1, closedNow));
  QVERIFY(hello > 0);
  QCOMPARE(closed.count(), 1);
  QCOMPARE(session.offer(), bayan::DocumentSession::Offer::Reopen);
  QCOMPARE(session.documentId(), 0);
  QCOMPARE(session.pageCount(), 0);
  QVERIFY(!session.accessibilityTree());
  QVERIFY(!session.message().contains(u"marker"_s) && !session.message().contains(u"<"_s));
  // The new session asks for the manifest again, and nothing else: the document waits for the user.
  QTest::qWait(200);
  QCOMPARE(engine.lastPostedRequest(), hello);

  // Another failure while the document is still closed.
  unrecoverable.insert(u"restart"_s, restartValue(2));
  QVERIFY(deliver(engine, engineEvent(u"engine.error"_s, unrecoverable)));
  QVERIFY(waitForRestart(*open->replies, engine, session, restarted(2, stillClosed)) > 0);
  QCOMPARE(closed.count(), 1);
  // restart null: the engine has stopped, and so does the session, with the stop message.
  unrecoverable.insert(u"restart"_s, QJsonValue::Null);
  QVERIFY(deliver(engine, engineEvent(u"engine.error"_s, unrecoverable)));
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(session.message(), stopped);
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
  QCOMPARE(session.message(), stopped);
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
  QCOMPARE(session.message(), stopped);
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
                       emitted = emitEvent(*engine, u"engine.error"_s, failure(restartValue(1)));
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
  // No doc.open, not for the old session's welcome either: only the new session's hello and its request for the manifest.
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
                                        reopening, stopped}));
  QVERIFY(!log.messages().contains(couldNotOpen));
  QCOMPARE(closed.count(), 4);
  // Four sessions, each with its hello, its request for the manifest and its doc.open, and nothing else: requests 1 to 12.
  QCOMPARE(engine->lastPostedRequest(), 12);
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
                           u"/payload/pages"_s, u"pages is not a list"_s));
  QTest::qWait(100);
  QCOMPARE(session.status(), Status::Closed);
  QCOMPARE(session.pageCount(), 0);
  session.reopen();
  QTRY_COMPARE(session.status(), Status::Ready);
}

QTEST_MAIN(TestDocumentSession)
#include "tst_documentsession.moc"
