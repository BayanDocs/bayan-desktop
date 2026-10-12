#include "protocol.h"

#include <QtCore/QStringView>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan::protocol {
namespace {

// The largest integer a JSON number carries exactly in every language the engine talks to (engine protocol §4).
constexpr qint64 kMaxExactInteger = (qint64{1} << 53) - 1;
// The most selection rectangles an overlay may carry.
constexpr qsizetype kMaxSelectionRects = 100'000;

// An integer within [minimum, maximum], or nothing if the value is not a number with an exact integer value in that range.
std::optional<qint64> integer(const QJsonValue &value, qint64 minimum, qint64 maximum) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > static_cast<double>(kMaxExactInteger)) {
    return std::nullopt;
  }
  const qint64 result = value.toInteger(std::numeric_limits<qint64>::min());
  if (result < minimum || result > maximum) {
    return std::nullopt;
  }
  return result;
}

std::optional<int> smallInteger(const QJsonValue &value, int minimum, int maximum) {
  const auto result = integer(value, minimum, maximum);
  return result ? std::optional<int>(static_cast<int>(*result)) : std::nullopt;
}

std::optional<PageRect> pageRect(const QJsonValue &value, int pageCount) {
  const QJsonObject object = value.toObject();
  const auto page = smallInteger(object.value(u"page"_s), 0, pageCount - 1);
  const auto rect = parseRect(object.value(u"rect"_s));
  if (!value.isObject() || !page || !rect) {
    return std::nullopt;
  }
  return PageRect{.page = *page, .rect = *rect};
}

std::optional<Role> role(const QJsonValue &value) {
  if (!value.isString()) {
    return std::nullopt;
  }
  const QString name = value.toString();
  if (name == u"document"_s) {
    return Role::Document;
  }
  if (name == u"heading"_s) {
    return Role::Heading;
  }
  if (name == u"paragraph"_s) {
    return Role::Paragraph;
  }
  if (name == u"list"_s) {
    return Role::List;
  }
  if (name == u"list_item"_s) {
    return Role::ListItem;
  }
  if (name == u"table"_s) {
    return Role::Table;
  }
  if (name == u"row"_s) {
    return Role::Row;
  }
  if (name == u"column_header"_s) {
    return Role::ColumnHeader;
  }
  if (name == u"cell"_s) {
    return Role::Cell;
  }
  return Role::Other;
}

// An optional string field: absent leaves target as it is; anything but a string is malformed.
bool readString(const QJsonObject &object, const QString &key, QString &target) {
  const QJsonValue field = object.value(key);
  if (field.isUndefined()) {
    return true;
  }
  if (!field.isString()) {
    return false;
  }
  target = field.toString();
  return true;
}

// The character boxes of a node's text: in text order, never overlapping, within the text (the protocol allows no more boxes than code
// units), so that a shell can find a character's box by its offset.
std::optional<std::vector<CharBox>> charBoxes(const QJsonValue &value, const QString &text) {
  if (!value.isArray() || value.toArray().size() > text.size()) {
    return std::nullopt;
  }
  const QJsonArray list = value.toArray();
  std::vector<CharBox> result;
  result.reserve(static_cast<std::size_t>(list.size()));
  int previousEnd = 0;
  for (const auto &entry : list) {
    const QJsonObject box = entry.toObject();
    const auto start = smallInteger(box.value(u"start"_s), 0, static_cast<int>(text.size()));
    const auto end = smallInteger(box.value(u"end"_s), 0, static_cast<int>(text.size()));
    const auto rect = parseRect(box.value(u"rect"_s));
    if (!entry.isObject() || !start || !end || !rect || *start >= *end || *start < previousEnd) {
      return std::nullopt;
    }
    previousEnd = *end;
    result.push_back(CharBox{.start = *start, .end = *end, .rect = *rect});
  }
  return result;
}

// A node still to be read while parsing a tree, and how deep it sits (the root is at depth 1).
struct PendingNode {
  QJsonValue value;
  int depth = 0;
};

// Reads a node's list of children: their identifiers go into the node, the children themselves onto the list of nodes still to read. Fails
// if the list is malformed, or if the tree would have more nodes than allowed, counting those read (knownNodes) and those waiting.
bool readChildren(const QJsonObject &object, int depth, std::size_t knownNodes, AccessibleNode &node, std::vector<PendingNode> &pending) {
  const QJsonValue children = object.value(u"children"_s);
  if (children.isUndefined()) {
    return true;
  }
  const QJsonArray list = children.toArray();
  if (!children.isArray() || std::cmp_greater(knownNodes + pending.size() + static_cast<std::size_t>(list.size()), kMaxTreeNodes)) {
    return false;
  }
  node.children.reserve(static_cast<std::size_t>(list.size()));
  for (const auto &child : list) {
    const auto id = integer(child.toObject().value(u"id"_s), 1, kMaxExactInteger);
    if (!id) {
      return false;
    }
    node.children.push_back(static_cast<quint64>(*id));
  }
  // Last child first, so that the nodes are read in reading order.
  for (auto child = list.end(); child != list.begin();) {
    --child;
    pending.push_back({.value = *child, .depth = depth + 1});
  }
  return true;
}

// The fields of one node, without its children.
std::optional<AccessibleNode> nodeFields(const QJsonObject &object, int pageCount) {
  AccessibleNode result;
  const auto id = integer(object.value(u"id"_s), 1, kMaxExactInteger);
  const auto parsedRole = role(object.value(u"role"_s));
  if (!id || !parsedRole || !readString(object, u"name"_s, result.name) || !readString(object, u"text"_s, result.text)) {
    return std::nullopt;
  }
  result.id = static_cast<quint64>(*id);
  result.role = *parsedRole;
  result.hasText = object.contains(u"text"_s);
  if (const QJsonValue level = object.value(u"level"_s); !level.isUndefined()) {
    const auto parsedLevel = smallInteger(level, 1, 9);
    if (!parsedLevel) {
      return std::nullopt;
    }
    result.level = *parsedLevel;
  }
  if (const QJsonValue editable = object.value(u"editable"_s); !editable.isUndefined()) {
    if (!editable.isBool()) {
      return std::nullopt;
    }
    result.editable = editable.toBool();
  }
  if (const QJsonValue bounds = object.value(u"bounds"_s); !bounds.isUndefined()) {
    result.bounds = pageRect(bounds, pageCount);
    if (!result.bounds) {
      return std::nullopt;
    }
  }
  // Character boxes describe the node's own text, on the page of its bounds.
  if (const QJsonValue chars = object.value(u"chars"_s); !chars.isUndefined()) {
    auto boxes = result.hasText ? charBoxes(chars, result.text) : std::nullopt;
    if (!boxes) {
      return std::nullopt;
    }
    result.chars = std::move(*boxes);
  }
  return result;
}

QString pointerKindName(PointerKind kind) {
  switch (kind) {
  case PointerKind::Down:
    return u"down"_s;
  case PointerKind::Move:
    return u"move"_s;
  case PointerKind::Up:
    return u"up"_s;
  case PointerKind::Cancel:
    return u"cancel"_s;
  }
  return u"cancel"_s;
}

QString pointerTypeName(PointerType type) {
  switch (type) {
  case PointerType::Mouse:
    return u"mouse"_s;
  case PointerType::Pen:
    return u"pen"_s;
  case PointerType::Touch:
    return u"touch"_s;
  }
  return u"mouse"_s;
}

QString compositionPhaseName(CompositionPhase phase) {
  switch (phase) {
  case CompositionPhase::Start:
    return u"start"_s;
  case CompositionPhase::Update:
    return u"update"_s;
  case CompositionPhase::End:
    return u"end"_s;
  }
  return u"end"_s;
}

} // namespace

bool BluRect::intersects(const BluRect &other) const {
  return !isEmpty() && !other.isEmpty() && x < other.right() && other.x < right() && y < other.bottom() && other.y < bottom();
}

std::optional<BluRect> parseRect(const QJsonValue &value) {
  const QJsonObject object = value.toObject();
  const auto x = integer(object.value(u"x"_s), -kMaxCoordinate, kMaxCoordinate);
  const auto y = integer(object.value(u"y"_s), -kMaxCoordinate, kMaxCoordinate);
  const auto width = integer(object.value(u"width"_s), 0, kMaxCoordinate);
  const auto height = integer(object.value(u"height"_s), 0, kMaxCoordinate);
  if (!value.isObject() || !x || !y || !width || !height) {
    return std::nullopt;
  }
  return BluRect{.x = *x, .y = *y, .width = *width, .height = *height};
}

std::optional<std::vector<Page>> parsePages(const QJsonArray &pages) {
  if (pages.size() > kMaxPages) {
    return std::nullopt;
  }
  std::vector<Page> result;
  result.reserve(static_cast<std::size_t>(pages.size()));
  for (const auto &value : pages) {
    const QJsonObject object = value.toObject();
    const int expectedIndex = static_cast<int>(result.size());
    const auto index = smallInteger(object.value(u"index"_s), expectedIndex, expectedIndex);
    const auto width = integer(object.value(u"width"_s), 1, kMaxPageSide);
    const auto height = integer(object.value(u"height"_s), 1, kMaxPageSide);
    const auto section = smallInteger(object.value(u"section"_s), 0, std::numeric_limits<int>::max());
    const auto x = integer(object.value(u"x"_s), -kMaxCoordinate, kMaxCoordinate);
    const auto y = integer(object.value(u"y"_s), -kMaxCoordinate, kMaxCoordinate);
    if (!value.isObject() || !index || !width || !height || !section || !x || !y) {
      return std::nullopt;
    }
    result.push_back(Page{.index = *index, .width = *width, .height = *height, .section = *section, .x = *x, .y = *y});
  }
  return result;
}

std::optional<std::vector<PageRect>> parseRegions(const QJsonArray &regions, int pageCount) {
  std::vector<PageRect> result;
  result.reserve(static_cast<std::size_t>(regions.size()));
  for (const auto &value : regions) {
    const auto region = pageRect(value, pageCount);
    if (!region) {
      return std::nullopt;
    }
    result.push_back(*region);
  }
  return result;
}

std::optional<Overlay> parseOverlay(const QJsonObject &payload, int pageCount) {
  Overlay result;
  const QJsonValue caret = payload.value(u"caret"_s);
  if (caret.isObject()) {
    const QJsonObject object = caret.toObject();
    const auto page = smallInteger(object.value(u"page"_s), 0, pageCount - 1);
    const auto rect = parseRect(object.value(u"rect"_s));
    const QJsonValue visible = object.value(u"visible"_s);
    if (!page || !rect || !visible.isBool()) {
      return std::nullopt;
    }
    result.caret = Caret{.page = *page, .rect = *rect, .visible = visible.toBool()};
  } else if (!caret.isNull()) {
    return std::nullopt;
  }
  const QJsonValue selection = payload.value(u"selection"_s);
  if (!selection.isArray()) {
    return std::nullopt;
  }
  for (const auto &perPage : selection.toArray()) {
    const QJsonObject object = perPage.toObject();
    const auto page = smallInteger(object.value(u"page"_s), 0, pageCount - 1);
    const QJsonValue rects = object.value(u"rects"_s);
    if (!page || !rects.isArray()) {
      return std::nullopt;
    }
    for (const auto &rectValue : rects.toArray()) {
      const auto rect = parseRect(rectValue);
      if (!rect || std::cmp_greater_equal(result.selection.size(), kMaxSelectionRects)) {
        return std::nullopt;
      }
      result.selection.push_back(PageRect{.page = *page, .rect = *rect});
    }
  }
  return result;
}

std::optional<AccessibleTree> parseAccessibleTree(const QJsonValue &value, int pageCount) {
  // Depth first with a list of nodes still to read, instead of recursion: the engine decides how deep the tree is.
  std::vector<PendingNode> pending{{.value = value, .depth = 1}};
  std::map<quint64, AccessibleNode> nodes;
  quint64 root = 0;
  while (!pending.empty()) {
    const PendingNode current = std::move(pending.back());
    pending.pop_back();
    const QJsonObject object = current.value.toObject();
    auto node = current.value.isObject() ? nodeFields(object, pageCount) : std::nullopt;
    if (!node || current.depth > kMaxTreeDepth || !readChildren(object, current.depth, nodes.size() + 1, *node, pending)) {
      return std::nullopt;
    }
    const quint64 id = node->id;
    root = root == 0 ? id : root;
    // Identifiers are unique; as every child is read from the same JSON as its parent's list, the nodes form a tree.
    if (!nodes.emplace(id, std::move(*node)).second || std::cmp_greater(nodes.size(), kMaxTreeNodes)) {
      return std::nullopt;
    }
  }
  return AccessibleTree(root, std::move(nodes));
}

const AccessibleNode &AccessibleTree::root() const { return nodes_.find(root_)->second; }

const AccessibleNode *AccessibleTree::find(quint64 id) const {
  const auto found = nodes_.find(id);
  return found == nodes_.end() ? nullptr : &found->second;
}

std::vector<const AccessibleNode *> AccessibleTree::readingOrder() const {
  std::vector<const AccessibleNode *> order;
  order.reserve(nodes_.size());
  std::vector<quint64> pending{root_};
  while (!pending.empty()) {
    const AccessibleNode *node = find(pending.back());
    pending.pop_back();
    if (node == nullptr) {
      continue;
    }
    order.push_back(node);
    pending.insert(pending.end(), node->children.rbegin(), node->children.rend());
  }
  return order;
}

AccessibleTree::Subtree AccessibleTree::subtree(quint64 id) const {
  Subtree result;
  std::vector<std::pair<quint64, int>> pending{{id, 1}};
  while (!pending.empty()) {
    const auto [current, depth] = pending.back();
    pending.pop_back();
    const AccessibleNode *node = find(current);
    if (node == nullptr) {
      continue;
    }
    result.ids.push_back(current);
    result.height = std::max(result.height, depth);
    for (const quint64 child : node->children) {
      pending.emplace_back(child, depth + 1);
    }
  }
  return result;
}

int AccessibleTree::depthOf(quint64 id) const {
  std::vector<std::pair<quint64, int>> pending{{root_, 1}};
  while (!pending.empty()) {
    const auto [current, depth] = pending.back();
    pending.pop_back();
    if (current == id) {
      return depth;
    }
    if (const AccessibleNode *node = find(current)) {
      for (const quint64 child : node->children) {
        pending.emplace_back(child, depth + 1);
      }
    }
  }
  return 0;
}

bool AccessibleTree::replace(const AccessibleTree &subtree) {
  const int depth = depthOf(subtree.root_);
  if (depth == 0) {
    return false;
  }
  const Subtree leaving = this->subtree(subtree.root_);
  const Subtree arriving = subtree.subtree(subtree.root_);
  if (depth + arriving.height - 1 > kMaxTreeDepth ||
      std::cmp_greater(nodes_.size() - leaving.ids.size() + subtree.nodes_.size(), kMaxTreeNodes)) {
    return false;
  }
  // The new subtree may reuse its own old identifiers, but none from elsewhere in the tree.
  const std::set<quint64> leavingIds(leaving.ids.begin(), leaving.ids.end());
  for (const auto &[id, node] : subtree.nodes_) {
    if (nodes_.contains(id) && !leavingIds.contains(id)) {
      return false;
    }
  }
  for (const quint64 id : leaving.ids) {
    nodes_.erase(id);
  }
  nodes_.insert(subtree.nodes_.begin(), subtree.nodes_.end());
  return true;
}

std::optional<std::optional<AccessibleCaret>> parseAccessibleCaret(const QJsonValue &value) {
  if (value.isNull()) {
    return std::optional<AccessibleCaret>();
  }
  const QJsonObject object = value.toObject();
  const auto nodeId = integer(object.value(u"node"_s), 1, kMaxExactInteger);
  const auto offset = smallInteger(object.value(u"offset"_s), 0, std::numeric_limits<int>::max());
  if (!value.isObject() || !nodeId || !offset) {
    return std::nullopt;
  }
  return std::optional<AccessibleCaret>(AccessibleCaret{.node = static_cast<quint64>(*nodeId), .offset = *offset});
}

QJsonObject toJson(const Modifiers &modifiers) {
  return QJsonObject{{u"shift"_s, modifiers.shift}, {u"ctrl"_s, modifiers.ctrl}, {u"alt"_s, modifiers.alt}, {u"meta"_s, modifiers.meta}};
}

QJsonObject toJson(const BluRect &rect) {
  return QJsonObject{{u"x"_s, rect.x}, {u"y"_s, rect.y}, {u"width"_s, rect.width}, {u"height"_s, rect.height}};
}

QJsonObject keyPayload(const KeyInput &key) {
  return QJsonObject{{u"key"_s, key.key},
                     {u"code"_s, key.code},
                     {u"modifiers"_s, toJson(key.modifiers)},
                     {u"repeat"_s, key.repeat},
                     {u"composing"_s, key.composing}};
}

QJsonObject pointerPayload(const PointerInput &pointer) {
  return QJsonObject{{u"kind"_s, pointerKindName(pointer.kind)},
                     {u"x"_s, pointer.position.x()},
                     {u"y"_s, pointer.position.y()},
                     {u"button"_s, pointer.button},
                     {u"clicks"_s, pointer.clicks},
                     {u"modifiers"_s, toJson(pointer.modifiers)},
                     {u"pointer_type"_s, pointerTypeName(pointer.pointerType)}};
}

QJsonObject compositionPayload(CompositionPhase phase, const QString &text, int selectionStart, int selectionEnd) {
  QJsonObject payload{{u"phase"_s, compositionPhaseName(phase)}, {u"text"_s, text}};
  if (phase == CompositionPhase::Update) {
    payload.insert(u"selection"_s, QJsonObject{{u"start"_s, selectionStart}, {u"end"_s, selectionEnd}});
  }
  return payload;
}

} // namespace bayan::protocol
