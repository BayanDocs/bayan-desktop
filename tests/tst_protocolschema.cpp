// Unit tests of the tests' checker of the engine protocol's JSON Schema (tests/support/protocolschema.h): it accepts the messages the
// engine sends, refuses those it could not send, and refuses to load a schema that uses anything it does not implement. It needs no engine.

#include "protocolschema.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

using namespace Qt::StringLiterals;
using bayan::test::ProtocolSchema;

namespace {

const ProtocolSchema &schema() { return ProtocolSchema::engineProtocol(); }

QJsonObject engineEvent(const QString &type, const QJsonObject &payload) {
  return QJsonObject{{u"v"_s, 0}, {u"type"_s, type}, {u"seq"_s, 7}, {u"payload"_s, payload}};
}

// engine.error as bayan-core's engine sends it after a panic (engine protocol §12).
QJsonObject panicError() {
  return QJsonObject{
      {u"code"_s, u"panic"_s},
      {u"message_id"_s, u"engine-error-panic"_s},
      {u"recoverable"_s, false},
      {u"args"_s, QJsonObject{}},
  };
}

// Whether one of the problems starts with this place and contains this text.
bool mentions(const QStringList &problems, const QString &pointer, const QString &text) {
  return std::ranges::any_of(
      problems, [&](const QString &problem) { return problem.trimmed().startsWith(pointer + u": "_s) && problem.contains(text); });
}

// Loads a small schema written for a test, and returns the problems of loading it.
QStringList loadProblems(const QJsonObject &root) {
  QJsonObject withDraft = root;
  if (!withDraft.contains(u"$schema"_s)) {
    withDraft.insert(u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s);
  }
  QStringList problems;
  const auto loaded = ProtocolSchema::load(QJsonDocument(withDraft).toJson(), problems);
  if (loaded && !problems.isEmpty()) {
    return {u"loaded although it has problems"_s};
  }
  return problems;
}

} // namespace

class TestProtocolSchema final : public QObject {
  Q_OBJECT

private slots:
  void loadsTheEngineProtocolSchema();
  void acceptsTheEnginesMessages();
  void rejectsEngineErrorWithANestedError();
  void rejectsAFieldOfTheWrongType();
  void rejectsAnUnknownFieldWhereTheSchemaForbidsOne();
  void rejectsWhatTheEngineNeverSends();
  void checksRepliesAgainstTheirRequest();
  void checksTheTypeOfEachReply();
  void numbersFollowTheirRustTypes();
  void alternativesAndBooleanSchemas();
  void refusesASchemaWithAKeywordItDoesNotImplement();
  void refusesUnknownFormatsReferencesAndDrafts();
};

void TestProtocolSchema::loadsTheEngineProtocolSchema() {
  // The copy in src/engine/stub/schema loads (engineProtocol() stops the program otherwise) and gives each request its reply's type.
  QCOMPARE(schema().replyDefinition(u"hello"_s), u"Welcome"_s);
  QCOMPARE(schema().replyDefinition(u"doc.open"_s), u"DocOpened"_s);
  QCOMPARE(schema().replyDefinition(u"view.set"_s), u"Empty"_s);
  QCOMPARE(schema().replyDefinition(u"query.a11y"_s), u"A11yTree"_s);
  QCOMPARE(schema().replyDefinition(u"view.pages"_s), QString()); // an event, not a request
  // Only the replies to hello and doc.open have a type of their own (reply_type).
  QCOMPARE(schema().replyType(u"hello"_s), u"welcome"_s);
  QCOMPARE(schema().replyType(u"doc.open"_s), u"doc.opened"_s);
  QCOMPARE(schema().replyType(u"view.set"_s), QString());
  QCOMPARE(schema().replyType(u"query.a11y"_s), QString());
}

void TestProtocolSchema::acceptsTheEnginesMessages() {
  QCOMPARE(schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, panicError())), QStringList());
  const QJsonObject welcome{
      {u"v"_s, 0},
      {u"re"_s, 1},
      {u"ok"_s, true},
      {u"type"_s, u"welcome"_s},
      {
          u"payload"_s,
          QJsonObject{
              {u"protocol_version"_s, 0},
              {u"engine_version"_s, u"0.1.0"_s},
              {u"layout_epoch"_s, 0},
              {u"features"_s, QJsonArray{u"mock_document"_s}},
          },
      },
  };
  QCOMPARE(schema().problemsOfMessageFromEngine(welcome, u"hello"_s), QStringList());
  const QJsonObject refused{
      {u"v"_s, 0},
      {u"re"_s, 2},
      {u"ok"_s, false},
      {
          u"error"_s,
          QJsonObject{
              {u"code"_s, u"unsupported_message"_s},
              {u"message_id"_s, u"engine-error-unsupported-message"_s},
              {u"args"_s, QJsonObject{{u"type"_s, u"no.such.message"_s}}},
          },
      },
  };
  QCOMPARE(schema().problemsOfMessageFromEngine(refused), QStringList());
  const QJsonObject page{{u"index"_s, 0}, {u"width"_s, 15'544'800}, {u"height"_s, 20'116'800}, {u"section"_s, 0}, {u"x"_s, 0}, {u"y"_s, 0}};
  QCOMPARE(
      schema().problemsOfMessageFromEngine(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, 1}, {u"pages"_s, QJsonArray{page}}})),
      QStringList());
  // Both sides ignore fields they do not know (engine protocol §4), and the schema allows them wherever it does not restrict them.
  QJsonObject later = panicError();
  later.insert(u"added_in_a_later_version"_s, true);
  QCOMPARE(schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, later)), QStringList());
}

void TestProtocolSchema::rejectsEngineErrorWithANestedError() {
  // The shape that tst_documentsession built by hand before DESK-003: the fields of the error nested under "error", as in a failed reply.
  const QJsonObject nested{
      {u"error"_s, QJsonObject{{u"code"_s, u"internal"_s}, {u"message_id"_s, u"engine-error-internal"_s}, {u"args"_s, QJsonObject{}}}},
      {u"recoverable"_s, false},
  };
  const QStringList problems = schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, nested));
  QVERIFY2(!problems.isEmpty(), "the nested error was accepted");
  for (const QString &field : {u"code"_s, u"message_id"_s, u"args"_s}) {
    QVERIFY2(mentions(problems, u"/payload"_s, u"lacks the required field "_s + field), qPrintable(problems.join(u'\n')));
  }
}

void TestProtocolSchema::rejectsAFieldOfTheWrongType() {
  QJsonObject error = panicError();
  error.insert(u"recoverable"_s, u"false"_s);
  const QStringList problems = schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, error));
  QVERIFY2(mentions(problems, u"/payload/recoverable"_s, u"is a string, not of type \"boolean\""_s), qPrintable(problems.join(u'\n')));
  // A code the protocol does not define is refused too: ErrorCode is a closed list.
  error = panicError();
  error.insert(u"code"_s, u"<b>marker</b>"_s);
  const QStringList unknownCode = schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, error));
  QVERIFY2(mentions(unknownCode, u"/payload/code"_s, u"matches none of the 14 alternatives of oneOf"_s),
           qPrintable(unknownCode.join(u'\n')));
}

void TestProtocolSchema::rejectsAnUnknownFieldWhereTheSchemaForbidsOne() {
  // The protocol's schema restricts unknown fields in one place: the args of an error or a warning may hold only codes and numbers
  // (strings, integers, booleans and lists of integers), never content such as an object quoting a document.
  QJsonObject error = panicError();
  error.insert(u"args"_s, QJsonObject{{u"limit"_s, u"documents"_s}, {u"quote"_s, QJsonObject{{u"text"_s, u"from the document"_s}}}});
  QStringList problems = schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, error));
  QVERIFY2(mentions(problems, u"/payload/args/quote"_s, u"matches none of the 4 alternatives of anyOf"_s),
           qPrintable(problems.join(u'\n')));
  QVERIFY(!mentions(problems, u"/payload/args/limit"_s, QString()));
  error.insert(u"args"_s, QJsonObject{{u"limit"_s, 1.5}});
  QVERIFY(
      mentions(schema().problemsOfMessageFromEngine(engineEvent(u"engine.error"_s, error)), u"/payload/args/limit"_s, u"matches none"_s));

  // Where a schema forbids unknown fields altogether (additionalProperties: false), the checker refuses them.
  QStringList loading;
  const auto closed = ProtocolSchema::load(QJsonDocument(QJsonObject{
                                                             {u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s},
                                                             {u"type"_s, u"object"_s},
                                                             {u"properties"_s, QJsonObject{{u"known"_s, QJsonObject{}}}},
                                                             {u"additionalProperties"_s, false},
                                                         })
                                               .toJson(),
                                           loading);
  if (!closed) {
    QFAIL(qPrintable(loading.join(u'\n')));
  }
  QCOMPARE(closed->problems(QJsonObject{{u"known"_s, 1}}), QStringList());
  problems = closed->problems(QJsonObject{{u"known"_s, 1}, {u"unknown"_s, 2}});
  QVERIFY2(mentions(problems, u"/unknown"_s, u"no value is allowed here"_s), qPrintable(problems.join(u'\n')));
}

void TestProtocolSchema::rejectsWhatTheEngineNeverSends() {
  // A request is a message to the engine, never from it.
  const QJsonObject hello{
      {u"v"_s, 0},
      {u"id"_s, 1},
      {u"type"_s, u"hello"_s},
      {u"payload"_s, QJsonObject{{u"protocol_versions"_s, QJsonArray{0}}}},
  };
  QVERIFY(!schema().problemsOfMessageFromEngine(hello).isEmpty());
  // An event type that protocol v0 does not have (diag.log is planned, not defined).
  QVERIFY(!schema().problemsOfMessageFromEngine(engineEvent(u"diag.log"_s, QJsonObject{{u"level"_s, u"info"_s}})).isEmpty());
  // An event without its sequence number, and one for another protocol version.
  QJsonObject unnumbered = engineEvent(u"engine.error"_s, panicError());
  unnumbered.remove(u"seq"_s);
  QVERIFY(!schema().problemsOfMessageFromEngine(unnumbered).isEmpty());
  QJsonObject future = engineEvent(u"engine.error"_s, panicError());
  future.insert(u"v"_s, 1);
  QVERIFY(!schema().problemsOfMessageFromEngine(future).isEmpty());
  // A reply without its payload, which a successful reply always has.
  QVERIFY(!schema().problemsOfMessageFromEngine(QJsonObject{{u"v"_s, 0}, {u"re"_s, 1}, {u"ok"_s, true}}, u"view.set"_s).isEmpty());
  // A page list that is not a list.
  QVERIFY(!schema()
               .problemsOfMessageFromEngine(engineEvent(u"view.pages"_s, QJsonObject{{u"doc_id"_s, 1}, {u"pages"_s, u"three"_s}}))
               .isEmpty());
}

void TestProtocolSchema::checksRepliesAgainstTheirRequest() {
  const QJsonObject empty{{u"v"_s, 0}, {u"re"_s, 3}, {u"ok"_s, true}, {u"payload"_s, QJsonObject{}}};
  QCOMPARE(schema().problemsOfMessageFromEngine(empty, u"view.set"_s), QStringList());
  // Without the request's type the payload could not be checked, so the reply is refused.
  QVERIFY(!schema().problemsOfMessageFromEngine(empty).isEmpty());
  // An empty payload is not a welcome, and a request type without a reply cannot be answered successfully.
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(empty, u"hello"_s), u"/payload"_s, u"lacks the required field protocol_version"_s));
  QVERIFY(!schema().problemsOfMessageFromEngine(empty, u"no.such.message"_s).isEmpty());
}

void TestProtocolSchema::checksTheTypeOfEachReply() {
  // A successful reply carries the type that x-bayan-messages gives the reply to its request (reply_type), and none where it gives none,
  // as bayan-core's engine sends it (Outbox::reply) and the stub does.
  const auto reply = [](const QJsonObject &payload, const QJsonValue &type) {
    QJsonObject message{{u"v"_s, 0}, {u"re"_s, 1}, {u"ok"_s, true}, {u"payload"_s, payload}};
    if (!type.isUndefined()) {
      message.insert(u"type"_s, type);
    }
    return message;
  };
  const QJsonObject welcome{
      {u"protocol_version"_s, 0},
      {u"engine_version"_s, u"0.1.0"_s},
      {u"layout_epoch"_s, 0},
      {u"features"_s, QJsonArray{u"mock_document"_s}},
  };
  const QJsonObject opened{
      {u"doc_id"_s, 1},
      {u"page_count_estimate"_s, 1},
      {u"warnings"_s, QJsonArray{}},
      {u"fonts"_s, QJsonObject{{u"missing"_s, QJsonArray{}}, {u"machine_dependent"_s, false}}},
  };
  QCOMPARE(schema().problemsOfMessageFromEngine(reply(welcome, u"welcome"_s), u"hello"_s), QStringList());
  QCOMPARE(schema().problemsOfMessageFromEngine(reply(opened, u"doc.opened"_s), u"doc.open"_s), QStringList());
  QCOMPARE(schema().problemsOfMessageFromEngine(reply(QJsonObject{}, QJsonValue::Undefined), u"view.set"_s), QStringList());

  // A valid welcome with the type of another reply, or with none, is not the reply to hello; nor is a reply to view.set that has a type.
  const QJsonObject mistyped = reply(welcome, u"doc.opened"_s);
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(mistyped, u"hello"_s), u"/type"_s, u"not \"welcome\""_s));
  QCOMPARE(schema().placesOfProblemsOfMessageFromEngine(mistyped, u"hello"_s), QStringList{u"/type"_s});
  const QJsonObject untyped = reply(welcome, QJsonValue::Undefined);
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(untyped, u"hello"_s), u"(the whole value)"_s, u"lacks the field type"_s));
  QCOMPARE(schema().placesOfProblemsOfMessageFromEngine(untyped, u"hello"_s), QStringList{u"/type"_s});
  const QJsonObject typedEmpty = reply(QJsonObject{}, u"welcome"_s);
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(typedEmpty, u"view.set"_s), u"/type"_s, u"has no type"_s));
  QCOMPARE(schema().placesOfProblemsOfMessageFromEngine(typedEmpty, u"view.set"_s), QStringList{u"/type"_s});
  // The typed ones could not come from the engine in reply to any request; the well-typed welcome could. So could the untyped one, but only
  // as the reply to a request such as view.set, whose payload (Empty) may hold any fields (engine protocol §4).
  QVERIFY(!schema().couldComeFromEngine(mistyped));
  QVERIFY(!schema().couldComeFromEngine(typedEmpty));
  QVERIFY(schema().couldComeFromEngine(reply(welcome, u"welcome"_s)));
  QVERIFY(schema().couldComeFromEngine(untyped));
  QCOMPARE(schema().problemsOfMessageFromEngine(untyped, u"view.set"_s), QStringList());
  // A type is a string in any reply.
  QVERIFY(!schema().problemsOfMessageFromEngine(reply(welcome, 1), u"hello"_s).isEmpty());

  // reply_type is read strictly: a string, on an entry that has a reply.
  const QJsonObject thing{{u"Thing"_s, QJsonObject{}}};
  const auto entry = [](const QJsonValue &replyType, bool withReply) {
    QJsonObject result{{u"direction"_s, u"to_engine"_s}, {u"type"_s, u"thing.get"_s}, {u"payload"_s, u"#/$defs/Thing"_s}};
    if (withReply) {
      result.insert(u"reply"_s, u"#/$defs/Thing"_s);
    }
    result.insert(u"reply_type"_s, replyType);
    return result;
  };
  for (const QJsonObject &wrong : {entry(3, true), entry(u""_s, true), entry(u"thing"_s, false)}) {
    QVERIFY(mentions(loadProblems(QJsonObject{{u"$defs"_s, thing}, {u"x-bayan-messages"_s, QJsonArray{wrong}}}),
                     u"#/x-bayan-messages (thing.get)"_s, u"reply_type"_s));
  }
  QCOMPARE(loadProblems(QJsonObject{{u"$defs"_s, thing}, {u"x-bayan-messages"_s, QJsonArray{entry(u"thing"_s, true)}}}), QStringList());
}

void TestProtocolSchema::numbersFollowTheirRustTypes() {
  const auto progress = [](const QJsonValue &pagesLaidOut) {
    return engineEvent(
        u"view.layout.progress"_s,
        QJsonObject{{u"doc_id"_s, 1}, {u"pages_laid_out"_s, pagesLaidOut}, {u"page_count_estimate"_s, 3}, {u"complete"_s, true}});
  };
  QCOMPARE(schema().problemsOfMessageFromEngine(progress(3)), QStringList());
  QCOMPARE(schema().problemsOfMessageFromEngine(progress(3.0)), QStringList()); // a whole number, as JSON Schema counts it
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(progress(2.5)), u"/payload/pages_laid_out"_s, u"not of type \"integer\""_s));
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(progress(-1)), u"/payload/pages_laid_out"_s, u"below the minimum 0"_s));
  // pages_laid_out is a u32 in the engine: 2^32 does not fit, although the schema gives no maximum.
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(progress(static_cast<qint64>(quint64{1} << 32U))), u"/payload/pages_laid_out"_s,
                   u"format uint32"_s));
  QCOMPARE(schema().problemsOfMessageFromEngine(progress(qint64{0xFFFF'FFFF})), QStringList());
  // seq stays below 2^53, so that JavaScript numbers hold it exactly.
  QJsonObject late = progress(3);
  late.insert(u"seq"_s, static_cast<qint64>(quint64{1} << 53U));
  QVERIFY(mentions(schema().problemsOfMessageFromEngine(late), u"/seq"_s, u"above the maximum 9007199254740991"_s));
  late.insert(u"seq"_s, static_cast<qint64>(quint64{1} << 53U) - 1);
  QCOMPARE(schema().problemsOfMessageFromEngine(late), QStringList());
}

void TestProtocolSchema::alternativesAndBooleanSchemas() {
  QStringList loading;
  const auto alternatives = ProtocolSchema::load(QJsonDocument(QJsonObject{
                                                                   {u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s},
                                                                   {
                                                                       u"$defs"_s,
                                                                       QJsonObject{
                                                                           {
                                                                               u"One"_s,
                                                                               QJsonObject{
                                                                                   {
                                                                                       u"oneOf"_s,
                                                                                       QJsonArray{
                                                                                           QJsonObject{{u"type"_s, u"integer"_s}},
                                                                                           QJsonObject{{u"type"_s, u"number"_s}},
                                                                                       },
                                                                                   },
                                                                               },
                                                                           },
                                                                           {
                                                                               u"Any"_s,
                                                                               QJsonObject{
                                                                                   {
                                                                                       u"anyOf"_s,
                                                                                       QJsonArray{
                                                                                           QJsonObject{{u"type"_s, u"integer"_s}},
                                                                                           QJsonObject{{u"type"_s, u"number"_s}},
                                                                                       },
                                                                                   },
                                                                               },
                                                                           },
                                                                           {u"Everything"_s, QJsonObject{{u"items"_s, true}}},
                                                                           {u"Nothing"_s, QJsonObject{{u"items"_s, false}}},
                                                                       },
                                                                   },
                                                               })
                                                     .toJson(),
                                                 loading);
  if (!alternatives) {
    QFAIL(qPrintable(loading.join(u'\n')));
  }
  const ProtocolSchema &checker = *alternatives;
  QCOMPARE(checker.problems(1.5, u"One"_s), QStringList());
  // 1 is an integer and a number: two alternatives of oneOf match, where exactly one must.
  QVERIFY(mentions(checker.problems(1, u"One"_s), u"(the whole value)"_s, u"matches 2 of the alternatives of oneOf"_s));
  QCOMPARE(checker.problems(1, u"Any"_s), QStringList());
  QVERIFY(!checker.problems(u"one"_s, u"Any"_s).isEmpty());
  QCOMPARE(checker.problems(QJsonArray{1, u"two"_s, QJsonObject{}}, u"Everything"_s), QStringList());
  QCOMPARE(checker.problems(QJsonArray{}, u"Nothing"_s), QStringList());
  QVERIFY(mentions(checker.problems(QJsonArray{1}, u"Nothing"_s), u"/0"_s, u"no value is allowed here"_s));
  QVERIFY(!checker.problems(1, u"Missing"_s).isEmpty());
}

void TestProtocolSchema::refusesASchemaWithAKeywordItDoesNotImplement() {
  // Keywords of JSON Schema that the generated schema does not use, wherever they appear: loading fails and names them, so that a later
  // schema that needs them cannot pass unchecked.
  const QList<QPair<QString, QJsonValue>> unimplemented{
      {u"pattern"_s, u"^[a-z]+$"_s},
      {u"minLength"_s, 1},
      {u"maxItems"_s, 3},
      {u"enum"_s, QJsonArray{u"a"_s}},
      {u"unevaluatedProperties"_s, false},
      {u"patternProperties"_s, QJsonObject{}},
      {u"prefixItems"_s, QJsonArray{}},
      {u"if"_s, QJsonObject{}},
      {u"allOf"_s, QJsonArray{QJsonObject{}}},
      {u"not"_s, QJsonObject{}},
      {u"$dynamicRef"_s, u"#meta"_s},
      {u"exclusiveMinimum"_s, 0},
      {u"dependentRequired"_s, QJsonObject{}},
      {u"contentEncoding"_s, u"base64"_s},
  };
  for (const auto &[keyword, value] : unimplemented) {
    const QJsonObject nested{{u"properties"_s, QJsonObject{{u"field"_s, QJsonObject{{u"type"_s, u"string"_s}, {keyword, value}}}}}};
    const QStringList problems = loadProblems(QJsonObject{{u"$defs"_s, QJsonObject{{u"Thing"_s, nested}}}});
    QVERIFY2(mentions(problems, u"#/$defs/Thing/properties/field"_s, u"does not implement the keyword "_s + keyword),
             qPrintable(keyword + u": "_s + problems.join(u'\n')));
  }
  // $defs, $schema and the protocol's extensions belong at the top only.
  QVERIFY(!loadProblems(QJsonObject{{u"$defs"_s, QJsonObject{{u"Thing"_s, QJsonObject{{u"$defs"_s, QJsonObject{}}}}}}}).isEmpty());
  // items as a list of schemas is the older drafts' form of prefixItems.
  QVERIFY(!loadProblems(QJsonObject{{u"items"_s, QJsonArray{QJsonObject{}}}}).isEmpty());
  // The protocol's own list of messages is read strictly too.
  const QJsonObject unknownField{
      {u"direction"_s, u"to_shell"_s},
      {u"type"_s, u"view.pages"_s},
      {u"payload"_s, u"#/$defs/Thing"_s},
      {u"reply_events"_s, QJsonArray{}},
  };
  QVERIFY(mentions(
      loadProblems(QJsonObject{{u"$defs"_s, QJsonObject{{u"Thing"_s, QJsonObject{}}}}, {u"x-bayan-messages"_s, QJsonArray{unknownField}}}),
      u"#/x-bayan-messages (view.pages)"_s, u"reply_events"_s));
  // What it implements loads.
  QCOMPARE(loadProblems(QJsonObject{{u"$defs"_s, QJsonObject{{u"Thing"_s, QJsonObject{{u"type"_s, QJsonArray{u"integer"_s, u"null"_s}},
                                                                                      {u"format"_s, u"uint32"_s},
                                                                                      {u"minimum"_s, 0},
                                                                                      {u"description"_s, u"A count."_s},
                                                                                      {u"default"_s, QJsonValue::Null}}}}}}),
           QStringList());
}

void TestProtocolSchema::refusesUnknownFormatsReferencesAndDrafts() {
  QVERIFY(mentions(loadProblems(QJsonObject{{u"format"_s, u"date-time"_s}}), u"#"_s, u"does not implement the format \"date-time\""_s));
  QVERIFY(mentions(loadProblems(QJsonObject{{u"format"_s, u"uint16"_s}}), u"#"_s, u"does not implement the format \"uint16\""_s));
  QVERIFY(mentions(loadProblems(QJsonObject{{u"$ref"_s, u"#/$defs/Missing"_s}}), u"#"_s, u"names no definition"_s));
  QVERIFY(mentions(loadProblems(QJsonObject{{u"$ref"_s, u"https://example.org/schema.json"_s}}), u"#"_s, u"names no definition"_s));
  QVERIFY(mentions(loadProblems(QJsonObject{{u"$schema"_s, u"http://json-schema.org/draft-07/schema#"_s}}), u"#"_s, u"$schema must be"_s));
  QVERIFY(mentions(loadProblems(QJsonObject{{u"type"_s, u"float"_s}}), u"#"_s, u"type must name"_s));
  QStringList problems;
  QVERIFY(!ProtocolSchema::load("[]"_ba, problems));
  QVERIFY(!problems.isEmpty());
}

QTEST_GUILESS_MAIN(TestProtocolSchema)
#include "tst_protocolschema.moc"
