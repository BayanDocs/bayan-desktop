// The tiles the document canvas has, and whether they still show what the engine would draw (DESK-002).
//
// A tile is rendered on the canvas's tile thread and arrives later. Meanwhile the engine may change the document (the user types) and say
// so with render.invalidate. To know whether a tile that arrives shows the document before or after such a change, every tile records
// renderedAfter, the last request the engine had already handled when it rendered the tile (Engine::lastPostedRequest), and every
// invalidation records the request whose handling caused it (DocumentSession tells). A tile is fresh if it was rendered after its region's
// last invalidation; otherwise it is dirty: the canvas keeps showing it, so nothing flickers, and renders it again.
//
// Used on the GUI thread only. Images are kept within a memory budget; the least recently shown ones go first.

#pragma once

#include "protocol.h"
#include "tilegrid.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace bayan {

class TileCache {
public:
  struct Entry {
    // The part of the page the tile shows, and its size in pixels.
    protocol::BluRect rect{};
    QSize pixels{};
    // The pixels, or a null image while the tile is missing.
    QImage image{};
    // Increases whenever the image changes, so the scene graph knows to upload it again.
    std::uint64_t version = 0;
    // See the file comment.
    qint64 renderedAfter = 0;
    bool dirty = false;
    // Failed attempts since the last success; the canvas stops asking after kMaxFailures until the region changes.
    int failures = 0;
    // The last frame that showed or wanted the tile.
    std::uint64_t lastUsed = 0;
  };

  static constexpr int kMaxFailures = 3;
  // How many recent invalidations are remembered for tiles that are still being rendered.
  static constexpr std::size_t kRememberedInvalidations = 256;
  // An invalidation of a page with more rectangles than this counts as one rectangle around all of them: the engine decides how many it
  // sends, and every tile that arrives is checked against every remembered rectangle.
  static constexpr std::size_t kMaxRectsPerInvalidation = 64;
  static constexpr qint64 kDefaultBudgetBytes = qint64{128} * 1024 * 1024;
  // Entries without an image that no frame used for this long are forgotten.
  static constexpr std::uint64_t kForgetEmptyAfterFrames = 600;

  explicit TileCache(qint64 budgetBytes = kDefaultBudgetBytes);

  // Records that a frame wants a tile, creating its entry if needed.
  void want(const TileKey &key, const protocol::BluRect &rect, QSize pixels, std::uint64_t frame);
  // Records that a frame shows a tile it already has, such as a stale tile of another zoom that stands in for a missing one.
  void touch(const TileKey &key, std::uint64_t frame);
  // Whether a tile should be rendered (again): it is missing or dirty, and the engine has not refused it too often.
  [[nodiscard]] bool needsRender(const TileKey &key) const;
  [[nodiscard]] const Entry *find(const TileKey &key) const;

  // A tile of another zoom that can stand in for a missing one, and how close its zoom is: 0 for the closest.
  struct StandIn {
    TileKey key{};
    std::size_t rank = 0;
  };
  // The tiles the cache has images of that cover part of an area of a page (in page coordinates), to stand in for a missing tile there.
  // scales are the zooms to take them from (device pixels per device-independent pixel), closest first, and tileSize the tiles' side in
  // device pixels. Tiles of the closest zoom come first; the next zoom adds its tiles only if the closer ones left part of the area
  // uncovered, and so on until a zoom has every tile of the area.
  [[nodiscard]] std::vector<StandIn> standInsFor(const protocol::Page &page, const protocol::BluRect &area,
                                                 const std::vector<double> &scales, int tileSize) const;

  // A tile arrived from the tile thread; image is null if the engine refused it. Unknown keys (evicted meanwhile) are ignored. Returns
  // whether the cache kept the image.
  bool deliver(const TileKey &key, const QImage &image, qint64 renderedAfter);
  // The engine changed these rectangles of a page while handling request cause.
  void invalidate(int page, const std::vector<protocol::BluRect> &rects, qint64 cause);
  // Forgets every tile, for example when the pages change. The changes remembered for tiles still being rendered stay.
  void clear();
  // Forgets every tile and every change: for another engine, whose request numbers start again.
  void reset();
  void setBudget(qint64 bytes);
  // Drops tiles that frame did not use, least recently used first, until the images fit the budget.
  void evict(std::uint64_t frame);

  [[nodiscard]] qint64 bytes() const { return bytes_; }
  [[nodiscard]] qint64 budget() const { return budget_; }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] const std::map<TileKey, Entry> &entries() const { return entries_; }

private:
  struct Invalidation {
    int page = 0;
    std::vector<protocol::BluRect> rects;
    qint64 cause = 0;
  };

  // Whether an invalidation after renderedAfter touched the rectangle.
  [[nodiscard]] bool changedSince(int page, const protocol::BluRect &rect, qint64 renderedAfter) const;
  void setImage(Entry &entry, const QImage &image);

  std::map<TileKey, Entry> entries_;
  std::deque<Invalidation> recent_;
  // Invalidations caused by requests up to this one have been forgotten; tiles rendered before it count as dirty.
  qint64 forgottenUpTo_ = 0;
  qint64 bytes_ = 0;
  qint64 budget_;
};

} // namespace bayan
