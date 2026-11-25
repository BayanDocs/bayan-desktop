// Unit tests for the protocol values and parsers (src/engine/protocol.h). Messages from the engine are untrusted input (AGENTS.md §6), so
// most of these tests check that a malformed value is refused as a whole rather than guessed at.

#include "protocol.h"
#include "protocolschema.h"

#include <QtCore/QDebug>
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

// Whether a fragment written for these tests is what the engine could send: the protocol's JSON Schema accepts it as this definition
// (tests/support/protocolschema.h). The tests check it for each valid fragment, so that the samples stay valid when the schema moves.
bool matches(const QJsonValue &fragment, const QString &definition) {
  const QStringList problems = bayan::test::ProtocolSchema::engineProtocol().problems(fragment, definition);
  if (!problems.isEmpty()) {
    qWarning().noquote() << "Not a valid" << definition << "of the engine protocol:\n" << problems.join(u'\n');
  }
  return problems.isEmpty();
}

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
  line.insert(u"chars"_s, QJsonArray{
                              QJsonObject{{u"start"_s, 0}, {u"end"_s, 1}, {u"rect"_s, rect(0, 0, 10, 100)}},
                              QJsonObject{{u"start"_s, 1}, {u"end"_s, 2}, {u"rect"_s, rect(10, 0, 10, 100)}},
                          });
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
  void welcomesAreReadFieldByField();
  void manifestKeysAreReadAsTheEngineGivesThem();
  void nodesMayHoldAPartOfTheirChildren();
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
  QVERIFY(matches(page(0, 0), u"PageInfo"_s));
  QVERIFY(matches(page(1, 21'000'000), u"PageInfo"_s));
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
  QVERIFY(matches(region, u"PageRect"_s));
  QCOMPARE(parseRegions(QJsonArray{region}, 2), (std::vector<PageRect>{{.page = 1, .rect = {.x = 0, .y = 0, .width = 5, .height = 5}}}));
  QVERIFY(!parseRegions(QJsonArray{region}, 1));
  QVERIFY(!parseRegions(QJsonArray{region}, 0));
  QVERIFY(!parseRegions(QJsonArray{QJsonObject{{u"page"_s, 0}}}, 2)); // no rectangle
  QCOMPARE(parseRegions(QJsonArray{}, 0), std::vector<PageRect>{});
}

void TestProtocol::overlaysHaveACaretOrNullAndSelectionsPerPage() {
  const QJsonObject caret{{u"page"_s, 0}, {u"rect"_s, rect(1, 2, 3, 4)}, {u"visible"_s, true}};
  const QJsonArray selection{QJsonObject{{u"page"_s, 1}, {u"rects"_s, QJsonArray{rect(0, 0, 1, 1), rect(2, 0, 1, 1)}}}};
  QVERIFY(matches(caret, u"Caret"_s));
  QVERIFY(matches(selection.at(0), u"SelectionRects"_s));
  // An overlay.update payload with these fields, its render revision and the remote cursors and highlights of later phases (§14.6).
  const auto overlay = [](const QJsonObject &fields) {
    QJsonObject payload{{u"revision"_s, 7}, {u"remote"_s, QJsonArray{}}, {u"highlights"_s, QJsonArray{}}};
    for (auto field = fields.begin(); field != fields.end(); ++field) {
      payload.insert(field.key(), field.value());
    }
    return payload;
  };
  QJsonObject sample = overlay(QJsonObject{{u"caret"_s, caret}, {u"selection"_s, selection}});
  sample.insert(u"doc_id"_s, 1);
  QVERIFY(matches(sample, u"OverlayUpdate"_s));
  const Overlay expected{
      .caret = Caret{.page = 0, .rect = {.x = 1, .y = 2, .width = 3, .height = 4}, .visible = true},
      .selection =
          {
              {.page = 1, .rect = {.x = 0, .y = 0, .width = 1, .height = 1}},
              {.page = 1, .rect = {.x = 2, .y = 0, .width = 1, .height = 1}},
          },
  };
  QCOMPARE(parseOverlay(overlay(QJsonObject{{u"caret"_s, caret}, {u"selection"_s, selection}}), 2), expected);
  QCOMPARE(parseOverlay(overlay(QJsonObject{{u"caret"_s, QJsonValue::Null}, {u"selection"_s, QJsonArray{}}}), 1), Overlay{});

  QJsonObject hidden = caret;
  hidden.remove(u"visible"_s);
  QVERIFY(!parseOverlay(overlay(QJsonObject{{u"caret"_s, hidden}, {u"selection"_s, QJsonArray{}}}), 1));
  QVERIFY(!parseOverlay(overlay(QJsonObject{{u"caret"_s, caret}, {u"selection"_s, QJsonArray{}}}), 0)); // no such page
  QVERIFY(!parseOverlay(overlay(QJsonObject{{u"caret"_s, QJsonValue::Null}}), 1));                      // no selection
  QVERIFY(!parseOverlay(overlay(QJsonObject{{u"selection"_s, QJsonArray{}}}), 1));                      // caret missing, not null
  QVERIFY(!parseOverlay(overlay(QJsonObject{{u"caret"_s, QJsonValue::Null}, {u"selection"_s, selection}}), 1));
  // The revision is a positive integer below 2^53, and the lists of later phases are there.
  const QJsonObject empty{{u"caret"_s, QJsonValue::Null}, {u"selection"_s, QJsonArray{}}};
  for (const QJsonValue &revision :
       {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5), QJsonValue(9'007'199'254'740'992.0), QJsonValue(u"7"_s)}) {
    QJsonObject payload = overlay(empty);
    payload.insert(u"revision"_s, revision);
    QVERIFY(!parseOverlay(payload, 1));
  }
  for (const QString &field : {u"revision"_s, u"remote"_s, u"highlights"_s}) {
    QJsonObject payload = overlay(empty);
    payload.remove(field);
    QVERIFY2(!parseOverlay(payload, 1), qPrintable(field));
  }
}

void TestProtocol::welcomesAreReadFieldByField() {
  // welcome (engine protocol §5, §14.1): the version the engine speaks, its version of at most 64 printable ASCII characters (item 6), the
  // layout epoch and the features.
  const QJsonObject welcome{
      {u"protocol_version"_s, 1},
      {u"engine_version"_s, u"1.2.3-alpha.1+build.5"_s},
      {u"layout_epoch"_s, 4},
      {u"features"_s, QJsonArray{u"mock_document"_s}},
  };
  QVERIFY(matches(welcome, u"Welcome"_s));
  const auto parsed = parseWelcome(welcome);
  if (!parsed) {
    QFAIL("the sample was refused");
  }
  QCOMPARE(parsed->protocolVersion, 1);
  QCOMPARE(parsed->engineVersion, u"1.2.3-alpha.1+build.5"_s);
  QCOMPARE(parsed->layoutEpoch, 4);
  QCOMPARE(parsed->features, QStringList{u"mock_document"_s});
  const auto with = [&welcome](const QString &field, const QJsonValue &value) {
    QJsonObject changed = welcome;
    if (value.isUndefined()) {
      changed.remove(field);
    } else {
      changed.insert(field, value);
    }
    return parseWelcome(changed).has_value();
  };
  QVERIFY(with(u"engine_version"_s, QString(64, u'1')));
  QVERIFY(!with(u"engine_version"_s, QString(65, u'1')));
  QVERIFY(!with(u"engine_version"_s, u""_s));
  QVERIFY(!with(u"engine_version"_s, u"1.0\n"_s));
  QVERIFY(!with(u"engine_version"_s, u"1.0.0-\u00e9"_s));
  QVERIFY(!with(u"engine_version"_s, 1));
  QVERIFY(!with(u"protocol_version"_s, -1));
  QVERIFY(!with(u"protocol_version"_s, 1.5));
  QVERIFY(!with(u"layout_epoch"_s, -1));
  QVERIFY(!with(u"features"_s, QJsonArray{1}));
  for (const QString &field : {u"protocol_version"_s, u"engine_version"_s, u"layout_epoch"_s, u"features"_s}) {
    QVERIFY2(!with(field, QJsonValue(QJsonValue::Undefined)), qPrintable(field));
  }
}

void TestProtocol::manifestKeysAreReadAsTheEngineGivesThem() {
  // The key data of the UI manifest (engine protocol §6.8, §14.3): bindings with all four modifiers and the engine's label, document keys
  // by key, and reserved keys by key or code with the modifiers that matter.
  const QJsonObject bold{
      {u"key"_s, u"b"_s}, {u"ctrl"_s, true}, {u"shift"_s, false}, {u"alt"_s, false}, {u"meta"_s, false}, {u"label"_s, u"Ctrl+B"_s},
  };
  const QJsonObject copy{
      {u"code"_s, u"KeyC"_s}, {u"ctrl"_s, true}, {u"shift"_s, false}, {u"alt"_s, false}, {u"meta"_s, false}, {u"label"_s, u"Ctrl+C"_s},
  };
  const QJsonArray commands{
      QJsonObject{{u"id"_s, u"format.bold.toggle"_s}, {u"label"_s, u"Bold"_s}, {u"bindings"_s, QJsonArray{bold}}},
      QJsonObject{{u"id"_s, u"edit.copy"_s}, {u"label"_s, u"Copy"_s}, {u"bindings"_s, QJsonArray{copy}}, {u"shell_action"_s, u"copy"_s}},
  };
  const QJsonObject keys{
      {u"document"_s, QJsonArray{QJsonObject{{u"key"_s, u"Backspace"_s}}}},
      {u"reserved"_s, QJsonArray{QJsonObject{{u"key"_s, u"Tab"_s}}, QJsonObject{{u"code"_s, u"KeyV"_s}, {u"ctrl"_s, true}}}},
  };
  const QJsonObject manifest{
      {u"manifest_version"_s, 0},
      {u"locale"_s, u"en-US"_s},
      {u"commands"_s, commands},
      {u"keys"_s, keys},
      {u"ribbon"_s, QJsonObject{{u"tabs"_s, QJsonArray{}}}},
  };
  QVERIFY(matches(manifest, u"UiManifest"_s));
  const auto parsed = parseUiManifest(manifest);
  if (!parsed) {
    QFAIL("the sample was refused");
  }
  QCOMPARE(parsed->commands.size(), std::size_t{2});
  const ManifestCommand &first = parsed->commands.at(0);
  QCOMPARE(first.id, u"format.bold.toggle"_s);
  QVERIFY(!first.shellAction);
  QCOMPARE(first.bindings,
           (std::vector<KeyEntry>{
               {.key = u"b"_s, .code = {}, .ctrl = true, .shift = false, .alt = false, .meta = false, .label = u"Ctrl+B"_s}}));
  QCOMPARE(parsed->commands.at(1).shellAction, std::optional<QString>(u"copy"_s));
  QCOMPARE(parsed->commands.at(1).bindings.at(0).code, u"KeyC"_s);
  const auto entry = [](const QString &key, const QString &code, std::optional<bool> ctrl = std::nullopt) {
    return KeyEntry{.key = key, .code = code, .ctrl = ctrl, .shift = std::nullopt, .alt = std::nullopt, .meta = std::nullopt, .label = {}};
  };
  QCOMPARE(parsed->documentKeys, std::vector<KeyEntry>{entry(u"Backspace"_s, {})});
  QCOMPARE(parsed->reservedKeys, (std::vector<KeyEntry>{entry(u"Tab"_s, {}), entry({}, u"KeyV"_s, true)}));

  // Each entry names its key exactly once; a binding has all four modifiers and a label.
  const auto withBinding = [&manifest](const QJsonObject &binding) {
    QJsonObject changed = manifest;
    changed.insert(u"commands"_s, QJsonArray{QJsonObject{{u"id"_s, u"x"_s}, {u"label"_s, u"X"_s}, {u"bindings"_s, QJsonArray{binding}}}});
    return parseUiManifest(changed).has_value();
  };
  QVERIFY(withBinding(bold));
  QJsonObject both = bold;
  both.insert(u"code"_s, u"KeyB"_s);
  QVERIFY(!withBinding(both));
  for (const QString &field : {u"key"_s, u"ctrl"_s, u"shift"_s, u"alt"_s, u"meta"_s, u"label"_s}) {
    QJsonObject missing = bold;
    missing.remove(field);
    QVERIFY2(!withBinding(missing), qPrintable(field));
  }
  QJsonObject empty = bold;
  empty.insert(u"key"_s, u""_s);
  QVERIFY(!withBinding(empty));
  const auto withKeys = [&manifest](const QString &list, const QJsonObject &item) {
    QJsonObject changed = manifest;
    QJsonObject lists = changed.value(u"keys"_s).toObject();
    lists.insert(list, QJsonArray{item});
    changed.insert(u"keys"_s, lists);
    return parseUiManifest(changed).has_value();
  };
  QVERIFY(!withKeys(u"document"_s, QJsonObject{{u"code"_s, u"KeyA"_s}}));
  QVERIFY(!withKeys(u"reserved"_s, QJsonObject{{u"key"_s, u"Tab"_s}, {u"ctrl"_s, u"yes"_s}}));
  QVERIFY(!withKeys(u"reserved"_s, QJsonObject{{u"key"_s, u"Tab"_s}, {u"code"_s, u"Tab"_s}}));
  for (const QString &field : {u"manifest_version"_s, u"locale"_s, u"commands"_s, u"keys"_s, u"ribbon"_s}) {
    QJsonObject missing = manifest;
    missing.remove(field);
    QVERIFY2(!parseUiManifest(missing), qPrintable(field));
  }
}

void TestProtocol::nodesMayHoldAPartOfTheirChildren() {
  // A node with more children than one message holds gives child_count and, unless its run starts at the first child, children_start
  // (engine protocol §14.7 item 1). The tree knows which children it lacks, and takes the parts that query.a11y loads.
  const auto paragraph = [](int id) { return QJsonObject{{u"id"_s, id}, {u"role"_s, u"paragraph"_s}, {u"text"_s, u"P%1"_s.arg(id)}}; };
  const auto document = [&paragraph](int count, int start, const std::vector<int> &ids) {
    QJsonArray children;
    for (const int id : ids) {
      children.append(paragraph(id));
    }
    QJsonObject node{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"child_count"_s, count}, {u"children"_s, children}};
    if (start > 0) {
      node.insert(u"children_start"_s, start);
    }
    return node;
  };
  QVERIFY(matches(document(6, 2, {4, 5}), u"A11yNode"_s));
  // The first two of six children: four are missing, from the third on.
  auto tree = parseAccessibleTree(document(6, 0, {2, 3}), 1);
  if (!tree) {
    QFAIL("the tree was refused");
  }
  QCOMPARE(tree->root().childCount, std::optional<quint32>(6));
  QCOMPARE(tree->root().childIndices, (std::vector<quint32>{0, 1}));
  QCOMPARE(tree->missingChildren(), (std::vector<MissingChildren>{{.node = 1, .start = 2, .count = 4}}));
  // A part with the third and fourth children; then one with the last two. Once the tree holds all six, nothing is missing.
  const auto middle = parseAccessibleTree(document(6, 2, {4, 5}), 1);
  if (!middle) {
    QFAIL("the middle part was refused");
  }
  QVERIFY(tree->addChildren(*middle));
  QCOMPARE(tree->missingChildren(), (std::vector<MissingChildren>{{.node = 1, .start = 4, .count = 2}}));
  const auto last = parseAccessibleTree(document(6, 4, {6, 7}), 1);
  if (!last) {
    QFAIL("the last part was refused");
  }
  QVERIFY(tree->addChildren(*last));
  QVERIFY(tree->missingChildren().empty());
  QCOMPARE(tree->root().children, (std::vector<quint64>{2, 3, 4, 5, 6, 7}));
  QVERIFY(!tree->root().childCount);
  std::vector<quint64> order;
  for (const AccessibleNode *node : tree->readingOrder()) {
    order.push_back(node->id);
  }
  QCOMPARE(order, (std::vector<quint64>{1, 2, 3, 4, 5, 6, 7}));
  // A run in the middle leaves a gap before it and one after it.
  const auto gaps = parseAccessibleTree(document(5, 2, {4}), 1);
  if (!gaps) {
    QFAIL("the tree with gaps was refused");
  }
  QCOMPARE(gaps->missingChildren(),
           (std::vector<MissingChildren>{{.node = 1, .start = 0, .count = 2}, {.node = 1, .start = 3, .count = 2}}));
  // A node with child_count and no children holds none of them; one whose run covers all of them holds them all.
  QJsonObject none{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"child_count"_s, 3}};
  const auto withNone = parseAccessibleTree(none, 1);
  if (!withNone) {
    QFAIL("the tree without children was refused");
  }
  QCOMPARE(withNone->missingChildren(), (std::vector<MissingChildren>{{.node = 1, .start = 0, .count = 3}}));
  const auto all = parseAccessibleTree(document(2, 0, {2, 3}), 1);
  if (!all) {
    QFAIL("the tree with all its children was refused");
  }
  QVERIFY(!all->root().childCount && all->missingChildren().empty());
  // More children than the node says it has, a run beyond them, or counts out of range break the protocol.
  QVERIFY(!parseAccessibleTree(document(1, 0, {2, 3}), 1));
  QVERIFY(!parseAccessibleTree(document(3, 2, {4, 5}), 1));
  QJsonObject negative = document(3, 0, {2});
  negative.insert(u"child_count"_s, -1);
  QVERIFY(!parseAccessibleTree(negative, 1));
  QJsonObject huge = document(3, 0, {2});
  huge.insert(u"children_start"_s, 4'294'967'296.0);
  QVERIFY(!parseAccessibleTree(huge, 1));
  // A part for a node that is not in the tree, or that reuses an identifier from elsewhere, changes nothing.
  auto other = parseAccessibleTree(document(6, 0, {2, 3}), 1);
  if (!other) {
    QFAIL("the second tree was refused");
  }
  QJsonObject stranger = document(6, 2, {4, 5});
  stranger.insert(u"id"_s, 99);
  const auto strangerPart = parseAccessibleTree(stranger, 1);
  if (!strangerPart) {
    QFAIL("the stranger's part was refused");
  }
  QVERIFY(!other->addChildren(*strangerPart));
  const auto reused = parseAccessibleTree(document(6, 2, {3, 4}), 1);
  if (!reused) {
    QFAIL("the part with a reused identifier was refused");
  }
  QVERIFY(!other->addChildren(*reused));
  QCOMPARE(other->missingChildren(), (std::vector<MissingChildren>{{.node = 1, .start = 2, .count = 4}}));
}

void TestProtocol::readsAnAccessibilityTree() {
  QVERIFY(matches(sampleTree(), u"A11yNode"_s));
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
  QVERIFY(matches(QJsonObject{{u"node"_s, 6}, {u"offset"_s, 2}}, u"A11yCaret"_s));
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
      // NOLINTNEXTLINE(readability-trailing-comma): clang-tidy 23 takes the comma after the empty {} for a trailing comma of that list.
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
