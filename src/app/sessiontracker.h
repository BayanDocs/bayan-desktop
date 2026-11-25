// Which engine session a message from the engine belongs to, and what the shell does with it (engine protocol §12.1 step 4, §14.1 item 5,
// §14.2): the one rule that DocumentSession applies to every reply and event before it reads them, kept apart so that the conformance
// vectors of restart.json (§15) test exactly it.
//
// - The error panic answers the message that made the engine fail, just before the engine.error that says what follows: ignored.
// - A reply to a request sent before the current session's hello belongs to an earlier session: ignored. Requests are numbered in the order
//   they are sent, so the hello's number divides them.
// - engine.error with recoverable false: the engine discarded its session. Its restart says what follows (§14.2): { number, limit }, the
//   engine is ready for restart number of limit, and the shell starts it with hello; null, the engine has stopped and answers nothing more.
//   The shell counts nothing itself. A recoverable that is not a boolean, or a restart that is missing or malformed, breaks the protocol
//   (§14.1 item 5). Both of those, and null, stop the shell's use of the engine.
// - Any other event before the current hello's welcome, or about a document that the current session did not open, belongs to an earlier
//   session: ignored.
// - Everything else belongs to the current session and is handled.

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QtTypes>

#include <cstdint>
#include <set>

namespace bayan {

class SessionTracker {
public:
  // A restart that the engine's restart allowance grants (engine protocol §14.2): which restart this is within ten minutes, from 1 to
  // limit, and how many the allowance grants (3).
  struct Restart {
    quint32 number = 0;
    quint32 limit = 0;
    friend bool operator==(const Restart &, const Restart &) = default;
  };

  enum class Action : std::uint8_t { Handle, Ignore, Restart, Stop };

  struct Decision {
    Action action = Action::Handle;
    // Which restart, for Action::Restart.
    Restart restart{};
    friend bool operator==(const Decision &, const Decision &) = default;
  };

  // The shell sent a request with this identifier, which the engine answers, or a message without one (id 0). A hello starts a session.
  void sent(const QString &type, qint64 id);
  // What the shell does with a reply, and with an event.
  [[nodiscard]] Decision reply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  [[nodiscard]] Decision event(const QString &type, const QJsonObject &payload);

private:
  // The current session's hello, and whether its welcome arrived.
  qint64 hello_ = 0;
  bool welcomed_ = false;
  // The current session's doc.open requests that wait for their reply, and the documents that it opened.
  std::set<qint64> opening_;
  std::set<qint64> documents_;
};

} // namespace bayan
