#include "benchmark.h"

#include "documentcanvas.h"
#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPauseAnimation>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QSequentialAnimationGroup>
#include <QtCore/QSysInfo>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// After windows.h, whose declarations it needs (a separate block, so that clang-format keeps the order).
#include <psapi.h>
#elif defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace bayan {
namespace {

constexpr double kNanosecondsPerMillisecond = 1e6;
// A frame counts as late if it took this much longer than the screen's refresh interval: it missed at least one refresh.
constexpr double kLateFactor = 1.5;
// How long to wait for the engine to show a key press before counting it as lost.
constexpr qint64 kKeyTimeoutNs = 2'000'000'000;

// The process's peak memory (resident set, or working set on Windows) in bytes, or -1 where unknown.
qint64 peakResidentBytes() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS counters{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
    return static_cast<qint64>(counters.PeakWorkingSetSize);
  }
  return -1;
#elif defined(Q_OS_UNIX)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1;
  }
  // glibc declares ru_maxrss inside an anonymous union with a field of the same size; the documented way to read it is by name.
  const auto peak = static_cast<qint64>(usage.ru_maxrss); // NOLINT(cppcoreguidelines-pro-type-union-access)
#ifdef Q_OS_MACOS
  return peak; // bytes on macOS
#else
  return peak * 1024; // kilobytes on Linux
#endif
#else
  return -1;
#endif
}

// Nearest-rank percentiles and the extremes of a list of milliseconds.
QJsonObject summary(std::vector<double> values) {
  QJsonObject result{{u"count"_s, static_cast<qint64>(values.size())}};
  if (values.empty()) {
    return result;
  }
  std::ranges::sort(values);
  const auto percentile = [&values](double share) {
    const auto rank = static_cast<std::size_t>(std::ceil(share * static_cast<double>(values.size())));
    return values.at(std::clamp<std::size_t>(rank, 1, values.size()) - 1);
  };
  double total = 0;
  for (const double value : values) {
    total += value;
  }
  result.insert(u"mean"_s, total / static_cast<double>(values.size()));
  result.insert(u"p50"_s, percentile(0.50));
  result.insert(u"p95"_s, percentile(0.95));
  result.insert(u"p99"_s, percentile(0.99));
  result.insert(u"max"_s, values.back());
  return result;
}

QString graphicsApiName(const QQuickWindow &window) {
  const QSGRendererInterface *renderer = window.rendererInterface();
  switch (renderer == nullptr ? QSGRendererInterface::Unknown : renderer->graphicsApi()) {
  case QSGRendererInterface::Software:
    return u"software"_s;
  case QSGRendererInterface::OpenGL:
    return u"opengl"_s;
  case QSGRendererInterface::Direct3D11:
    return u"direct3d11"_s;
  case QSGRendererInterface::Direct3D12:
    return u"direct3d12"_s;
  case QSGRendererInterface::Vulkan:
    return u"vulkan"_s;
  case QSGRendererInterface::Metal:
    return u"metal"_s;
  case QSGRendererInterface::Null:
    return u"null"_s;
  default:
    return u"unknown"_s;
  }
}

int milliseconds(double base, double scale) { return std::max(1, static_cast<int>(std::lround(base * scale))); }

} // namespace

Benchmark::Benchmark(QQmlApplicationEngine &qml, DocumentSession &session, Options options)
    : qml_(qml), session_(session), options_(std::move(options)) {
  poll_.setInterval(20ms);
  connect(&poll_, &QTimer::timeout, this, &Benchmark::waitForDocument);
  deadline_.setSingleShot(true);
  connect(&deadline_, &QTimer::timeout, this, [this] { fail(u"timed out"_s); });
}

Benchmark::~Benchmark() {
  if (window_ != nullptr) {
    disconnect(window_, nullptr, this, nullptr);
  }
}

void Benchmark::start() {
  poll_.start();
  deadline_.start(std::chrono::milliseconds(milliseconds(120'000.0, std::max(1.0, options_.durationScale))));
}

void Benchmark::waitForDocument() {
  if (session_.status() == DocumentSession::Status::Failed) {
    fail(u"the engine session failed"_s);
    return;
  }
  const QList<QObject *> roots = qml_.rootObjects();
  window_ = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.constFirst());
  canvas_ = window_ == nullptr ? nullptr : window_->findChild<DocumentCanvas *>(u"documentCanvas"_s);
  if (canvas_ == nullptr || session_.status() != DocumentSession::Status::Ready || !canvas_->tilesComplete()) {
    return;
  }
  poll_.stop();
  disconnect(&poll_, nullptr, this, nullptr);
  clock_.start();
  // frameSwapped comes from the render thread with Qt's threaded render loop; record it there, without waiting for the GUI thread.
  connect(window_, &QQuickWindow::frameSwapped, this, &Benchmark::onFrameSwapped, Qt::DirectConnection);
  runScroll();
}

void Benchmark::onFrameSwapped() {
  const qint64 now = clock_.nsecsElapsed();
  {
    const std::scoped_lock lock(framesMutex_);
    frames_.push_back({.time = now, .movement = movement_.load()});
  }
  // Keystroke to pixel: the first frame that shows complete tiles again after showing incomplete ones since the key was pressed.
  if (keyPressedAt_.load() >= 0 && keyShownAt_.load() < 0 && canvas_ != nullptr) {
    if (!canvas_->tilesComplete()) {
      sawIncomplete_.store(true);
    } else if (sawIncomplete_.load()) {
      keyShownAt_.store(now);
    }
  }
}

void Benchmark::beginPhase(Phase phase) {
  phase_ = phase;
  canvas_->resetStatistics();
  const std::scoped_lock lock(framesMutex_);
  frames_.clear();
  phaseStart_ = clock_.nsecsElapsed();
}

void Benchmark::trackMovement(QAbstractAnimation *animation) {
  connect(animation, &QAbstractAnimation::stateChanged, this,
          [this](QAbstractAnimation::State state) { movement_.store(state == QAbstractAnimation::Running ? ++movements_ : 0); });
}

void Benchmark::endPhase() {
  std::vector<Frame> frames;
  {
    const std::scoped_lock lock(framesMutex_);
    frames.swap(frames_);
  }
  // Intervals between frames of the same movement of the view; the first frame of a movement follows an idle pause, not a late frame.
  std::vector<double> intervals;
  qint64 framesMoving = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const Frame &frame = frames.at(i);
    framesMoving += frame.movement != 0 ? 1 : 0;
    if (i > 0 && frame.movement != 0 && frame.movement == frames.at(i - 1).movement) {
      intervals.push_back(static_cast<double>(frame.time - frames.at(i - 1).time) / kNanosecondsPerMillisecond);
    }
  }
  const double refreshRate = window_->screen() != nullptr ? window_->screen()->refreshRate() : 0.0;
  qint64 late = 0;
  if (refreshRate > 0.0) {
    const double budget = 1000.0 / refreshRate * kLateFactor;
    late = std::ranges::count_if(intervals, [budget](double interval) { return interval > budget; });
  }
  const CanvasStatistics &statistics = canvas_->statistics();
  QJsonObject phase{
      {u"duration_ms"_s, static_cast<double>(clock_.nsecsElapsed() - phaseStart_) / kNanosecondsPerMillisecond},
      {u"frames_presented"_s, static_cast<qint64>(frames.size())},
      {u"frames_while_moving"_s, framesMoving},
      {u"frame_interval_ms"_s, summary(intervals)},
      {u"late_frames"_s, late},
      {u"frames_prepared"_s, statistics.frames},
      {u"frames_with_stretched_tiles"_s, statistics.framesWithStandIns},
      {u"frames_with_missing_tiles"_s, statistics.framesWithHoles},
      {u"tiles_rendered"_s, statistics.tilesRendered},
      {u"tiles_refused"_s, statistics.tilesRefused},
      {u"texture_uploads"_s, statistics.textureUploads},
      {u"texture_upload_bytes"_s, statistics.textureUploadBytes},
      {u"tile_wait_ms"_s, summary(statistics.tileWaitMs)},
      {u"tile_render_ms"_s, summary(statistics.tileRenderMs)},
      {u"tile_shown_ms"_s, summary(statistics.tileShownMs)},
      {u"tile_cache_bytes"_s, canvas_->tileCache().bytes()},
      {u"peak_resident_bytes"_s, peakResidentBytes()},
  };
  if (phase_ == Phase::Typing) {
    phase.insert(u"keys"_s, keysPressed_);
    phase.insert(u"keys_lost"_s, keysLost_);
    phase.insert(u"keystroke_to_pixel_ms"_s, summary(keystrokeToPixelMs_));
  }
  phases_.insert(phaseName(phase_), phase);
}

QString Benchmark::phaseName(Phase phase) {
  switch (phase) {
  case Phase::Scroll:
    return u"scroll"_s;
  case Phase::Zoom:
    return u"zoom"_s;
  case Phase::Typing:
    return u"typing"_s;
  case Phase::Waiting:
  case Phase::Done:
    break;
  }
  return u"other"_s;
}

void Benchmark::runScroll() {
  beginPhase(Phase::Scroll);
  canvas_->setContentY(0);
  const double bottom = std::max(0.0, canvas_->contentHeight() - canvas_->height());
  auto *sequence = new QSequentialAnimationGroup(this); // NOLINT(cppcoreguidelines-owning-memory): owned by this object.
  for (const auto &[from, to] : {std::pair{0.0, bottom}, std::pair{bottom, 0.0}}) {
    auto *scroll = new QPropertyAnimation(canvas_, "contentY", sequence); // NOLINT(cppcoreguidelines-owning-memory): owned by the group.
    scroll->setStartValue(from);
    scroll->setEndValue(to);
    scroll->setDuration(milliseconds(3000.0, options_.durationScale));
    sequence->addAnimation(scroll);
  }
  trackMovement(sequence);
  connect(sequence, &QAbstractAnimation::finished, this, [this] {
    endPhase();
    runZoom();
  });
  animation_ = sequence;
  sequence->start(QAbstractAnimation::DeleteWhenStopped);
}

void Benchmark::runZoom() {
  beginPhase(Phase::Zoom);
  auto *sequence = new QSequentialAnimationGroup(this); // NOLINT(cppcoreguidelines-owning-memory): owned by this object.
  double from = 1.0;
  for (const double to : {2.0, 0.5, 1.0}) {
    auto *zoom = new QPropertyAnimation(canvas_, "zoom", sequence); // NOLINT(cppcoreguidelines-owning-memory): owned by the group.
    zoom->setStartValue(from);
    zoom->setEndValue(to);
    zoom->setDuration(milliseconds(1500.0, options_.durationScale));
    trackMovement(zoom);
    sequence->addAnimation(zoom);
    // A pause after each pinch: the canvas asks for tiles at the new zoom once it has settled.
    sequence->addPause(milliseconds(750.0, options_.durationScale));
    from = to;
  }
  connect(sequence, &QAbstractAnimation::finished, this, [this] {
    endPhase();
    runTyping();
  });
  animation_ = sequence;
  sequence->start(QAbstractAnimation::DeleteWhenStopped);
}

void Benchmark::runTyping() {
  if (!canvas_->inputMethodQuery(Qt::ImEnabled).toBool() || options_.typingKeys <= 0) {
    phases_.insert(u"typing"_s, QJsonObject{{u"skipped"_s, u"the engine has no editable text at the caret"_s}});
    writeResults();
    return;
  }
  beginPhase(Phase::Typing);
  canvas_->setContentY(0);
  canvas_->forceActiveFocus(Qt::OtherFocusReason);
  // Let the view settle at the top before the first key.
  QTimer::singleShot(milliseconds(500.0, std::max(1.0, options_.durationScale)), this, &Benchmark::pressNextKey);
}

void Benchmark::pressNextKey() {
  if (keysPressed_ >= options_.typingKeys) {
    keyPressedAt_.store(-1);
    endPhase();
    // Leave the editable line as it was.
    for (int i = 0; i < keysPressed_; ++i) {
      QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
      QCoreApplication::sendEvent(canvas_, &backspace);
    }
    writeResults();
    return;
  }
  ++keysPressed_;
  keyShownAt_.store(-1);
  sawIncomplete_.store(false);
  keyPressedAt_.store(clock_.nsecsElapsed());
  QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, u"a"_s);
  QCoreApplication::sendEvent(canvas_, &press);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, u"a"_s);
  QCoreApplication::sendEvent(canvas_, &release);
  // Wait for the frame that shows the key, then a short pause before the next one, as a typist would.
  auto *wait = new QTimer(this); // NOLINT(cppcoreguidelines-owning-memory): owned by this object, deleted below.
  wait->setInterval(2ms);
  connect(wait, &QTimer::timeout, this, [this, wait] {
    const qint64 pressed = keyPressedAt_.load();
    const qint64 shown = keyShownAt_.load();
    if (shown < 0 && clock_.nsecsElapsed() - pressed < kKeyTimeoutNs) {
      return;
    }
    wait->stop();
    wait->deleteLater();
    if (shown >= 0) {
      keystrokeToPixelMs_.push_back(static_cast<double>(shown - pressed) / kNanosecondsPerMillisecond);
    } else {
      ++keysLost_;
    }
    QTimer::singleShot(60ms, this, &Benchmark::pressNextKey);
  });
  wait->start();
}

void Benchmark::writeResults() {
  phase_ = Phase::Done;
  deadline_.stop();
  const QScreen *screen = window_->screen();
  results_ = QJsonObject{
      {u"format"_s, u"bayan-desktop-benchmark"_s},
      {u"format_version"_s, 1},
      {u"qt_version"_s, QString::fromLatin1(qVersion())},
      {u"engine_version"_s, session_.engineVersion()},
      {u"platform"_s, QSysInfo::prettyProductName()},
      {u"cpu_architecture"_s, QSysInfo::currentCpuArchitecture()},
      {u"qpa_platform"_s, QGuiApplication::platformName()},
      {u"graphics_api"_s, graphicsApiName(*window_)},
      {u"screen"_s,
       QJsonObject{{u"refresh_rate"_s, screen != nullptr ? screen->refreshRate() : 0.0},
                   {u"device_pixel_ratio"_s, window_->effectiveDevicePixelRatio()},
                   {u"logical_size"_s, screen != nullptr ? QJsonArray{screen->size().width(), screen->size().height()} : QJsonArray{}}}},
      {u"canvas_size"_s, QJsonArray{canvas_->width(), canvas_->height()}},
      {u"tile_size"_s, canvas_->tileSize()},
      {u"tile_cache_budget_bytes"_s, canvas_->tileCache().budget()},
      {u"duration_scale"_s, options_.durationScale},
      {u"phases"_s, phases_},
  };
  const QByteArray json = QJsonDocument(results_).toJson(QJsonDocument::Indented);
  QFile file(options_.outputPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size()) {
    fail(u"could not write the results file"_s);
    return;
  }
  file.close();
  // A short summary for the terminal; the file has everything.
  for (const QString &name : phases_.keys()) {
    const QJsonObject phase = phases_.value(name).toObject();
    const QJsonObject intervals = phase.value(u"frame_interval_ms"_s).toObject();
    if (intervals.value(u"count"_s).toInteger() > 0) {
      std::printf("%-7s %4lld frames while moving: interval p50 %6.2f ms  p95 %6.2f ms  p99 %6.2f ms  max %7.2f ms  late %lld\n",
                  qPrintable(name), phase.value(u"frames_while_moving"_s).toInteger(), intervals.value(u"p50"_s).toDouble(),
                  intervals.value(u"p95"_s).toDouble(), intervals.value(u"p99"_s).toDouble(), intervals.value(u"max"_s).toDouble(),
                  phase.value(u"late_frames"_s).toInteger());
    } else {
      std::printf("%-7s %4lld frames\n", qPrintable(name), phase.value(u"frames_presented"_s).toInteger());
    }
    if (phase.contains(u"keystroke_to_pixel_ms"_s)) {
      const QJsonObject keys = phase.value(u"keystroke_to_pixel_ms"_s).toObject();
      std::printf("        keystroke to pixel p50 %6.2f ms  p95 %6.2f ms  p99 %6.2f ms  (%lld keys, %lld lost)\n",
                  keys.value(u"p50"_s).toDouble(), keys.value(u"p95"_s).toDouble(), keys.value(u"p99"_s).toDouble(),
                  phase.value(u"keys"_s).toInteger(), phase.value(u"keys_lost"_s).toInteger());
    }
  }
  // The figures are the benchmark's output: if they cannot be written, the benchmark failed.
  QCoreApplication::exit(std::fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

void Benchmark::fail(const QString &reason) {
  poll_.stop();
  deadline_.stop();
  qCritical("benchmark failed: %s", qUtf8Printable(reason));
  QCoreApplication::exit(EXIT_FAILURE);
}

} // namespace bayan
