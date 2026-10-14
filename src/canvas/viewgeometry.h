// Where the document canvas shows pages, and how its coordinates relate to the engine's (engine protocol §7).
//
// View space is the page area at zoom 1, in BLU, as view.pages describes it. The canvas shows it scaled by the zoom factor, surrounded by a
// margin, and centred horizontally when it is narrower than the canvas (the protocol allows moving the whole page area, as long as pointer
// positions move with it). Pages and tiles are placed on whole device pixels, so that a tile rendered at the display's scale is copied to
// the screen pixel for pixel, sharp at any device pixel ratio, fractional ones such as 1.25 or 1.5 on Windows included. This is view
// arithmetic only: which page goes where is the engine's decision, the canvas just applies the zoom and the scroll position.

#pragma once

#include "protocol.h"
#include "tilegrid.h"

#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSizeF>

#include <vector>

namespace bayan {

// What the canvas currently shows.
struct ViewState {
  // The zoom factor: 1 shows the document at its real size.
  double zoom = 1.0;
  // Physical pixels per logical pixel of the screen the canvas is on.
  double devicePixelRatio = 1.0;
  // The canvas's size, in logical pixels.
  QSizeF viewportSize{};
  // The scroll position: which part of the content is at the canvas's top-left corner, in logical pixels.
  QPointF contentPosition{};
  // The canvas's position in its window, in logical pixels, so that pages can be placed on whole device pixels of the window.
  QPointF sceneOffset{};
};

class ViewGeometry {
public:
  // The space around the page area, in logical pixels.
  static constexpr double kMargin = 24.0;

  ViewGeometry(const std::vector<protocol::Page> &pages, const ViewState &state);

  [[nodiscard]] const ViewState &state() const { return state_; }
  // Device pixels per device-independent pixel at zoom 1: zoom × device pixel ratio.
  [[nodiscard]] double scale() const { return state_.zoom * state_.devicePixelRatio; }
  // The size of everything that can be scrolled to, in logical pixels, margins included.
  [[nodiscard]] QSizeF contentSize() const;
  // The largest scroll position.
  [[nodiscard]] QPointF maximumContentPosition() const;
  // The scroll position limited to what can be scrolled to.
  [[nodiscard]] QPointF clamped(QPointF contentPosition) const;
  // Where view space's origin (BLU 0, 0) appears, in the canvas's coordinates, on a whole device pixel of the window.
  [[nodiscard]] QPointF origin() const { return origin_; }

  // A page's top-left corner in device pixels from the origin, at the display scale.
  [[nodiscard]] qint64 pageLeft(const protocol::Page &page) const;
  [[nodiscard]] qint64 pageTop(const protocol::Page &page) const;
  // A page's area in the canvas's coordinates.
  [[nodiscard]] QRectF pageItemRect(const protocol::Page &page) const;
  // A rectangle in a page's coordinates (BLU), in the canvas's coordinates.
  [[nodiscard]] QRectF itemRect(const protocol::Page &page, const protocol::BluRect &rect) const;
  // Where a tile of a grid appears in the canvas's coordinates. A grid of the display's scale lands on whole device pixels; a grid of
  // another scale (a stale tile while the zoom changes) is stretched to cover the same part of the page.
  [[nodiscard]] QRectF tileItemRect(const protocol::Page &page, const TileGrid &grid, int column, int row) const;
  // The part of a page that is visible, grown by margin logical pixels on every side, in the grid's device pixels relative to the page's
  // top-left corner. Empty if the page is out of sight.
  [[nodiscard]] DeviceRect visiblePixels(const protocol::Page &page, const TileGrid &grid, double margin) const;

  // A point of the canvas in device-independent pixels from the view origin, as input.pointer wants it (§6.5, §7).
  [[nodiscard]] QPointF viewPoint(QPointF itemPoint) const;
  // A point of the canvas in view space (BLU, as floating point).
  [[nodiscard]] QPointF viewSpacePoint(QPointF itemPoint) const;
  // The canvas's area in device-independent pixels from the view origin, as view.set wants it.
  [[nodiscard]] QRectF viewport() const;
  // The scroll position that shows the view-space point (BLU) at itemPoint at another zoom, before clamping.
  [[nodiscard]] QPointF contentPositionKeeping(QPointF viewSpacePoint, QPointF itemPoint, double zoom) const;

private:
  struct Area {
    qint64 left = 0;
    qint64 top = 0;
    qint64 right = 0;
    qint64 bottom = 0;
  };
  // The logical-pixel left edge of the page area at a zoom, before snapping: centred if it is narrower than the canvas.
  [[nodiscard]] double areaLeftEdge(double zoom, double contentX) const;
  [[nodiscard]] double logicalWidth(double zoom) const;
  [[nodiscard]] double logicalHeight(double zoom) const;

  ViewState state_;
  Area area_;
  QPointF origin_;
};

} // namespace bayan
