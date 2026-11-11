#include "enginemessages.h"

#include "protocolschema.h"

#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <algorithm>
#include <cstdint>

using namespace Qt::StringLiterals;

namespace bayan::test {
namespace {

const std::uint8_t *bytesOf(const QByteArray &data) {
  return reinterpret_cast<const std::uint8_t *>(data.constData()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): C passes bytes.
}

QByteArray compact(const QJsonObject &message) { return QJsonDocument(message).toJson(QJsonDocument::Compact); }

// Whether the engine could send this message; logs the problems if not.
bool couldBeSent(const QJsonObject &message, const QString &requestType) {
  const QStringList problems = ProtocolSchema::engineProtocol().problemsOfMessageFromEngine(message, requestType);
  if (problems.isEmpty()) {
    return true;
  }
  qWarning().noquote() << "The engine could not send" << QString::fromUtf8(compact(message)) << "\n" << problems.join(u'\n');
  return false;
}

} // namespace

QJsonObject engineEvent(const QString &type, const QJsonObject &payload) {
  return QJsonObject{{u"v"_s, Engine::kProtocolVersion}, {u"type"_s, type}, {u"seq"_s, 1'000'000}, {u"payload"_s, payload}};
}

bool deliver(Engine &engine, const QJsonObject &message, const QString &requestType) {
  if (!couldBeSent(message, requestType)) {
    return false;
  }
  const QByteArray json = compact(message);
  engine.deliverFromEngineThread(bytesOf(json), static_cast<std::size_t>(json.size()));
  return true;
}

bool emitEvent(Engine &engine, const QString &type, const QJsonObject &payload) {
  if (!couldBeSent(engineEvent(type, payload), {})) {
    return false;
  }
  emit engine.eventReceived(type, payload);
  return true;
}

bool deliverMalformed(Engine &engine, const QByteArray &json, const QString &brokenAt, const QString &why) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    // Text that is not a JSON object is broken as a whole.
    if (!brokenAt.isEmpty()) {
      qWarning().noquote() << "Meant to be broken at" << brokenAt << "(" << why
                           << "), but it is not a JSON object:" << QString::fromUtf8(json);
      return false;
    }
  } else {
    // An object must be one the engine could not send in reply to any request, and every problem the schema check finds must lie in the
    // one broken place.
    const ProtocolSchema &schema = ProtocolSchema::engineProtocol();
    const QJsonObject message = document.object();
    if (schema.couldComeFromEngine(message)) {
      qWarning().noquote() << "Meant to be malformed (" << why << "), but the engine could send it:" << QString::fromUtf8(json);
      return false;
    }
    const QStringList places = schema.placesOfProblemsOfMessageFromEngine(message);
    const bool there =
        std::ranges::all_of(places, [&brokenAt](const QString &place) { return place == brokenAt || place.startsWith(brokenAt + u'/'); });
    if (brokenAt.isEmpty() || places.isEmpty() || !there) {
      qWarning().noquote() << "Meant to be broken only at" << (brokenAt.isEmpty() ? u"(nothing)"_s : brokenAt) << "(" << why
                           << "), but the schema check finds:\n"
                           << schema.problemsOfMessageFromEngine(message).join(u'\n');
      return false;
    }
  }
  engine.deliverFromEngineThread(bytesOf(json), static_cast<std::size_t>(json.size()));
  return true;
}

bool deliverOversized(Engine &engine, const QJsonObject &message, std::size_t claimedLength) {
  if (!couldBeSent(message, {})) {
    return false;
  }
  if (claimedLength <= static_cast<std::size_t>(Engine::kMaxMessageBytes)) {
    qWarning() << "The claimed length" << claimedLength << "is not over the limit, so the wrapper would read that many bytes.";
    return false;
  }
  const QByteArray json = compact(message);
  engine.deliverFromEngineThread(bytesOf(json), claimedLength);
  return true;
}

void deliverNothing(Engine &engine) { engine.deliverFromEngineThread(nullptr, 0); }

} // namespace bayan::test
