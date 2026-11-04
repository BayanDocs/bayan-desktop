// Tests of the interface in QML (src/qml) around the document canvas, against the engine the build links: the "Pages" item before the
// canvas and its scrolling keys, the zoom buttons and their announcements (ADR-0020 §5), plain text for engine strings (threat T18), and
// what the interface shows, announces and does with keyboard focus after engine failures (engine protocol §12.1). They load Main.qml as the
// application does and run on the offscreen platform, where screen readers are played by recording Qt's accessibility events.

#include "canvasaccessible.h"
#include "documentcanvas.h"
#include "documentsession.h"
#include "engine.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QInputMethodEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlExtensionPlugin>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QTest>

#include <memory>

// The interface is a static QML module (src/CMakeLists.txt), as in the application.
Q_IMPORT_QML_PLUGIN(Bayan_DesktopPlugin)

using namespace Qt::StringLiterals;

namespace {

using Status = bayan::DocumentSession::Status;

// Whether the linked engine is the stub, which reports a version ending in -stub (also as the stand-in SDK of scripts/test-engine-sdk.sh).
bool isStub() { return bayan::Engine::version().endsWith(u"-stub"_s); }

QJsonObject allowPanic() { return QJsonObject{{u"test"_s, QJsonObject{{u"allow_panic"_s, true}}}}; }

QString restarted(int restart, const QString &document) {
  return u"The engine stopped working and was restarted (%1 of 3). %2"_s.arg(restart).arg(document);
}
const QString closedNow = u"The document was closed and can be reopened."_s;
const QString stillClosed = u"The document is still closed and can be reopened."_s;
const QString stoppedAgain =
    u"The engine stopped working again, so it was not restarted. Close BayanDocs and open it again to restart the engine."_s;

// The announcements screen readers would read (QAccessibleAnnouncementEvent, which QML's Accessible.announce sends), recorded while an
// Announcements object exists.
QStringList &announced() {
  static QStringList messages;
  return messages;
}

void recordAnnouncement(QAccessibleEvent *event) {
  if (event->type() == QAccessible::Announcement) {
    announced().append(
        static_cast<QAccessibleAnnouncementEvent *>(event)->message()); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
  }
}

class Announcements {
public:
  Announcements() : previous_(QAccessible::installUpdateHandler(&recordAnnouncement)) { announced().clear(); }
  ~Announcements() { QAccessible::installUpdateHandler(previous_); }
  Announcements(const Announcements &) = delete;
  Announcements &operator=(const Announcements &) = delete;
  Announcements(Announcements &&) = delete;
  Announcements &operator=(Announcements &&) = delete;

  [[nodiscard]] static const QStringList &messages() { return announced(); }

private:
  QAccessible::UpdateHandler previous_;
};

// The application's window, with its session and engine. The QML engine, and the window with it, goes first.
struct Interface {
  std::unique_ptr<bayan::Engine> engine;
  std::unique_ptr<bayan::DocumentSession> session;
  std::unique_ptr<QQmlApplicationEngine> qml;
  QQuickWindow *window = nullptr;
  QQuickItem *pages = nullptr;
  bayan::DocumentCanvas *canvas = nullptr;
  // The last request posted when the session received the latest unrecoverable engine.error, just before it handled it: requests up to
  // this one were sent while the session still had its document, as it may until it learns of the failure.
  qint64 postedBeforeFailure = -1;

  Interface() = default;
  Interface(const Interface &) = delete;
  Interface &operator=(const Interface &) = delete;
  Interface(Interface &&) = delete;
  Interface &operator=(Interface &&) = delete;
  ~Interface() {
    qml.reset();
    session.reset();
    engine.reset();
  }

  [[nodiscard]] QQuickItem *item(const QString &name) const { return window->findChild<QQuickItem *>(name); }
};

// Opens the interface on an engine with this configuration and waits until it shows the document with keyboard focus in it.
std::unique_ptr<Interface> openInterface(const QJsonObject &config = {}) {
  auto ui = std::make_unique<Interface>();
  ui->engine = bayan::Engine::create(config);
  if (!ui->engine) {
    return nullptr;
  }
  // Connected before the session's own connection, so that it runs first.
  QObject::connect(ui->engine.get(), &bayan::Engine::eventReceived, ui->engine.get(),
                   [raw = ui.get()](const QString &type, const QJsonObject &payload) {
                     if (type == u"engine.error"_s && payload.value(u"recoverable"_s) != QJsonValue(true)) {
                       raw->postedBeforeFailure = raw->engine->lastPostedRequest();
                     }
                   });
  ui->session = std::make_unique<bayan::DocumentSession>(*ui->engine);
  ui->qml = std::make_unique<QQmlApplicationEngine>();
  ui->qml->setInitialProperties({{u"session"_s, QVariant::fromValue(ui->session.get())}});
  ui->qml->loadFromModule(u"Bayan.Desktop"_s, u"Main"_s);
  const QList<QObject *> roots = ui->qml->rootObjects();
  ui->window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.constFirst());
  if (ui->window == nullptr) {
    return nullptr;
  }
  ui->canvas = ui->window->findChild<bayan::DocumentCanvas *>(u"documentCanvas"_s);
  ui->pages = ui->item(u"pages"_s);
  if (ui->canvas == nullptr || ui->pages == nullptr) {
    return nullptr;
  }
  ui->session->start();
  ui->window->requestActivate();
  if (!QTest::qWaitFor([&ui] { return ui->session->status() == Status::Ready && ui->canvas->tilesComplete(); }) ||
      !QTest::qWaitFor([&ui] { return ui->canvas->hasActiveFocus(); })) {
    return nullptr;
  }
  return ui;
}

QString accessibleName(QObject *object) {
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(object);
  return accessible != nullptr ? accessible->text(QAccessible::Name) : u"<no accessible interface>"_s;
}

// The text items of the interface's own QML files (module Bayan.Desktop) under root, not those inside Qt Quick Controls' styles.
QList<QQuickItem *> interfaceTexts(QQuickItem *root) {
  QList<QQuickItem *> texts;
  QList<QQuickItem *> pending{root};
  while (!pending.isEmpty()) {
    QQuickItem *item = pending.takeLast();
    const QQmlContext *context = qmlContext(item);
    if (item->inherits("QQuickText") && context != nullptr && context->baseUrl().path().contains(u"/Bayan/Desktop/"_s)) {
      texts.append(item);
    }
    pending.append(item->childItems());
  }
  return texts;
}

// Clicks an item in the middle, as the mouse would.
void click(const Interface &ui, const QQuickItem *item) {
  QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

} // namespace

class TestInterface final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void thePagesAreATabStopBeforeTheDocument();
  void thePagesScrollWithTheKeysThatTheDocumentSendsToTheEngine();
  void zoomButtonsZoomAndAnnounceTheLevel();
  void everyTextOfTheInterfaceIsPlainText();
  void markupInAnEngineStringIsShownAsPlainText();
  void failuresAreShownAndAnnouncedAndKeepFocusInThePages();
  void aCompositionInProgressNeverReachesAFailedEngine();
  void focusOnTheOfferGoesToThePagesWhenTheOfferGoes();
};

void TestInterface::initTestCase() {
  // As main.cpp does.
  QQuickStyle::setStyle(u"Fusion"_s);
  bayan::installCanvasAccessibleFactory();
}

void TestInterface::thePagesAreATabStopBeforeTheDocument() {
  const auto ui = openInterface();
  QVERIFY(ui);
  // Right before the canvas in the tab order, and a named, focusable pane for screen readers.
  QCOMPARE(ui->canvas->nextItemInFocusChain(false), ui->pages);
  QCOMPARE(ui->pages->nextItemInFocusChain(true), ui->canvas);
  QCOMPARE(accessibleName(ui->pages), u"Pages"_s);
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(ui->pages);
  QVERIFY(accessible != nullptr);
  QCOMPARE(accessible->role(), QAccessible::Pane);
  QVERIFY(accessible->state().focusable);
  QQuickItem *frame = ui->item(u"pagesFocusFrame"_s);
  QVERIFY(frame != nullptr);
  QVERIFY(!frame->isVisible());
  // Shift+Tab from the document reaches the Pages, which show that they have the focus; Tab goes back into the document.
  QTest::keyClick(ui->window, Qt::Key_Backtab, Qt::ShiftModifier);
  QTRY_VERIFY(ui->pages->hasActiveFocus());
  QVERIFY(frame->isVisible());
  QVERIFY(accessible->state().focused);
  QTest::keyClick(ui->window, Qt::Key_Tab);
  QTRY_VERIFY(ui->canvas->hasActiveFocus());
  QVERIFY(!frame->isVisible());
}

void TestInterface::thePagesScrollWithTheKeysThatTheDocumentSendsToTheEngine() {
  const auto ui = openInterface();
  QVERIFY(ui);
  bayan::DocumentCanvas &canvas = *ui->canvas;
  bayan::Engine &engine = *ui->engine;
  const double bottom = canvas.contentHeight() - canvas.height();
  QVERIFY(bottom > bayan::DocumentCanvas::kArrowScrollStep);
  QTest::qWait(300); // Lets a view.set scheduled while the document opened go out first.
  // In the document, the keys go to the engine and the view stays.
  qint64 before = engine.lastPostedRequest();
  QTest::keyClick(ui->window, Qt::Key_PageDown);
  QTest::keyClick(ui->window, Qt::Key_End);
  QTest::keyClick(ui->window, Qt::Key_Down);
  QCOMPARE(engine.lastPostedRequest(), before + 3);
  QTest::qWait(300);
  QCOMPARE(canvas.contentY(), 0.0);
  // On the Pages, they scroll, and nothing goes to the engine.
  ui->pages->forceActiveFocus(Qt::TabFocusReason);
  QVERIFY(ui->pages->hasActiveFocus());
  before = engine.lastPostedRequest();
  QTest::keyClick(ui->window, Qt::Key_PageDown);
  QTRY_COMPARE(canvas.contentY(), std::min(canvas.height() * 0.9, bottom));
  QTest::keyClick(ui->window, Qt::Key_End);
  QTRY_COMPARE(canvas.contentY(), bottom);
  QTest::keyClick(ui->window, Qt::Key_Home);
  QTRY_COMPARE(canvas.contentY(), 0.0);
  QTest::keyClick(ui->window, Qt::Key_Down);
  QTRY_COMPARE(canvas.contentY(), bayan::DocumentCanvas::kArrowScrollStep);
  QTest::keyClick(ui->window, Qt::Key_PageUp);
  QTRY_COMPARE(canvas.contentY(), 0.0);
  QCOMPARE(engine.lastPostedRequest(), before);
  QVERIFY(ui->pages->hasActiveFocus());
}

void TestInterface::zoomButtonsZoomAndAnnounceTheLevel() {
  const auto ui = openInterface();
  QVERIFY(ui);
  QQuickItem *zoomIn = ui->item(u"zoomInButton"_s);
  QQuickItem *zoomOut = ui->item(u"zoomOutButton"_s);
  QQuickItem *level = ui->item(u"zoomLevel"_s);
  QVERIFY(zoomIn != nullptr && zoomOut != nullptr && level != nullptr);
  QCOMPARE(accessibleName(zoomIn), u"Zoom in"_s);
  QCOMPARE(accessibleName(zoomOut), u"Zoom out"_s);
  QCOMPARE(QAccessible::queryAccessibleInterface(zoomIn)->role(), QAccessible::Button);
  QCOMPARE(level->property("text").toString(), u"Zoom 100 %"_s);
  const Announcements announcements;
  click(*ui, zoomIn);
  QCOMPARE(ui->canvas->zoom(), 1.1);
  QCOMPARE(level->property("text").toString(), u"Zoom 110 %"_s);
  QCOMPARE(Announcements::messages(), QStringList{u"Zoom 110 %"_s});
  // From the keyboard: Tab reaches the buttons, which show the focus (visualFocus, ADR-0020 §5), and Space presses them.
  zoomOut->forceActiveFocus(Qt::TabFocusReason);
  QVERIFY(zoomOut->property("visualFocus").toBool());
  QTest::keyClick(ui->window, Qt::Key_Space);
  QCOMPARE(ui->canvas->zoom(), 1.0);
  QCOMPARE(Announcements::messages().last(), u"Zoom 100 %"_s);
  QTest::keyClick(ui->window, Qt::Key_Tab);
  QTRY_VERIFY(zoomIn->hasActiveFocus());
  QVERIFY(zoomIn->property("visualFocus").toBool());
  QVERIFY(!zoomOut->property("visualFocus").toBool());
  // Up to 500 %, where Zoom in is disabled and hands the keyboard focus to Zoom out instead of losing it.
  for (int press = 0; press < 7; ++press) {
    QTest::keyClick(ui->window, Qt::Key_Space);
  }
  QCOMPARE(ui->canvas->zoom(), bayan::DocumentCanvas::kMaximumZoom);
  QCOMPARE(Announcements::messages().last(), u"Zoom 500 %"_s);
  QVERIFY(!zoomIn->isEnabled());
  QTRY_VERIFY(zoomOut->hasActiveFocus());
  QVERIFY(zoomOut->property("visualFocus").toBool());
  QTest::keyClick(ui->window, Qt::Key_Space);
  QCOMPARE(ui->canvas->zoom(), 4.0);
  QVERIFY(zoomIn->isEnabled());
  QCOMPARE(Announcements::messages().size(), 10);
}

void TestInterface::everyTextOfTheInterfaceIsPlainText() {
  // Every text item of the interface's own QML is plain text, so that none can turn engine or document text into markup (threat T18): an
  // <img> in it would load an image, from the network too. Fails if the search no longer finds the texts that show engine data.
  const auto ui = openInterface();
  QVERIFY(ui);
  const QList<QQuickItem *> texts = interfaceTexts(ui->window->contentItem());
  QStringList names;
  for (const QQuickItem *text : std::as_const(texts)) {
    names.append(text->objectName());
    QVERIFY2(text->property("textFormat").toInt() == 0, // Text.PlainText
             qPrintable(u"%1 (%2) is not plain text"_s.arg(text->objectName(), qmlContext(text)->baseUrl().toString())));
  }
  QVERIFY2(texts.size() >= 4, qPrintable(names.join(u", "_s)));
  for (const QString &name : {u"statusText"_s, u"zoomLevel"_s, u"sessionMessage"_s}) {
    QVERIFY2(names.contains(name), qPrintable(name));
  }
}

void TestInterface::markupInAnEngineStringIsShownAsPlainText() {
  if (!isStub()) {
    QSKIP("Needs the stub's engine_version test switch; the other engine's strings hold no markup.");
  }
  // A local image 400 pixels tall: shown as rich text, the status bar's text would be that tall; as plain text it is one short line.
  QTemporaryDir folder;
  QVERIFY(folder.isValid());
  QImage tall(8, 400, QImage::Format_RGB32);
  tall.fill(QColor(Qt::red));
  const QString image = QDir(folder.path()).filePath(u"tall.png"_s);
  QVERIFY(tall.save(image));
  const QString version = u"<img src=\"%1\"><b>bold</b>"_s.arg(QUrl::fromLocalFile(image).toString());
  const auto ui = openInterface(QJsonObject{{u"test"_s, QJsonObject{{u"engine_version"_s, version}}}});
  QVERIFY(ui);
  QQuickItem *status = ui->item(u"statusText"_s);
  QVERIFY(status != nullptr);
  QVERIFY(status->property("text").toString().contains(version));
  QTest::qWait(300); // Time for an image to load, if one were loaded.
  QCOMPARE(status->property("lineCount").toInt(), 1);
  QVERIFY2(status->implicitHeight() < 100, qPrintable(QString::number(status->implicitHeight())));
}

void TestInterface::failuresAreShownAndAnnouncedAndKeepFocusInThePages() {
  const auto ui = openInterface(allowPanic());
  QVERIFY(ui);
  bayan::Engine &engine = *ui->engine;
  QQuickItem *message = ui->item(u"sessionMessage"_s);
  QQuickItem *offer = ui->item(u"offerButton"_s);
  QQuickItem *status = ui->item(u"statusText"_s);
  QVERIFY(message != nullptr && offer != nullptr && status != nullptr);
  QVERIFY(!message->isVisible() && !offer->isVisible());
  const Announcements announcements;

  // A failure while the document has the keyboard focus: the message is shown and announced, focus stays in the Pages, and the offer is
  // there, one Tab away, but no key meant for the document takes it.
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(message->property("text").toString(), restarted(1, closedNow));
  QVERIFY(message->isVisible());
  QCOMPARE(Announcements::messages(), QStringList{restarted(1, closedNow)});
  QTRY_VERIFY(ui->pages->hasActiveFocus());
  QVERIFY(!ui->canvas->isVisible());
  QVERIFY(offer->isVisible());
  QCOMPARE(offer->property("text").toString(), u"Reopen the document"_s);
  QCOMPARE(accessibleName(offer), u"Reopen the document"_s);
  QCOMPARE(status->property("text").toString(), u"No document open"_s);
  QTest::qWait(200); // The new session's hello has been answered.
  const qint64 hello = engine.lastPostedRequest();
  for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space, Qt::Key_A}) {
    QTest::keyClick(ui->window, key);
  }
  QTest::qWait(200);
  QCOMPARE(engine.lastPostedRequest(), hello);
  QCOMPARE(ui->session->status(), Status::Closed);
  QVERIFY(ui->pages->hasActiveFocus());

  // The user takes the offer from the keyboard: the document is reopened and the keyboard focus goes into it, since the button is gone.
  QTest::keyClick(ui->window, Qt::Key_Tab);
  QTRY_VERIFY(offer->hasActiveFocus());
  QVERIFY(offer->property("visualFocus").toBool());
  QTest::keyClick(ui->window, Qt::Key_Space);
  QTRY_COMPARE(ui->session->status(), Status::Ready);
  QTRY_VERIFY(ui->canvas->hasActiveFocus());
  QVERIFY(!offer->isVisible());
  QVERIFY(!message->isVisible());
  QCOMPARE(Announcements::messages(),
           (QStringList{restarted(1, closedNow), u"Reopening the document…"_s, u"The document was reopened."_s}));

  // Two more failures, the second while the document is closed, and the fourth stops: each is announced, the offer goes at the stop, and
  // focus stays in the Pages throughout.
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(message->property("text").toString(), restarted(2, closedNow));
  QTRY_VERIFY(ui->pages->hasActiveFocus());
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(message->property("text").toString(), restarted(3, stillClosed));
  QVERIFY(offer->isVisible());
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(message->property("text").toString(), stoppedAgain);
  QCOMPARE(ui->session->status(), Status::Failed);
  QVERIFY(!offer->isVisible());
  QVERIFY(ui->pages->hasActiveFocus());
  QCOMPARE(status->property("text").toString(), u"Engine stopped"_s);
  QCOMPARE(Announcements::messages().mid(3), (QStringList{restarted(2, closedNow), restarted(3, stillClosed), stoppedAgain}));
}

void TestInterface::aCompositionInProgressNeverReachesAFailedEngine() {
  // When a failure closes the document, keyboard focus leaves the canvas, which ends an input-method composition in progress. By then the
  // session has forgotten the document, so the composition goes nowhere: after a restart only the new hello reaches the engine, and after
  // the stop nothing does (engine protocol §12.1, point 1).
  const auto ui = openInterface(allowPanic());
  QVERIFY(ui);
  bayan::Engine &engine = *ui->engine;
  // The input method starts composing "あ" in the document, as a Japanese input method does with the first key.
  const auto compose = [&ui] {
    QInputMethodEvent preedit(u"あ"_s, {QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, 1, 1, QVariant())});
    QCoreApplication::sendEvent(ui->canvas, &preedit);
    return ui->canvas->composing();
  };
  // Requests the session sends before the failure reaches it, such as a view.set, still belong to the live session; the test counts from
  // the moment the session receives the engine.error.
  for (int restart = 1; restart <= 3; ++restart) {
    QVERIFY(compose());
    QVERIFY(engine.post(u"diag.panic"_s));
    QTRY_COMPARE(ui->session->message(), restarted(restart, closedNow));
    QTRY_VERIFY(ui->pages->hasActiveFocus());
    QVERIFY(!ui->canvas->composing());
    QTest::qWait(200);
    QVERIFY(ui->postedBeforeFailure > 0);
    QCOMPARE(engine.lastPostedRequest(), ui->postedBeforeFailure + 1); // The new session's hello, nothing else.
    ui->session->reopen();
    QTRY_COMPARE(ui->session->status(), Status::Ready);
    ui->canvas->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(ui->canvas->hasActiveFocus());
  }
  QVERIFY(compose());
  const qint64 previous = ui->postedBeforeFailure;
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(ui->session->status(), Status::Failed);
  QVERIFY(!ui->canvas->composing());
  QTest::qWait(200);
  QVERIFY(ui->postedBeforeFailure > previous);
  QCOMPARE(engine.lastPostedRequest(), ui->postedBeforeFailure); // Nothing at all.
}

void TestInterface::focusOnTheOfferGoesToThePagesWhenTheOfferGoes() {
  // The offer goes at the stop. Keyboard focus on its button then moves to the Pages: a hidden button would keep it, and the next key would
  // reach an invisible control (ADR-0020 §5, visible focus).
  const auto ui = openInterface(allowPanic());
  QVERIFY(ui);
  bayan::Engine &engine = *ui->engine;
  QQuickItem *offer = ui->item(u"offerButton"_s);
  QVERIFY(offer != nullptr);
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(ui->session->message(), restarted(1, closedNow));
  QTRY_VERIFY(ui->pages->hasActiveFocus());
  QTest::keyClick(ui->window, Qt::Key_Tab);
  QTRY_VERIFY(offer->hasActiveFocus());
  QVERIFY(offer->property("visualFocus").toBool());
  // Failures while the offer has the focus: the offer stays, and so does the focus.
  for (int restart = 2; restart <= 3; ++restart) {
    QVERIFY(engine.post(u"diag.panic"_s));
    QTRY_COMPARE(ui->session->message(), restarted(restart, stillClosed));
    QVERIFY(offer->isVisible());
    QVERIFY(offer->hasActiveFocus());
  }
  QVERIFY(engine.post(u"diag.panic"_s));
  QTRY_COMPARE(ui->session->status(), Status::Failed);
  QVERIFY(!offer->isVisible());
  QTRY_COMPARE(ui->window->activeFocusItem(), ui->pages);
  QVERIFY(!offer->hasActiveFocus());
}

QTEST_MAIN(TestInterface)
#include "tst_interface.moc"
