// The measurement mode (`bayan-desktop --benchmark <file>`, DESK-002 AC-1 and AC-5): drives the document canvas through a fixed script
// and writes what it measured as JSON.
//
// 1. Scroll: from the top of the document to the bottom and back, at a steady speed, as one long animation.
// 2. Zoom: from 100 % to 200 %, to 50 % and back to 100 %, as a pinch would, pausing after each step so the new tiles arrive.
// 3. Typing (only with an engine that has an editable line): presses keys in the editable line and measures, for each, the time from the
//    key press to the frame that shows the engine's new pixels (keystroke to pixel, PERF-04).
//
// For every phase it records the interval between frames as the window presented them (frameSwapped) while the view moves, that is while a
// scroll or zoom animation runs (Qt draws nothing while nothing changes, so the pauses in between are not late frames), how many of those
// frames were late compared with the screen's refresh rate, how often a visible tile showed a stretched stand-in or nothing, the tiles'
// waiting, rendering and showing times, the memory the tiles used and the process's peak memory. The figures mean something only on real
// hardware with a real GPU; the offscreen platform that CI uses draws with the CPU and has no refresh rate, so there the mode only proves
// that it works.

#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <atomic>
#include <mutex>
#include <vector>

class QAbstractAnimation;
class QQmlApplicationEngine;
class QQuickWindow;

namespace bayan {

class DocumentCanvas;
class DocumentSession;

class Benchmark final : public QObject {
  Q_OBJECT

public:
  struct Options {
    QString outputPath{};
    // Multiplies the length of every phase; CI uses a short run.
    double durationScale = 1.0;
    // Keys pressed in the typing phase.
    int typingKeys = 40;
  };

  Benchmark(QQmlApplicationEngine &qml, DocumentSession &session, Options options);
  ~Benchmark() override;
  Q_DISABLE_COPY_MOVE(Benchmark)

  void start();

private:
  enum class Phase : std::uint8_t { Waiting, Scroll, Zoom, Typing, Done };
  // The phase's name in the output.
  [[nodiscard]] static QString phaseName(Phase phase);

  void waitForDocument();
  void beginPhase(Phase phase);
  void endPhase();
  void runScroll();
  void runZoom();
  void runTyping();
  void pressNextKey();
  // Counts the frames presented while the animation runs as frames of one movement of the view.
  void trackMovement(QAbstractAnimation *animation);
  void onFrameSwapped();
  void writeResults();
  void fail(const QString &reason);

  QQmlApplicationEngine &qml_;
  DocumentSession &session_;
  Options options_;
  QQuickWindow *window_ = nullptr;
  DocumentCanvas *canvas_ = nullptr;
  Phase phase_ = Phase::Waiting;
  QTimer poll_;
  QTimer deadline_;
  QElapsedTimer clock_;
  QAbstractAnimation *animation_ = nullptr;

  // A presented frame: when, in nanoseconds since clock_ started, and during which movement of the view (0: none).
  struct Frame {
    qint64 time = 0;
    int movement = 0;
  };
  // Written on the render thread.
  std::mutex framesMutex_;
  std::vector<Frame> frames_;
  qint64 phaseStart_ = 0;
  // The movement of the view in progress (0: none), and the number of the last one.
  std::atomic<int> movement_{0};
  int movements_ = 0;

  // Typing: the key being measured and whether the canvas has shown incomplete tiles since it was pressed (set on the render thread).
  int keysPressed_ = 0;
  int keysLost_ = 0;
  std::atomic<qint64> keyPressedAt_{-1};
  std::atomic<bool> sawIncomplete_{false};
  std::atomic<qint64> keyShownAt_{-1};
  std::vector<double> keystrokeToPixelMs_;

  QJsonObject results_;
  QJsonObject phases_;
};

} // namespace bayan
