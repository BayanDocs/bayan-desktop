// A strict checker for the engine protocol's JSON Schema, written with Qt's JSON classes, so that the tests need no JSON Schema library
// (DESK-003).
//
// bayan-core defines the engine's messages once, as Rust types, generates the protocol's JSON Schema (draft 2020-12) from them and ships it
// in its C SDK (ADR-0012). The tests check every message that they hand the shell as if from the engine against a copy of that schema,
// src/engine/stub/schema/engine-protocol.schema.json, which the test engine.sdk_copies compares byte for byte with the SDK's, so that they
// test only messages the engine could send.
//
// The checker implements the parts of JSON Schema that the generated schema uses, and nothing else:
// - $ref to the schema's own definitions ("#/$defs/<name>"), $defs, and the boolean schemas true and false;
// - the applicators properties, additionalProperties, items, oneOf and anyOf;
// - the assertions type, const, required, minimum and maximum;
// - format, for the formats the generator gives Rust's number types (uint8, uint32, uint64, int32, int64 and double), as an assertion:
//   JSON Schema only annotates formats by default, but the engine cannot send a number outside its Rust type's range;
// - the annotations $schema (which must name draft 2020-12), $id, title, description and default, and bayan-core's extensions
//   x-bayan-messages (read for the type of each request's reply), x-bayan-protocol-version and x-bayan-web-type.
// Loading a schema that uses any other keyword or format anywhere fails, so that a later schema that needs more cannot pass unchecked.

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace bayan::test {

class ProtocolSchema {
public:
  // Reads a schema. Returns nothing, with the reasons in problems, if it is not a JSON object or uses anything the checker does not
  // implement.
  static std::optional<ProtocolSchema> load(const QByteArray &json, QStringList &problems);

  // The engine protocol's schema: the copy in src/engine/stub/schema, compiled into the tests. Stops the test program if it cannot be
  // loaded.
  static const ProtocolSchema &engineProtocol();

  // The problems of a value against the definition with this name (such as "EngineError"), or against the whole schema if the name is
  // empty; empty if the value is valid. Each problem starts with the place in the value, as a JSON Pointer.
  [[nodiscard]] QStringList problems(const QJsonValue &value, const QString &definition = {}) const;

  // The problems of a message from the engine to the shell, which must be a reply (Reply) or an event (Event), exactly one of the two. A
  // successful reply's payload must also have the type that x-bayan-messages gives the reply to requestType, the type of the request it
  // answers; without requestType a successful reply is refused, so that no payload goes unchecked.
  [[nodiscard]] QStringList problemsOfMessageFromEngine(const QJsonObject &message, const QString &requestType = {}) const;

  // The name of the definition of the reply to a request of this type, from x-bayan-messages (such as "Welcome" for hello), or empty.
  [[nodiscard]] QString replyDefinition(const QString &requestType) const { return replies_.value(requestType); }

  // Whether the engine could send this message in reply to some request: problemsOfMessageFromEngine() finds no problem with it for at
  // least one type of request (or without one, for an event or a failed reply).
  [[nodiscard]] bool couldComeFromEngine(const QJsonObject &message) const;

private:
  ProtocolSchema() = default;

  QJsonObject root_;
  // The definitions under $defs, by name.
  QHash<QString, QJsonValue> definitions_;
  // The definition of each request's reply, by request type (x-bayan-messages).
  QHash<QString, QString> replies_;
};

} // namespace bayan::test
