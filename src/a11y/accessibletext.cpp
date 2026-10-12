#include "accessibletext.h"

#include <QtCore/QChar>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace bayan {

AccessibleText::AccessibleText(const protocol::AccessibleTree &tree) : name_(tree.root().name) {
  for (const protocol::AccessibleNode *node : tree.readingOrder()) {
    if (!node->hasText) {
      continue;
    }
    if (!segments_.empty()) {
      // One node per line: the bridges and the screen readers find lines and paragraphs by their line breaks.
      text_.append(u'\n');
    }
    segments_.push_back(Segment{.node = node->id,
                                .role = node->role,
                                .start = static_cast<int>(text_.size()),
                                .length = static_cast<int>(node->text.size()),
                                .editable = node->editable,
                                .bounds = node->bounds,
                                .chars = node->chars});
    text_.append(node->text);
  }
}

const AccessibleText::Segment *AccessibleText::segmentAt(int offset) const {
  // The first segment whose end is at or after the offset.
  const auto found = std::ranges::lower_bound(segments_, offset, std::less<>(), &Segment::end);
  if (found == segments_.end() || offset < found->start) {
    return nullptr;
  }
  return &*found;
}

const AccessibleText::Segment *AccessibleText::segmentOf(quint64 node) const {
  const auto found = std::ranges::find(segments_, node, &Segment::node);
  return found == segments_.end() ? nullptr : &*found;
}

std::optional<int> AccessibleText::offsetOf(quint64 node, int nodeOffset) const {
  const Segment *segment = segmentOf(node);
  if (segment == nullptr) {
    return std::nullopt;
  }
  return segment->start + std::clamp(nodeOffset, 0, segment->length);
}

std::optional<protocol::PageRect> AccessibleText::characterBox(int offset) const {
  const Segment *segment = segmentAt(offset);
  if (segment == nullptr || !segment->bounds) {
    return std::nullopt;
  }
  const int local = offset - segment->start;
  for (const protocol::CharBox &box : segment->chars) {
    if (local >= box.start && local < box.end) {
      return protocol::PageRect{.page = segment->bounds->page, .rect = box.rect};
    }
  }
  if (local == segment->length) {
    // The end of the line: a zero-width box after the last character, or at the start of the node's box if it has no characters.
    protocol::BluRect end = segment->bounds->rect;
    if (!segment->chars.empty()) {
      end = segment->chars.back().rect;
      end.x = end.right();
    }
    end.width = 0;
    return protocol::PageRect{.page = segment->bounds->page, .rect = end};
  }
  return std::nullopt;
}

AccessibleText::Change AccessibleText::difference(const QString &before, const QString &after) {
  const qsizetype shorter = std::min(before.size(), after.size());
  qsizetype prefix = 0;
  while (prefix < shorter && before.at(prefix) == after.at(prefix)) {
    ++prefix;
  }
  qsizetype suffix = 0;
  while (suffix < shorter - prefix && before.at(before.size() - 1 - suffix) == after.at(after.size() - 1 - suffix)) {
    ++suffix;
  }
  // Never split a surrogate pair at either end of the change.
  if (prefix > 0 && prefix < before.size() && before.at(prefix).isLowSurrogate()) {
    --prefix;
  }
  if (suffix > 0 && before.size() - suffix > prefix && before.at(before.size() - suffix).isLowSurrogate()) {
    --suffix;
  }
  return {.position = static_cast<int>(prefix),
          .removed = before.mid(prefix, before.size() - prefix - suffix),
          .inserted = after.mid(prefix, after.size() - prefix - suffix)};
}

} // namespace bayan
