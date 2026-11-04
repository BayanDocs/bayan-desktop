#include "restartallowance.h"

#include <QtCore/QElapsedTimer>

#include <utility>

namespace bayan {
namespace {

QElapsedTimer startedTimer() {
  QElapsedTimer timer;
  timer.start();
  return timer;
}

} // namespace

RestartAllowance::RestartAllowance() : clock_([timer = startedTimer()] { return std::chrono::milliseconds(timer.elapsed()); }) {}

RestartAllowance::RestartAllowance(Clock clock) : clock_(std::move(clock)) {}

std::optional<int> RestartAllowance::take() {
  const std::chrono::milliseconds now = clock_();
  // Restarts that happened ten minutes ago or earlier no longer count.
  while (!restarts_.empty() && now - restarts_.front() >= kWindow) {
    restarts_.pop_front();
  }
  if (std::cmp_greater_equal(restarts_.size(), kMaxRestarts)) {
    return std::nullopt;
  }
  restarts_.push_back(now);
  return static_cast<int>(restarts_.size());
}

} // namespace bayan
