// How often the shell may start a new engine session after an unrecoverable engine error (engine protocol §12.1, the recovery policy both
// shells follow): at most three times in any ten minutes, measured with a monotonic clock, so that changing the system time changes
// nothing. A panic is a bug that is usually tied to one message or one document, so one restart normally recovers, and three let the user
// reopen a document after each of its first three failures; an engine that keeps failing within minutes fails for a reason that
// restarting does not remove, and restarting it without end would keep the processor busy while the user can do nothing. Restarts older
// than ten minutes no longer count, so occasional failures in a long working session never use up the allowance.
//
// The same numbers are in bayan-web's src/engine/recovery.ts; change the spec and both shells together.

#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <optional>

namespace bayan {

class RestartAllowance {
public:
  // The most restarts within kWindow.
  static constexpr int kMaxRestarts = 3;
  // The span over which restarts are counted.
  static constexpr std::chrono::milliseconds kWindow = std::chrono::minutes(10);

  // A monotonic clock: the time since some fixed moment.
  using Clock = std::function<std::chrono::milliseconds()>;

  // Measures time with QElapsedTimer, which is monotonic on every platform the shell runs on.
  RestartAllowance();
  // Measures time with the given monotonic clock (tests pass one they control).
  explicit RestartAllowance(Clock clock);

  // Asks for one more restart. Returns its number within the window (1 to kMaxRestarts), to tell the user which restart this is, or nothing
  // when the allowance is used up and the shell must stop instead.
  [[nodiscard]] std::optional<int> take();

private:
  Clock clock_;
  // When the restarts of the current window happened, oldest first.
  std::deque<std::chrono::milliseconds> restarts_;
};

} // namespace bayan
