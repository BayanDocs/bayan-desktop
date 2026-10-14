#include "viewgeometry.h"

#include <algorithm>
#include <cmath>

namespace bayan {
namespace {

constexpr auto kBluPerDip = static_cast<double>(protocol::kBluPerDip);

double finiteOr(double value, double fallback) { return std::isfinite(value) ? value : fallback; }

// Rounds a logical coordinate to the nearest whole device pixel of the window.
double snapped(double logical, double sceneOffset, double devicePixelRatio) {
  return (std::round((sceneOffset + logical) * devicePixelRatio) / devicePixelRatio) - sceneOffset;
}

} // namespace

ViewGeometry::ViewGeometry(const std::vector<protocol::Page> &pages, const ViewState &state) : state_(state) {
  if (!(state_.zoom > 0.0) || !std::isfinite(state_.zoom)) {
    state_.zoom = 1.0;
  }
  if (!(state_.devicePixelRatio > 0.0) || !std::isfinite(state_.devicePixelRatio)) {
    state_.devicePixelRatio = 1.0;
  }
  state_.viewportSize =
      QSizeF(std::max(0.0, finiteOr(state_.viewportSize.width(), 0.0)), std::max(0.0, finiteOr(state_.viewportSize.height(), 0.0)));
  if (!pages.empty()) {
    area_ = {.left = pages.front().x, .top = pages.front().y, .right = pages.front().x, .bottom = pages.front().y};
    for (const protocol::Page &page : pages) {
      area_.left = std::min(area_.left, page.x);
      area_.top = std::min(area_.top, page.y);
      area_.right = std::max(area_.right, page.x + page.width);
      area_.bottom = std::max(area_.bottom, page.y + page.height);
    }
  }
  state_.contentPosition = clamped(state_.contentPosition);
  const double zoom = state_.zoom;
  const double left = areaLeftEdge(zoom, state_.contentPosition.x()) - (static_cast<double>(area_.left) * zoom / kBluPerDip);
  const double top = kMargin - state_.contentPosition.y() - (static_cast<double>(area_.top) * zoom / kBluPerDip);
  origin_ = QPointF(snapped(left, state_.sceneOffset.x(), state_.devicePixelRatio),
                    snapped(top, state_.sceneOffset.y(), state_.devicePixelRatio));
}

double ViewGeometry::logicalWidth(double zoom) const { return static_cast<double>(area_.right - area_.left) * zoom / kBluPerDip; }

double ViewGeometry::logicalHeight(double zoom) const { return static_cast<double>(area_.bottom - area_.top) * zoom / kBluPerDip; }

double ViewGeometry::areaLeftEdge(double zoom, double contentX) const {
  const double width = logicalWidth(zoom);
  if (width + (2 * kMargin) <= state_.viewportSize.width()) {
    return (state_.viewportSize.width() - width) / 2;
  }
  return kMargin - contentX;
}

QSizeF ViewGeometry::contentSize() const { return {logicalWidth(state_.zoom) + (2 * kMargin), logicalHeight(state_.zoom) + (2 * kMargin)}; }

QPointF ViewGeometry::maximumContentPosition() const {
  const QSizeF content = contentSize();
  return {std::max(0.0, content.width() - state_.viewportSize.width()), std::max(0.0, content.height() - state_.viewportSize.height())};
}

QPointF ViewGeometry::clamped(QPointF contentPosition) const {
  const QPointF maximum = maximumContentPosition();
  return {std::clamp(finiteOr(contentPosition.x(), 0.0), 0.0, maximum.x()),
          std::clamp(finiteOr(contentPosition.y(), 0.0), 0.0, maximum.y())};
}

qint64 ViewGeometry::pageLeft(const protocol::Page &page) const { return std::llround(static_cast<double>(page.x) * scale() / kBluPerDip); }

qint64 ViewGeometry::pageTop(const protocol::Page &page) const { return std::llround(static_cast<double>(page.y) * scale() / kBluPerDip); }

QRectF ViewGeometry::pageItemRect(const protocol::Page &page) const {
  const TileGrid grid(scale(), TileGrid::kDefaultTileSize);
  const double ratio = state_.devicePixelRatio;
  return {origin_.x() + (static_cast<double>(pageLeft(page)) / ratio), origin_.y() + (static_cast<double>(pageTop(page)) / ratio),
          static_cast<double>(grid.pageWidthPixels(page)) / ratio, static_cast<double>(grid.pageHeightPixels(page)) / ratio};
}

QRectF ViewGeometry::itemRect(const protocol::Page &page, const protocol::BluRect &rect) const {
  const QRectF pageRect = pageItemRect(page);
  const double factor = state_.zoom / kBluPerDip;
  return {pageRect.x() + (static_cast<double>(rect.x) * factor), pageRect.y() + (static_cast<double>(rect.y) * factor),
          static_cast<double>(rect.width) * factor, static_cast<double>(rect.height) * factor};
}

QRectF ViewGeometry::tileItemRect(const protocol::Page &page, const TileGrid &grid, int column, int row) const {
  const DeviceRect pixels = grid.tilePixels(page, column, row);
  const double stretch = scale() / grid.scale();
  const double ratio = state_.devicePixelRatio;
  const double left = static_cast<double>(pageLeft(page)) + (static_cast<double>(pixels.x) * stretch);
  const double top = static_cast<double>(pageTop(page)) + (static_cast<double>(pixels.y) * stretch);
  return {origin_.x() + (left / ratio), origin_.y() + (top / ratio), static_cast<double>(pixels.width) * stretch / ratio,
          static_cast<double>(pixels.height) * stretch / ratio};
}

DeviceRect ViewGeometry::visiblePixels(const protocol::Page &page, const TileGrid &grid, double margin) const {
  const double ratio = state_.devicePixelRatio;
  // The canvas, grown by the margin, in display device pixels relative to the page's top-left corner.
  const double left = ((-origin_.x() - margin) * ratio) - static_cast<double>(pageLeft(page));
  const double top = ((-origin_.y() - margin) * ratio) - static_cast<double>(pageTop(page));
  const double right = ((state_.viewportSize.width() - origin_.x() + margin) * ratio) - static_cast<double>(pageLeft(page));
  const double bottom = ((state_.viewportSize.height() - origin_.y() + margin) * ratio) - static_cast<double>(pageTop(page));
  // The same area in the grid's device pixels.
  const double toGrid = grid.scale() / scale();
  const auto gridLeft = static_cast<qint64>(std::floor(left * toGrid));
  const auto gridTop = static_cast<qint64>(std::floor(top * toGrid));
  const auto gridRight = static_cast<qint64>(std::ceil(right * toGrid));
  const auto gridBottom = static_cast<qint64>(std::ceil(bottom * toGrid));
  const DeviceRect area{.x = gridLeft, .y = gridTop, .width = gridRight - gridLeft, .height = gridBottom - gridTop};
  return area.intersected({.x = 0, .y = 0, .width = grid.pageWidthPixels(page), .height = grid.pageHeightPixels(page)});
}

QPointF ViewGeometry::viewPoint(QPointF itemPoint) const { return itemPoint - origin_; }

QPointF ViewGeometry::viewSpacePoint(QPointF itemPoint) const { return (itemPoint - origin_) * (kBluPerDip / state_.zoom); }

QRectF ViewGeometry::viewport() const { return {-origin_.x(), -origin_.y(), state_.viewportSize.width(), state_.viewportSize.height()}; }

QPointF ViewGeometry::contentPositionKeeping(QPointF viewSpacePoint, QPointF itemPoint, double zoom) const {
  if (!(zoom > 0.0) || !std::isfinite(zoom)) {
    return state_.contentPosition;
  }
  const double factor = zoom / kBluPerDip;
  // The origin that puts the point at itemPoint, and the scroll position that gives that origin (see the constructor).
  const QPointF origin = itemPoint - (viewSpacePoint * factor);
  return {kMargin - (static_cast<double>(area_.left) * factor) - origin.x(),
          kMargin - (static_cast<double>(area_.top) * factor) - origin.y()};
}

} // namespace bayan
