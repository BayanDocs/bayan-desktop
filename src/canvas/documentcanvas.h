// The document canvas: the Qt Quick item that shows the engine's pages and takes the user's input (DESK-002, ADR-0013 §6).
//
// - Tiles: the canvas works out which tiles of which pages the view needs at the current zoom and screen density (TileGrid,
//   ViewGeometry), asks the tile thread to render them (TileRenderer), keeps them (TileCache) and puts them into Qt Quick's scene graph as
//   textures, each uploaded once and moved, not redrawn, while the view scrolls. Tiles land on whole device pixels, so they stay sharp at
//   fractional scaling on Windows and on Retina screens.
// - Zoom: while the zoom changes (pinch, Control+wheel, the Zoom in and Zoom out buttons), the canvas stretches the tiles it has, and once
//   the zoom has settled it asks for tiles at the new zoom, showing the stretched ones until the new ones arrive.
// - Overlays: the caret and the selection come from overlay.update and are drawn over the tiles; the caret blinks here, so blinking never
//   re-renders a page (architecture §8).
// - Input: keys, pointer events and input-method composition go to the engine as input messages (KeyMapping, ImeTranslator), and the
//   input method learns the caret's position and the text around it from the engine's overlay and accessibility tree. Every key but Tab
//   goes to the engine, with its modifiers: the shell binds none, so that it hides no Word shortcut (Control+= is subscript), and the
//   engine receives the same keys from both shells (engine protocol §6.5). The keyboard scrolls and zooms through the document area
//   instead (DocumentArea.qml and Main.qml, with scrollWithKey, zoomIn and zoomOut), so both stay keyboard-operable (ADR-0020 §5).
// - Accessibility: CanvasAccessible presents the engine's accessibility tree to screen readers; the canvas keeps its text (AccessibleText)
//   and reports changes of the text and the caret.
//
// The canvas decides nothing about the document: where pages and the caret go, what the pixels are and what a key does are the engine's.

#pragma once

#include "accessibletext.h"
#include "canvasnode.h"
#include "documentsession.h"
#include "imetranslator.h"
#include "tilecache.h"
#include "tilegrid.h"
#include "tilerenderer.h"
#include "viewgeometry.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantAnimation>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtQml/qqmlregistration.h>
#include <QtQuick/QQuickItem>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bayan {

// Measurements the canvas collects for the benchmark (DESK-002 AC-1). Sample lists stop growing at kMaxSamples.
struct CanvasStatistics {
  static constexpr std::size_t kMaxSamples = 100'000;
  // Milliseconds from asking for a tile to the tile thread starting it, and from starting it to the engine's answer.
  std::vector<double> tileWaitMs;
  std::vector<double> tileRenderMs;
  // Milliseconds from the first frame that needed a tile on screen (missing, or out of date after a change) to the first frame that showed
  // its new pixels: how long part of the view waited. Tiles rendered ahead, before they scrolled into view, count from when they came into
  // view, so they add samples near one frame.
  std::vector<double> tileShownMs;
  // Frames prepared; of those, frames in which a visible tile showed a stretched tile of another zoom, or nothing at all.
  qint64 frames = 0;
  qint64 framesWithStandIns = 0;
  qint64 framesWithHoles = 0;
  qint64 tilesRendered = 0;
  qint64 tilesRefused = 0;
  qint64 textureUploads = 0;
  qint64 textureUploadBytes = 0;
};

// Not final: QML creates it through a subclass of its own (QQmlPrivate::QQmlElement).
class DocumentCanvas : public QQuickItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(DocumentCanvas)
  Q_PROPERTY(bayan::DocumentSession *session READ session WRITE setSession NOTIFY sessionChanged FINAL)
  Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
  Q_PROPERTY(double contentX READ contentX WRITE setContentX NOTIFY contentPositionChanged FINAL)
  Q_PROPERTY(double contentY READ contentY WRITE setContentY NOTIFY contentPositionChanged FINAL)
  Q_PROPERTY(double contentWidth READ contentWidth NOTIFY contentSizeChanged FINAL)
  Q_PROPERTY(double contentHeight READ contentHeight NOTIFY contentSizeChanged FINAL)
  Q_PROPERTY(bool tilesComplete READ tilesComplete NOTIFY tilesCompleteChanged FINAL)
  Q_PROPERTY(bool canZoomIn READ canZoomIn NOTIFY zoomChanged FINAL)
  Q_PROPERTY(bool canZoomOut READ canZoomOut NOTIFY zoomChanged FINAL)
  Q_PROPERTY(QColor pageShadowColor READ pageShadowColor WRITE setPageShadowColor NOTIFY colorsChanged FINAL)
  Q_PROPERTY(QColor pagePlaceholderColor READ pagePlaceholderColor WRITE setPagePlaceholderColor NOTIFY colorsChanged FINAL)
  Q_PROPERTY(QColor caretColor READ caretColor WRITE setCaretColor NOTIFY colorsChanged FINAL)
  Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY colorsChanged FINAL)

public:
  // Zoom from 10 % to 500 %, as Word offers (the engine accepts 5 % to 6,400 %).
  static constexpr double kMinimumZoom = 0.1;
  static constexpr double kMaximumZoom = 5.0;
  // The zooms that zoomIn and zoomOut step through: the web shell's zoom steps (bayan-web's document-controller.ts), and the canvas's
  // smallest zoom, 10 %, below them.
  static constexpr std::array kZoomSteps{0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0};
  // How far the arrow keys scroll the document area, in logical pixels.
  static constexpr double kArrowScrollStep = 40.0;

  explicit DocumentCanvas(QQuickItem *parent = nullptr);
  // Stops the tile thread. The session and its engine must outlive the canvas, or the canvas's session must be cleared first.
  ~DocumentCanvas() override;
  Q_DISABLE_COPY_MOVE(DocumentCanvas)

  [[nodiscard]] DocumentSession *session() const { return session_; }
  void setSession(DocumentSession *session);
  [[nodiscard]] double zoom() const { return zoom_; }
  // Zooms around the canvas's centre.
  void setZoom(double zoom);
  [[nodiscard]] double contentX() const { return contentPosition_.x(); }
  void setContentX(double x) { setContentPosition({x, contentPosition_.y()}); }
  [[nodiscard]] double contentY() const { return contentPosition_.y(); }
  void setContentY(double y) { setContentPosition({contentPosition_.x(), y}); }
  void setContentPosition(QPointF position);
  [[nodiscard]] double contentWidth() const { return contentSize_.width(); }
  [[nodiscard]] double contentHeight() const { return contentSize_.height(); }
  // Whether the last frame showed every visible tile at the current zoom, up to date: nothing missing, stretched or waiting.
  [[nodiscard]] bool tilesComplete() const { return tilesComplete_.load(std::memory_order_relaxed); }

  [[nodiscard]] QColor pageShadowColor() const { return pageShadowColor_; }
  void setPageShadowColor(const QColor &color);
  [[nodiscard]] QColor pagePlaceholderColor() const { return pagePlaceholderColor_; }
  void setPagePlaceholderColor(const QColor &color);
  [[nodiscard]] QColor caretColor() const { return caretColor_; }
  void setCaretColor(const QColor &color);
  [[nodiscard]] QColor selectionColor() const { return selectionColor_; }
  void setSelectionColor(const QColor &color);

  // Zooms so that the document point under itemPoint stays under it (pinch, Control+wheel).
  Q_INVOKABLE void zoomAt(double zoom, QPointF itemPoint);
  // Zooms to the next step of kZoomSteps above or below the current zoom, around the canvas's centre (the Zoom in and Zoom out buttons).
  Q_INVOKABLE void zoomIn();
  Q_INVOKABLE void zoomOut();
  [[nodiscard]] bool canZoomIn() const { return zoom_ < kMaximumZoom; }
  [[nodiscard]] bool canZoomOut() const { return zoom_ > kMinimumZoom; }
  // Asks for tiles at the current zoom now instead of after the zoom has settled, for example at the end of a pinch.
  Q_INVOKABLE void commitZoom();
  Q_INVOKABLE void scrollBy(double dx, double dy) { setContentPosition(contentPosition_ + QPointF(dx, dy)); }
  // Scrolls with a short animation, as for a mouse wheel's notches and the Page Up and Page Down keys.
  void scrollSmoothlyBy(QPointF delta);
  // Scrolls for a key pressed while the document area, the "Pages" item (DocumentArea.qml), has the keyboard focus, as a browser scrolls a
  // focused scroller: Page Up and Page Down by most of the view, Home and End (also with Control) to the top and the bottom, and the arrow
  // keys by kArrowScrollStep. key and modifiers are Qt::Key and Qt::KeyboardModifiers values. Returns whether the key was one of those.
  Q_INVOKABLE bool scrollWithKey(int key, int modifiers);

  // The current view: zoom, scroll position and where the pages are.
  [[nodiscard]] ViewGeometry geometry() const;
  // A rectangle of a page (BLU) in the canvas's coordinates, or nothing if there is no such page.
  [[nodiscard]] std::optional<QRectF> itemRectFor(int page, const protocol::BluRect &rect) const;
  // The caret of overlay.update in the canvas's coordinates, whether it blinks on or off right now.
  [[nodiscard]] std::optional<QRectF> caretItemRect() const;
  // Scrolls just enough to show a rectangle given in the canvas's coordinates.
  void revealItemRect(const QRectF &rect);

  // For the accessibility bridge (CanvasAccessible).
  [[nodiscard]] const AccessibleText &accessibleText() const { return accessibleText_; }
  // The accessibility caret as an offset of accessibleText(), or 0 if the engine reported none.
  [[nodiscard]] int accessibleCursorPosition() const;
  // Set when an accessibility interface for the canvas exists, which means a screen reader or a test is looking; until then the canvas
  // does not spend time reporting text changes.
  void setAccessibleInterfaceActive(bool active) { accessibleInterfaceActive_ = active; }
  // Deletes text next to the caret, counted in UTF-16 code units, with Backspace and Delete keys: protocol v0 has no message that deletes
  // a range, so input methods and accessibility edits that replace text use this (see ImeTranslator). Deletes no more than the editable
  // text at the caret holds, and nothing without one.
  void deleteAroundCaret(int unitsBefore, int unitsAfter);

  // Settings and figures for the benchmark and the tests.
  void setTileSize(int devicePixels);
  [[nodiscard]] int tileSize() const { return tileSize_; }
  void setTileCacheBudget(qint64 bytes);
  [[nodiscard]] const TileCache &tileCache() const { return cache_; }
  [[nodiscard]] const CanvasStatistics &statistics() const { return statistics_; }
  void resetStatistics() { statistics_ = {}; }
  [[nodiscard]] bool composing() const { return ime_.composing(); }
  // How the canvas asks the platform's input method to end a composition: QInputMethod::commit() and reset() (see finishComposition).
  // Tests replace them to play a platform's input context, which the offscreen platform lacks.
  struct InputMethodControl {
    std::function<void()> commit;
    std::function<void()> reset;
  };
  [[nodiscard]] static InputMethodControl platformInputMethodControl();
  void setInputMethodControl(InputMethodControl control) { inputMethodControl_ = std::move(control); }
  // What the input method asks: the caret's rectangle, the text around it and whether text can be typed here (public in QQuickItem).
  [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
  void sessionChanged();
  void zoomChanged();
  void contentPositionChanged();
  void contentSizeChanged();
  void tilesCompleteChanged();
  void colorsChanged();

protected:
  QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
  void updatePolish() override;
  void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
  void itemChange(ItemChange change, const ItemChangeData &value) override;
  void keyPressEvent(QKeyEvent *event) override;
  void inputMethodEvent(QInputMethodEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseUngrabEvent() override;
  void wheelEvent(QWheelEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;

private:
  using Clock = std::chrono::steady_clock;

  // The text and caret offset the input method sees.
  struct EditContext {
    QString text{};
    int cursor = 0;
  };

  void onPagesChanged();
  void onInvalidated(const std::vector<protocol::PageRect> &regions, qint64 cause);
  void onOverlayChanged();
  void onAccessibilityChanged();
  void onTileRendered(const TileRenderer::Result &result);
  // Starts a new tile thread for the session's engine, if there is a session, after stopping the old one.
  void startRenderer();
  void updateContentSize();
  void updateDevicePixelRatio();
  // The view, the tiles or the pages changed: marks the tiles incomplete until the next frame and asks for that frame.
  void viewChanged();
  // Tells the session what the canvas shows, after a short delay that lets resizing and zooming settle.
  void scheduleView();
  // Sends a pending view.set at once: the engine converts pointer positions with the zoom of the last one.
  void flushView();
  void sendView();
  void restartCaretBlink();
  void updateInputMethod(Qt::InputMethodQueries queries) const;
  [[nodiscard]] std::optional<EditContext> editContext() const;
  void pointerPressed(QMouseEvent *event);
  void sendPointer(protocol::PointerKind kind, const QMouseEvent &event);
  void performImeAction(const ImeAction &action);
  // Commits an unfinished composition, for example when the canvas loses focus or the user clicks.
  void finishComposition();
  // What updatePolish works out for a frame, step by step (defined in documentcanvas.cpp).
  struct FramePlan;
  // The tiles of one page that the frame shows or wants rendered ahead.
  void planPage(const protocol::Page &page, FramePlan &plan);
  // One tile: wanted and rendered if needed, and shown (or covered by stand-ins) if visible.
  void planTile(const protocol::Page &page, const TileKey &key, bool visible, FramePlan &plan);
  [[nodiscard]] TileRenderer::Job renderJob(const protocol::Page &page, const TileGrid &grid, const TileKey &key, Clock::time_point now);
  // The scales of the other zooms the cache holds tiles of, closest to scale first, for stand-ins.
  [[nodiscard]] std::vector<double> standInScales(double scale) const;
  // Adds stretched tiles of other zooms that cover a missing tile, from the closest zoom that has them (TileCache::standInsFor); returns
  // whether any was found.
  bool addStandIns(const protocol::Page &page, const TileKey &missing, FramePlan &plan);
  void planOverlays(const ViewGeometry &view);
  // Reports whether the frame showed every tile it should, after the uploads of updatePaintNode.
  void reportCompleteness(bool complete);
  void recordShown(const TileKey &key);

  QPointer<DocumentSession> session_;
  std::unique_ptr<TileRenderer> renderer_;
  TileCache cache_;
  std::vector<protocol::Page> tilePages_;
  int tileSize_ = TileGrid::kDefaultTileSize;
  // The zoom the user sees, and the zoom of the tiles asked for (equal once the zoom has settled).
  double zoom_ = 1.0;
  double tileZoom_ = 1.0;
  double devicePixelRatio_ = 1.0;
  QPointF contentPosition_;
  QSizeF contentSize_;
  QTimer zoomSettle_;
  QTimer viewTimer_;
  QTimer caretBlink_;
  bool caretOn_ = true;
  QVariantAnimation scrollAnimation_;
  QPointF scrollTarget_;

  // Prepared by updatePolish on the GUI thread, read by updatePaintNode while the GUI thread waits.
  std::uint64_t frame_ = 0;
  std::vector<CanvasTile> displayTiles_;
  std::vector<QRectF> displayPages_;
  std::vector<QRectF> displaySelection_;
  std::optional<QRectF> displayCaret_;
  bool completeCandidate_ = false;
  std::atomic<bool> tilesComplete_{false};

  QColor pageShadowColor_{0, 0, 0, 51};
  QColor pagePlaceholderColor_{250, 250, 250};
  QColor caretColor_{0, 0, 0};
  QColor selectionColor_{51, 153, 255, 90};

  // When each tile on its way was first asked for, for CanvasStatistics::tileWaitMs, and when each visible tile that is missing or out of
  // date was first needed on screen, for CanvasStatistics::tileShownMs.
  std::map<TileKey, Clock::time_point> wantedSince_;
  std::map<TileKey, Clock::time_point> visibleSince_;
  CanvasStatistics statistics_;

  ImeTranslator ime_;
  InputMethodControl inputMethodControl_ = platformInputMethodControl();
  // The text around the caret when the current composition started; the input method sees it, without the composition, until it ends.
  std::optional<EditContext> compositionContext_;
  AccessibleText accessibleText_;
  int accessibleCursor_ = 0;
  bool accessibleInterfaceActive_ = false;

  // Pointer state: the buttons the engine was told about, and the click count.
  Qt::MouseButtons pressedButtons_;
  int clicks_ = 0;
  Clock::time_point lastPress_;
  QPointF lastPressPosition_;
  Qt::MouseButton lastPressButton_ = Qt::NoButton;
};

} // namespace bayan
