#include "protocol.h"

#include <QtCore/QStringView>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan::protocol {
namespace {

// The largest integer a JSON number carries exactly in every language the engine talks to (engine protocol §4).
constexpr qint64 kMaxExactInteger = static_cast<qint64>(quint64{1} << 53U) - 1;
// The most selection rectangles an overlay may carry.
constexpr qsizetype kMaxSelectionRects = 100'000;
// The largest count and index of a node's children (the protocol's uint32).
constexpr qint64 kMaxChildIndex = std::numeric_limits<quint32>::max();

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

// An optional count or index of children: absent and null leave target as it is.
bool readChildIndex(const QJsonObject &object, const QString &key, std::optional<quint32> &target) {
  const QJsonValue field = object.value(key);
  if (field.isUndefined() || field.isNull()) {
    return true;
  }
  const auto value = integer(field, 0, kMaxChildIndex);
  if (!value) {
    return false;
  }
  target = static_cast<quint32>(*value);
  return true;
}

// Reads which of a node's children it holds (engine protocol §14.7 item 1): all of them, or, with child_count, an unbroken run from
// children_start (0 when absent), which must lie within child_count. A node whose run covers all of its children holds them all.
bool readChildRange(const QJsonObject &object, AccessibleNode &node) {
  std::optional<quint32> count;
  std::optional<quint32> start;
  if (!readChildIndex(object, u"child_count"_s, count) || !readChildIndex(object, u"children_start"_s, start)) {
    return false;
  }
  if (!count) {
    return true;
  }
  const qint64 first = node.children.empty() ? 0 : start.value_or(0);
  const auto held = static_cast<qint64>(node.children.size());
  if (first + held > qint64{*count}) {
    return false;
  }
  if (first == 0 && held == qint64{*count}) {
    return true;
  }
  node.childCount = count;
  node.childIndices.resize(node.children.size());
  std::iota(node.childIndices.begin(), node.childIndices.end(), static_cast<quint32>(first));
  return true;
}

// Reads a node's list of children: their identifiers go into the node, the children themselves onto the list of nodes still to read. Fails
// if the list is malformed, or if the tree would have more nodes than allowed, counting those read (knownNodes) and those waiting.
bool readChildren(const QJsonObject &object, int depth, std::size_t knownNodes, AccessibleNode &node, std::vector<PendingNode> &pending) {
  const QJsonValue children = object.value(u"children"_s);
  if (children.isUndefined() || children.isNull()) {
    return readChildRange(object, node);
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
  return readChildRange(object, node);
}

// Whether a value is an engine version as the protocol limits it (engine protocol §14.1 item 6): at most 64 ASCII characters. The shell
// also refuses an empty one and control characters, which no version as Semantic Versioning writes it contains.
bool isEngineVersion(const QJsonValue &value) {
  const QString version = value.toString();
  return value.isString() && !version.isEmpty() && version.size() <= kMaxEngineVersionLength &&
         std::ranges::all_of(version, [](QChar character) { return character.unicode() >= 0x20 && character.unicode() < 0x7f; });
}

// A modifier of a key entry: a boolean, or for an entry that may leave it out (a reserved key), absent or null.
bool readModifier(const QJsonObject &object, const QString &name, bool required, std::optional<bool> &target) {
  const QJsonValue value = object.value(name);
  if (value.isBool()) {
    target = value.toBool();
    return true;
  }
  return !required && (value.isUndefined() || value.isNull());
}

// The kinds of key entries of the UI manifest (engine protocol §14.3): a command's binding, with all four modifiers and a label; a reserved
// key, with the modifiers that matter; and a document key, which names a key and has no modifiers.
enum class KeyEntryKind : std::uint8_t { Binding, Reserved, Document };

std::optional<KeyEntry> keyEntry(const QJsonValue &value, KeyEntryKind kind) {
  if (!value.isObject()) {
    return std::nullopt;
  }
  const QJsonObject object = value.toObject();
  KeyEntry entry;
  // Exactly one of key and code names the key; a document key has only key. Neither may be empty: an entry always names a key.
  const QJsonValue key = object.value(u"key"_s);
  const QJsonValue code = kind == KeyEntryKind::Document ? QJsonValue() : object.value(u"code"_s);
  const bool byKey = !key.isUndefined() && !key.isNull();
  const bool byCode = !code.isUndefined() && !code.isNull();
  const QJsonValue &name = byKey ? key : code;
  if (byKey == byCode || !name.isString() || name.toString().isEmpty()) {
    return std::nullopt;
  }
  (byKey ? entry.key : entry.code) = name.toString();
  if (kind == KeyEntryKind::Document) {
    return entry;
  }
  const bool binding = kind == KeyEntryKind::Binding;
  if (!readModifier(object, u"ctrl"_s, binding, entry.ctrl) || !readModifier(object, u"shift"_s, binding, entry.shift) ||
      !readModifier(object, u"alt"_s, binding, entry.alt) || !readModifier(object, u"meta"_s, binding, entry.meta)) {
    return std::nullopt;
  }
  if (binding) {
    const QJsonValue label = object.value(u"label"_s);
    if (!label.isString()) {
      return std::nullopt;
    }
    entry.label = label.toString();
  }
  return entry;
}

std::optional<std::vector<KeyEntry>> keyEntries(const QJsonValue &value, KeyEntryKind kind) {
  if (!value.isArray()) {
    return std::nullopt;
  }
  std::vector<KeyEntry> entries;
  for (const auto &item : value.toArray()) {
    auto entry = keyEntry(item, kind);
    if (!entry) {
      return std::nullopt;
    }
    entries.push_back(std::move(*entry));
  }
  return entries;
}

std::optional<ManifestCommand> manifestCommand(const QJsonValue &value) {
  const QJsonObject object = value.toObject();
  const QJsonValue id = object.value(u"id"_s);
  const QJsonValue label = object.value(u"label"_s);
  const QJsonValue shellAction = object.value(u"shell_action"_s);
  auto bindings = keyEntries(object.value(u"bindings"_s), KeyEntryKind::Binding);
  if (!value.isObject() || !id.isString() || !label.isString() || !bindings ||
      !(shellAction.isUndefined() || shellAction.isNull() || shellAction.isString())) {
    return std::nullopt;
  }
  return ManifestCommand{
      .id = id.toString(),
      .label = label.toString(),
      .bindings = std::move(*bindings),
      .shellAction = shellAction.isString() ? std::optional<QString>(shellAction.toString()) : std::nullopt,
  };
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
  // The revision that the rectangles belong to (engine protocol §14.6), and the remote cursors and find highlights of later phases, which
  // the shell does not draw yet, must be there all the same.
  if (!parseRevision(payload.value(u"revision"_s)) || !payload.value(u"remote"_s).isArray() || !payload.value(u"highlights"_s).isArray()) {
    return std::nullopt;
  }
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

std::optional<Welcome> parseWelcome(const QJsonObject &payload) {
  const auto version = smallInteger(payload.value(u"protocol_version"_s), 0, std::numeric_limits<int>::max());
  const QJsonValue engineVersion = payload.value(u"engine_version"_s);
  const auto epoch = integer(payload.value(u"layout_epoch"_s), 0, kMaxExactInteger);
  const QJsonValue features = payload.value(u"features"_s);
  if (!version || !isEngineVersion(engineVersion) || !epoch || !features.isArray()) {
    return std::nullopt;
  }
  Welcome welcome{.protocolVersion = *version, .engineVersion = engineVersion.toString(), .layoutEpoch = *epoch, .features = {}};
  for (const auto &feature : features.toArray()) {
    if (!feature.isString()) {
      return std::nullopt;
    }
    welcome.features.append(feature.toString());
  }
  return welcome;
}

std::optional<quint64> parseRevision(const QJsonValue &value) {
  const auto revision = integer(value, 1, kMaxExactInteger);
  return revision ? std::optional<quint64>(static_cast<quint64>(*revision)) : std::nullopt;
}

std::optional<UiManifest> parseUiManifest(const QJsonObject &payload) {
  const QJsonValue commands = payload.value(u"commands"_s);
  const QJsonObject keys = payload.value(u"keys"_s).toObject();
  auto documentKeys = keyEntries(keys.value(u"document"_s), KeyEntryKind::Document);
  auto reservedKeys = keyEntries(keys.value(u"reserved"_s), KeyEntryKind::Reserved);
  if (!integer(payload.value(u"manifest_version"_s), 0, kMaxChildIndex) || !payload.value(u"locale"_s).isString() || !commands.isArray() ||
      !payload.value(u"keys"_s).isObject() || !documentKeys || !reservedKeys || !payload.value(u"ribbon"_s).isObject()) {
    return std::nullopt;
  }
  UiManifest manifest{.commands = {}, .documentKeys = std::move(*documentKeys), .reservedKeys = std::move(*reservedKeys)};
  for (const auto &value : commands.toArray()) {
    auto command = manifestCommand(value);
    if (!command) {
      return std::nullopt;
    }
    manifest.commands.push_back(std::move(*command));
  }
  return manifest;
}

std::optional<AccessibleTree> parseAccessibleTree(const QJsonValue &value, int pageCount, int rootLevel) {
  // Depth first with a list of nodes still to read, instead of recursion: the engine decides how deep the tree is. Levels count from the
  // document node, so that the root of a subtree starts at its own level.
  if (rootLevel < 1 || rootLevel > kMaxTreeDepth) {
    return std::nullopt;
  }
  std::vector<PendingNode> pending{{.value = value, .depth = rootLevel}};
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

int AccessibleTree::levelOf(quint64 id) const {
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

std::vector<MissingChildren> AccessibleTree::missingChildren() const {
  std::vector<MissingChildren> missing;
  for (const AccessibleNode *node : readingOrder()) {
    if (!node->childCount) {
      continue;
    }
    const quint32 count = *node->childCount;
    // childIndices rise; every gap between them, and after the last, is missing.
    quint32 next = 0;
    for (const quint32 index : node->childIndices) {
      if (index > next) {
        missing.push_back({.node = node->id, .start = next, .count = index - next});
      }
      next = index + 1;
    }
    if (next < count) {
      missing.push_back({.node = node->id, .start = next, .count = count - next});
    }
  }
  return missing;
}

bool AccessibleTree::fits(const AccessibleTree &part, const std::vector<quint64> &leaving) const {
  // The part's nodes may reuse identifiers of the nodes leaving, and its root may be a node that stays, but none from elsewhere.
  const std::set<quint64> leavingIds(leaving.begin(), leaving.end());
  return std::ranges::none_of(part.nodes_, [this, &part, &leavingIds](const auto &item) {
    return item.first != part.root_ && nodes_.contains(item.first) && !leavingIds.contains(item.first);
  });
}

bool AccessibleTree::replace(const AccessibleTree &subtree) {
  const int level = levelOf(subtree.root_);
  if (level == 0) {
    return false;
  }
  const Subtree leaving = this->subtree(subtree.root_);
  const Subtree arriving = subtree.subtree(subtree.root_);
  if (level + arriving.height - 1 > kMaxTreeDepth || !fits(subtree, leaving.ids)) {
    return false;
  }
  for (const quint64 id : leaving.ids) {
    nodes_.erase(id);
  }
  nodes_.insert(subtree.nodes_.begin(), subtree.nodes_.end());
  return true;
}

bool AccessibleTree::addChildren(const AccessibleTree &part) {
  const int level = levelOf(part.root_);
  if (level == 0 || level + part.subtree(part.root_).height - 1 > kMaxTreeDepth) {
    return false;
  }
  const AccessibleNode &incoming = part.root();
  AccessibleNode &node = nodes_.find(part.root_)->second;
  // Every child of the node by its index, then those of the part in their place; children at indices the node no longer has go too.
  const auto indicesOf = [](const AccessibleNode &of) {
    std::vector<quint32> indices = of.childIndices;
    if (!of.childCount) {
      indices.resize(of.children.size());
      std::iota(indices.begin(), indices.end(), quint32{0});
    }
    return indices;
  };
  const quint64 total = incoming.childCount ? *incoming.childCount : incoming.children.size();
  std::map<quint32, quint64> children;
  const std::vector<quint32> held = indicesOf(node);
  for (std::size_t i = 0; i < held.size(); ++i) {
    children.emplace(held.at(i), node.children.at(i));
  }
  std::vector<quint64> leaving;
  const std::vector<quint32> arriving = indicesOf(incoming);
  for (std::size_t i = 0; i < arriving.size(); ++i) {
    if (const auto old = children.find(arriving.at(i)); old != children.end()) {
      leaving.push_back(old->second);
    }
    children.insert_or_assign(arriving.at(i), incoming.children.at(i));
  }
  for (auto child = children.lower_bound(static_cast<quint32>(std::min<quint64>(total, kMaxChildIndex))); child != children.end();) {
    leaving.push_back(child->second);
    child = children.erase(child);
  }
  // Each child replaced, or beyond the node's children now, leaves with its subtree; the part may bring the same identifiers back.
  std::vector<quint64> leavingNodes;
  for (const quint64 child : leaving) {
    const Subtree gone = subtree(child);
    leavingNodes.insert(leavingNodes.end(), gone.ids.begin(), gone.ids.end());
  }
  if (!fits(part, leavingNodes)) {
    return false;
  }
  for (const quint64 id : leavingNodes) {
    nodes_.erase(id);
  }
  for (const auto &[id, partNode] : part.nodes_) {
    if (id != part.root_) {
      nodes_.insert_or_assign(id, partNode);
    }
  }
  // The node's own fields as the engine has them now, and the children it holds.
  AccessibleNode updated = incoming;
  updated.children.clear();
  updated.childIndices.clear();
  for (const auto &[index, child] : children) {
    updated.childIndices.push_back(index);
    updated.children.push_back(child);
  }
  updated.childCount.reset();
  if (std::cmp_less(updated.children.size(), total)) {
    updated.childCount = static_cast<quint32>(total);
  } else {
    updated.childIndices.clear();
  }
  nodes_.insert_or_assign(part.root_, std::move(updated));
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
  return QJsonObject{
      {u"key"_s, key.key},       {u"code"_s, key.code},           {u"modifiers"_s, toJson(key.modifiers)},
      {u"repeat"_s, key.repeat}, {u"composing"_s, key.composing},
  };
}

QJsonObject pointerPayload(const PointerInput &pointer) {
  return QJsonObject{
      {u"kind"_s, pointerKindName(pointer.kind)},
      {u"x"_s, pointer.position.x()},
      {u"y"_s, pointer.position.y()},
      {u"button"_s, pointer.button},
      {u"clicks"_s, pointer.clicks},
      {u"modifiers"_s, toJson(pointer.modifiers)},
      {u"pointer_type"_s, pointerTypeName(pointer.pointerType)},
  };
}

QJsonObject compositionPayload(CompositionPhase phase, const QString &text, int selectionStart, int selectionEnd) {
  QJsonObject payload{{u"phase"_s, compositionPhaseName(phase)}, {u"text"_s, text}};
  if (phase == CompositionPhase::Update) {
    payload.insert(u"selection"_s, QJsonObject{{u"start"_s, selectionStart}, {u"end"_s, selectionEnd}});
  }
  return payload;
}

} // namespace bayan::protocol
