#include "sessiontracker.h"

#include <QtCore/QJsonValue>

#include <cmath>
#include <limits>
#include <optional>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// The largest identifier the protocol uses: below 2^53 (engine protocol §4).
constexpr qint64 kMaxExactInteger = static_cast<qint64>(quint64{1} << 53U) - 1;

// An integer from minimum to maximum, or nothing.
std::optional<qint64> integer(const QJsonValue &value, qint64 minimum, qint64 maximum) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > static_cast<double>(kMaxExactInteger)) {
    return std::nullopt;
  }
  const qint64 result = value.toInteger(std::numeric_limits<qint64>::min());
  return result >= minimum && result <= maximum ? std::optional<qint64>(result) : std::nullopt;
}

// The restart of an engine.error (engine protocol §14.2): { number, limit }, with number from 1 to limit, within the protocol's uint32.
// Nothing for null, and for anything malformed.
std::optional<SessionTracker::Restart> restartOf(const QJsonValue &value) {
  const QJsonObject restart = value.toObject();
  constexpr qint64 kMaxValue = std::numeric_limits<quint32>::max();
  const auto limit = integer(restart.value(u"limit"_s), 1, kMaxValue);
  const auto number = limit ? integer(restart.value(u"number"_s), 1, *limit) : std::nullopt;
  if (!value.isObject() || !number) {
    return std::nullopt;
  }
  return SessionTracker::Restart{.number = static_cast<quint32>(*number), .limit = static_cast<quint32>(*limit)};
}

} // namespace

void SessionTracker::sent(const QString &type, qint64 id) {
  if (type == u"hello"_s) {
    hello_ = id;
    welcomed_ = false;
    opening_.clear();
    documents_.clear();
  } else if (type == u"doc.open"_s && id > 0) {
    opening_.insert(id);
  }
}

SessionTracker::Decision SessionTracker::reply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error) {
  if (!ok && error.value(u"code"_s).toString() == u"panic"_s) {
    opening_.erase(requestId);
    return {.action = Action::Ignore};
  }
  if (hello_ <= 0 || requestId < hello_) {
    return {.action = Action::Ignore};
  }
  if (requestId == hello_) {
    welcomed_ = ok;
  }
  if (opening_.erase(requestId) > 0 && ok) {
    if (const auto document = integer(payload.value(u"doc_id"_s), 1, kMaxExactInteger)) {
      documents_.insert(*document);
    }
  }
  return {.action = Action::Handle};
}

SessionTracker::Decision SessionTracker::event(const QString &type, const QJsonObject &payload) {
  if (type == u"engine.error"_s) {
    const QJsonValue recoverable = payload.value(u"recoverable"_s);
    if (!recoverable.isBool()) {
      return {.action = Action::Stop};
    }
    if (!recoverable.toBool()) {
      if (const auto restart = restartOf(payload.value(u"restart"_s))) {
        return {.action = Action::Restart, .restart = *restart};
      }
      return {.action = Action::Stop};
    }
  }
  if (!welcomed_) {
    return {.action = Action::Ignore};
  }
  const QJsonValue document = payload.value(u"doc_id"_s);
  if (const auto id = integer(document, 0, kMaxExactInteger); id && !documents_.contains(*id)) {
    return {.action = Action::Ignore};
  }
  return {.action = Action::Handle};
}

} // namespace bayan
