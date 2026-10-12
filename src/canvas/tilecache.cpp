#include "tilecache.h"

#include <algorithm>
#include <utility>

namespace bayan {

TileCache::TileCache(qint64 budgetBytes) : budget_(std::max<qint64>(0, budgetBytes)) {}

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
  for (auto &[key, entry] : entries_) {
    if (key.page != page || entry.renderedAfter >= cause) {
      continue;
    }
    if (std::ranges::any_of(rects, [&entry](const protocol::BluRect &changed) { return changed.intersects(entry.rect); })) {
      entry.dirty = true;
      entry.failures = 0;
    }
  }
  recent_.push_back({.page = page, .rects = rects, .cause = cause});
  while (recent_.size() > kRememberedInvalidations) {
    forgottenUpTo_ = std::max(forgottenUpTo_, recent_.front().cause);
    recent_.pop_front();
  }
}

void TileCache::clear() {
  entries_.clear();
  bytes_ = 0;
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
