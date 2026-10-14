#include "tilecache.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bayan {
namespace {

protocol::BluRect boundingRect(const std::vector<protocol::BluRect> &rects) {
  if (rects.empty()) {
    return {};
  }
  qint64 left = rects.front().x;
  qint64 top = rects.front().y;
  qint64 right = rects.front().right();
  qint64 bottom = rects.front().bottom();
  for (const protocol::BluRect &rect : rects) {
    left = std::min(left, rect.x);
    top = std::min(top, rect.y);
    right = std::max(right, rect.right());
    bottom = std::max(bottom, rect.bottom());
  }
  return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

} // namespace

TileCache::TileCache(qint64 budgetBytes) : budget_(std::max<qint64>(0, budgetBytes)) {}

void TileCache::setBudget(qint64 bytes) { budget_ = std::max<qint64>(0, bytes); }

void TileCache::want(const TileKey &key, const protocol::BluRect &rect, QSize pixels, std::uint64_t frame) {
  Entry &entry = entries_[key];
  entry.rect = rect;
  entry.pixels = pixels;
  entry.lastUsed = frame;
}

void TileCache::touch(const TileKey &key, std::uint64_t frame) {
  if (const auto found = entries_.find(key); found != entries_.end()) {
    found->second.lastUsed = frame;
  }
}

bool TileCache::needsRender(const TileKey &key) const {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return true;
  }
  const Entry &entry = found->second;
  return (entry.image.isNull() || entry.dirty) && entry.failures < kMaxFailures;
}

const TileCache::Entry *TileCache::find(const TileKey &key) const {
  const auto found = entries_.find(key);
  return found == entries_.end() ? nullptr : &found->second;
}

std::vector<TileCache::StandIn> TileCache::standInsFor(const protocol::Page &page, const protocol::BluRect &area,
                                                       const std::vector<double> &scales, int tileSize) const {
  std::vector<StandIn> result;
  for (std::size_t rank = 0; rank < scales.size(); ++rank) {
    // The area in this zoom's device pixels, rounded outwards.
    const TileGrid grid(scales.at(rank), tileSize);
    const double perBlu = grid.scale() / static_cast<double>(protocol::kBluPerDip);
    const auto left = static_cast<qint64>(std::floor(static_cast<double>(area.x) * perBlu));
    const auto top = static_cast<qint64>(std::floor(static_cast<double>(area.y) * perBlu));
    const auto right = static_cast<qint64>(std::ceil(static_cast<double>(area.right()) * perBlu));
    const auto bottom = static_cast<qint64>(std::ceil(static_cast<double>(area.bottom()) * perBlu));
    bool everyTile = true;
    for (const TileKey &key : grid.tilesIntersecting(page, {.x = left, .y = top, .width = right - left, .height = bottom - top})) {
      const Entry *entry = find(key);
      if (entry == nullptr || entry->image.isNull()) {
        everyTile = false;
      } else {
        result.push_back({.key = key, .rank = rank});
      }
    }
    if (everyTile) {
      break;
    }
  }
  return result;
}

void TileCache::setImage(Entry &entry, const QImage &image) {
  bytes_ -= entry.image.sizeInBytes();
  entry.image = image;
  bytes_ += entry.image.sizeInBytes();
  ++entry.version;
}

bool TileCache::deliver(const TileKey &key, const QImage &image, qint64 renderedAfter) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return false;
  }
  Entry &entry = found->second;
  if (image.isNull() || image.size() != entry.pixels) {
    ++entry.failures;
    return false;
  }
  setImage(entry, image);
  entry.failures = 0;
  entry.renderedAfter = renderedAfter;
  entry.dirty = changedSince(key.page, entry.rect, renderedAfter);
  return true;
}

bool TileCache::changedSince(int page, const protocol::BluRect &rect, qint64 renderedAfter) const {
  if (renderedAfter < forgottenUpTo_) {
    return true;
  }
  return std::ranges::any_of(recent_, [&](const Invalidation &invalidation) {
    return invalidation.page == page && invalidation.cause > renderedAfter &&
           std::ranges::any_of(invalidation.rects, [&](const protocol::BluRect &changed) { return changed.intersects(rect); });
  });
}

void TileCache::invalidate(int page, const std::vector<protocol::BluRect> &rects, qint64 cause) {
  // Marking a tile dirty that did not change costs a render, never a wrong picture.
  std::vector<protocol::BluRect> changed = rects.size() <= kMaxRectsPerInvalidation ? rects : std::vector{boundingRect(rects)};
  for (auto &[key, entry] : entries_) {
    if (key.page != page || entry.renderedAfter >= cause) {
      continue;
    }
    if (std::ranges::any_of(changed, [&entry](const protocol::BluRect &rect) { return rect.intersects(entry.rect); })) {
      entry.dirty = true;
      entry.failures = 0;
    }
  }
  recent_.push_back({.page = page, .rects = std::move(changed), .cause = cause});
  while (recent_.size() > kRememberedInvalidations) {
    forgottenUpTo_ = std::max(forgottenUpTo_, recent_.front().cause);
    recent_.pop_front();
  }
}

void TileCache::clear() {
  entries_.clear();
  bytes_ = 0;
}

void TileCache::reset() {
  clear();
  recent_.clear();
  forgottenUpTo_ = 0;
}

void TileCache::evict(std::uint64_t frame) {
  // Tiles that were wanted but never arrived (the view moved on) hold no image; forget them after a while so they do not pile up.
  std::erase_if(entries_, [frame](const auto &item) {
    const Entry &entry = item.second;
    return entry.image.isNull() && entry.lastUsed + kForgetEmptyAfterFrames < frame;
  });
  if (bytes_ <= budget_) {
    return;
  }
  std::vector<std::pair<std::uint64_t, TileKey>> candidates;
  for (const auto &[key, entry] : entries_) {
    if (entry.lastUsed != frame && !entry.image.isNull()) {
      candidates.emplace_back(entry.lastUsed, key);
    }
  }
  std::ranges::sort(candidates);
  for (const auto &candidate : candidates) {
    if (bytes_ <= budget_) {
      break;
    }
    const auto found = entries_.find(candidate.second);
    bytes_ -= found->second.image.sizeInBytes();
    entries_.erase(found);
  }
}

} // namespace bayan
