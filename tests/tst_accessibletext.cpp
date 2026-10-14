// Unit tests for the document text that the accessibility bridge gives screen readers (src/a11y/accessibletext.h): the engine's
// accessibility tree as one multi-line text in reading order, and the mapping between its offsets and the engine's nodes and boxes.

#include "accessibletext.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

#include <array>
#include <utility>

using namespace Qt::StringLiterals;
using namespace bayan;
using protocol::Role;

namespace {

QJsonObject rect(qint64 x, qint64 y, qint64 width, qint64 height) {
  return QJsonObject{{u"x"_s, x}, {u"y"_s, y}, {u"width"_s, width}, {u"height"_s, height}};
}

QJsonObject textNode(qint64 id, const QString &role, const QString &text) {
  return QJsonObject{{u"id"_s, id}, {u"role"_s, role}, {u"text"_s, text}};
}

// Like the mock document: a heading, a table whose cells have the text, and an empty editable line with a box but no characters yet.
protocol::AccessibleTree sampleTree() {
  QJsonObject heading = textNode(2, u"heading"_s, u"Title"_s);
  heading.insert(u"bounds"_s, QJsonObject{{u"page"_s, 0}, {u"rect"_s, rect(100, 200, 500, 50)}});
  QJsonArray chars;
  const std::array<std::pair<qint64, qint64>, 5> boxes{{{100, 20}, {120, 10}, {130, 8}, {138, 8}, {146, 9}}};
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    chars.append(QJsonObject{{u"start"_s, static_cast<int>(i)},
                             {u"end"_s, static_cast<int>(i) + 1},
                             {u"rect"_s, rect(boxes.at(i).first, 200, boxes.at(i).second, 50)}});
  }
  heading.insert(u"chars"_s, chars);
  const QJsonObject row{
      {u"id"_s, 4}, {u"role"_s, u"row"_s}, {u"children"_s, QJsonArray{textNode(5, u"cell"_s, u"A"_s), textNode(6, u"cell"_s, u"B"_s)}}};
  const QJsonObject table{{u"id"_s, 3}, {u"role"_s, u"table"_s}, {u"children"_s, QJsonArray{row}}};
  QJsonObject line = textNode(7, u"paragraph"_s, QString());
  line.insert(u"editable"_s, true);
  line.insert(u"bounds"_s, QJsonObject{{u"page"_s, 1}, {u"rect"_s, rect(0, 1000, 4000, 300)}});
  const QJsonObject root{
      {u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"name"_s, u"Sample"_s}, {u"children"_s, QJsonArray{heading, table, line}}};
  auto tree = protocol::parseAccessibleTree(root, 2);
  if (!tree) {
    qFatal("the sample tree was refused");
  }
  return std::move(*tree);
}

} // namespace

class TestAccessibleText final : public QObject {
  Q_OBJECT

private slots:
  void joinsTheTextsInReadingOrder();
  void findsTheSegmentOfAnOffset();
  void mapsNodeOffsetsToTextOffsets();
  void findsCharacterBoxes();
  void describesTheSmallestChange();
  void anEmptyTreeHasNoText();
};

void TestAccessibleText::joinsTheTextsInReadingOrder() {
  const AccessibleText text(sampleTree());
  // The document and the table have no text of their own; the empty editable line still has a line.
  QCOMPARE(text.text(), u"Title\nA\nB\n"_s);
  QCOMPARE(text.name(), u"Sample"_s);
  QCOMPARE(text.segments().size(), std::size_t{4});
  QCOMPARE(text.segments().at(0).start, 0);
  QCOMPARE(text.segments().at(1).start, 6);
  QCOMPARE(text.segments().at(2).start, 8);
  QCOMPARE(text.segments().at(3).start, 10);
  QCOMPARE(text.segments().at(3).length, 0);
  QVERIFY(text.segments().at(3).editable);
  QCOMPARE(text.segments().at(1).role, Role::Cell);
}

void TestAccessibleText::findsTheSegmentOfAnOffset() {
  const AccessibleText text(sampleTree());
  QCOMPARE(text.segmentAt(0)->node, quint64{2});
  // The end of a line (where the line break follows) belongs to that line.
  QCOMPARE(text.segmentAt(5)->node, quint64{2});
  QCOMPARE(text.segmentAt(6)->node, quint64{5});
  QCOMPARE(text.segmentAt(10)->node, quint64{7});
  QVERIFY(text.segmentAt(11) == nullptr);
  QVERIFY(text.segmentAt(-1) == nullptr);
  QCOMPARE(text.segmentOf(6)->start, 8);
  QVERIFY(text.segmentOf(3) == nullptr); // the table has no text
}

void TestAccessibleText::mapsNodeOffsetsToTextOffsets() {
  const AccessibleText text(sampleTree());
  QCOMPARE(text.offsetOf(2, 3), std::optional(3));
  QCOMPARE(text.offsetOf(7, 0), std::optional(10));
  // Offsets beyond a node's text are clamped to it.
  QCOMPARE(text.offsetOf(5, 7), std::optional(7));
  QCOMPARE(text.offsetOf(5, -2), std::optional(6));
  QCOMPARE(text.offsetOf(99, 0), std::nullopt);
}

void TestAccessibleText::findsCharacterBoxes() {
  using protocol::PageRect;
  const AccessibleText text(sampleTree());
  QCOMPARE(text.characterBox(1), (std::optional<PageRect>{{.page = 0, .rect = {.x = 120, .y = 200, .width = 10, .height = 50}}}));
  // After the last character: an empty box at its right edge, where a caret would be.
  QCOMPARE(text.characterBox(5), (std::optional<PageRect>{{.page = 0, .rect = {.x = 155, .y = 200, .width = 0, .height = 50}}}));
  // An empty line: an empty box at the start of the line's box.
  QCOMPARE(text.characterBox(10), (std::optional<PageRect>{{.page = 1, .rect = {.x = 0, .y = 1000, .width = 0, .height = 300}}}));
  // Cells without boxes have none.
  QCOMPARE(text.characterBox(6), std::nullopt);
}

void TestAccessibleText::describesTheSmallestChange() {
  using Change = AccessibleText::Change;
  const auto check = [](const QString &before, const QString &after, const Change &expected) {
    const Change change = AccessibleText::difference(before, after);
    QCOMPARE(change.position, expected.position);
    QCOMPARE(change.removed, expected.removed);
    QCOMPARE(change.inserted, expected.inserted);
  };
  check(u"Title\n"_s, u"Title\nok"_s, {.position = 6, .removed = QString(), .inserted = u"ok"_s});
  check(u"abcd"_s, u"ad"_s, {.position = 1, .removed = u"bc"_s, .inserted = QString()});
  check(u"cafe"_s, u"café"_s, {.position = 3, .removed = u"e"_s, .inserted = u"é"_s});
  check(u"same"_s, u"same"_s, {.position = 4, .removed = QString(), .inserted = QString()});
  // Repeated letters: the change is placed after the common start.
  check(u"aaa"_s, u"aaaa"_s, {.position = 3, .removed = QString(), .inserted = u"a"_s});
  // An emoji whose second code unit changes is reported whole.
  check(u"x😀y"_s, u"x😃y"_s, {.position = 1, .removed = u"😀"_s, .inserted = u"😃"_s});
  // Emoji with the same second code unit but a different first one, at the end.
  const QString before = u"x"_s + QChar(0xd83d) + QChar(0xde00);
  const QString after = u"x"_s + QChar(0xd83c) + QChar(0xde00);
  check(before, after, {.position = 1, .removed = before.mid(1), .inserted = after.mid(1)});
}

void TestAccessibleText::anEmptyTreeHasNoText() {
  const AccessibleText none;
  QVERIFY(none.isEmpty() && none.text().isEmpty() && none.name().isEmpty());
  QVERIFY(none.segmentAt(0) == nullptr);
  QCOMPARE(none.characterBox(0), std::nullopt);
  const auto textless = protocol::parseAccessibleTree(QJsonObject{{u"id"_s, 1}, {u"role"_s, u"document"_s}, {u"name"_s, u"Empty"_s}}, 1);
  if (!textless) {
    QFAIL("the tree was refused");
  }
  const AccessibleText empty(*textless);
  QVERIFY(empty.isEmpty());
  QCOMPARE(empty.name(), u"Empty"_s);
}

QTEST_GUILESS_MAIN(TestAccessibleText)
#include "tst_accessibletext.moc"
