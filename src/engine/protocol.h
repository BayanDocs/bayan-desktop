// The engine's protocol data as C++ values: the handshake, the UI manifest's key data, pages, overlays, the accessibility tree and input
// events (engine protocol §5, §6, §7, §14).
//
// The shell receives these as JSON and keeps them only to draw and to relay: it places pages and overlays where the engine says, and hands
// the accessibility tree to the platform's screen readers. It never derives document behaviour from them (ADR-0013 §3). Every parser checks
// sizes and ranges, because messages from the engine are treated as untrusted input like any other (AGENTS.md §6): a malformed value makes
// the whole message invalid rather than being guessed at, and the shell then stops using the engine (engine protocol §14.1 item 5).

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QtTypes>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bayan::protocol {

// A device-independent pixel is 1/96 inch, and an inch is 1,828,800 BLU (ADR-0005, engine protocol §7).
inline constexpr qint64 kBluPerDip = 19'050;
// The largest coordinate the shell accepts in geometry from the engine: the engine keeps tile rectangles within ±2^40 BLU (§13).
inline constexpr qint64 kMaxCoordinate = static_cast<qint64>(quint64{1} << 40U);
// Page sizes larger than this (about 140 metres) are treated as a broken message rather than displayed.
inline constexpr qint64 kMaxPageSide = static_cast<qint64>(quint64{1} << 38U);
// The most pages a document may announce; more is treated as a broken message.
inline constexpr int kMaxPages = 1'000'000;
// The longest engine version (engine protocol §14.1 item 6): at most 64 ASCII characters.
inline constexpr int kMaxEngineVersionLength = 64;

// welcome, the reply to hello (engine protocol §5, §14.1).
struct Welcome {
  int protocolVersion = 0;
  // The core's version, at most kMaxEngineVersionLength printable ASCII characters.
  QString engineVersion;
  qint64 layoutEpoch = 0;
  QStringList features;
};
[[nodiscard]] std::optional<Welcome> parseWelcome(const QJsonObject &payload);

// A render revision (engine protocol §14.6): a positive integer below 2^53. Nothing if the value is not one.
[[nodiscard]] std::optional<quint64> parseRevision(const QJsonValue &value);

// A rectangle in BLU, in page coordinates or view space. Width and height are never negative.
struct BluRect {
  qint64 x = 0;
  qint64 y = 0;
  qint64 width = 0;
  qint64 height = 0;

  [[nodiscard]] qint64 right() const { return x + width; }
  [[nodiscard]] qint64 bottom() const { return y + height; }
  [[nodiscard]] bool isEmpty() const { return width <= 0 || height <= 0; }
  // Whether the two rectangles share an area (touching edges do not count).
  [[nodiscard]] bool intersects(const BluRect &other) const;
  friend bool operator==(const BluRect &, const BluRect &) = default;
};

// One page of view.pages: its size and where it sits in view space (§7).
struct Page {
  int index = 0;
  qint64 width = 0;
  qint64 height = 0;
  int section = 0;
  qint64 x = 0;
  qint64 y = 0;
  friend bool operator==(const Page &, const Page &) = default;
};

// A rectangle on one page, in that page's coordinates.
struct PageRect {
  int page = 0;
  BluRect rect{};
  friend bool operator==(const PageRect &, const PageRect &) = default;
};

// The caret of overlay.update.
struct Caret {
  int page = 0;
  BluRect rect{};
  bool visible = false;
  friend bool operator==(const Caret &, const Caret &) = default;
};

// What overlay.update asks the shell to draw over the tiles. Remote cursors and find highlights belong to later phases and are not kept.
struct Overlay {
  std::optional<Caret> caret;
  std::vector<PageRect> selection;
  friend bool operator==(const Overlay &, const Overlay &) = default;
};

// One entry of the UI manifest's key data (engine protocol §14.3): a binding of a command, a document key or a reserved key. It names the
// key by key, the DOM's KeyboardEvent.key, or by code, the DOM's KeyboardEvent.code (exactly one of them is not empty), and gives the
// modifiers that matter; one that it leaves out matches either state. A binding gives all four, and label, the binding as the platform
// writes it, formatted by the engine; the shell shows it where it shows the shortcut and never builds such text itself.
struct KeyEntry {
  QString key;
  QString code;
  std::optional<bool> ctrl;
  std::optional<bool> shift;
  std::optional<bool> alt;
  std::optional<bool> meta;
  QString label;
  friend bool operator==(const KeyEntry &, const KeyEntry &) = default;
};

// A command of the UI manifest, with its bindings resolved for the session's platform and runtime.
struct ManifestCommand {
  QString id;
  QString label;
  std::vector<KeyEntry> bindings;
  // For a command that the shell carries out itself through the platform: copy, cut or paste in v1, or a value of a later manifest.
  std::optional<QString> shellAction;
  friend bool operator==(const ManifestCommand &, const ManifestCommand &) = default;
};

// The key data of the UI manifest (engine protocol §6.8, §14.3): the commands with their bindings, the keys that the engine handles in the
// document area with any modifiers, and the presses that the shell keeps.
struct UiManifest {
  std::vector<ManifestCommand> commands;
  std::vector<KeyEntry> documentKeys;
  std::vector<KeyEntry> reservedKeys;
  friend bool operator==(const UiManifest &, const UiManifest &) = default;
};
// The reply to ui.manifest. Its ribbon must be an object, as the protocol requires, but is not read yet.
[[nodiscard]] std::optional<UiManifest> parseUiManifest(const QJsonObject &payload);

// The roles of accessibility nodes (§6.6). Roles that a later engine adds arrive as Other, so an older shell keeps working (§9).
enum class Role : std::uint8_t { Document, Heading, Paragraph, List, ListItem, Table, Row, ColumnHeader, Cell, Other };

// The box of one character of a node's text: offsets in UTF-16 code units, the rectangle on the page of the node's bounds.
struct CharBox {
  int start = 0;
  int end = 0;
  BluRect rect{};
  friend bool operator==(const CharBox &, const CharBox &) = default;
};

// A node of the engine's accessibility tree (§6.6). Its children are named by their identifiers, in reading order; the tree holds them.
// The engine may give a node with only a part of its children, when the whole would not fit into one message (§14.7 item 1): such a node
// has childCount, its number of children, and childIndices, the index of each child that the tree holds, in the order of children. The
// shell loads the other parts with query.a11y and a children range, and once the tree holds every child, both are empty again.
struct AccessibleNode {
  quint64 id = 0;
  Role role = Role::Other;
  int level = 0;
  QString name;
  // The node's own text; hasText tells an empty text (an empty editable line) from no text at all (a table).
  QString text;
  bool hasText = false;
  bool editable = false;
  std::optional<PageRect> bounds;
  std::vector<CharBox> chars;
  std::vector<quint64> children;
  std::optional<quint32> childCount;
  std::vector<quint32> childIndices;
  friend bool operator==(const AccessibleNode &, const AccessibleNode &) = default;
};

// Children that a node lacks: query.a11y { node, children: { start, count } } loads them (§14.7 item 1).
struct MissingChildren {
  quint64 node = 0;
  quint32 start = 0;
  quint32 count = 0;
  friend bool operator==(const MissingChildren &, const MissingChildren &) = default;
};

// The accessibility caret: a position in a node's text, in UTF-16 code units.
struct AccessibleCaret {
  quint64 node = 0;
  int offset = 0;
  friend bool operator==(const AccessibleCaret &, const AccessibleCaret &) = default;
};

// The limits of an accessibility tree (engine protocol §13, §14.7 item 1), which the engine keeps: at most 64 levels deep, the document
// node being level 1, and at most 200,000 nodes in one reply to query.a11y or one change of an a11y.update. A tree beyond them breaks the
// protocol. The tree a shell holds may have more nodes, once it has loaded the parts of nodes with many children.
inline constexpr int kMaxTreeDepth = 64;
inline constexpr int kMaxTreeNodes = 200'000;

// The engine's accessibility tree, kept flat: every node by its identifier, and the root's. Nothing walks it recursively, so a deep tree
// from the engine cannot exhaust the shell's stack. Only parseAccessibleTree creates trees, so every tree is well formed: identifiers are
// unique, every child exists and has one parent, no node holds more children than it has, and the depth limit holds.
class AccessibleTree {
public:
  [[nodiscard]] const AccessibleNode &root() const;
  [[nodiscard]] const AccessibleNode *find(quint64 id) const;
  [[nodiscard]] std::size_t size() const { return nodes_.size(); }
  // The nodes in reading order: depth first, each node before its children, as the engine lists them.
  [[nodiscard]] std::vector<const AccessibleNode *> readingOrder() const;
  // How deep a node sits: 1 for the root, which is the document node in the tree of a whole document; 0 if it is not in the tree.
  [[nodiscard]] int levelOf(quint64 id) const;
  // The children that nodes lack, in reading order, and for each node from its first child to its last.
  [[nodiscard]] std::vector<MissingChildren> missingChildren() const;
  // Replaces the node with subtree's root identifier, and everything below it, with subtree (a11y.update, kind "replace"). Returns false
  // and changes nothing if there is no such node, or if the result would exceed the depth limit or reuse an identifier from elsewhere.
  [[nodiscard]] bool replace(const AccessibleTree &subtree);
  // Adds a part of a node's children that query.a11y with a children range loaded: part's root is that node, as the engine has it now, with
  // the children of the range and their subtrees. Children of the node that the part holds replace those at the same indices; the node's
  // own fields and its number of children become the part's. Returns false and changes nothing if there is no such node, or if the result
  // would exceed the depth limit or reuse an identifier from elsewhere.
  [[nodiscard]] bool addChildren(const AccessibleTree &part);
  friend bool operator==(const AccessibleTree &, const AccessibleTree &) = default;

private:
  friend std::optional<AccessibleTree> parseAccessibleTree(const QJsonValue &value, int pageCount, int rootLevel);
  AccessibleTree(quint64 root, std::map<quint64, AccessibleNode> nodes) : root_(root), nodes_(std::move(nodes)) {}

  // A node and everything below it: their identifiers, and how many levels deep it goes (1 for a node without children).
  struct Subtree {
    std::vector<quint64> ids;
    int height = 0;
  };
  [[nodiscard]] Subtree subtree(quint64 id) const;
  // Whether part's nodes may join this tree: none of its identifiers is used elsewhere, outside the subtrees of the nodes leaving.
  [[nodiscard]] bool fits(const AccessibleTree &part, const std::vector<quint64> &leaving) const;

  quint64 root_ = 0;
  std::map<quint64, AccessibleNode> nodes_;
};

// Parsers. Each returns nothing if the value does not have the shape and ranges the protocol allows.
[[nodiscard]] std::optional<BluRect> parseRect(const QJsonValue &value);
// The pages of view.pages, in order; their index fields must count 0, 1, 2 and so on.
[[nodiscard]] std::optional<std::vector<Page>> parsePages(const QJsonArray &pages);
// The regions of render.invalidate. Pages are checked against pageCount.
[[nodiscard]] std::optional<std::vector<PageRect>> parseRegions(const QJsonArray &regions, int pageCount);
// The payload of overlay.update (without doc_id, which the caller checks).
[[nodiscard]] std::optional<Overlay> parseOverlay(const QJsonObject &payload, int pageCount);
// A node and its subtree: the root of query.a11y, or the node of an a11y.update change. rootLevel is the level of that node in the
// document's tree (1 for the document node), so that the depth limit counts from the document. Nothing if the tree is malformed, beyond the
// limits, or a node holds more children than it says it has (§14.7 item 1).
[[nodiscard]] std::optional<AccessibleTree> parseAccessibleTree(const QJsonValue &value, int pageCount, int rootLevel = 1);
// The caret of query.a11y and a11y.update: an object, or null for "no caret" (the outer optional is empty if the value is malformed).
[[nodiscard]] std::optional<std::optional<AccessibleCaret>> parseAccessibleCaret(const QJsonValue &value);

// The modifier keys of an input event, with the meanings of the DOM's KeyboardEvent (§6.5): meta is Command on macOS and the Windows key
// on Windows.
struct Modifiers {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool meta = false;
  friend bool operator==(const Modifiers &, const Modifiers &) = default;
};

// input.key: key and code use the DOM's KeyboardEvent values.
struct KeyInput {
  QString key{};
  QString code{};
  Modifiers modifiers{};
  bool repeat = false;
  bool composing = false;
  friend bool operator==(const KeyInput &, const KeyInput &) = default;
};

enum class PointerKind : std::uint8_t { Down, Move, Up, Cancel };
enum class PointerType : std::uint8_t { Mouse, Pen, Touch };

// input.pointer: the position is in device-independent pixels from the view origin (§6.5, §7).
struct PointerInput {
  PointerKind kind = PointerKind::Down;
  QPointF position{};
  int button = 0;
  int clicks = 0;
  Modifiers modifiers{};
  PointerType pointerType = PointerType::Mouse;
};

enum class CompositionPhase : std::uint8_t { Start, Update, End };

// The JSON payloads of the input messages, without doc_id, which the session adds.
[[nodiscard]] QJsonObject toJson(const Modifiers &modifiers);
[[nodiscard]] QJsonObject keyPayload(const KeyInput &key);
[[nodiscard]] QJsonObject pointerPayload(const PointerInput &pointer);
[[nodiscard]] QJsonObject compositionPayload(CompositionPhase phase, const QString &text, int selectionStart, int selectionEnd);
[[nodiscard]] QJsonObject toJson(const BluRect &rect);

} // namespace bayan::protocol
