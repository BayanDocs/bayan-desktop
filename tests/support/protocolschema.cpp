#include "protocolschema.h"

#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QStringView>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan::test {
namespace {

using Definitions = QHash<QString, QJsonValue>;

// A problem of a value: where it is (a JSON Pointer), what is wrong, whether it is a const that does not match (a sign that the value was
// meant for another alternative of a oneOf or anyOf), how deeply it is indented under the alternative it explains, the name of the
// field it misses, if that is the problem, and whether it only says that no alternative matched (the closest one's problems follow it).
struct Problem {
  QString pointer;
  QString text;
  bool constMismatch = false;
  int indent = 0;
  QString missingField{};
  bool listsAlternatives = false;
};
using Problems = QList<Problem>;

// The dialect of JSON Schema the checker implements.
constexpr QStringView kDraft = u"https://json-schema.org/draft/2020-12/schema";
// Where a $ref may point: the schema's own definitions.
constexpr QStringView kDefinitionsPrefix = u"#/$defs/";
// Nested checks beyond this depth are refused, so that no schema or message can exhaust the stack.
constexpr int kMaxDepth = 200;
// The formats the checker knows: those the generator gives Rust's number types.
constexpr std::array<QStringView, 6> kFormats{u"uint8", u"uint32", u"uint64", u"int32", u"int64", u"double"};
// The names of JSON Schema's types.
constexpr std::array<QStringView, 7> kTypeNames{u"null", u"boolean", u"object", u"array", u"number", u"string", u"integer"};
// The fields of an entry of x-bayan-messages.
constexpr std::array<QStringView, 6> kMessageFields{u"direction", u"type", u"payload", u"reply", u"reply_type", u"web_reply"};

bool isOneOf(const auto &names, QStringView name) { return std::ranges::find(names, name) != names.end(); }

// The name of the definition a $ref points to, or empty if it points anywhere else.
QString definitionName(const QString &reference) {
  if (!reference.startsWith(kDefinitionsPrefix)) {
    return {};
  }
  const QString name = reference.mid(kDefinitionsPrefix.size());
  // A name with / or ~ would need JSON Pointer's escapes, which the generated schema never uses.
  return name.contains(u'/') || name.contains(u'~') ? QString() : name;
}

// The place of a problem in the value, as a JSON Pointer, followed by a colon.
QString at(const QString &pointer) { return (pointer.isEmpty() ? u"(the whole value)"_s : pointer) + u": "_s; }

QStringList written(const Problems &problems) {
  QStringList lines;
  for (const Problem &problem : problems) {
    lines.append(QString(qsizetype{problem.indent} * 2, u' ') + at(problem.pointer) + problem.text);
  }
  return lines;
}

// How close a failed alternative came to matching, smaller being closer: first how deep its deepest problem lies (the deeper, the more of
// the value matched before it failed), then how many consts it mismatched (a mismatched discriminator, such as an event's type, means that
// the value was meant for another alternative), then how many problems it has.
struct Closeness {
  qsizetype negativeDepth = 0;
  qsizetype constMismatches = 0;
  qsizetype count = 0;
  friend auto operator<=>(const Closeness &, const Closeness &) = default;
};

Closeness closeness(const Problems &problems) {
  Closeness result{.negativeDepth = 0, .constMismatches = 0, .count = problems.size()};
  for (const Problem &problem : problems) {
    result.constMismatches += problem.constMismatch ? 1 : 0;
    result.negativeDepth = std::min(result.negativeDepth, -problem.pointer.count(u'/'));
  }
  return result;
}

// A field name as a JSON Pointer reference token.
QString escaped(QString name) { return name.replace(u'~', u"~0"_s).replace(u'/', u"~1"_s); }

// Where a problem is, as a JSON Pointer: the wrong value's place, or the place where a missing field belongs.
QString place(const Problem &problem) {
  return problem.missingField.isEmpty() ? problem.pointer : problem.pointer + u'/' + escaped(problem.missingField);
}

QString compact(const QJsonValue &value) {
  // Wrapped in an array, written, and unwrapped again, which works for every kind of value.
  const QString wrapped = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
  return wrapped.mid(1, wrapped.size() - 2);
}

QString typeName(const QJsonValue &value) {
  switch (value.type()) {
  case QJsonValue::Null:
    return u"null"_s;
  case QJsonValue::Bool:
    return u"a boolean"_s;
  case QJsonValue::Double:
    return u"a number"_s;
  case QJsonValue::String:
    return u"a string"_s;
  case QJsonValue::Array:
    return u"an array"_s;
  case QJsonValue::Object:
    return u"an object"_s;
  case QJsonValue::Undefined:
    break;
  }
  return u"missing"_s;
}

// A JSON number as an exact integer, if it is a whole number in qint64's range (Qt keeps such numbers exactly). toInteger() returns its
// argument for any other number, so two different arguments tell the cases apart.
std::optional<qint64> exactInteger(const QJsonValue &value) {
  const qint64 first = value.toInteger(0);
  return first == value.toInteger(1) ? std::optional(first) : std::nullopt;
}

// Whether a JSON number is a whole number, as JSON Schema's "integer" asks (1.0 is one).
bool isInteger(const QJsonValue &value) {
  if (!value.isDouble()) {
    return false;
  }
  if (exactInteger(value)) {
    return true;
  }
  const double number = value.toDouble();
  return std::isfinite(number) && std::trunc(number) == number;
}

// Orders two JSON numbers: exactly if both are whole numbers in qint64's range, otherwise as doubles.
std::partial_ordering compare(const QJsonValue &left, const QJsonValue &right) {
  const auto exactLeft = exactInteger(left);
  const auto exactRight = exactInteger(right);
  if (exactLeft && exactRight) {
    return *exactLeft <=> *exactRight;
  }
  return left.toDouble() <=> right.toDouble();
}

// Whether a JSON number fits the Rust type that its format names.
bool fitsFormat(const QJsonValue &number, QStringView format) {
  if (format == u"double") {
    // Any JSON number: JSON has no infinities and no NaN.
    return true;
  }
  const std::optional<qint64> exact = exactInteger(number);
  if (format == u"uint64" && !exact) {
    // Whole numbers from 2^63 to 2^64 - 1 do not fit in a qint64, and Qt keeps them as doubles.
    const double value = number.toDouble();
    return std::trunc(value) == value && value >= 0x1p63 && value < 0x1p64;
  }
  if (!exact) {
    return false;
  }
  if (format == u"uint64") {
    return *exact >= 0;
  }
  if (format == u"int64") {
    return true;
  }
  if (format == u"int32") {
    return *exact >= std::numeric_limits<std::int32_t>::min() && *exact <= std::numeric_limits<std::int32_t>::max();
  }
  if (format == u"uint32") {
    return *exact >= 0 && std::cmp_less_equal(*exact, std::numeric_limits<std::uint32_t>::max());
  }
  if (format == u"uint8") {
    return *exact >= 0 && std::cmp_less_equal(*exact, std::numeric_limits<std::uint8_t>::max());
  }
  return false;
}

bool hasTypeName(const QJsonValue &value, QStringView name) {
  if (name == u"null") {
    return value.isNull();
  }
  if (name == u"boolean") {
    return value.isBool();
  }
  if (name == u"object") {
    return value.isObject();
  }
  if (name == u"array") {
    return value.isArray();
  }
  if (name == u"string") {
    return value.isString();
  }
  if (name == u"number") {
    return value.isDouble();
  }
  if (name == u"integer") {
    return isInteger(value);
  }
  return false;
}

// Whether a value has the type that a "type" keyword names: one name, or a list of them.
bool hasType(const QJsonValue &value, const QJsonValue &type) {
  const QJsonArray names = type.isArray() ? type.toArray() : QJsonArray{type};
  return std::ranges::any_of(names, [&value](const QJsonValue &name) { return hasTypeName(value, name.toString()); });
}

// Checks, before anything is validated, that a schema uses only what the checker implements, and collects its references. root is true for
// the schema's top level, the only place for $schema, $id, $defs and the protocol's own extensions.
// NOLINTNEXTLINE(misc-no-recursion): schemas nest; the depth is limited by kMaxDepth.
void inspect(const QJsonValue &node, const QString &path, bool root, int depth, QStringList &references, QStringList &problems) {
  if (depth > kMaxDepth) {
    problems.append(path + u": the schema nests more than %1 levels deep"_s.arg(kMaxDepth));
    return;
  }
  if (node.isBool()) {
    return;
  }
  if (!node.isObject()) {
    problems.append(path + u": a schema must be an object or a boolean, not %1"_s.arg(typeName(node)));
    return;
  }
  const QJsonObject keywords = node.toObject();
  for (auto entry = keywords.begin(); entry != keywords.end(); ++entry) {
    const QString &keyword = entry.key();
    const QJsonValue value = entry.value();
    // Annotations that are strings, the top's $schema and $id among them; and keywords whose value is any value, or which load() reads.
    const bool text = keyword == u"title"_s || keyword == u"description"_s || keyword == u"x-bayan-web-type"_s ||
                      (root && (keyword == u"$schema"_s || keyword == u"$id"_s));
    const bool anything = keyword == u"default"_s || keyword == u"const"_s ||
                          (root && (keyword == u"x-bayan-messages"_s || keyword == u"x-bayan-protocol-version"_s));
    if (text) {
      if (!value.isString()) {
        problems.append(path + u": %1 must be a string"_s.arg(keyword));
      }
    } else if (anything) {
      // Nothing to check here.
    } else if (keyword == u"$ref"_s) {
      if (value.isString()) {
        references.append(value.toString());
      } else {
        problems.append(path + u": $ref must be a string"_s);
      }
    } else if (keyword == u"type"_s) {
      const QJsonArray names = value.isArray() ? value.toArray() : QJsonArray{value};
      if (names.isEmpty() ||
          !std::ranges::all_of(names, [](const QJsonValue &name) { return name.isString() && isOneOf(kTypeNames, name.toString()); })) {
        problems.append(path + u": type must name JSON Schema's types"_s);
      }
    } else if (keyword == u"required"_s) {
      if (!value.isArray() || !std::ranges::all_of(value.toArray(), [](const QJsonValue &name) { return name.isString(); })) {
        problems.append(path + u": required must be a list of names"_s);
      }
    } else if (keyword == u"minimum"_s || keyword == u"maximum"_s) {
      if (!value.isDouble()) {
        problems.append(path + u": %1 must be a number"_s.arg(keyword));
      }
    } else if (keyword == u"format"_s) {
      if (!value.isString() || !isOneOf(kFormats, value.toString())) {
        problems.append(path + u": the checker does not implement the format %1"_s.arg(compact(value)));
      }
    } else if (keyword == u"properties"_s || (root && keyword == u"$defs"_s)) {
      if (!value.isObject()) {
        problems.append(path + u": %1 must be an object of schemas"_s.arg(keyword));
        continue;
      }
      const QJsonObject schemas = value.toObject();
      for (auto schema = schemas.begin(); schema != schemas.end(); ++schema) {
        inspect(schema.value(), path + u'/' + keyword + u'/' + escaped(schema.key()), false, depth + 1, references, problems);
      }
    } else if (keyword == u"additionalProperties"_s || keyword == u"items"_s) {
      // In draft 2020-12, items is one schema for every element; lists of schemas are prefixItems, which the checker does not implement.
      inspect(value, path + u'/' + keyword, false, depth + 1, references, problems);
    } else if (keyword == u"oneOf"_s || keyword == u"anyOf"_s) {
      const QJsonArray schemas = value.toArray();
      if (!value.isArray() || schemas.isEmpty()) {
        problems.append(path + u": %1 must be a list of schemas"_s.arg(keyword));
        continue;
      }
      for (qsizetype index = 0; index < schemas.size(); ++index) {
        inspect(schemas.at(index), path + u'/' + keyword + u'/' + QString::number(index), false, depth + 1, references, problems);
      }
    } else {
      problems.append(path + u": the checker does not implement the keyword %1 here"_s.arg(keyword));
    }
  }
}

// Checks value, found at pointer in the value checked, against a schema that inspect() accepted, and adds its problems. depth counts the
// nested checks. Schemas and messages nest, so it is recursive, with the depth limited by kMaxDepth, and it has one branch for each
// keyword. NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): see above.
bool check(const Definitions &definitions, const QJsonValue &schema, const QJsonValue &value, const QString &pointer, int depth,
           Problems &problems) {
  if (depth > kMaxDepth) {
    problems.append({.pointer = pointer, .text = u"nests more than %1 levels deep"_s.arg(kMaxDepth)});
    return false;
  }
  if (schema.isBool()) {
    if (!schema.toBool()) {
      problems.append({.pointer = pointer, .text = u"no value is allowed here"_s});
    }
    return schema.toBool();
  }
  if (!schema.isObject()) {
    problems.append({.pointer = pointer, .text = u"the schema has nothing to check this against"_s});
    return false;
  }
  const QJsonObject keywords = schema.toObject();
  if (const QJsonValue type = keywords.value(u"type"_s); !type.isUndefined() && !hasType(value, type)) {
    problems.append({.pointer = pointer, .text = u"is %1, not of type %2"_s.arg(typeName(value), compact(type))});
    // The other keywords would only repeat the mismatch.
    return false;
  }
  bool valid = true;
  if (const QJsonValue reference = keywords.value(u"$ref"_s); reference.isString()) {
    valid = check(definitions, definitions.value(definitionName(reference.toString())), value, pointer, depth + 1, problems) && valid;
  }
  if (const QJsonValue expected = keywords.value(u"const"_s); !expected.isUndefined() && value != expected) {
    problems.append({.pointer = pointer, .text = u"is %1, not %2"_s.arg(compact(value), compact(expected)), .constMismatch = true});
    valid = false;
  }
  if (value.isDouble()) {
    if (const QJsonValue minimum = keywords.value(u"minimum"_s); minimum.isDouble() && compare(value, minimum) < 0) {
      problems.append({.pointer = pointer, .text = u"is %1, below the minimum %2"_s.arg(compact(value), compact(minimum))});
      valid = false;
    }
    if (const QJsonValue maximum = keywords.value(u"maximum"_s); maximum.isDouble() && compare(value, maximum) > 0) {
      problems.append({.pointer = pointer, .text = u"is %1, above the maximum %2"_s.arg(compact(value), compact(maximum))});
      valid = false;
    }
    if (const QJsonValue format = keywords.value(u"format"_s); format.isString() && !fitsFormat(value, format.toString())) {
      problems.append({.pointer = pointer, .text = u"is %1, which does not fit the format %2"_s.arg(compact(value), format.toString())});
      valid = false;
    }
  }
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (const QJsonValue name : keywords.value(u"required"_s).toArray()) {
      if (!object.contains(name.toString())) {
        problems.append(
            {.pointer = pointer, .text = u"lacks the required field %1"_s.arg(name.toString()), .missingField = name.toString()});
        valid = false;
      }
    }
    const QJsonObject properties = keywords.value(u"properties"_s).toObject();
    const QJsonValue additional = keywords.value(u"additionalProperties"_s);
    for (auto field = object.begin(); field != object.end(); ++field) {
      const QString fieldPointer = pointer + u'/' + escaped(field.key());
      if (properties.contains(field.key())) {
        valid = check(definitions, properties.value(field.key()), field.value(), fieldPointer, depth + 1, problems) && valid;
      } else if (!additional.isUndefined()) {
        valid = check(definitions, additional, field.value(), fieldPointer, depth + 1, problems) && valid;
      }
    }
  }
  if (const QJsonValue items = keywords.value(u"items"_s); value.isArray() && !items.isUndefined()) {
    const QJsonArray array = value.toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
      valid = check(definitions, items, array.at(index), pointer + u'/' + QString::number(index), depth + 1, problems) && valid;
    }
  }
  // oneOf: exactly one alternative matches; anyOf: at least one does.
  for (const QString &keyword : {u"oneOf"_s, u"anyOf"_s}) {
    if (!keywords.contains(keyword)) {
      continue;
    }
    const QJsonArray alternatives = keywords.value(keyword).toArray();
    qsizetype matches = 0;
    // The problems of the alternative that comes closest, to explain a value that matches none.
    std::optional<Problems> closest;
    for (const QJsonValue alternative : alternatives) {
      Problems found;
      if (check(definitions, alternative, value, pointer, depth + 1, found)) {
        ++matches;
      } else if (!closest || closeness(found) < closeness(*closest)) {
        closest = std::move(found);
      }
    }
    if (matches == 1 || (keyword == u"anyOf"_s && matches > 1)) {
      continue;
    }
    valid = false;
    if (matches > 1) {
      problems.append(
          {.pointer = pointer, .text = u"matches %1 of the alternatives of oneOf, where exactly one must match"_s.arg(matches)});
      continue;
    }
    problems.append({.pointer = pointer,
                     .text = u"matches none of the %1 alternatives of %2; the closest one says:"_s.arg(alternatives.size()).arg(keyword),
                     .listsAlternatives = true});
    for (Problem problem : closest.value_or(Problems())) {
      ++problem.indent;
      problems.append(std::move(problem));
    }
  }
  return valid;
}

// What x-bayan-messages says about the reply to a request: the request's type, the reply's definition and the reply's type, each empty if
// there is none.
struct ReplyOf {
  QString requestType;
  QString definition;
  QString type;
};

// The problems of a message from the engine (ProtocolSchema::problemsOfMessageFromEngine()); a successful reply is checked as reply says.
Problems messageProblems(const Definitions &definitions, const QJsonObject &message, const ReplyOf &reply) {
  // The engine sends replies and events, exactly one of the two in each message, and never a request.
  const QJsonObject fromEngine{{u"oneOf"_s, QJsonArray{QJsonObject{{u"$ref"_s, QString(kDefinitionsPrefix.toString() + u"Reply"_s)}},
                                                       QJsonObject{{u"$ref"_s, QString(kDefinitionsPrefix.toString() + u"Event"_s)}}}}};
  if (!definitions.contains(u"Reply"_s) || !definitions.contains(u"Event"_s)) {
    return {{.pointer = {}, .text = u"the schema defines no Reply or no Event"_s}};
  }
  Problems found;
  if (!check(definitions, fromEngine, message, {}, 0, found) || !message.contains(u"re"_s) || message.value(u"ok"_s) != QJsonValue(true)) {
    return found;
  }
  // A successful reply must be the reply to its request.
  if (reply.requestType.isEmpty()) {
    return {
        {.pointer = {}, .text = u"a successful reply: the test must say which type of request it answers, so that it can be checked"_s}};
  }
  if (reply.definition.isEmpty()) {
    return {
        {.pointer = {}, .text = u"a successful reply to %1, which has no reply in the schema's x-bayan-messages"_s.arg(reply.requestType)}};
  }
  // Its type is the reply_type of the request's entry, or none, as the engine sends it. Reply's own check has made sure that a type is a
  // string.
  const QJsonValue type = message.value(u"type"_s);
  if (reply.type.isEmpty() && !type.isUndefined()) {
    found.append({.pointer = u"/type"_s, .text = u"is %1, but a reply to %2 has no type"_s.arg(compact(type), reply.requestType)});
  } else if (!reply.type.isEmpty() && type.isUndefined()) {
    found.append({.pointer = {},
                  .text = u"lacks the field type, which is \"%1\" in a reply to %2"_s.arg(reply.type, reply.requestType),
                  .missingField = u"type"_s});
  } else if (!reply.type.isEmpty() && type != QJsonValue(reply.type)) {
    found.append({.pointer = u"/type"_s,
                  .text = u"is %1, not \"%2\", the type of a reply to %3"_s.arg(compact(type), reply.type, reply.requestType)});
  }
  check(definitions, definitions.value(reply.definition), message.value(u"payload"_s), u"/payload"_s, 1, found);
  return found;
}

} // namespace

std::optional<ProtocolSchema> ProtocolSchema::load(const QByteArray &json, QStringList &problems) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    problems.append(u"#: the schema is not a JSON object"_s);
    return std::nullopt;
  }
  const qsizetype before = problems.size();
  ProtocolSchema schema;
  schema.root_ = document.object();
  if (schema.root_.value(u"$schema"_s) != QJsonValue(kDraft.toString())) {
    problems.append(u"#: $schema must be %1, the draft the checker implements"_s.arg(kDraft));
  }
  QStringList references;
  inspect(schema.root_, u"#"_s, true, 0, references, problems);
  const QJsonObject definitions = schema.root_.value(u"$defs"_s).toObject();
  for (auto definition = definitions.begin(); definition != definitions.end(); ++definition) {
    schema.definitions_.insert(definition.key(), definition.value());
  }
  const auto defined = [&schema](const QString &reference) {
    const QString name = definitionName(reference);
    return !name.isEmpty() && schema.definitions_.contains(name);
  };
  for (const QString &reference : std::as_const(references)) {
    if (!defined(reference)) {
      problems.append(u"#: $ref %1 names no definition of this schema"_s.arg(reference));
    }
  }

  // The list of messages, which says which definition is the reply to each request.
  const QJsonValue messages = schema.root_.value(u"x-bayan-messages"_s);
  if (!messages.isUndefined() && !messages.isArray()) {
    problems.append(u"#/x-bayan-messages: must be a list"_s);
  }
  for (const QJsonValue entryValue : messages.toArray()) {
    const QJsonObject entry = entryValue.toObject();
    const QString type = entry.value(u"type"_s).toString();
    const QString where = u"#/x-bayan-messages (%1)"_s.arg(type.isEmpty() ? u"an entry without a type"_s : type);
    if (!entryValue.isObject() || type.isEmpty() || schema.replies_.contains(type)) {
      problems.append(where + u": every entry must be an object with a type of its own"_s);
      continue;
    }
    for (auto field = entry.begin(); field != entry.end(); ++field) {
      if (!isOneOf(kMessageFields, field.key())) {
        problems.append(where + u": the checker does not know the field %1"_s.arg(field.key()));
      }
    }
    const QString direction = entry.value(u"direction"_s).toString();
    if (direction != u"to_engine"_s && direction != u"to_shell"_s) {
      problems.append(where + u": direction must be to_engine or to_shell"_s);
    }
    // web_reply names the reply as the web worker host delivers it (render.tile's pixels as an ImageBitmap, engine protocol §3.2), a type
    // that exists only for the TypeScript declarations: no JSON has its shape, and the desktop never receives it, so the checker only
    // checks that it names a definition.
    for (const QString &field : {u"payload"_s, u"reply"_s, u"web_reply"_s}) {
      const QJsonValue reference = entry.value(field);
      if ((field == u"payload"_s || !reference.isUndefined()) && !defined(reference.toString())) {
        problems.append(where + u": %1 names no definition of this schema"_s.arg(field));
      }
    }
    // reply_type is the type a successful reply carries, such as "welcome"; a reply without it has none.
    const QJsonValue replyType = entry.value(u"reply_type"_s);
    if (!replyType.isUndefined() && (!replyType.isString() || replyType.toString().isEmpty() || !entry.contains(u"reply"_s))) {
      problems.append(where + u": reply_type must be the type of a reply, on an entry that has one"_s);
    }
    schema.replies_.insert(type, definitionName(entry.value(u"reply"_s).toString()));
    schema.replyTypes_.insert(type, replyType.toString());
  }
  if (problems.size() > before) {
    return std::nullopt;
  }
  return schema;
}

const ProtocolSchema &ProtocolSchema::engineProtocol() {
  static const ProtocolSchema schema = [] {
    QFile file(u":/bayan/test/engine-protocol.schema.json"_s);
    if (!file.open(QIODevice::ReadOnly)) {
      qFatal("The tests' copy of the engine protocol's schema is missing from their resources.");
    }
    QStringList problems;
    std::optional<ProtocolSchema> loaded = load(file.readAll(), problems);
    if (!loaded) {
      qFatal("The engine protocol's schema uses what the tests' checker does not implement: %s", qUtf8Printable(problems.join(u"; "_s)));
    }
    return *std::move(loaded);
  }();
  return schema;
}

QStringList ProtocolSchema::problems(const QJsonValue &value, const QString &definition) const {
  if (!definition.isEmpty() && !definitions_.contains(definition)) {
    return {u"the schema has no definition named %1"_s.arg(definition)};
  }
  Problems found;
  check(definitions_, definition.isEmpty() ? QJsonValue(root_) : definitions_.value(definition), value, {}, 0, found);
  return written(found);
}

bool ProtocolSchema::couldComeFromEngine(const QJsonObject &message) const {
  if (problemsOfMessageFromEngine(message).isEmpty()) {
    return true;
  }
  // A successful reply is checked against the reply of each type of request in turn.
  const bool succeeded = message.contains(u"re"_s) && message.value(u"ok"_s) == QJsonValue(true);
  return succeeded && std::ranges::any_of(replies_.keys(), [this, &message](const QString &requestType) {
           return !replies_.value(requestType).isEmpty() && problemsOfMessageFromEngine(message, requestType).isEmpty();
         });
}

QStringList ProtocolSchema::problemsOfMessageFromEngine(const QJsonObject &message, const QString &requestType) const {
  return written(messageProblems(definitions_, message,
                                 {.requestType = requestType, .definition = replyDefinition(requestType), .type = replyType(requestType)}));
}

QStringList ProtocolSchema::placesOfProblemsOfMessageFromEngine(const QJsonObject &message, const QString &requestType) const {
  QStringList places;
  for (const Problem &problem :
       messageProblems(definitions_, message,
                       {.requestType = requestType, .definition = replyDefinition(requestType), .type = replyType(requestType)})) {
    if (!problem.listsAlternatives) {
      places.append(place(problem));
    }
  }
  return places;
}

} // namespace bayan::test
