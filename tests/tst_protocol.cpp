// Unit tests for the protocol values and parsers (src/engine/protocol.h). Messages from the engine are untrusted input (AGENTS.md §6), so
// most of these tests check that a malformed value is refused as a whole rather than guessed at.

#include "protocol.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace bayan::protocol;

namespace {

QJsonObject rect(QJsonValue x, QJsonValue y, QJsonValue width, QJsonValue height) {
  return QJsonObject{{u"x"_s, x}, {u"y"_s, y}, {u"width"_s, width}, {u"height"_s, height}};
}

QJsonObject page(int index, qint64 y) {
  return QJsonObject{{u"index"_s, index}, {u"width"_s, 15'544'800}, {u"height"_s, 20'116'800}, {u"section"_s, 0}, {u"x"_s, 0}, {u"y"_s, y}};
}

QJsonObject textNode(qint64 id, const QString &role, const QString &text) {
  return QJsonObject{{u"id"_s, id}, {u"role"_s, role}, {u"text"_s, text}};
}

// A tree shaped like the mock document's: a document with a heading, a table without text, and an editable line with character boxes.
QJsonObject sampleTree() {
  QJsonObject heading = textNode(2, u"heading"_s, u"Title"_s);
  heading.insert(u"level"_s, 1);
  heading.insert(u"bounds"_s, QJsonObject{{u"page"_s, 0}, {u"rect"_s, rect(10, 20, 300, 40)}});
  const QJsonObject cell = textNode(5, u"cell"_s, u"A"_s);
  const QJsonObject row{{u"id"_s, 4}, {u"role"_s, u"row"_s}, {u"children"_s, QJsonArray{cell}}};
  const QJsonObject table{{u"id"_s, 3}, {u"role"_s, u"table"_s}, {u"children"_s, QJsonArray{row}}};
  QJsonObject line = textNode(6, u"paragraph"_s, u"ab"_s);
  line.insert(u"editable"_s, true);
  line.insert(u"bounds"_s, QJsonObject{{u"page"_s, 1}, {u"rect"_s, rect(0, 0, 1000, 100)}});
  line.insert(u"chars"_s, QJsonArray{QJsonObject{{u"start"_s, 0}, {u"end"_s, 1}, {u"rect"_s, rect(0, 0, 10, 100)}},
                                     QJsonObject{{u"start"_s, 1}, {u"end"_s, 2}, {u"rect"_s, rect(10, 0, 10, 100)}}});
  return QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"name"_s, u"Sample"_s}, {u"children"_s, QJsonArray{heading, table, line}}};
}

// A chain of nested nodes, depth levels deep, whose root has the identifier rootId and the others 1000 and up.
QJsonObject chain(int depth, qint64 rootId = 1) {
  QJsonObject node = textNode(depth == 1 ? rootId : 1000 + depth, u"paragraph"_s, u"x"_s);
  for (int level = depth - 1; level >= 1; --level) {
    QJsonObject parent{{u"id"_s, level == 1 ? rootId : 1000 + level}, {u"role"_s, u"paragraph"_s}, {u"children"_s, QJsonArray{node}}};
    node = parent;
  }
  return node;
}

} // namespace

class TestProtocol final : public QObject {
  Q_OBJECT

private slots:
  void rectanglesAreExactIntegersInRange();
  void rectanglesIntersectOnlyWithArea();
  void pagesCountFromZeroWithSaneSizes();
  void regionsMustNameExistingPages();
  void overlaysHaveACaretOrNullAndSelectionsPerPage();
  void readsAnAccessibilityTree();
  void refusesMalformedAccessibilityTrees();
  void caretIsAPositionOrNull();
  void replacingANodeKeepsTheTreeValid();
  void inputPayloadsHaveTheProtocolsShape();
};

void TestProtocol::rectanglesAreExactIntegersInRange() {
  QCOMPARE(parseRect(rect(1, -2, 3, 4)), (BluRect{.x = 1, .y = -2, .width = 3, .height = 4}));
  QCOMPARE(parseRect(rect(0, 0, 0, 0)), BluRect{});                            // empty rectangles are well formed
  QVERIFY(!parseRect(rect(1.5, 0, 1, 1)));                                     // not an integer
  QVERIFY(!parseRect(rect(0, 0, -1, 1)));                                      // negative width
  QVERIFY(!parseRect(rect(u"1"_s, 0, 1, 1)));                                  // a string
  QVERIFY(!parseRect(QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 1}})); // a field missing
  QVERIFY(!parseRect(rect(static_cast<double>(kMaxCoordinate) * 2, 0, 1, 1))); // beyond the coordinate limit
  QVERIFY(!parseRect(rect(std::pow(2.0, 60), 0, 1, 1)));                       // beyond what a JSON number carries exactly
  QVERIFY(!parseRect(QJsonValue(QJsonArray{0, 0, 1, 1})));
  QVERIFY(!parseRect(QJsonValue()));
}

void TestProtocol::rectanglesIntersectOnlyWithArea() {
  const BluRect a{.x = 0, .y = 0, .width = 10, .height = 10};
  QVERIFY(a.intersects({.x = 9, .y = 9, .width = 5, .height = 5}));
  QVERIFY(!a.intersects({.x = 10, .y = 0, .width = 5, .height = 5})); // touching edges
  QVERIFY(!a.intersects({.x = 2, .y = 2, .width = 0, .height = 5}));  // empty
  QVERIFY(a.intersects({.x = -5, .y = -5, .width = 100, .height = 100}));
}

void TestProtocol::pagesCountFromZeroWithSaneSizes() {
  const auto pages = parsePages(QJsonArray{page(0, 0), page(1, 21'000'000)}).value_or(std::vector<Page>{});
  QCOMPARE(pages.size(), std::size_t{2});
  QCOMPARE(pages.at(1), (Page{.index = 1, .width = 15'544'800, .height = 20'116'800, .section = 0, .x = 0, .y = 21'000'000}));
  QCOMPARE(parsePages(QJsonArray{}), std::vector<Page>{});
  QVERIFY(!parsePages(QJsonArray{page(1, 0)}));             // must start at 0
  QVERIFY(!parsePages(QJsonArray{page(0, 0), page(0, 1)})); // must count up
  QJsonObject flat = page(0, 0);
  flat.insert(u"height"_s, 0);
  QVERIFY(!parsePages(QJsonArray{flat}));
  QJsonObject huge = page(0, 0);
  huge.insert(u"width"_s, static_cast<double>(kMaxPageSide) * 2);
  QVERIFY(!parsePages(QJsonArray{huge}));
  QJsonObject placeless = page(0, 0);
  placeless.remove(u"x"_s);
  QVERIFY(!parsePages(QJsonArray{placeless}));
  QVERIFY(!parsePages(QJsonArray{u"page"_s}));
}

void TestProtocol::regionsMustNameExistingPages() {
  const QJsonObject region{{u"page"_s, 1}, {u"rect"_s, rect(0, 0, 5, 5)}};
  QCOMPARE(parseRegions(QJsonArray{region}, 2), (std::vector<PageRect>{{.page = 1, .rect = {.x = 0, .y = 0, .width = 5, .height = 5}}}));
  QVERIFY(!parseRegions(QJsonArray{region}, 1));
  QVERIFY(!parseRegions(QJsonArray{region}, 0));
  QVERIFY(!parseRegions(QJsonArray{QJsonObject{{u"page"_s, 0}}}, 2)); // no rectangle
  QCOMPARE(parseRegions(QJsonArray{}, 0), std::vector<PageRect>{});
}

void TestProtocol::overlaysHaveACaretOrNullAndSelectionsPerPage() {
  const QJsonObject caret{{u"page"_s, 0}, {u"rect"_s, rect(1, 2, 3, 4)}, {u"visible"_s, true}};
  const QJsonArray selection{QJsonObject{{u"page"_s, 1}, {u"rects"_s, QJsonArray{rect(0, 0, 1, 1), rect(2, 0, 1, 1)}}}};
  const Overlay expected{.caret = Caret{.page = 0, .rect = {.x = 1, .y = 2, .width = 3, .height = 4}, .visible = true},
                         .selection = {{.page = 1, .rect = {.x = 0, .y = 0, .width = 1, .height = 1}},
                                       {.page = 1, .rect = {.x = 2, .y = 0, .width = 1, .height = 1}}}};
  QCOMPARE(parseOverlay(QJsonObject{{u"caret"_s, caret}, {u"selection"_s, selection}}, 2), expected);
  QCOMPARE(parseOverlay(QJsonObject{{u"caret"_s, QJsonValue::Null}, {u"selection"_s, QJsonArray{}}}, 1), Overlay{});

  QJsonObject hidden = caret;
  hidden.remove(u"visible"_s);
  QVERIFY(!parseOverlay(QJsonObject{{u"caret"_s, hidden}, {u"selection"_s, QJsonArray{}}}, 1));
  QVERIFY(!parseOverlay(QJsonObject{{u"caret"_s, caret}, {u"selection"_s, QJsonArray{}}}, 0)); // no such page
  QVERIFY(!parseOverlay(QJsonObject{{u"caret"_s, QJsonValue::Null}}, 1));                      // no selection
  QVERIFY(!parseOverlay(QJsonObject{{u"selection"_s, QJsonArray{}}}, 1));                      // caret missing, not null
  QVERIFY(!parseOverlay(QJsonObject{{u"caret"_s, QJsonValue::Null}, {u"selection"_s, selection}}, 1));
}

void TestProtocol::readsAnAccessibilityTree() {
  const auto tree = parseAccessibleTree(sampleTree(), 2);
  if (!tree) {
    QFAIL("the sample tree was refused");
  }
  QCOMPARE(tree->size(), std::size_t{6});
  const AccessibleNode &root = tree->root();
  QCOMPARE(root.role, Role::Document);
  QCOMPARE(root.name, u"Sample"_s);
  QVERIFY(!root.hasText);
  QCOMPARE(root.children, (std::vector<quint64>{2, 3, 6}));
  // Reading order: depth first, each node before its children.
  std::vector<quint64> order;
  for (const AccessibleNode *node : tree->readingOrder()) {
    order.push_back(node->id);
  }
  QCOMPARE(order, (std::vector<quint64>{1, 2, 3, 4, 5, 6}));
  const AccessibleNode *heading = tree->find(2);
  QVERIFY(heading != nullptr && heading->role == Role::Heading && heading->level == 1);
  QVERIFY(heading->hasText && heading->text == u"Title"_s);
  QCOMPARE(heading->bounds, (std::optional<PageRect>{{.page = 0, .rect = {.x = 10, .y = 20, .width = 300, .height = 40}}}));
  const AccessibleNode *table = tree->find(3);
  QVERIFY(table != nullptr && table->role == Role::Table && !table->hasText);
  QCOMPARE(tree->find(5)->role, Role::Cell);
  const AccessibleNode *line = tree->find(6);
  QVERIFY(line != nullptr && line->editable);
  QCOMPARE(line->chars.size(), std::size_t{2});
  QCOMPARE(line->chars.at(1), (CharBox{.start = 1, .end = 2, .rect = {.x = 10, .y = 0, .width = 10, .height = 100}}));
  QVERIFY(tree->find(99) == nullptr);

  // A role a later engine adds arrives as Other (engine protocol §9).
  const auto future = parseAccessibleTree(textNode(1, u"footnote"_s, u"1"_s), 1);
  QVERIFY(future && future->root().role == Role::Other);
  // An empty text is still a text (an empty editable line).
  const auto empty = parseAccessibleTree(textNode(1, u"paragraph"_s, QString()), 1);
  QVERIFY(empty && empty->root().hasText && empty->root().text.isEmpty());
}

void TestProtocol::refusesMalformedAccessibilityTrees() {
  // Duplicate identifiers, also between a parent and its child.
  QJsonObject twice = sampleTree();
  twice.insert(u"children"_s, QJsonArray{textNode(2, u"paragraph"_s, u"a"_s), textNode(2, u"paragraph"_s, u"b"_s)});
  QVERIFY(!parseAccessibleTree(twice, 2));
  QJsonObject itself = textNode(1, u"paragraph"_s, u"a"_s);
  itself.insert(u"children"_s, QJsonArray{textNode(1, u"paragraph"_s, u"b"_s)});
  QVERIFY(!parseAccessibleTree(itself, 1));
  // Bounds on a page that does not exist.
  QVERIFY(!parseAccessibleTree(sampleTree(), 1));
  // An identifier of 0, a heading level of 10, a role that is not a string, a text that is not a string, a child that is not an object.
  QVERIFY(!parseAccessibleTree(textNode(0, u"paragraph"_s, u"a"_s), 1));
  QJsonObject level = textNode(1, u"heading"_s, u"a"_s);
  level.insert(u"level"_s, 10);
  QVERIFY(!parseAccessibleTree(level, 1));
  QVERIFY(!parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, 3}}, 1));
  QVERIFY(!parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"paragraph"_s}, {u"text"_s, 3}}, 1));
  QVERIFY(!parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"children"_s, QJsonArray{7}}}, 1));
  QVERIFY(!parseAccessibleTree(QJsonValue(u"tree"_s), 1));
  // Character boxes that overlap, go beyond the text, or exist without a text.
  const auto withChars = [](const QString &text, const QJsonArray &chars) {
    QJsonObject node = textNode(1, u"paragraph"_s, text);
    node.insert(u"chars"_s, chars);
    return node;
  };
  const auto box = [](int start, int end) { return QJsonObject{{u"start"_s, start}, {u"end"_s, end}, {u"rect"_s, rect(0, 0, 1, 1)}}; };
  QVERIFY(parseAccessibleTree(withChars(u"ab"_s, QJsonArray{box(0, 1), box(1, 2)}), 1));
  QVERIFY(!parseAccessibleTree(withChars(u"ab"_s, QJsonArray{box(0, 2), box(1, 2)}), 1));
  QVERIFY(!parseAccessibleTree(withChars(u"ab"_s, QJsonArray{box(0, 3)}), 1));
  QVERIFY(!parseAccessibleTree(withChars(u"ab"_s, QJsonArray{box(1, 1)}), 1));
  const QJsonObject textless{{u"id"_s, 1}, {u"role"_s, u"paragraph"_s}, {u"chars"_s, QJsonArray{}}};
  QVERIFY(!parseAccessibleTree(textless, 1));
  // Depth: 64 levels are fine, 65 are not.
  QVERIFY(parseAccessibleTree(chain(kMaxTreeDepth), 1));
  QVERIFY(!parseAccessibleTree(chain(kMaxTreeDepth + 1), 1));
  // Size: more nodes than the limit, as one long list of children.
  QJsonArray many;
  for (int id = 2; id <= kMaxTreeNodes + 1; ++id) {
    many.append(QJsonObject{{u"id"_s, id}, {u"role"_s, u"paragraph"_s}});
  }
  QVERIFY(!parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"children"_s, many}}, 1));
  many.removeLast();
  QVERIFY(parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"children"_s, many}}, 1));
  // Children that are not a list.
  QVERIFY(!parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"children"_s, QJsonObject{}}}, 1));
}

void TestProtocol::caretIsAPositionOrNull() {
  using Parsed = std::optional<std::optional<AccessibleCaret>>;
  QCOMPARE(parseAccessibleCaret(QJsonObject{{u"node"_s, 6}, {u"offset"_s, 2}}), Parsed(AccessibleCaret{.node = 6, .offset = 2}));
  QCOMPARE(parseAccessibleCaret(QJsonValue::Null), Parsed(std::optional<AccessibleCaret>()));
  QVERIFY(!parseAccessibleCaret(QJsonObject{{u"node"_s, 6}}));
  QVERIFY(!parseAccessibleCaret(QJsonObject{{u"node"_s, 6}, {u"offset"_s, -1}}));
  QVERIFY(!parseAccessibleCaret(QJsonObject{{u"node"_s, 0}, {u"offset"_s, 0}}));
  QVERIFY(!parseAccessibleCaret(QJsonValue(QJsonValue::Undefined))); // missing is not null
}

void TestProtocol::replacingANodeKeepsTheTreeValid() {
  auto tree = parseAccessibleTree(sampleTree(), 2);
  if (!tree) {
    QFAIL("the sample tree was refused");
  }
  const auto subtree = [](const QJsonObject &node) {
    auto parsed = parseAccessibleTree(node, 2);
    if (!parsed) {
      qFatal("a test subtree was refused");
    }
    return std::move(*parsed);
  };
  // The editable line changes its text.
  QJsonObject line = textNode(6, u"paragraph"_s, u"abc"_s);
  line.insert(u"editable"_s, true);
  QVERIFY(tree->replace(subtree(line)));
  QCOMPARE(tree->find(6)->text, u"abc"_s);
  QCOMPARE(tree->size(), std::size_t{6});
  // The table gets a new row with fresh identifiers, and keeps its own: the old row (4) and its cell (5) go, the new ones come.
  const QJsonObject row{{u"id"_s, 7}, {u"role"_s, u"row"_s}, {u"children"_s, QJsonArray{textNode(8, u"cell"_s, u"C"_s)}}};
  const QJsonObject keptRow{{u"id"_s, 4}, {u"role"_s, u"row"_s}, {u"children"_s, QJsonArray{textNode(5, u"cell"_s, u"A"_s)}}};
  QVERIFY(tree->replace(subtree(QJsonObject{{u"id"_s, 3}, {u"role"_s, u"table"_s}, {u"children"_s, QJsonArray{keptRow, row}}})));
  QVERIFY(tree->find(8) != nullptr && tree->find(5) != nullptr);
  QCOMPARE(tree->find(3)->children, (std::vector<quint64>{4, 7}));
  QVERIFY(tree->replace(subtree(QJsonObject{{u"id"_s, 3}, {u"role"_s, u"table"_s}, {u"children"_s, QJsonArray{row}}})));
  QVERIFY(tree->find(4) == nullptr && tree->find(5) == nullptr);
  QCOMPARE(tree->size(), std::size_t{6});
  // A replacement that would reuse an identifier from elsewhere in the tree is refused, and the tree stays as it was.
  const AccessibleTree before = *tree;
  const QJsonObject stolen{{u"id"_s, 3}, {u"role"_s, u"table"_s}, {u"children"_s, QJsonArray{textNode(2, u"row"_s, u"x"_s)}}};
  QVERIFY(!tree->replace(subtree(stolen)));
  QCOMPARE(*tree, before);
  // A node that is not in the tree cannot be replaced.
  QVERIFY(!tree->replace(subtree(textNode(99, u"paragraph"_s, u"x"_s))));
  // Nor can a replacement make the tree deeper than the limit: the line sits at depth 2, so a chain of 64 levels in its place is too deep,
  // and one of 63 just fits.
  QVERIFY(!tree->replace(subtree(chain(kMaxTreeDepth, 6))));
  QCOMPARE(*tree, before);
  QVERIFY(tree->replace(subtree(chain(kMaxTreeDepth - 1, 6))));
  // The root itself can be replaced.
  QVERIFY(tree->replace(subtree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"name"_s, u"New"_s}})));
  QCOMPARE(tree->size(), std::size_t{1});
  QCOMPARE(tree->root().name, u"New"_s);
}

void TestProtocol::inputPayloadsHaveTheProtocolsShape() {
  const QJsonObject key = keyPayload({.key = u"a"_s, .code = u"KeyA"_s, .modifiers = {.shift = true}, .repeat = true, .composing = false});
  QCOMPARE(key.value(u"key"_s).toString(), u"a"_s);
  QCOMPARE(key.value(u"code"_s).toString(), u"KeyA"_s);
  QCOMPARE(key.value(u"modifiers"_s).toObject(),
           (QJsonObject{{u"shift"_s, true}, {u"ctrl"_s, false}, {u"alt"_s, false}, {u"meta"_s, false}}));
  QVERIFY(key.value(u"repeat"_s).toBool() && !key.value(u"composing"_s).toBool());

  const QJsonObject pointer = pointerPayload(
      {.kind = PointerKind::Up, .position = QPointF(1.5, 2), .button = 2, .clicks = 2, .modifiers = {}, .pointerType = PointerType::Pen});
  QCOMPARE(pointer.value(u"kind"_s).toString(), u"up"_s);
  QCOMPARE(pointer.value(u"x"_s).toDouble(), 1.5);
  QCOMPARE(pointer.value(u"y"_s).toDouble(), 2.0);
  QCOMPARE(pointer.value(u"button"_s).toInt(), 2);
  QCOMPARE(pointer.value(u"clicks"_s).toInt(), 2);
  QCOMPARE(pointer.value(u"pointer_type"_s).toString(), u"pen"_s);

  // Only an update carries the selection.
  const QJsonObject update = compositionPayload(CompositionPhase::Update, u"にほん"_s, 0, 3);
  QCOMPARE(update.value(u"phase"_s).toString(), u"update"_s);
  QCOMPARE(update.value(u"selection"_s).toObject(), (QJsonObject{{u"start"_s, 0}, {u"end"_s, 3}}));
  const QJsonObject start = compositionPayload(CompositionPhase::Start, QString(), 0, 0);
  QCOMPARE(start.value(u"phase"_s).toString(), u"start"_s);
  QVERIFY(!start.contains(u"selection"_s));

  QCOMPARE(toJson(BluRect{.x = -1, .y = 2, .width = 3, .height = 4}), rect(-1, 2, 3, 4));
}

QTEST_GUILESS_MAIN(TestProtocol)
#include "tst_protocol.moc"
