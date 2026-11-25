// The conformance vectors of engine protocol §15: shared test cases with their expected results, which bayan-core's tests check against the
// engine, its bindings and reference shells, and both shells against their own code. They are read from the copies in
// src/engine/stub/schema/conformance, compiled into this test, which engine.sdk_copies compares with the C SDK's whenever the app is built
// against one. Every case marked shell or desktop runs here:
// - restart.json: the messages a shell sends and receives around engine failures, through SessionTracker, with which DocumentSession
//   sorts every message from the engine into handle, ignore, restart and stop (§12.1, §14.2); tst_documentsession tests what the
//   session then does;
// - tile-grid.json: the page sizes and tile rectangles of TileGrid (§14.6 item 3);
// - a11y-limits.json: which accessibility trees the shell accepts, and which parts of a node's children it asks for (§14.7 item 1);
// - keys.json: which presses KeyBindings sends to the engine, with the key data that the engine resolves for each platform and runtime,
//   which the file's engine cases hold (§14.3); and the shell must be able to read the key data of every platform and runtime.
// A case of these files that the test does not recognize, by its for, its fields or its inputs, fails it, so that a new case cannot pass
// unnoticed (§15); cases for the engine, the bindings and the web shell are recognized and left to them.

#include "engine.h"
#include "keybindings.h"
#include "protocol.h"
#include "sessiontracker.h"
#include "tilegrid.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtTest/QTest>

#include <optional>

using namespace Qt::StringLiterals;
using namespace bayan;

namespace {

// A vector file, after the checks of its format that §15 gives: { format, format_version, protocol_version, area, cases, removed }.
QJsonObject vectorFile(const QString &area) {
  QFile file(u":/bayan/test/conformance/"_s + area + u".json"_s);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning("The conformance vectors of %s are missing", qPrintable(area));
    return {};
  }
  return QJsonDocument::fromJson(file.readAll()).object();
}

QSet<QString> keysOf(const QJsonObject &object) {
  const QStringList keys = object.keys();
  return {keys.cbegin(), keys.cend()};
}

QSet<QString> forOf(const QJsonObject &testCase) {
  QSet<QString> who;
  for (const auto &value : testCase.value(u"for"_s).toArray()) {
    who.insert(value.toString());
  }
  return who;
}

// Whether a case is this shell's: marked shell or desktop.
bool isOurs(const QJsonObject &testCase) {
  const QSet<QString> who = forOf(testCase);
  return who.contains(u"shell"_s) || who.contains(u"desktop"_s);
}

// The kinds of case a file may hold: who they are for, and their fields. A case that matches none of them is one the test does not know.
struct CaseKind {
  QSet<QString> fields;
  QSet<QString> forValues;
};

// Checks the format of a file and that it holds only cases of the given kinds; returns the cases, or nothing after logging what is wrong.
std::optional<QJsonArray> recognizedCases(const QString &area, const QList<CaseKind> &kinds) {
  const QJsonObject file = vectorFile(area);
  if (file.isEmpty()) {
    return std::nullopt;
  }
  if (file.value(u"format"_s) != u"bayan-engine-conformance"_s || file.value(u"format_version"_s) != QJsonValue(1) ||
      file.value(u"protocol_version"_s) != QJsonValue(Engine::kProtocolVersion) || file.value(u"area"_s) != area ||
      !file.value(u"cases"_s).isArray() || !file.value(u"removed"_s).isArray() ||
      keysOf(file) != QSet<QString>{u"format"_s, u"format_version"_s, u"protocol_version"_s, u"area"_s, u"cases"_s, u"removed"_s}) {
    qWarning("%s.json does not have the format of engine protocol §15 for protocol version %d", qPrintable(area), Engine::kProtocolVersion);
    return std::nullopt;
  }
  static const QRegularExpression idPattern(u"^[a-z0-9]+(-[a-z0-9]+)*$"_s);
  QSet<QString> ids;
  for (const auto &removed : file.value(u"removed"_s).toArray()) {
    ids.insert(removed.toString());
  }
  for (const auto &value : file.value(u"cases"_s).toArray()) {
    const QJsonObject testCase = value.toObject();
    const QString id = testCase.value(u"id"_s).toString();
    const bool known = std::ranges::any_of(kinds, [&testCase](const CaseKind &kind) {
      const QSet<QString> who = forOf(testCase);
      return keysOf(testCase) == kind.fields && !who.isEmpty() && who.size() == testCase.value(u"for"_s).toArray().size() &&
             (who - kind.forValues).isEmpty();
    });
    if (!known || !idPattern.match(id).hasMatch() || ids.contains(id) || !testCase.value(u"description"_s).isString()) {
      qWarning("%s.json: the case %s is not one this test recognizes", qPrintable(area), qPrintable(id));
      return std::nullopt;
    }
    ids.insert(id);
  }
  return file.value(u"cases"_s).toArray();
}

QString compact(const QJsonValue &value) {
  return QString::fromUtf8(value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                                            : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

// restart.json: what a step expects, as SessionTracker's decision; nothing for an expectation the test does not know.
std::optional<SessionTracker::Decision> expectedDecision(const QJsonValue &expect) {
  using Action = SessionTracker::Action;
  if (expect == u"handle"_s) {
    return SessionTracker::Decision{.action = Action::Handle};
  }
  if (expect == u"ignore"_s) {
    return SessionTracker::Decision{.action = Action::Ignore};
  }
  if (expect == u"stop"_s) {
    return SessionTracker::Decision{.action = Action::Stop};
  }
  const QJsonObject restart = expect.toObject().value(u"restart"_s).toObject();
  if (keysOf(expect.toObject()) == QSet<QString>{u"restart"_s} && keysOf(restart) == QSet<QString>{u"number"_s, u"limit"_s}) {
    return SessionTracker::Decision{
        .action = Action::Restart,
        .restart =
            {
                .number = static_cast<quint32>(restart.value(u"number"_s).toInteger()),
                .limit = static_cast<quint32>(restart.value(u"limit"_s).toInteger()),
            },
    };
  }
  return std::nullopt;
}

// a11y-limits.json: the tree that a case's generator describes (the generator's description in conformance.schema.json).
QJsonObject paragraph(qint64 id, const QString &text) { return QJsonObject{{u"id"_s, id}, {u"role"_s, u"paragraph"_s}, {u"text"_s, text}}; }

std::optional<QJsonObject> generatedTree(const QJsonObject &tree) {
  QJsonObject document{{u"id"_s, 1}, {u"role"_s, u"document"_s}};
  const QString generator = tree.value(u"generator"_s).toString();
  if (generator == u"chain"_s && keysOf(tree) == QSet<QString>{u"generator"_s, u"depth"_s}) {
    const qint64 depth = tree.value(u"depth"_s).toInteger();
    std::optional<QJsonObject> below;
    for (qint64 level = depth; level >= 2; --level) {
      QJsonObject node = paragraph(level, QString::number(level));
      if (below) {
        node.insert(u"children"_s, QJsonArray{*below});
      }
      below = node;
    }
    if (below) {
      document.insert(u"children"_s, QJsonArray{*below});
    }
    return document;
  }
  const QSet<QString> wideFields{u"generator"_s, u"children"_s, u"child_count"_s, u"children_start"_s, u"text_units"_s};
  if (generator == u"wide"_s && tree.contains(u"children"_s) && (keysOf(tree) - wideFields).isEmpty()) {
    const qint64 held = tree.value(u"children"_s).toInteger();
    const qint64 start = tree.value(u"children_start"_s).toInteger(0);
    const qint64 units = tree.value(u"text_units"_s).toInteger(0);
    QJsonArray children;
    for (qint64 index = start; index < start + held; ++index) {
      children.append(paragraph(index + 2, units > 0 ? QString(static_cast<qsizetype>(units), u'x') : QString::number(index)));
    }
    if (tree.contains(u"child_count"_s)) {
      document.insert(u"child_count"_s, tree.value(u"child_count"_s));
      if (held > 0) {
        document.insert(u"children_start"_s, start);
      }
    }
    if (held > 0 || !tree.contains(u"child_count"_s)) {
      document.insert(u"children"_s, children);
    }
    return document;
  }
  return std::nullopt;
}

// keys.json: a press as input.key would carry it.
std::optional<protocol::KeyInput> pressOf(const QJsonObject &press) {
  const QSet<QString> fields{u"key"_s, u"code"_s, u"ctrl"_s, u"shift"_s, u"alt"_s, u"meta"_s, u"composing"_s};
  if (keysOf(press) != fields) {
    return std::nullopt;
  }
  return protocol::KeyInput{
      .key = press.value(u"key"_s).toString(),
      .code = press.value(u"code"_s).toString(),
      .modifiers =
          {
              .shift = press.value(u"shift"_s).toBool(),
              .ctrl = press.value(u"ctrl"_s).toBool(),
              .alt = press.value(u"alt"_s).toBool(),
              .meta = press.value(u"meta"_s).toBool(),
          },
      .repeat = false,
      .composing = press.value(u"composing"_s).toBool(),
  };
}

// The key data of a manifest case, as the reply to ui.manifest carries them.
std::optional<protocol::UiManifest> manifestOf(const QJsonObject &manifestCase) {
  const QJsonObject expected = manifestCase.value(u"expect"_s).toObject();
  if (keysOf(expected) != QSet<QString>{u"commands"_s, u"keys"_s}) {
    return std::nullopt;
  }
  return protocol::parseUiManifest(QJsonObject{
      {u"manifest_version"_s, 0},
      {u"locale"_s, u"en-US"_s},
      {u"commands"_s, expected.value(u"commands"_s)},
      {u"keys"_s, expected.value(u"keys"_s)},
      {u"ribbon"_s, QJsonObject{{u"tabs"_s, QJsonArray{}}}},
  });
}

} // namespace

class TestConformance final : public QObject {
  Q_OBJECT

private slots:
  void restartCases();
  void tileGridCases();
  void accessibilityLimitCases();
  void keyCases();
  void unknownCasesAreNotRecognized();
};

void TestConformance::restartCases() {
  const auto cases = recognizedCases(
      u"restart"_s,
      {
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"steps"_s}, .forValues = {u"shell"_s, u"desktop"_s, u"web"_s}},
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"failures_ms"_s, u"expect"_s}, .forValues = {u"binding"_s}},
          {
              .fields = {u"id"_s, u"description"_s, u"for"_s, u"runtime"_s, u"config"_s, u"messages"_s, u"replays"_s, u"expect"_s},
              .forValues = {u"binding"_s},
          },
      });
  if (!cases) {
    QFAIL("the vectors could not be read");
  }
  int run = 0;
  for (const auto &value : *cases) {
    const QJsonObject testCase = value.toObject();
    if (!isOurs(testCase)) {
      continue;
    }
    const QString id = testCase.value(u"id"_s).toString();
    SessionTracker tracker;
    for (const auto &stepValue : testCase.value(u"steps"_s).toArray()) {
      const QJsonObject step = stepValue.toObject();
      if (keysOf(step) == QSet<QString>{u"shell_sends"_s}) {
        const QJsonObject message = step.value(u"shell_sends"_s).toObject();
        tracker.sent(message.value(u"type"_s).toString(), message.value(u"id"_s).toInteger(0));
        continue;
      }
      // The desktop has no worker host, so a case of this shell with one of its control messages is one the test does not know.
      QVERIFY2(keysOf(step) == (QSet<QString>{u"engine_sends"_s, u"expect"_s}), qPrintable(id + u": a step of an unknown kind"_s));
      const QJsonObject message = step.value(u"engine_sends"_s).toObject();
      const auto expected = expectedDecision(step.value(u"expect"_s));
      if (!expected) {
        QFAIL(qPrintable(id + u": an unknown expectation "_s + compact(step.value(u"expect"_s))));
      }
      const QJsonObject payload = message.value(u"payload"_s).toObject();
      const SessionTracker::Decision decision = message.contains(u"re"_s)
                                                    ? tracker.reply(message.value(u"re"_s).toInteger(), message.value(u"ok"_s).toBool(),
                                                                    payload, message.value(u"error"_s).toObject())
                                                    : tracker.event(message.value(u"type"_s).toString(), payload);
      QVERIFY2(decision == *expected, qPrintable(id + u": "_s + compact(message)));
    }
    ++run;
  }
  QVERIFY(run >= 6);
}

void TestConformance::tileGridCases() {
  const auto cases = recognizedCases(u"tile-grid"_s, {
                                                         {
                                                             .fields =
                                                                 {
                                                                     u"id"_s,
                                                                     u"description"_s,
                                                                     u"for"_s,
                                                                     u"page"_s,
                                                                     u"zoom"_s,
                                                                     u"device_scale"_s,
                                                                     u"tile_size"_s,
                                                                     u"expect"_s,
                                                                 },
                                                             .forValues = {u"shell"_s},
                                                         },
                                                     });
  if (!cases) {
    QFAIL("the vectors could not be read");
  }
  int run = 0;
  for (const auto &value : *cases) {
    const QJsonObject testCase = value.toObject();
    const QString id = testCase.value(u"id"_s).toString();
    const QJsonObject size = testCase.value(u"page"_s).toObject();
    const protocol::Page page{
        .index = 0,
        .width = size.value(u"width"_s).toInteger(),
        .height = size.value(u"height"_s).toInteger(),
        .section = 0,
        .x = 0,
        .y = 0,
    };
    // s = zoom × device_scale, in double precision (§14.6 item 3).
    const double scale = testCase.value(u"zoom"_s).toDouble() * testCase.value(u"device_scale"_s).toDouble();
    const int tileSize = testCase.value(u"tile_size"_s).toInt();
    const TileGrid grid(scale, tileSize);
    QVERIFY2(grid.tileSize() == tileSize, qPrintable(id + u": a tile size the canvas does not use"_s));
    const QJsonObject expected = testCase.value(u"expect"_s).toObject();
    const QJsonObject pixels = expected.value(u"pixels"_s).toObject();
    QVERIFY2(grid.pageWidthPixels(page) == pixels.value(u"width"_s).toInteger(), qPrintable(id));
    QVERIFY2(grid.pageHeightPixels(page) == pixels.value(u"height"_s).toInteger(), qPrintable(id));
    QVERIFY2(grid.columns(page) == expected.value(u"columns"_s).toInt(), qPrintable(id));
    QVERIFY2(grid.rows(page) == expected.value(u"rows"_s).toInt(), qPrintable(id));
    for (const auto &tileValue : expected.value(u"tiles"_s).toArray()) {
      const QJsonObject tile = tileValue.toObject();
      const int column = tile.value(u"column"_s).toInt();
      const int row = tile.value(u"row"_s).toInt();
      const DeviceRect devicePixels = grid.tilePixels(page, column, row);
      const QJsonObject wantedPixels = tile.value(u"pixels"_s).toObject();
      QVERIFY2((devicePixels == DeviceRect{.x = wantedPixels.value(u"x"_s).toInteger(),
                                           .y = wantedPixels.value(u"y"_s).toInteger(),
                                           .width = wantedPixels.value(u"width"_s).toInteger(),
                                           .height = wantedPixels.value(u"height"_s).toInteger()}),
               qPrintable(id + u": the pixels of tile "_s + QString::number(column) + u","_s + QString::number(row)));
      QVERIFY2(protocol::toJson(grid.tileRect(page, column, row)) == tile.value(u"blu"_s).toObject(),
               qPrintable(id + u": the rectangle of tile "_s + QString::number(column) + u","_s + QString::number(row)));
    }
    ++run;
  }
  QVERIFY(run >= 10);
}

void TestConformance::accessibilityLimitCases() {
  const auto cases = recognizedCases(
      u"a11y-limits"_s,
      {
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"root_level"_s, u"tree"_s, u"expect"_s}, .forValues = {u"shell"_s}},
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"tree"_s, u"expect"_s}, .forValues = {u"engine"_s}},
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"tree"_s, u"caret"_s, u"expect"_s}, .forValues = {u"engine"_s}},
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"tree"_s, u"query"_s, u"expect"_s}, .forValues = {u"engine"_s}},
      });
  if (!cases) {
    QFAIL("the vectors could not be read");
  }
  int run = 0;
  for (const auto &value : *cases) {
    const QJsonObject testCase = value.toObject();
    if (!isOurs(testCase)) {
      continue;
    }
    const QString id = testCase.value(u"id"_s).toString();
    const auto tree = generatedTree(testCase.value(u"tree"_s).toObject());
    if (!tree) {
      QFAIL(qPrintable(id + u": a tree of an unknown generator"_s));
    }
    const QJsonObject expected = testCase.value(u"expect"_s).toObject();
    QVERIFY2(keysOf(expected) == (QSet<QString>{u"accept"_s, u"queries"_s}), qPrintable(id + u": an unknown expectation"_s));
    // A tree beyond the limits, or one that contradicts itself, breaks the protocol: the shell does not accept it (§14.1 item 5).
    const auto parsed = protocol::parseAccessibleTree(*tree, 1, testCase.value(u"root_level"_s).toInt());
    QVERIFY2(parsed.has_value() == expected.value(u"accept"_s).toBool(), qPrintable(id));
    QJsonArray queries;
    if (parsed) {
      for (const protocol::MissingChildren &missing : parsed->missingChildren()) {
        queries.append(QJsonObject{
            {u"node"_s, static_cast<qint64>(missing.node)},
            {u"children"_s, QJsonObject{{u"start"_s, qint64{missing.start}}, {u"count"_s, qint64{missing.count}}}},
        });
      }
    }
    QVERIFY2(queries == expected.value(u"queries"_s).toArray(), qPrintable(id + u": "_s + compact(queries)));
    ++run;
  }
  QVERIFY(run >= 10);
}

void TestConformance::keyCases() {
  const auto cases = recognizedCases(
      u"keys"_s,
      {
          {.fields = {u"id"_s, u"description"_s, u"for"_s, u"platform"_s, u"runtime"_s, u"expect"_s}, .forValues = {u"engine"_s}},
          {
              .fields = {u"id"_s, u"description"_s, u"for"_s, u"manifest"_s, u"layout"_s, u"press"_s, u"expect"_s},
              .forValues = {u"engine"_s, u"shell"_s},
          },
      });
  if (!cases) {
    QFAIL("the vectors could not be read");
  }
  // The key data of every platform and runtime, as the engine resolves them: the shell must be able to read each of them.
  QMap<QString, KeyBindings> manifests;
  for (const auto &value : *cases) {
    const QJsonObject testCase = value.toObject();
    if (testCase.contains(u"platform"_s)) {
      const auto manifest = manifestOf(testCase);
      if (!manifest) {
        QFAIL(qPrintable(testCase.value(u"id"_s).toString() + u": key data the shell cannot read"_s));
      }
      manifests.insert(testCase.value(u"id"_s).toString(), KeyBindings(*manifest));
    }
  }
  QCOMPARE(manifests.size(), 14);
  int run = 0;
  for (const auto &value : *cases) {
    const QJsonObject testCase = value.toObject();
    if (!isOurs(testCase)) {
      continue;
    }
    const QString id = testCase.value(u"id"_s).toString();
    const auto press = pressOf(testCase.value(u"press"_s).toObject());
    if (!press) {
      QFAIL(qPrintable(id + u": a press with unknown fields"_s));
    }
    const QString manifest = testCase.value(u"manifest"_s).toString();
    QVERIFY2(manifests.contains(manifest), qPrintable(id + u": no manifest case "_s + manifest));
    const QJsonObject expected = testCase.value(u"expect"_s).toObject();
    QVERIFY2(expected.value(u"send"_s).isBool(), qPrintable(id + u": an unknown expectation"_s));
    QVERIFY2(manifests.value(manifest).sends(*press) == expected.value(u"send"_s).toBool(), qPrintable(id));
    ++run;
  }
  QVERIFY(run >= 30);
}

void TestConformance::unknownCasesAreNotRecognized() {
  // The recognizer itself: a case marked for a party it does not know, or with a field it does not know, fails, as does a file of another
  // format version.
  const QList<CaseKind> kinds{{.fields = {u"id"_s, u"description"_s, u"for"_s, u"steps"_s}, .forValues = {u"shell"_s}}};
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"the case .* is not one this test recognizes"_s));
  QVERIFY(!recognizedCases(u"restart"_s, kinds));
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"missing"_s));
  QVERIFY(!recognizedCases(u"no-such-area"_s, kinds));
}

QTEST_GUILESS_MAIN(TestConformance)
#include "tst_conformance.moc"
