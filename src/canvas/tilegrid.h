// How the document canvas divides pages into tiles (DESK-002).
//
// The engine renders any rectangle of a page at any pixel size (engine protocol §6.4, §7). The canvas asks for square tiles of a fixed
// number of device pixels, laid out on a grid that starts at each page's top-left corner, at one scale: device pixels per
// device-independent pixel at zoom 1, which is the zoom factor times the screen's device pixel ratio. Tiles of one scale meet exactly: the
// edges between them are the same BLU coordinates on both sides, so nothing shows a seam. The grid is pure arithmetic over the page sizes
// the engine announces; it decides nothing about the document.

#pragma once

#include "protocol.h"

#include <QtCore/QtTypes>

#include <compare>
#include <vector>

namespace bayan {

// A rectangle of device pixels. 64-bit, because long documents at high zoom exceed the range of int.
struct DeviceRect {
  qint64 x = 0;
  qint64 y = 0;
  qint64 width = 0;
  qint64 height = 0;

  [[nodiscard]] qint64 right() const { return x + width; }
  [[nodiscard]] qint64 bottom() const { return y + height; }
  [[nodiscard]] bool isEmpty() const { return width <= 0 || height <= 0; }
  [[nodiscard]] DeviceRect intersected(const DeviceRect &other) const;
  friend bool operator==(const DeviceRect &, const DeviceRect &) = default;
};

// Identifies a tile: its page, the scale it was rendered at, and its place in that page's grid.
struct TileKey {
  int page = 0;
  double scale = 1.0;
  int column = 0;
  int row = 0;
  friend auto operator<=>(const TileKey &, const TileKey &) = default;
};

class TileGrid {
public:
  // The tile size the canvas uses unless told otherwise, in device pixels.
  static constexpr int kDefaultTileSize = 512;
  static constexpr int kMinimumTileSize = 64;
  static constexpr int kMaximumTileSize = 2048;

  // scale: device pixels per device-independent pixel at zoom 1 (zoom × device pixel ratio); tileSize: the side of a tile in device pixels.
  TileGrid(double scale, int tileSize);

  [[nodiscard]] double scale() const { return scale_; }
  [[nodiscard]] int tileSize() const { return tileSize_; }

  // The page coordinate, in BLU, of the edge before device pixel n (counted from the page's top-left corner).
  [[nodiscard]] qint64 bluAt(qint64 devicePixel) const;
  // A page's size in device pixels: enough pixels to cover all of it.
  [[nodiscard]] qint64 pageWidthPixels(const protocol::Page &page) const;
  [[nodiscard]] qint64 pageHeightPixels(const protocol::Page &page) const;
  [[nodiscard]] int columns(const protocol::Page &page) const;
  [[nodiscard]] int rows(const protocol::Page &page) const;

  // The device pixels a tile covers, relative to the page's top-left corner. Tiles in the last column and row may be smaller.
  [[nodiscard]] DeviceRect tilePixels(const protocol::Page &page, int column, int row) const;
  // The part of the page a tile shows, in page coordinates: exactly the BLU edges of its first and last pixel.
  [[nodiscard]] protocol::BluRect tileRect(const protocol::Page &page, int column, int row) const;
  // The tiles of a page that intersect an area given in device pixels relative to the page's top-left corner, row by row.
  [[nodiscard]] std::vector<TileKey> tilesIntersecting(const protocol::Page &page, const DeviceRect &area) const;

private:
  // Device pixels for a length in BLU, rounded up.
  [[nodiscard]] qint64 pixelsFor(qint64 blu) const;

  double scale_;
  int tileSize_;
};

} // namespace bayan
