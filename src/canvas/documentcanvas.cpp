#include "documentcanvas.h"

#include "canvasaccessible.h"
#include "keymapping.h"

#include <QtCore/QMetaObject>
#include <QtGui/QAccessible>
#include <QtGui/QFocusEvent>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QPointingDevice>
#include <QtGui/QStyleHints>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGNode>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// How far beyond the visible area tiles are rendered ahead, as a share of the canvas's height, so that scrolling finds them ready.
constexpr double kPrefetchShare = 0.5;
// Textures uploaded per frame at most; more would make a frame late. The rest follow in the next frames.
constexpr int kMaxUploadsPerFrame = 8;
// Logical pixels scrolled per line of a mouse wheel notch.
constexpr double kWheelLineStep = 20.0;
// Zoom factor per mouse wheel notch with Control held, and per press of Control+plus or minus.
constexpr double kZoomStep = 1.1;
constexpr double kKeyZoomStep = 1.25;
// The wheel's standard step (QWheelEvent::angleDelta for one notch).
constexpr double kWheelNotch = 120.0;
// Space kept around something revealed for a screen reader.
constexpr double kRevealMargin = 24.0;

double milliseconds(std::chrono::steady_clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

void addSample(std::vector<double> &samples, double value) {
  if (samples.size() < CanvasStatistics::kMaxSamples) {
    samples.push_back(value);
  }
}

protocol::PointerType pointerTypeOf(const QMouseEvent &event) {
  const QPointingDevice *device = event.pointingDevice();
  if (device == nullptr) {
    return protocol::PointerType::Mouse;
  }
  switch (device->type()) {
  case QInputDevice::DeviceType::Stylus:
  case QInputDevice::DeviceType::Airbrush:
  case QInputDevice::DeviceType::Puck:
    return protocol::PointerType::Pen;
  case QInputDevice::DeviceType::TouchScreen:
    return protocol::PointerType::Touch;
  default:
    return protocol::PointerType::Mouse;
  }
}

// The DOM's button numbers (engine protocol §6.5): 0 primary, 1 auxiliary, 2 secondary, 3 back, 4 forward.
int domButton(Qt::MouseButton button) {
  switch (button) {
  case Qt::LeftButton:
    return 0;
  case Qt::MiddleButton:
    return 1;
  case Qt::RightButton:
    return 2;
  case Qt::BackButton:
    return 3;
  case Qt::ForwardButton:
    return 4;
  default:
    return 5;
  }
}

// The number of characters (Unicode code points) in a range of UTF-16 text.
int characterCount(QStringView text) { return static_cast<int>(text.toUcs4().size()); }

} // namespace

DocumentCanvas::DocumentCanvas(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents);
  setFlag(ItemAcceptsInputMethod);
  setClip(true);
  setAcceptedMouseButtons(Qt::AllButtons);
  setActiveFocusOnTab(true);
  installCanvasAccessibleFactory();

  zoomSettle_.setSingleShot(true);
  zoomSettle_.setInterval(150ms);
  connect(&zoomSettle_, &QTimer::timeout, this, &DocumentCanvas::commitZoom);
  viewTimer_.setSingleShot(true);
  viewTimer_.setInterval(100ms);
  connect(&viewTimer_, &QTimer::timeout, this, &DocumentCanvas::sendView);
  connect(&caretBlink_, &QTimer::timeout, this, [this] {
    caretOn_ = !caretOn_;
    polish();
  });
  scrollAnimation_.setDuration(120);
  scrollAnimation_.setEasingCurve(QEasingCurve::OutCubic);
  connect(&scrollAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) { setContentPosition(value.toPointF()); });
  // A canvas created with a parent item is already in its window, and the ItemSceneChange that said so reached QQuickItem::itemChange, not
  // this class's, because it was sent from QQuickItem's constructor. Without this, a canvas made in C++ in a window of 125 % would draw
  // tiles of 100 % stretched until the next change of screen.
  updateDevicePixelRatio();
}

DocumentCanvas::~DocumentCanvas() {
  // The tile thread uses the engine; stop it before anything else goes.
  renderer_.reset();
}

void DocumentCanvas::setSession(DocumentSession *session) {
  if (session == session_) {
    return;
  }
  if (session_ != nullptr) {
    disconnect(session_, nullptr, this, nullptr);
  }
  renderer_.reset();
  // The tiles, and the changes remembered for them, belong to the old session's engine; another engine numbers its requests from 1 again.
  cache_.reset();
  wantedSince_.clear();
  visibleSince_.clear();
  tilePages_.clear();
  ime_.reset();
  compositionContext_.reset();
  session_ = session;
  if (session_ != nullptr) {
    startRenderer();
    connect(session_, &DocumentSession::pagesChanged, this, &DocumentCanvas::onPagesChanged);
    connect(session_, &DocumentSession::invalidated, this, &DocumentCanvas::onInvalidated);
    connect(session_, &DocumentSession::overlayChanged, this, &DocumentCanvas::onOverlayChanged);
    connect(session_, &DocumentSession::accessibilityChanged, this, &DocumentCanvas::onAccessibilityChanged);
    connect(session_, &DocumentSession::statusChanged, this, [this] { viewChanged(); });
    // A session destroyed first takes its engine's use with it: stop the tile thread at once (session_ is already null by then).
    connect(session_, &QObject::destroyed, this, [this] {
      renderer_.reset();
      cache_.reset();
      wantedSince_.clear();
      visibleSince_.clear();
      viewChanged();
      emit sessionChanged();
    });
    sendView();
  }
  onPagesChanged();
  onAccessibilityChanged();
  emit sessionChanged();
}

void DocumentCanvas::startRenderer() {
  // Destroying the old tile thread first also drops the tiles it rendered that have not reached onTileRendered yet.
  renderer_.reset();
  if (session_ != nullptr) {
    renderer_ = std::make_unique<TileRenderer>(session_->engine());
    connect(renderer_.get(), &TileRenderer::tileRendered, this, &DocumentCanvas::onTileRendered);
  }
}

ViewGeometry DocumentCanvas::geometry() const {
  static const std::vector<protocol::Page> noPages;
  const QPointF sceneOffset = window() != nullptr ? mapToScene(QPointF(0, 0)) : QPointF();
  return ViewGeometry(session_ != nullptr ? session_->pages() : noPages, ViewState{.zoom = zoom_,
                                                                                   .devicePixelRatio = devicePixelRatio_,
                                                                                   .viewportSize = size(),
                                                                                   .contentPosition = contentPosition_,
                                                                                   .sceneOffset = sceneOffset});
}

void DocumentCanvas::setContentPosition(QPointF position) {
  const QPointF clamped = geometry().clamped(position);
  if (clamped == contentPosition_) {
    return;
  }
  contentPosition_ = clamped;
  emit contentPositionChanged();
  viewChanged();
  updateInputMethod(Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
}

void DocumentCanvas::updateContentSize() {
  const QSizeF size = geometry().contentSize();
  if (size != contentSize_) {
    contentSize_ = size;
    emit contentSizeChanged();
  }
  // A smaller content (zooming out) may have moved the largest scroll position below the current one.
  setContentPosition(contentPosition_);
}

void DocumentCanvas::setZoom(double zoom) { zoomAt(zoom, QPointF(width() / 2, height() / 2)); }

void DocumentCanvas::zoomAt(double zoom, QPointF itemPoint) {
  zoom = std::isfinite(zoom) ? std::clamp(zoom, kMinimumZoom, kMaximumZoom) : zoom_;
  if (zoom == zoom_) {
    return;
  }
  const ViewGeometry before = geometry();
  const QPointF documentPoint = before.viewSpacePoint(itemPoint);
  const QPointF position = before.contentPositionKeeping(documentPoint, itemPoint, zoom);
  zoom_ = zoom;
  updateContentSize();
  contentPosition_ = geometry().clamped(position);
  emit zoomChanged();
  emit contentPositionChanged();
  // New tiles once the zoom has settled; until then the current ones are stretched.
  zoomSettle_.start();
  viewChanged();
  updateInputMethod(Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImFont);
}

void DocumentCanvas::commitZoom() {
  zoomSettle_.stop();
  if (tileZoom_ != zoom_) {
    tileZoom_ = zoom_;
    scheduleView();
  }
  viewChanged();
}

void DocumentCanvas::scrollSmoothlyBy(QPointF delta) {
  const bool running = scrollAnimation_.state() == QAbstractAnimation::Running;
  const QPointF target = geometry().clamped((running ? scrollTarget_ : contentPosition_) + delta);
  scrollAnimation_.stop();
  scrollTarget_ = target;
  scrollAnimation_.setStartValue(contentPosition_);
  scrollAnimation_.setEndValue(target);
  scrollAnimation_.start();
}

void DocumentCanvas::setPageShadowColor(const QColor &color) {
  if (color != pageShadowColor_) {
    pageShadowColor_ = color;
    emit colorsChanged();
    update();
  }
}

void DocumentCanvas::setPagePlaceholderColor(const QColor &color) {
  if (color != pagePlaceholderColor_) {
    pagePlaceholderColor_ = color;
    emit colorsChanged();
    update();
  }
}

void DocumentCanvas::setCaretColor(const QColor &color) {
  if (color != caretColor_) {
    caretColor_ = color;
    emit colorsChanged();
    update();
  }
}

void DocumentCanvas::setSelectionColor(const QColor &color) {
  if (color != selectionColor_) {
    selectionColor_ = color;
    emit colorsChanged();
    update();
  }
}

void DocumentCanvas::setTileSize(int devicePixels) {
  const int size = std::clamp(devicePixels, TileGrid::kMinimumTileSize, TileGrid::kMaximumTileSize);
  if (size != tileSize_) {
    tileSize_ = size;
    // Tiles of another size do not fit the new grid. A tile of the old grid that is still being rendered may have the key and the size of a
    // new tile while it shows another part of the page: a new tile thread never delivers it.
    startRenderer();
    cache_.clear();
    wantedSince_.clear();
    visibleSince_.clear();
    viewChanged();
  }
}

void DocumentCanvas::setTileCacheBudget(qint64 bytes) {
  // Takes effect with the next frame's eviction; the tiles and the changes remembered for those being rendered stay.
  cache_.setBudget(bytes);
  polish();
}

std::optional<QRectF> DocumentCanvas::itemRectFor(int page, const protocol::BluRect &rect) const {
  if (session_ == nullptr || page < 0 || std::cmp_greater_equal(page, session_->pages().size())) {
    return std::nullopt;
  }
  return geometry().itemRect(session_->pages().at(static_cast<std::size_t>(page)), rect);
}

std::optional<QRectF> DocumentCanvas::caretItemRect() const {
  if (session_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<protocol::Caret> &caret = session_->overlay().caret;
  return caret ? itemRectFor(caret->page, caret->rect) : std::nullopt;
}

void DocumentCanvas::revealItemRect(const QRectF &rect) {
  QPointF delta;
  if (rect.left() < kRevealMargin) {
    delta.rx() = rect.left() - kRevealMargin;
  } else if (rect.right() > width() - kRevealMargin) {
    delta.rx() = std::min(rect.right() - width() + kRevealMargin, rect.left() - kRevealMargin);
  }
  if (rect.top() < kRevealMargin) {
    delta.ry() = rect.top() - kRevealMargin;
  } else if (rect.bottom() > height() - kRevealMargin) {
    delta.ry() = std::min(rect.bottom() - height() + kRevealMargin, rect.top() - kRevealMargin);
  }
  if (!delta.isNull()) {
    setContentPosition(contentPosition_ + delta);
  }
}

int DocumentCanvas::accessibleCursorPosition() const { return accessibleCursor_; }

void DocumentCanvas::deleteAroundCaret(int unitsBefore, int unitsAfter) {
  // The engine deletes one character per key: count the characters the code units hold, in the editable text around the caret. The counts
  // come from the input method, so only the text there is can be deleted; without editable text at the caret, nothing can.
  const auto context = session_ != nullptr ? editContext() : std::nullopt;
  if (!context) {
    return;
  }
  const QStringView text(context->text);
  const qsizetype cursor = context->cursor;
  const qsizetype start = cursor - std::clamp<qsizetype>(unitsBefore, 0, cursor);
  const qsizetype end = cursor + std::clamp<qsizetype>(unitsAfter, 0, text.size() - cursor);
  const int before = characterCount(text.sliced(start, cursor - start));
  const int after = characterCount(text.sliced(cursor, end - cursor));
  for (int i = 0; i < before; ++i) {
    session_->sendKey({.key = u"Backspace"_s, .code = u"Backspace"_s});
  }
  for (int i = 0; i < after; ++i) {
    session_->sendKey({.key = u"Delete"_s, .code = u"Delete"_s});
  }
}

void DocumentCanvas::onPagesChanged() {
  const std::vector<protocol::Page> pages = session_ != nullptr ? session_->pages() : std::vector<protocol::Page>{};
  if (pages != tilePages_) {
    // Tiles are cut from the pages' sizes; new pages need new tiles.
    tilePages_ = pages;
    cache_.clear();
    wantedSince_.clear();
    visibleSince_.clear();
  }
  updateContentSize();
  scheduleView();
  viewChanged();
}

void DocumentCanvas::onInvalidated(const std::vector<protocol::PageRect> &regions, qint64 cause) {
  std::map<int, std::vector<protocol::BluRect>> perPage;
  for (const protocol::PageRect &region : regions) {
    perPage[region.page].push_back(region.rect);
  }
  for (const auto &[page, rects] : perPage) {
    cache_.invalidate(page, rects, cause);
  }
  viewChanged();
}

void DocumentCanvas::onOverlayChanged() {
  restartCaretBlink();
  polish();
  updateInputMethod(Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImFont);
}

void DocumentCanvas::onAccessibilityChanged() {
  const QString previousText = accessibleText_.text();
  const QString previousName = accessibleText_.name();
  const int previousCursor = accessibleCursor_;
  accessibleText_ =
      session_ != nullptr && session_->accessibilityTree() ? AccessibleText(*session_->accessibilityTree()) : AccessibleText();
  accessibleCursor_ = 0;
  if (const auto caret = session_ != nullptr ? session_->accessibilityCaret() : std::nullopt) {
    accessibleCursor_ = accessibleText_.offsetOf(caret->node, caret->offset).value_or(0);
  }
  if (accessibleInterfaceActive_) {
    // Tell the platform's screen readers what changed: the text first, then the caret, as text editors do.
    if (accessibleText_.name() != previousName) {
      QAccessibleEvent event(this, QAccessible::NameChanged);
      QAccessible::updateAccessibility(&event);
    }
    if (accessibleText_.text() != previousText) {
      const AccessibleText::Change change = AccessibleText::difference(previousText, accessibleText_.text());
      QAccessibleTextUpdateEvent event(this, change.position, change.removed, change.inserted);
      QAccessible::updateAccessibility(&event);
    }
    if (accessibleCursor_ != previousCursor || accessibleText_.text() != previousText) {
      QAccessibleTextCursorEvent event(this, accessibleCursor_);
      QAccessible::updateAccessibility(&event);
    }
  }
  if (!ime_.composing()) {
    // During a composition the input method keeps seeing the text from before it started.
    updateInputMethod(Qt::ImEnabled | Qt::ImReadOnly | Qt::ImSurroundingText | Qt::ImCursorPosition | Qt::ImAnchorPosition |
                      Qt::ImAbsolutePosition | Qt::ImTextBeforeCursor | Qt::ImTextAfterCursor);
  }
}

void DocumentCanvas::onTileRendered(const TileRenderer::Result &result) {
  addSample(statistics_.tileWaitMs, milliseconds(result.started - result.wanted));
  addSample(statistics_.tileRenderMs, milliseconds(result.finished - result.started));
  if (result.image.isNull()) {
    ++statistics_.tilesRefused;
  } else {
    ++statistics_.tilesRendered;
  }
  cache_.deliver(result.key, result.image, result.renderedAfter);
  wantedSince_.erase(result.key);
  polish();
}

void DocumentCanvas::recordShown(const TileKey &key) {
  if (const auto found = visibleSince_.find(key); found != visibleSince_.end()) {
    addSample(statistics_.tileShownMs, milliseconds(Clock::now() - found->second));
    visibleSince_.erase(found);
  }
}

void DocumentCanvas::updateDevicePixelRatio() {
  const double ratio = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
  if (ratio > 0.0 && ratio != devicePixelRatio_) {
    // A new screen density needs tiles of the new scale; the old ones are stretched until they arrive.
    devicePixelRatio_ = ratio;
    scheduleView();
    viewChanged();
  }
}

void DocumentCanvas::viewChanged() {
  // What the last frame showed no longer says whether the next one will be complete: until that frame has been prepared, it is not.
  if (tilesComplete_.exchange(false, std::memory_order_relaxed)) {
    emit tilesCompleteChanged();
  }
  polish();
}

void DocumentCanvas::scheduleView() {
  if (session_ != nullptr) {
    viewTimer_.start();
  }
}

void DocumentCanvas::flushView() {
  if (tileZoom_ != zoom_) {
    commitZoom();
  }
  if (viewTimer_.isActive()) {
    sendView();
  }
}

void DocumentCanvas::sendView() {
  viewTimer_.stop();
  if (session_ == nullptr) {
    return;
  }
  // The engine converts pointer positions with this zoom, and lays out what the viewport shows first (engine protocol §6.2). Scrolling
  // alone does not send view.set: the v0 engine answers every view.set by invalidating every page (the DESK-002 report discusses this).
  const ViewGeometry view = geometry();
  session_->setView({.viewport = view.viewport(), .zoom = tileZoom_, .deviceScale = devicePixelRatio_});
}

void DocumentCanvas::restartCaretBlink() {
  caretOn_ = true;
  const int flashTime = QGuiApplication::styleHints()->cursorFlashTime();
  if (hasActiveFocus() && flashTime > 0) {
    caretBlink_.start(flashTime / 2);
  } else {
    caretBlink_.stop();
  }
}

void DocumentCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    updateContentSize();
    scheduleView();
  }
  viewChanged();
  updateInputMethod(Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImInputItemClipRectangle);
}

void DocumentCanvas::itemChange(ItemChange change, const ItemChangeData &value) {
  QQuickItem::itemChange(change, value);
  if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
    updateDevicePixelRatio();
  }
}

// What updatePolish works out for a frame. The tiles to show and the overlays go straight into the members that updatePaintNode reads.
struct DocumentCanvas::FramePlan {
  ViewGeometry view;
  // The grid of the tiles asked for (the current zoom once it has settled), and how far ahead tiles are wanted, in logical pixels.
  TileGrid grid;
  double prefetch = 0.0;
  QRectF visibleArea;
  QRectF wantedArea;
  std::vector<double> otherScales;
  Clock::time_point now;
  std::vector<TileRenderer::Job> visibleJobs;
  std::vector<TileRenderer::Job> prefetchJobs;
  // Stretched tiles of other zooms, with the rank of their zoom (0: the closest).
  std::vector<std::pair<std::size_t, CanvasTile>> standIns;
  // The visible tiles of the current grid.
  std::vector<TileKey> visible;
  // Whether every visible tile is shown, fresh, at the current zoom; and whether stand-ins or holes were needed.
  bool complete = true;
  bool stretched = false;
  bool holes = false;
};

void DocumentCanvas::updatePolish() {
  ++frame_;
  displayTiles_.clear();
  displayPages_.clear();
  displaySelection_.clear();
  displayCaret_.reset();
  completeCandidate_ = false;
  if (session_ == nullptr || renderer_ == nullptr || session_->pages().empty() || width() <= 0 || height() <= 0) {
    update();
    return;
  }
  const double prefetch = height() * kPrefetchShare;
  const QRectF visibleArea(0, 0, width(), height());
  const TileGrid grid(tileZoom_ * devicePixelRatio_, tileSize_);
  FramePlan plan{.view = geometry(),
                 .grid = grid,
                 .prefetch = prefetch,
                 .visibleArea = visibleArea,
                 .wantedArea = visibleArea.adjusted(-prefetch, -prefetch, prefetch, prefetch),
                 .otherScales = standInScales(grid.scale()),
                 .now = Clock::now(),
                 .visibleJobs = {},
                 .prefetchJobs = {},
                 .standIns = {},
                 .visible = {},
                 .complete = tileZoom_ == zoom_,
                 .stretched = false,
                 .holes = false};
  for (const protocol::Page &page : session_->pages()) {
    planPage(page, plan);
  }
  // Stand-ins of different zooms overlap. Drawn from the farthest zoom to the closest, the sharpest end up on top.
  std::ranges::stable_sort(plan.standIns, std::ranges::greater{}, [](const auto &standIn) { return standIn.first; });
  for (auto &standIn : plan.standIns) {
    displayTiles_.push_back(std::move(standIn.second));
  }
  // Visible tiles first, then the ones around them; within each, the order of the pages and rows, top to bottom.
  plan.visibleJobs.insert(plan.visibleJobs.end(), std::make_move_iterator(plan.prefetchJobs.begin()),
                          std::make_move_iterator(plan.prefetchJobs.end()));
  // Tiles no longer asked for are not waiting; if they are asked for again, they wait from then.
  std::erase_if(wantedSince_, [&jobs = plan.visibleJobs](const auto &item) {
    return std::ranges::find(jobs, item.first, &TileRenderer::Job::key) == jobs.end();
  });
  renderer_->setJobs(std::move(plan.visibleJobs));
  completeCandidate_ = plan.complete;
  // A tile that left the view before it arrived no longer keeps anyone waiting.
  std::erase_if(visibleSince_, [&plan](const auto &item) { return std::ranges::find(plan.visible, item.first) == plan.visible.end(); });
  planOverlays(plan.view);

  ++statistics_.frames;
  statistics_.framesWithStandIns += plan.stretched ? 1 : 0;
  statistics_.framesWithHoles += plan.holes ? 1 : 0;
  cache_.evict(frame_);
  update();
}

std::vector<double> DocumentCanvas::standInScales(double scale) const {
  std::vector<double> scales;
  for (const auto &[key, entry] : cache_.entries()) {
    if (key.scale != scale && !entry.image.isNull() && std::ranges::find(scales, key.scale) == scales.end()) {
      scales.push_back(key.scale);
    }
  }
  std::ranges::sort(scales, [scale](double a, double b) { return std::abs(std::log(a / scale)) < std::abs(std::log(b / scale)); });
  return scales;
}

void DocumentCanvas::planPage(const protocol::Page &page, FramePlan &plan) {
  const QRectF pageRect = plan.view.pageItemRect(page);
  if (!pageRect.intersects(plan.wantedArea)) {
    return;
  }
  if (pageRect.intersects(plan.visibleArea)) {
    displayPages_.push_back(pageRect);
  }
  const DeviceRect visiblePixels = plan.view.visiblePixels(page, plan.grid, 0.0);
  for (const TileKey &key : plan.grid.tilesIntersecting(page, plan.view.visiblePixels(page, plan.grid, plan.prefetch))) {
    const DeviceRect pixels = plan.grid.tilePixels(page, key.column, key.row);
    planTile(page, key, !pixels.intersected(visiblePixels).isEmpty(), plan);
  }
}

void DocumentCanvas::planTile(const protocol::Page &page, const TileKey &key, bool visible, FramePlan &plan) {
  const DeviceRect pixels = plan.grid.tilePixels(page, key.column, key.row);
  cache_.want(key, plan.grid.tileRect(page, key.column, key.row), QSize(static_cast<int>(pixels.width), static_cast<int>(pixels.height)),
              frame_);
  if (cache_.needsRender(key)) {
    (visible ? plan.visibleJobs : plan.prefetchJobs).push_back(renderJob(page, plan.grid, key, plan.now));
  }
  if (!visible) {
    return;
  }
  plan.visible.push_back(key);
  const TileCache::Entry *entry = cache_.find(key);
  if (entry == nullptr || entry->image.isNull() || entry->dirty) {
    visibleSince_.try_emplace(key, plan.now);
  }
  if (entry != nullptr && !entry->image.isNull()) {
    displayTiles_.push_back({.key = key,
                             .rect = plan.view.tileItemRect(page, plan.grid, key.column, key.row),
                             .image = entry->image,
                             .version = entry->version,
                             .current = true,
                             .exact = plan.grid.scale() == plan.view.scale()});
    plan.complete = plan.complete && !entry->dirty;
    return;
  }
  plan.complete = false;
  if (addStandIns(page, key, plan)) {
    plan.stretched = true;
  } else {
    plan.holes = true;
  }
}

TileRenderer::Job DocumentCanvas::renderJob(const protocol::Page &page, const TileGrid &grid, const TileKey &key, Clock::time_point now) {
  const DeviceRect pixels = grid.tilePixels(page, key.column, key.row);
  return {.key = key,
          .request = QJsonObject{{u"doc_id"_s, session_->documentId()},
                                 {u"page"_s, page.index},
                                 {u"rect"_s, protocol::toJson(grid.tileRect(page, key.column, key.row))},
                                 {u"width"_s, pixels.width},
                                 {u"height"_s, pixels.height},
                                 {u"zoom"_s, tileZoom_},
                                 {u"device_scale"_s, devicePixelRatio_},
                                 {u"mode"_s, u"interactive"_s}},
          .pixels = QSize(static_cast<int>(pixels.width), static_cast<int>(pixels.height)),
          .wanted = wantedSince_.try_emplace(key, now).first->second};
}

bool DocumentCanvas::addStandIns(const protocol::Page &page, const TileKey &missing, FramePlan &plan) {
  const std::vector<TileCache::StandIn> standIns =
      cache_.standInsFor(page, plan.grid.tileRect(page, missing.column, missing.row), plan.otherScales, tileSize_);
  for (const TileCache::StandIn &standIn : standIns) {
    const TileKey &key = standIn.key;
    const TileCache::Entry *entry = cache_.find(key);
    if (entry == nullptr || std::ranges::any_of(plan.standIns, [&key](const auto &added) { return added.second.key == key; })) {
      continue;
    }
    cache_.touch(key, frame_);
    plan.standIns.emplace_back(standIn.rank,
                               CanvasTile{.key = key,
                                          .rect = plan.view.tileItemRect(page, TileGrid(key.scale, tileSize_), key.column, key.row),
                                          .image = entry->image,
                                          .version = entry->version,
                                          .current = false,
                                          .exact = false});
  }
  return !standIns.empty();
}

void DocumentCanvas::planOverlays(const ViewGeometry &view) {
  // Overlays, in the canvas's coordinates. The caret is put on whole device pixels and is at least one device pixel wide.
  const std::vector<protocol::Page> &pages = session_->pages();
  const protocol::Overlay &overlay = session_->overlay();
  for (const protocol::PageRect &selection : overlay.selection) {
    if (std::cmp_less(selection.page, pages.size())) {
      displaySelection_.push_back(view.itemRect(pages.at(static_cast<std::size_t>(selection.page)), selection.rect));
    }
  }
  const std::optional<protocol::Caret> &caret = overlay.caret;
  if (!caret || !caret->visible || !hasActiveFocus() || !caretOn_ || !std::cmp_less(caret->page, pages.size())) {
    return;
  }
  QRectF rect = view.itemRect(pages.at(static_cast<std::size_t>(caret->page)), caret->rect);
  const double ratio = devicePixelRatio_;
  const double sceneLeft = mapToScene(rect.topLeft()).x();
  rect.moveLeft((std::round(sceneLeft * ratio) / ratio) - mapToScene(QPointF(0, 0)).x());
  rect.setWidth(std::max(rect.width(), 1.0 / ratio));
  displayCaret_ = rect;
}

QSGNode *DocumentCanvas::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/) {
  QQuickWindow *window = this->window();
  if (window == nullptr) {
    return oldNode;
  }
  auto *root = static_cast<CanvasNode *>(oldNode); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast): only this class creates it.
  if (root == nullptr) {
    root = new CanvasNode; // NOLINT(cppcoreguidelines-owning-memory): the scene graph owns the returned node.
  }
  root->setPages(*window, displayPages_, pageShadowColor_, pagePlaceholderColor_);
  const CanvasNode::TileUpdate tiles = root->setTiles(*window, displayTiles_, kMaxUploadsPerFrame);
  root->setOverlays(*window, displaySelection_, displayCaret_, selectionColor_, caretColor_);
  statistics_.textureUploads += tiles.uploads;
  statistics_.textureUploadBytes += tiles.uploadBytes;
  for (const TileKey &key : tiles.newlyShown) {
    recordShown(key);
  }
  // Completeness is reported after the uploads, so "complete" means "on screen".
  reportCompleteness(completeCandidate_ && !tiles.deferred);
  if (tiles.deferred) {
    QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
  }
  return root;
}

void DocumentCanvas::reportCompleteness(bool complete) {
  // Called on the render thread while the GUI thread is blocked, so this is safe; the signal goes to the GUI thread.
  if (tilesComplete_.exchange(complete, std::memory_order_relaxed) != complete) {
    QMetaObject::invokeMethod(this, &DocumentCanvas::tilesCompleteChanged, Qt::QueuedConnection);
  }
}

bool DocumentCanvas::handleViewKey(const QKeyEvent &event) {
  // View keys belong to the shell until the engine moves the caret through documents (Phase 2), which will then reveal the caret.
  const bool control = event.modifiers().testFlag(Qt::ControlModifier);
  const QPointF centre(width() / 2, height() / 2);
  if (event.matches(QKeySequence::ZoomIn) || (control && event.key() == Qt::Key_Equal)) {
    zoomAt(zoom_ * kKeyZoomStep, centre);
    return true;
  }
  if (event.matches(QKeySequence::ZoomOut)) {
    zoomAt(zoom_ / kKeyZoomStep, centre);
    return true;
  }
  if (control && event.key() == Qt::Key_0) {
    zoomAt(1.0, centre);
    return true;
  }
  if (event.matches(QKeySequence::MoveToNextPage)) {
    scrollSmoothlyBy({0, height() * 0.9});
    return true;
  }
  if (event.matches(QKeySequence::MoveToPreviousPage)) {
    scrollSmoothlyBy({0, -height() * 0.9});
    return true;
  }
  if (event.matches(QKeySequence::MoveToStartOfDocument)) {
    scrollSmoothlyBy({0, -contentPosition_.y()});
    return true;
  }
  if (event.matches(QKeySequence::MoveToEndOfDocument)) {
    scrollSmoothlyBy({0, contentHeight()});
    return true;
  }
  return false;
}

void DocumentCanvas::keyPressEvent(QKeyEvent *event) {
  if (handleViewKey(*event)) {
    event->accept();
    return;
  }
  // Tab moves the keyboard focus out of the canvas, so the keyboard never gets stuck in it (ADR-0020 §5).
  if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab || session_ == nullptr || session_->documentId() <= 0) {
    event->ignore();
    return;
  }
  const KeyMappingOptions options = KeyMappingOptions::forPlatform();
  session_->sendKey(keyInput(*event, ime_.composing(), options));
  if (!ime_.composing()) {
    if (const auto text = typedText(*event, options)) {
      session_->sendText(*text);
    }
  }
  event->accept();
}

void DocumentCanvas::performImeAction(const ImeAction &action) {
  switch (action.kind) {
  case ImeAction::Kind::Start:
    compositionContext_ = editContext();
    session_->sendComposition(protocol::CompositionPhase::Start);
    break;
  case ImeAction::Kind::Update:
    session_->sendComposition(protocol::CompositionPhase::Update, action.text, action.selectionStart, action.selectionEnd);
    break;
  case ImeAction::Kind::End:
    session_->sendComposition(protocol::CompositionPhase::End);
    compositionContext_.reset();
    break;
  case ImeAction::Kind::Commit:
    session_->sendText(action.text);
    compositionContext_.reset();
    break;
  case ImeAction::Kind::DeleteBefore:
    deleteAroundCaret(action.length, 0);
    break;
  case ImeAction::Kind::DeleteAfter:
    deleteAroundCaret(0, action.length);
    break;
  }
}

void DocumentCanvas::inputMethodEvent(QInputMethodEvent *event) {
  if (session_ == nullptr || session_->documentId() <= 0) {
    event->ignore();
    return;
  }
  for (const ImeAction &action : ime_.translate(*event)) {
    performImeAction(action);
  }
  event->accept();
}

void DocumentCanvas::finishComposition() {
  if (!ime_.composing()) {
    return;
  }
  // Ask the platform's input method to commit, as Qt Quick's own text editors do; it answers with an input-method event.
  QGuiApplication::inputMethod()->commit();
  if (ime_.composing() && session_ != nullptr) {
    // No input method answered (for example the offscreen platform): commit the composition as it stands.
    session_->sendText(ime_.composition());
    ime_.reset();
    compositionContext_.reset();
  }
}

std::optional<DocumentCanvas::EditContext> DocumentCanvas::editContext() const {
  if (compositionContext_) {
    return compositionContext_;
  }
  if (session_ == nullptr) {
    return std::nullopt;
  }
  const auto &tree = session_->accessibilityTree();
  const auto caret = session_->accessibilityCaret();
  const protocol::AccessibleNode *node = tree && caret ? tree->find(caret->node) : nullptr;
  if (node == nullptr || !caret || !node->editable || !node->hasText) {
    return std::nullopt;
  }
  return EditContext{.text = node->text, .cursor = std::clamp(caret->offset, 0, static_cast<int>(node->text.size()))};
}

QVariant DocumentCanvas::inputMethodQuery(Qt::InputMethodQuery query) const {
  // The input method asks where to show its candidate window and which text surrounds the caret. The caret comes from overlay.update, the
  // text from the editable node of the accessibility tree that holds the accessibility caret (engine protocol §6.3, §6.6).
  const auto context = editContext();
  const auto caret = caretItemRect();
  switch (query) {
  case Qt::ImEnabled:
    return context.has_value();
  case Qt::ImReadOnly:
    return !context.has_value();
  case Qt::ImHints:
    return QVariant::fromValue(static_cast<int>(Qt::ImhMultiLine));
  case Qt::ImCursorRectangle:
  case Qt::ImAnchorRectangle:
    return caret.value_or(QRectF());
  case Qt::ImInputItemClipRectangle:
    return boundingRect();
  case Qt::ImFont: {
    QFont font;
    font.setPixelSize(std::clamp(static_cast<int>(std::lround(caret.value_or(QRectF()).height())), 8, 96));
    return font;
  }
  case Qt::ImSurroundingText:
    return context ? QVariant(context->text) : QVariant();
  case Qt::ImCursorPosition:
  case Qt::ImAnchorPosition:
  case Qt::ImAbsolutePosition:
    return context ? QVariant(context->cursor) : QVariant();
  case Qt::ImTextBeforeCursor:
    return context ? QVariant(context->text.left(context->cursor)) : QVariant();
  case Qt::ImTextAfterCursor:
    return context ? QVariant(context->text.mid(context->cursor)) : QVariant();
  case Qt::ImCurrentSelection:
    return QString();
  case Qt::ImEnterKeyType:
    return QVariant::fromValue(static_cast<int>(Qt::EnterKeyDefault));
  default:
    return QQuickItem::inputMethodQuery(query);
  }
}

void DocumentCanvas::updateInputMethod(Qt::InputMethodQueries queries) const {
  if (hasActiveFocus()) {
    QGuiApplication::inputMethod()->update(queries);
  }
}

void DocumentCanvas::sendPointer(protocol::PointerKind kind, const QMouseEvent &event) {
  if (session_ == nullptr) {
    return;
  }
  const protocol::PointerInput pointer{.kind = kind,
                                       .position = geometry().viewPoint(event.position()),
                                       .button = domButton(event.button() == Qt::NoButton ? Qt::LeftButton : event.button()),
                                       .clicks = clicks_,
                                       .modifiers = domModifiers(event.modifiers(), KeyMappingOptions::forPlatform()),
                                       .pointerType = pointerTypeOf(event)};
  session_->sendPointer(pointer);
}

void DocumentCanvas::pointerPressed(QMouseEvent *event) {
  forceActiveFocus(Qt::MouseFocusReason);
  finishComposition();
  // The engine turns the position into a document position with the zoom of the last view.set.
  flushView();
  const auto now = Clock::now();
  const auto interval = std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval());
  const auto dragDistance = static_cast<qreal>(QGuiApplication::styleHints()->startDragDistance());
  const bool repeated = now - lastPress_ <= interval && event->button() == lastPressButton_ &&
                        (event->position() - lastPressPosition_).manhattanLength() <= dragDistance;
  clicks_ = repeated ? clicks_ + 1 : 1;
  lastPress_ = now;
  lastPressPosition_ = event->position();
  lastPressButton_ = event->button();
  pressedButtons_ |= event->button();
  sendPointer(protocol::PointerKind::Down, *event);
  event->accept();
}

void DocumentCanvas::mousePressEvent(QMouseEvent *event) { pointerPressed(event); }

void DocumentCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
  // Qt Quick delivers the second press of a double click to mousePressEvent, which counted it as the second click, and then this event for
  // the same press. Accept it, so that it goes nowhere else, and send nothing: the engine would see a third press without a release.
  event->accept();
}

void DocumentCanvas::mouseMoveEvent(QMouseEvent *event) {
  // Only moves while a button is held: v0 has no hover feedback, and every move is a message.
  if (pressedButtons_ != Qt::NoButton) {
    sendPointer(protocol::PointerKind::Move, *event);
  }
  event->accept();
}

void DocumentCanvas::mouseReleaseEvent(QMouseEvent *event) {
  pressedButtons_ &= ~event->button();
  sendPointer(protocol::PointerKind::Up, *event);
  event->accept();
}

void DocumentCanvas::mouseUngrabEvent() {
  // Another item or the platform took the pointer away mid-gesture (for example a pinch began).
  if (pressedButtons_ != Qt::NoButton && session_ != nullptr) {
    pressedButtons_ = Qt::NoButton;
    session_->sendPointer({.kind = protocol::PointerKind::Cancel});
  }
}

void DocumentCanvas::wheelEvent(QWheelEvent *event) {
  // Control+wheel zooms around the pointer (Command+wheel on macOS, where Qt reports Command as Control).
  if (event->modifiers().testFlag(Qt::ControlModifier)) {
    const QPoint angle = event->angleDelta();
    const double notches = (angle.y() != 0 ? angle.y() : angle.x()) / kWheelNotch;
    if (notches != 0.0) {
      zoomAt(zoom_ * std::pow(kZoomStep, notches), event->position());
    }
    event->accept();
    return;
  }
  if (!event->pixelDelta().isNull()) {
    // Touchpads report pixels, smooth already and with the platform's own momentum.
    scrollBy(-event->pixelDelta().x(), -event->pixelDelta().y());
  } else {
    // Mouse wheels report notches: scroll by the platform's number of lines per notch, animated so that the movement looks continuous.
    QPointF notches = QPointF(event->angleDelta()) / kWheelNotch;
    if (event->modifiers().testFlag(Qt::ShiftModifier) && notches.x() == 0.0) {
      notches = QPointF(notches.y(), 0.0);
    }
    const double step = QGuiApplication::styleHints()->wheelScrollLines() * kWheelLineStep;
    scrollSmoothlyBy(-notches * step);
  }
  event->accept();
}

void DocumentCanvas::focusInEvent(QFocusEvent *event) {
  // The base class reports the focus to screen readers.
  QQuickItem::focusInEvent(event);
  restartCaretBlink();
  polish();
}

void DocumentCanvas::focusOutEvent(QFocusEvent *event) {
  QQuickItem::focusOutEvent(event);
  finishComposition();
  caretBlink_.stop();
  polish();
}

} // namespace bayan
