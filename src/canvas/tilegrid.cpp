#include "tilegrid.h"

#include <algorithm>
#include <cmath>

namespace bayan {
namespace {

// Floating-point products such as 897.0000000001 must not turn into an extra pixel; anything this close to a whole pixel counts as it.
constexpr double kPixelTolerance = 1e-6;

} // namespace

DeviceRect DeviceRect::intersected(const DeviceRect &other) const {
  const qint64 left = std::max(x, other.x);
  const qint64 top = std::max(y, other.y);
  const qint64 rightEdge = std::min(right(), other.right());
  const qint64 bottomEdge = std::min(bottom(), other.bottom());
  if (rightEdge <= left || bottomEdge <= top) {
    return {};
  }
  return {.x = left, .y = top, .width = rightEdge - left, .height = bottomEdge - top};
}

TileGrid::TileGrid(double scale, int tileSize)
    : scale_(scale > 0.0 && std::isfinite(scale) ? scale : 1.0), tileSize_(std::clamp(tileSize, kMinimumTileSize, kMaximumTileSize)) {}

qint64 TileGrid::bluAt(qint64 devicePixel) const {
  return std::llround(static_cast<double>(devicePixel) * static_cast<double>(protocol::kBluPerDip) / scale_);
}

qint64 TileGrid::pixelsFor(qint64 blu) const {
  const double pixels = static_cast<double>(blu) * scale_ / static_cast<double>(protocol::kBluPerDip);
  return std::max<qint64>(1, static_cast<qint64>(std::ceil(pixels - kPixelTolerance)));
}

qint64 TileGrid::pageWidthPixels(const protocol::Page &page) const { return pixelsFor(page.width); }

qint64 TileGrid::pageHeightPixels(const protocol::Page &page) const { return pixelsFor(page.height); }

int TileGrid::columns(const protocol::Page &page) const { return static_cast<int>((pageWidthPixels(page) + tileSize_ - 1) / tileSize_); }

int TileGrid::rows(const protocol::Page &page) const { return static_cast<int>((pageHeightPixels(page) + tileSize_ - 1) / tileSize_); }

DeviceRect TileGrid::tilePixels(const protocol::Page &page, int column, int row) const {
  const qint64 left = qint64{column} * tileSize_;
  const qint64 top = qint64{row} * tileSize_;
  return {.x = left,
          .y = top,
          .width = std::min<qint64>(tileSize_, pageWidthPixels(page) - left),
          .height = std::min<qint64>(tileSize_, pageHeightPixels(page) - top)};
}

protocol::BluRect TileGrid::tileRect(const protocol::Page &page, int column, int row) const {
  const DeviceRect pixels = tilePixels(page, column, row);
  const qint64 left = bluAt(pixels.x);
  const qint64 top = bluAt(pixels.y);
  // At least one BLU wide and high, as the engine requires; at any scale the shell uses, a pixel is far more than one BLU.
  return {.x = left,
          .y = top,
          .width = std::max<qint64>(1, bluAt(pixels.right()) - left),
          .height = std::max<qint64>(1, bluAt(pixels.bottom()) - top)};
}

std::vector<TileKey> TileGrid::tilesIntersecting(const protocol::Page &page, const DeviceRect &area) const {
  const DeviceRect visible = area.intersected({.x = 0, .y = 0, .width = pageWidthPixels(page), .height = pageHeightPixels(page)});
  std::vector<TileKey> tiles;
  if (visible.isEmpty()) {
    return tiles;
  }
  const auto firstColumn = static_cast<int>(visible.x / tileSize_);
  const auto lastColumn = static_cast<int>((visible.right() - 1) / tileSize_);
  const auto firstRow = static_cast<int>(visible.y / tileSize_);
  const auto lastRow = static_cast<int>((visible.bottom() - 1) / tileSize_);
  tiles.reserve(static_cast<std::size_t>(lastColumn - firstColumn + 1) * static_cast<std::size_t>(lastRow - firstRow + 1));
  for (int row = firstRow; row <= lastRow; ++row) {
    for (int column = firstColumn; column <= lastColumn; ++column) {
      tiles.push_back({.page = page.index, .scale = scale_, .column = column, .row = row});
    }
  }
  return tiles;
}

} // namespace bayan
