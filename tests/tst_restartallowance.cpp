// Unit tests for the restart allowance (src/app/restartallowance.h): at most three restarts in any ten minutes, on a monotonic clock
// (engine protocol §12.1, point 1). A clock the test controls stands in for QElapsedTimer.

#include "restartallowance.h"

#include <QtCore/QElapsedTimer>
#include <QtTest/QTest>

#include <chrono>
#include <optional>

using namespace std::chrono_literals;

class TestRestartAllowance final : public QObject {
  Q_OBJECT

private slots:
  void threeRestartsThenNone();
  void restartsOlderThanTenMinutesNoLongerCount();
  void theClockIsMonotonic();
};

void TestRestartAllowance::threeRestartsThenNone() {
  std::chrono::milliseconds now = 0ms;
  bayan::RestartAllowance allowance([&now] { return now; });
  QCOMPARE(allowance.take(), std::optional<int>(1));
  now = 1min;
  QCOMPARE(allowance.take(), std::optional<int>(2));
  now = 2min;
  QCOMPARE(allowance.take(), std::optional<int>(3));
  // The fourth within ten minutes: the shell stops instead; asking again changes nothing.
  now = 3min;
  QCOMPARE(allowance.take(), std::nullopt);
  QCOMPARE(allowance.take(), std::nullopt);
  QCOMPARE(bayan::RestartAllowance::kMaxRestarts, 3);
  QCOMPARE(bayan::RestartAllowance::kWindow, std::chrono::milliseconds(10min));
}

void TestRestartAllowance::restartsOlderThanTenMinutesNoLongerCount() {
  std::chrono::milliseconds now = 0ms;
  bayan::RestartAllowance allowance([&now] { return now; });
  QCOMPARE(allowance.take(), std::optional<int>(1));
  now = 4min;
  QCOMPARE(allowance.take(), std::optional<int>(2));
  now = 5min;
  QCOMPARE(allowance.take(), std::optional<int>(3));
  // Just under ten minutes after the first restart, all three still count.
  now = 10min - 1ms;
  QCOMPARE(allowance.take(), std::nullopt);
  // Ten minutes after it, the first no longer counts: one restart is allowed, the third of the window.
  now = 10min;
  QCOMPARE(allowance.take(), std::optional<int>(3));
  QCOMPARE(allowance.take(), std::nullopt);
  // Long after every restart, the allowance is full again, and the count starts at 1.
  now = 60min;
  QCOMPARE(allowance.take(), std::optional<int>(1));
}

void TestRestartAllowance::theClockIsMonotonic() {
  // The default clock is QElapsedTimer's, which must not follow changes of the system time (engine protocol §12.1).
  QVERIFY(QElapsedTimer::isMonotonic());
  bayan::RestartAllowance allowance;
  QCOMPARE(allowance.take(), std::optional<int>(1));
  QCOMPARE(allowance.take(), std::optional<int>(2));
  QCOMPARE(allowance.take(), std::optional<int>(3));
  QCOMPARE(allowance.take(), std::nullopt);
}

QTEST_GUILESS_MAIN(TestRestartAllowance)
#include "tst_restartallowance.moc"
