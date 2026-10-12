// Tests of the document canvas in a window (DESK-002), against the engine the build links. The stub engine (BAYAN_ENGINE=stub) shows one
// test page and has no input or accessibility; the real engine of bayan-core's C SDK (BAYAN_ENGINE=sdk) has CORE-007's mock document with
// an editable line and an accessibility tree. Tests that need the mock document skip with the stub; CI runs them against the real engine.
// They run on the offscreen platform, so input methods and screen readers are played by sending Qt's events and calling Qt's
// accessibility interfaces directly, as the platform bridges would.

#include "accessibletext.h"
#include "canvasnode.h"
#include "documentcanvas.h"
#include "documentsession.h"
#include "engine.h"
#include "pixelprobe.h"

#include <QtCore/QDir>
#include <QtCore/QList>
#include <QtGui/QAccessible>
#include <QtGui/QAccessibleEditableTextInterface>
#include <QtGui/QAccessibleTextInterface>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QStyleHints>
#include <QtGui/QTextCharFormat>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

// A window with a canvas showing the engine's document. The canvas is destroyed with the window, before the session and the engine.
struct Fixture {
  std::unique_ptr<bayan::Engine> engine;
  std::unique_ptr<bayan::DocumentSession> session;
  std::unique_ptr<QQuickWindow> window;
  bayan::DocumentCanvas *canvas = nullptr;

  Fixture() = default;
  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
  Fixture(Fixture &&) = delete;
  Fixture &operator=(Fixture &&) = delete;
  ~Fixture() {
    window.reset();
    session.reset();
    engine.reset();
  }

  [[nodiscard]] bool hasMockDocument() const { return session->features().contains(u"mock_document"_s); }
};

std::unique_ptr<Fixture> openDocument(QSize size = QSize(900, 700)) {
  auto fixture = std::make_unique<Fixture>();
  fixture->engine = bayan::Engine::create();
  if (!fixture->engine) {
    return nullptr;
  }
  fixture->session = std::make_unique<bayan::DocumentSession>(*fixture->engine);
  fixture->window = std::make_unique<QQuickWindow>();
  fixture->window->resize(size);
  fixture->canvas = new bayan::DocumentCanvas(fixture->window->contentItem()); // NOLINT(cppcoreguidelines-owning-memory): a child item.
  fixture->canvas->setSize(size);
  fixture->canvas->setSession(fixture->session.get());
  fixture->window->show();
  fixture->session->start();
  if (!QTest::qWaitFor(
          [&fixture] { return fixture->session->status() == bayan::DocumentSession::Status::Ready && fixture->canvas->tilesComplete(); })) {
    return nullptr;
  }
  fixture->window->requestActivate();
  fixture->canvas->forceActiveFocus();
  if (!QTest::qWaitFor([&fixture] { return fixture->canvas->hasActiveFocus(); })) {
    return nullptr;
  }
  return fixture;
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

// The accessibility caret's offset in its node, or -1 without a caret.
int caretOffset(const bayan::DocumentSession &session) {
  const auto caret = session.accessibilityCaret();
  return caret ? caret->offset : -1;
}

// A rectangle of a page in the canvas's coordinates, or an empty one.
QRectF itemRect(const bayan::DocumentCanvas &canvas, const std::optional<bayan::protocol::PageRect> &rect) {
  return rect ? canvas.itemRectFor(rect->page, rect->rect).value_or(QRectF()) : QRectF();
}

// Whether the run must have the real engine's mock document: CI sets BAYAN_REQUIRE_MOCK_DOCUMENT when it tests the real SDK, so that the
// tests that need it fail rather than skip if it is missing.
bool mockDocumentRequired() { return qEnvironmentVariableIsSet("BAYAN_REQUIRE_MOCK_DOCUMENT"); }

// Saves what the window shows as <name>@<scale>x.png in the folder that BAYAN_TEST_SCREENSHOTS names, once the canvas shows complete tiles:
// pictures for the DESK-002 report and for looking at a run. Without the variable nothing is saved.
void saveScreenshot(const Fixture &fixture, const QString &name) {
  const QString folder = qEnvironmentVariable("BAYAN_TEST_SCREENSHOTS");
  if (folder.isEmpty() || !QDir().mkpath(folder)) {
    return;
  }
  (void)QTest::qWaitFor([&fixture] { return fixture.canvas->tilesComplete(); });
  const QString file = u"%1@%2x.png"_s.arg(name).arg(fixture.window->effectiveDevicePixelRatio());
  if (!fixture.window->grabWindow().save(QDir(folder).filePath(file))) {
    qWarning("Could not save the screenshot %s", qPrintable(file));
  }
}

std::optional<QString> pixelProblem(const Fixture &fixture) {
  return bayan::compareWithEnginePixels(*fixture.canvas, fixture.window->grabWindow());
}

// Types ASCII text key by key, as a keyboard does (QTest::keyClicks exists only for widgets).
void typeText(QWindow *window, const QString &text) {
  for (const QChar character : text) {
    QTest::keyClick(window, character.toLatin1());
  }
}

void sendIme(QObject *target, const QString &preedit, const QList<QInputMethodEvent::Attribute> &attributes, const QString &commit = {},
             int replaceFrom = 0, int replaceLength = 0) {
  QInputMethodEvent event(preedit, attributes);
  if (!commit.isEmpty() || replaceLength > 0) {
    event.setCommitString(commit, replaceFrom, replaceLength);
  }
  QCoreApplication::sendEvent(target, &event);
}

// What Qt's Windows input context sends while composing: the converted clause highlighted with a background, and the cursor.
QList<QInputMethodEvent::Attribute> clause(int start, int length, int cursor) {
  QTextCharFormat selected;
  selected.setBackground(Qt::black);
  return {QInputMethodEvent::Attribute(QInputMethodEvent::TextFormat, start, length, selected),
          QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, cursor, 0, QVariant())};
}

// What a plain composition looks like: an underlined text and a visible cursor.
QList<QInputMethodEvent::Attribute> underlined(int length, int cursor) {
  QTextCharFormat format;
  format.setFontUnderline(true);
  return {QInputMethodEvent::Attribute(QInputMethodEvent::TextFormat, 0, length, format),
          QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, cursor, 1, QVariant())};
}

// Accessibility events recorded by the update handler (QAccessible::installUpdateHandler).
struct RecordedEvent {
  QAccessible::Event type = QAccessible::InvalidEvent;
  int position = -1;
  QString removed{};
  QString inserted{};
};
std::vector<RecordedEvent> &recordedEvents() {
  static std::vector<RecordedEvent> events;
  return events;
}
void recordEvent(QAccessibleEvent *event) {
  RecordedEvent recorded{.type = event->type()};
  if (event->type() == QAccessible::TextUpdated) {
    const auto *update = static_cast<QAccessibleTextUpdateEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
    recorded.position = update->changePosition();
    recorded.removed = update->textRemoved();
    recorded.inserted = update->textInserted();
  } else if (event->type() == QAccessible::TextCaretMoved) {
    recorded.position =
        static_cast<QAccessibleTextCursorEvent *>(event)->cursorPosition(); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
  }
  recordedEvents().push_back(recorded);
}

} // namespace

class TestCanvas final : public QObject {
  Q_OBJECT

private slots:
  void showsTheEnginesPixels();
  void scrollingShowsTheEnginesPixelsAndStopsAtTheEnds();
  void zoomStretchesTilesUntilTheNewOnesArrive();
  void zoomKeepsThePointUnderThePointer();
  void standInsAreDrawnInTheOrderGiven();
  void aNewSessionForgetsTheOldEnginesChanges();
  void keysAndTheWheelMoveTheView();
  void tabLeavesTheCanvas();
  void stubHasNoEditableText();
  void typingReachesTheEditableLine();
  void japaneseCompositionShowsAndCommits();
  void chineseCompositionCommitsTheCandidate();
  void koreanSyllablesCommitOneByOne();
  void cancelledCompositionLeavesTheText();
  void accentMenuReplacesTheCharacterBeforeTheCaret();
  void deletingAroundTheCaretStaysInTheEditableText();
  void clickMovesTheCaret();
  void doubleClickIsTwoPressesAndTwoReleases();
  void inputMethodSeesTheCaretAndTheTextAroundIt();
  void accessibilityExposesTheDocumentText();
  void accessibilityReportsTextAndCaretChanges();
  void accessibleEditingGoesThroughTheEngine();
  void destroyingTheWindowWhileTilesRenderIsSafe();
};

void TestCanvas::showsTheEnginesPixels() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  // Tiles are rendered for the window's device pixels (125 % in app.canvas_scale125), not stretched from 100 %.
  QCOMPARE(fixture->canvas->geometry().scale(), fixture->window->effectiveDevicePixelRatio());
  const auto problem = pixelProblem(*fixture);
  QVERIFY2(!problem, qPrintable(problem.value_or(QString())));
  saveScreenshot(*fixture, u"document"_s);
  // The page area is centred: the margins left and right of the first page are equal.
  const QRectF page = fixture->canvas->geometry().pageItemRect(fixture->session->pages().front());
  QVERIFY(qAbs(page.left() - (fixture->canvas->width() - page.right())) <= 1.0);
}

void TestCanvas::scrollingShowsTheEnginesPixelsAndStopsAtTheEnds() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  QVERIFY(canvas.contentHeight() > canvas.height());
  canvas.scrollBy(0, 300);
  QCOMPARE(canvas.contentY(), 300.0);
  QTRY_VERIFY(canvas.tilesComplete());
  const auto problem = pixelProblem(*fixture);
  QVERIFY2(!problem, qPrintable(problem.value_or(QString())));
  canvas.scrollBy(0, 1e9);
  QCOMPARE(canvas.contentY(), canvas.contentHeight() - canvas.height());
  canvas.scrollBy(0, -1e9);
  QCOMPARE(canvas.contentY(), 0.0);
}

void TestCanvas::zoomStretchesTilesUntilTheNewOnesArrive() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  canvas.resetStatistics();
  canvas.zoomAt(2.0, QPointF(canvas.width() / 2, canvas.height() / 2));
  QCOMPARE(canvas.zoom(), 2.0);
  // Until the zoom settles, the frame shows the old tiles stretched, and is not complete.
  QTRY_VERIFY(canvas.statistics().frames > 0);
  QVERIFY(!canvas.tilesComplete());
  QVERIFY(canvas.statistics().framesWithStandIns > 0 || canvas.statistics().framesWithHoles == 0);
  canvas.commitZoom();
  QTRY_VERIFY(canvas.tilesComplete());
  const auto problem = pixelProblem(*fixture);
  QVERIFY2(!problem, qPrintable(problem.value_or(QString())));
  // The zoom stays within Word's range.
  canvas.setZoom(100);
  QCOMPARE(canvas.zoom(), bayan::DocumentCanvas::kMaximumZoom);
  canvas.setZoom(0);
  QCOMPARE(canvas.zoom(), bayan::DocumentCanvas::kMinimumZoom);
}

void TestCanvas::zoomKeepsThePointUnderThePointer() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  canvas.scrollBy(0, 200);
  const QPointF pointer(300, 250);
  const QPointF before = canvas.geometry().viewSpacePoint(pointer);
  canvas.zoomAt(1.5, pointer);
  const QPointF after = canvas.geometry().viewSpacePoint(pointer);
  // Within a device pixel, which at zoom 1.5 is 12,700 BLU.
  QVERIFY2(qAbs(before.x() - after.x()) < 12'700 && qAbs(before.y() - after.y()) < 12'700,
           qPrintable(u"(%1, %2) became (%3, %4)"_s.arg(before.x()).arg(before.y()).arg(after.x()).arg(after.y())));
}

void TestCanvas::standInsAreDrawnInTheOrderGiven() {
  // The canvas hands the scene graph its stand-ins from the farthest zoom to the closest, so that the sharpest are drawn on top. The node
  // must keep that order, whatever order the tiles first appeared in.
  const auto fixture = openDocument();
  QVERIFY(fixture);
  QQuickWindow &window = *fixture->window;
  QImage image(16, 16, QImage::Format_RGBA8888_Premultiplied);
  image.fill(Qt::white);
  const auto standIn = [&image](double scale) {
    return bayan::CanvasTile{.key = {.page = 0, .scale = scale, .column = 0, .row = 0},
                             .rect = QRectF(0, 0, 16, 16),
                             .image = image,
                             .version = 1,
                             .current = false,
                             .exact = false};
  };
  const bayan::CanvasTile blurry = standIn(1.0);
  const bayan::CanvasTile sharp = standIn(1.25);
  std::vector<std::vector<bayan::TileKey>> orders;
  // Nodes and textures belong to the scene graph: make them while it synchronizes with the GUI thread, as updatePaintNode does.
  const QMetaObject::Connection connection = connect(
      &window, &QQuickWindow::beforeSynchronizing, this,
      [&window, &orders, &blurry, &sharp] {
        if (!orders.empty()) {
          return;
        }
        bayan::CanvasNode node;
        (void)node.setTiles(window, {sharp, blurry}, 8);
        orders.push_back(node.standInOrder());
        (void)node.setTiles(window, {blurry, sharp}, 8);
        orders.push_back(node.standInOrder());
        (void)node.setTiles(window, {blurry, sharp}, 8);
        orders.push_back(node.standInOrder());
      },
      Qt::DirectConnection);
  window.update();
  QTRY_COMPARE(orders.size(), std::size_t{3});
  disconnect(connection);
  QVERIFY(orders.at(0) == (std::vector{sharp.key, blurry.key}));
  QVERIFY(orders.at(1) == (std::vector{blurry.key, sharp.key}));
  QVERIFY(orders.at(2) == orders.at(1));
}

void TestCanvas::aNewSessionForgetsTheOldEnginesChanges() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  // Views of other zooms: the engine changes every page while handling requests that a new engine will not reach for a while.
  int invalidations = 0;
  const QMetaObject::Connection counter =
      connect(fixture->session.get(), &bayan::DocumentSession::invalidated, this, [&invalidations] { ++invalidations; });
  for (int step = 1; step <= 20; ++step) {
    fixture->session->setView({.viewport = QRectF(0, 0, 900, 700), .zoom = 1.0 + (step * 0.01), .deviceScale = 1.0});
  }
  // At least those 20: the canvas may send a view of its own meanwhile.
  QTRY_VERIFY(invalidations >= 20);
  disconnect(counter);
  QTRY_VERIFY(canvas.tilesComplete());
  // Another document in the same canvas, from a new engine that counts its requests from 1 again.
  const auto engine = bayan::Engine::create();
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  canvas.setSession(&session);
  session.start();
  QTRY_VERIFY(session.status() == bayan::DocumentSession::Status::Ready);
  QTRY_VERIFY(canvas.tilesComplete());
  canvas.setSession(nullptr);
}

void TestCanvas::keysAndTheWheelMoveTheView() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  // Page Down scrolls by most of the view (the stub's single page is shorter than that, so only to the end).
  QTest::keyClick(fixture->window.get(), Qt::Key_PageDown);
  QTRY_COMPARE(canvas.contentY(), std::min(canvas.height() * 0.9, canvas.contentHeight() - canvas.height()));
  QTest::keyClick(fixture->window.get(), Qt::Key_PageUp);
  QTRY_COMPARE(canvas.contentY(), 0.0);
  // A mouse wheel notch scrolls down by the platform's lines per notch.
  QWheelEvent wheel(QPointF(100, 100), fixture->window->mapToGlobal(QPointF(100, 100)), QPoint(), QPoint(0, -120), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(&canvas, &wheel);
  QTRY_VERIFY(canvas.contentY() > 0.0);
  // With Control, it zooms; Control+0 goes back to 100 %.
  QWheelEvent zoomWheel(QPointF(100, 100), fixture->window->mapToGlobal(QPointF(100, 100)), QPoint(), QPoint(0, 120), Qt::NoButton,
                        Qt::ControlModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(&canvas, &zoomWheel);
  QCOMPARE(canvas.zoom(), 1.1);
  QTest::keyClick(fixture->window.get(), Qt::Key_0, Qt::ControlModifier);
  QCOMPARE(canvas.zoom(), 1.0);
  QTest::keyClick(fixture->window.get(), Qt::Key_Plus, Qt::ControlModifier);
  QCOMPARE(canvas.zoom(), 1.25);
}

void TestCanvas::tabLeavesTheCanvas() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  // A second focusable item after the canvas: Tab must move the focus there, so keyboard users are never trapped in the document.
  QQuickItem next(fixture->window->contentItem());
  next.setActiveFocusOnTab(true);
  QTest::keyClick(fixture->window.get(), Qt::Key_Tab);
  QTRY_VERIFY(next.hasActiveFocus());
}

void TestCanvas::stubHasNoEditableText() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (fixture->hasMockDocument()) {
    QSKIP("The real engine has an editable line; this checks the stub.");
  }
  QVERIFY(!fixture->canvas->inputMethodQuery(Qt::ImEnabled).toBool());
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(fixture->canvas);
  QVERIFY(accessible != nullptr && accessible->textInterface() != nullptr);
  QCOMPARE(accessible->textInterface()->characterCount(), 0);
  QCOMPARE(accessible->text(QAccessible::Name), u"Document"_s);
}

void TestCanvas::typingReachesTheEditableLine() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  typeText(fixture->window.get(), u"Hi there"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"Hi there"_s);
  // Backspace removes the "e" before the caret; two steps left and Delete remove the "e" after it: "Hi th|er" becomes "Hi th|r".
  QTest::keyClick(fixture->window.get(), Qt::Key_Backspace);
  QTest::keyClick(fixture->window.get(), Qt::Key_Left);
  QTest::keyClick(fixture->window.get(), Qt::Key_Left);
  QTest::keyClick(fixture->window.get(), Qt::Key_Delete);
  QTRY_COMPARE(editableText(*fixture->session), u"Hi thr"_s);
  QCOMPARE(caretOffset(*fixture->session), 5);
  // A shortcut types nothing.
  QTest::keyClick(fixture->window.get(), Qt::Key_A, Qt::ControlModifier);
  typeText(fixture->window.get(), u"!"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"Hi th!r"_s);
  // The canvas shows the engine's new pixels.
  QTRY_VERIFY(fixture->canvas->tilesComplete());
  saveScreenshot(*fixture, u"typing"_s);
  const auto problem = pixelProblem(*fixture);
  QVERIFY2(!problem, qPrintable(problem.value_or(QString())));
}

void TestCanvas::japaneseCompositionShowsAndCommits() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  typeText(fixture->window.get(), u"a"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"a"_s);
  // Romaji input: n, ni, nih, niho, nihon... the input method shows kana as pre-edit text.
  sendIme(&canvas, u"ｎ"_s, underlined(1, 1));
  sendIme(&canvas, u"に"_s, underlined(1, 1));
  sendIme(&canvas, u"にほ"_s, underlined(2, 2));
  sendIme(&canvas, u"にほん"_s, underlined(3, 3));
  QVERIFY(canvas.composing());
  // The engine shows the composition at the caret, inside the editable line.
  QTRY_COMPARE(editableText(*fixture->session), u"aにほん"_s);
  saveScreenshot(*fixture, u"ime-japanese-kana"_s);
  // Space converts: the whole clause is selected.
  sendIme(&canvas, u"日本"_s, clause(0, 2, 0));
  QTRY_COMPARE(editableText(*fixture->session), u"a日本"_s);
  QTRY_VERIFY(canvas.tilesComplete());
  const auto composing = pixelProblem(*fixture);
  QVERIFY2(!composing, qPrintable(composing.value_or(QString())));
  saveScreenshot(*fixture, u"ime-japanese-converted"_s);
  // Enter commits.
  sendIme(&canvas, QString(), {}, u"日本"_s);
  QVERIFY(!canvas.composing());
  typeText(fixture->window.get(), u"b"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"a日本b"_s);
  saveScreenshot(*fixture, u"ime-japanese-committed"_s);
  QCOMPARE(caretOffset(*fixture->session), 4);
}

void TestCanvas::chineseCompositionCommitsTheCandidate() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  // Pinyin: the letters are the pre-edit text; choosing a candidate commits the characters.
  sendIme(&canvas, u"zhong"_s, underlined(5, 5));
  sendIme(&canvas, u"zhong'wen"_s, underlined(9, 9));
  QTRY_COMPARE(editableText(*fixture->session), u"zhong'wen"_s);
  saveScreenshot(*fixture, u"ime-chinese-pinyin"_s);
  sendIme(&canvas, QString(), {}, u"中文"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"中文"_s);
  saveScreenshot(*fixture, u"ime-chinese-committed"_s);
  QVERIFY(!canvas.composing());
}

void TestCanvas::koreanSyllablesCommitOneByOne() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  // 2-Set Korean: g k s → 한 is composed letter by letter; the next consonant commits it and starts the next syllable.
  sendIme(&canvas, u"ㅎ"_s, clause(0, 1, 0));
  sendIme(&canvas, u"하"_s, clause(0, 1, 0));
  sendIme(&canvas, u"한"_s, clause(0, 1, 0));
  QTRY_COMPARE(editableText(*fixture->session), u"한"_s);
  sendIme(&canvas, u"ㄱ"_s, clause(0, 1, 0), u"한"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"한ㄱ"_s);
  saveScreenshot(*fixture, u"ime-korean-composing"_s);
  sendIme(&canvas, u"그"_s, clause(0, 1, 0));
  sendIme(&canvas, u"글"_s, clause(0, 1, 0));
  sendIme(&canvas, QString(), {}, u"글"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"한글"_s);
  saveScreenshot(*fixture, u"ime-korean-committed"_s);
  QVERIFY(!canvas.composing());
}

void TestCanvas::cancelledCompositionLeavesTheText() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  typeText(fixture->window.get(), u"xy"_s);
  sendIme(&canvas, u"か"_s, underlined(1, 1));
  QTRY_COMPARE(editableText(*fixture->session), u"xyか"_s);
  // Escape in the input method: an empty pre-edit without a commit.
  sendIme(&canvas, QString(), {});
  QTRY_COMPARE(editableText(*fixture->session), u"xy"_s);
  QVERIFY(!canvas.composing());
}

void TestCanvas::accentMenuReplacesTheCharacterBeforeTheCaret() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  // macOS: hold "e", choose "é": the input method replaces the "e" before the caret.
  typeText(fixture->window.get(), u"caf"_s);
  typeText(fixture->window.get(), u"e"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"cafe"_s);
  sendIme(fixture->canvas, QString(), {}, u"é"_s, -1, 1);
  QTRY_COMPARE(editableText(*fixture->session), u"café"_s);
}

void TestCanvas::deletingAroundTheCaretStaysInTheEditableText() {
  // The counts come from the input method. Without editable text (the stub), or with none around the caret (the mock document's empty
  // line), the engine is not asked to delete anything.
  const auto fixture = openDocument();
  QVERIFY(fixture);
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  QTest::qWait(250); // Let a delayed view.set go first.
  const qint64 before = fixture->engine->lastPostedRequest();
  canvas.deleteAroundCaret(1'000'000, 1'000'000);
  QCOMPARE(fixture->engine->lastPostedRequest(), before);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    return;
  }
  // Replacements that reach beyond either end of the text delete up to that end.
  typeText(fixture->window.get(), u"abc"_s);
  QTest::keyClick(fixture->window.get(), Qt::Key_Left);
  QTRY_COMPARE(caretOffset(*fixture->session), 2);
  sendIme(&canvas, QString(), {}, u"X"_s, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
  QTRY_COMPARE(editableText(*fixture->session), u"Xc"_s);
  QTRY_COMPARE(caretOffset(*fixture->session), 1);
  sendIme(&canvas, QString(), {}, u"Y"_s, 0, std::numeric_limits<int>::max());
  QTRY_COMPARE(editableText(*fixture->session), u"XY"_s);
}

void TestCanvas::clickMovesTheCaret() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  typeText(fixture->window.get(), u"abcdef"_s);
  QTRY_COMPARE(caretOffset(*fixture->session), 6);
  // Click on the left part of the third character's box: the engine puts the caret before it.
  const auto *node = editableNode(fixture->session->accessibilityTree());
  QVERIFY(node != nullptr && node->chars.size() == 6 && node->bounds);
  const int page = node->bounds.value_or(bayan::protocol::PageRect{}).page;
  const QRectF box = itemRect(*fixture->canvas, bayan::protocol::PageRect{.page = page, .rect = node->chars.at(2).rect});
  QVERIFY(!box.isEmpty());
  const QPoint click(qRound(box.left() + (box.width() / 4)), qRound(box.center().y()));
  QTest::mouseClick(fixture->window.get(), Qt::LeftButton, Qt::NoModifier, fixture->canvas->mapToScene(click).toPoint());
  QTRY_COMPARE(caretOffset(*fixture->session), 2);
  typeText(fixture->window.get(), u"X"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"abXcdef"_s);
}

void TestCanvas::doubleClickIsTwoPressesAndTwoReleases() {
  // Works with either engine: every pointer message is a request, so the engine's request count tells how many the canvas sent.
  const auto fixture = openDocument();
  QVERIFY(fixture);
  QWindow *window = fixture->window.get();
  const QPoint point = fixture->canvas->mapToScene(QPointF(200, 200)).toPoint();
  // Let a delayed view.set go first; then nothing but the clicks sends messages.
  QTest::qWait(250);
  const qint64 before = fixture->engine->lastPostedRequest();
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
  QCOMPARE(fixture->engine->lastPostedRequest() - before, 2);
  // Qt Quick delivers a double click as press, release, press, double click, release: the engine gets two presses (the second with two
  // clicks) and two releases, not a third press without a release.
  QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, point);
  QCOMPARE(fixture->engine->lastPostedRequest() - before, 6);
}

void TestCanvas::inputMethodSeesTheCaretAndTheTextAroundIt() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  bayan::DocumentCanvas &canvas = *fixture->canvas;
  typeText(fixture->window.get(), u"hello"_s);
  QTRY_COMPARE(canvas.inputMethodQuery(Qt::ImSurroundingText).toString(), u"hello"_s);
  QVERIFY(canvas.inputMethodQuery(Qt::ImEnabled).toBool());
  QCOMPARE(canvas.inputMethodQuery(Qt::ImCursorPosition).toInt(), 5);
  QCOMPARE(canvas.inputMethodQuery(Qt::ImTextBeforeCursor).toString(), u"hello"_s);
  QCOMPARE(canvas.inputMethodQuery(Qt::ImTextAfterCursor).toString(), QString());
  // The cursor rectangle is the engine's caret, inside the editable line's box.
  const QRectF cursor = canvas.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
  const auto *node = editableNode(fixture->session->accessibilityTree());
  QVERIFY(node != nullptr);
  const QRectF frame = itemRect(canvas, node->bounds);
  QVERIFY(!frame.isEmpty() && !cursor.isEmpty());
  QVERIFY2(frame.contains(cursor.center()), "the input method's cursor rectangle is not in the editable line");
  // While composing, the input method sees the text from before the composition, and the cursor rectangle follows the composition.
  sendIme(&canvas, u"ねこ"_s, underlined(2, 2));
  QTRY_COMPARE(editableText(*fixture->session), u"helloねこ"_s);
  QCOMPARE(canvas.inputMethodQuery(Qt::ImSurroundingText).toString(), u"hello"_s);
  QCOMPARE(canvas.inputMethodQuery(Qt::ImCursorPosition).toInt(), 5);
  QTRY_VERIFY(canvas.inputMethodQuery(Qt::ImCursorRectangle).toRectF().left() > cursor.left());
  sendIme(&canvas, QString(), {}, u"猫"_s);
  QTRY_COMPARE(canvas.inputMethodQuery(Qt::ImSurroundingText).toString(), u"hello猫"_s);
  // Scrolling moves the cursor rectangle with the page.
  const QRectF before = canvas.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
  canvas.scrollBy(0, 40);
  QCOMPARE(canvas.inputMethodQuery(Qt::ImCursorRectangle).toRectF().top(), before.top() - 40);
}

void TestCanvas::accessibilityExposesTheDocumentText() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(fixture->canvas);
  QVERIFY(accessible != nullptr && accessible->isValid());
  QCOMPARE(accessible->role(), QAccessible::EditableText);
  QCOMPARE(accessible->text(QAccessible::Name), u"BayanDocs mock document"_s);
  const QAccessible::State state = accessible->state();
  QVERIFY(state.focusable && state.focused && state.editable && state.multiLine);
  QVERIFY(accessible->parent() != nullptr);
  QAccessibleTextInterface *text = accessible->textInterface();
  QVERIFY(text != nullptr && accessible->editableTextInterface() != nullptr);
  const QString all = text->text(0, text->characterCount());
  QCOMPARE(all, accessible->text(QAccessible::Value));
  // Reading order: the heading first, one node per line, the table's cells included.
  int start = -1;
  int end = -1;
  QCOMPARE(text->textAtOffset(0, QAccessible::LineBoundary, &start, &end).trimmed(), u"BayanDocs mock document"_s);
  QVERIFY(all.contains(u"The box below is the editable line."_s));
  QVERIFY(all.contains(u"\nTypeScript\n"_s));
  QVERIFY(all.endsWith(u"This is the end of the mock document."_s));
  // The caret is in the editable line, which is empty.
  const int caret = text->cursorPosition();
  QCOMPARE(text->textAtOffset(caret, QAccessible::LineBoundary, &start, &end).trimmed(), QString());
  // Characters have screen rectangles inside the canvas; the caret's lies in the editable line.
  const QRect canvasRect = accessible->rect();
  QVERIFY(canvasRect.contains(text->characterRect(0).center()));
  QVERIFY(canvasRect.contains(text->characterRect(caret).center()));
  QCOMPARE(text->offsetAtPoint(canvasRect.center()), -1);
}

void TestCanvas::accessibilityReportsTextAndCaretChanges() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(fixture->canvas);
  QVERIFY(accessible != nullptr);
  const int caret = accessible->textInterface()->cursorPosition();
  recordedEvents().clear();
  const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(&recordEvent);
  typeText(fixture->window.get(), u"ok"_s);
  QTRY_COMPARE(accessible->textInterface()->cursorPosition(), caret + 2);
  QAccessible::installUpdateHandler(previous);
  QString inserted;
  bool caretMoved = false;
  for (const RecordedEvent &event : recordedEvents()) {
    if (event.type == QAccessible::TextUpdated) {
      QCOMPARE(event.position, caret + static_cast<int>(inserted.size()));
      QVERIFY(event.removed.isEmpty());
      inserted += event.inserted;
    }
    caretMoved = caretMoved || (event.type == QAccessible::TextCaretMoved && event.position == caret + 2);
  }
  QCOMPARE(inserted, u"ok"_s);
  QVERIFY(caretMoved);
}

void TestCanvas::accessibleEditingGoesThroughTheEngine() {
  const auto fixture = openDocument();
  QVERIFY(fixture);
  if (!fixture->hasMockDocument()) {
    QVERIFY2(!mockDocumentRequired(), "the engine has no mock document");
    QSKIP("Needs the mock document of the real engine (BAYAN_ENGINE=sdk).");
  }
  QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(fixture->canvas);
  QVERIFY(accessible != nullptr);
  QAccessibleTextInterface *text = accessible->textInterface();
  QAccessibleEditableTextInterface *editable = accessible->editableTextInterface();
  const int caret = text->cursorPosition();
  editable->insertText(caret, u"dictated"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"dictated"_s);
  QTRY_COMPARE(text->cursorPosition(), caret + 8);
  editable->replaceText(caret + 5, caret + 8, u"ion"_s);
  QTRY_COMPARE(editableText(*fixture->session), u"dictaion"_s);
  editable->deleteText(caret + 5, caret + 8);
  QTRY_COMPARE(editableText(*fixture->session), u"dicta"_s);
  // Text elsewhere (the heading) cannot be edited, and the engine is not asked.
  editable->deleteText(0, 4);
  editable->insertText(0, u"nope"_s);
  QTest::qWait(100);
  QCOMPARE(editableText(*fixture->session), u"dicta"_s);
  QVERIFY(text->text(0, 9) == u"BayanDocs"_s);
}

void TestCanvas::destroyingTheWindowWhileTilesRenderIsSafe() {
  // The tile thread is busy rendering when the window and its canvas go away: the canvas stops the thread before the engine is destroyed.
  // Sanitizer builds turn any mistake here into a failure.
  for (int round = 0; round < 5; ++round) {
    auto engine = bayan::Engine::create();
    QVERIFY(engine);
    bayan::DocumentSession session(*engine);
    auto window = std::make_unique<QQuickWindow>();
    window->resize(800, 600);
    auto *canvas = new bayan::DocumentCanvas(window->contentItem()); // NOLINT(cppcoreguidelines-owning-memory): a child item.
    canvas->setSize(QSizeF(800, 600));
    canvas->setTileSize(64);
    canvas->setSession(&session);
    window->show();
    session.start();
    QTRY_VERIFY(session.status() == bayan::DocumentSession::Status::Ready);
    QTest::qWait(round * 5);
    window.reset();
  }
}

QTEST_MAIN(TestCanvas)
#include "tst_canvas.moc"
