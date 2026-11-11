// How the tests hand the shell messages as if the engine had sent them (DESK-003). Every such message goes through one of these helpers,
// which first checks it against the engine protocol's JSON Schema (protocolschema.h), so that a test cannot test a message that does not
// exist:
// - deliver() and emitEvent() take only messages the engine could send;
// - deliverMalformed() takes only input the engine could never send, for the tests of how the shell defends itself against an engine that
//   sends something broken, and refuses a message the schema accepts, so that such a test can never quietly test a valid message instead;
// - deliverOversized() and deliverNothing() hand over a valid message with a false length, and a null pointer.
// Each returns false without handing anything over, after logging what is wrong, when its input is not what it takes: call it in QVERIFY().

#pragma once

#include "engine.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cstddef>

namespace bayan::test {

// An event's envelope, with a sequence number far beyond those the engine has used in a test.
QJsonObject engineEvent(const QString &type, const QJsonObject &payload);

// Hands the wrapper a message on this thread, as the engine's callback does, if the engine could send it: an event, or a reply. A
// successful reply's payload must be the reply to a request of type requestType.
[[nodiscard]] bool deliver(Engine &engine, const QJsonObject &message, const QString &requestType = {});

// Emits an event of the wrapper as if it had just arrived from the engine, if the engine could send it, for a test that must hand it over
// synchronously, from inside a slot of its own.
[[nodiscard]] bool emitEvent(Engine &engine, const QString &type, const QJsonObject &payload);

// Hands the wrapper text that is not a message the engine could send, as the engine's callback would; why says what is wrong with it.
[[nodiscard]] bool deliverMalformed(Engine &engine, const QByteArray &json, const QString &why);

// Hands the wrapper a message that the engine could send, but claims that it is claimedLength bytes long, more than the wrapper accepts, so
// that only its length can make the wrapper refuse it; the wrapper must not read beyond the message.
[[nodiscard]] bool deliverOversized(Engine &engine, const QJsonObject &message, std::size_t claimedLength);

// Hands the wrapper a null pointer instead of a message.
void deliverNothing(Engine &engine);

} // namespace bayan::test
