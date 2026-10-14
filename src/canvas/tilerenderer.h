// Renders tiles for the document canvas on a thread of its own (DESK-002).
//
// bayan_render_tile waits until the engine has drawn the tile (engine protocol §8), so the GUI thread must never call it while the user
// scrolls. The canvas hands this object a list of tiles, most urgent first; one thread renders them one after another, through the C
// interface straight into the image's memory, and hands each finished image back to the GUI thread. The engine draws tiles one at a time in
// v0, so a second thread would only wait in line. When the view moves on, the canvas replaces the list, so tiles that are no longer wanted
// are never rendered.

#pragma once

#include "engine.h"
#include "tilegrid.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtGui/QImage>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace bayan {

class TileRenderer final : public QObject {
  Q_OBJECT

public:
  using Clock = std::chrono::steady_clock;

  struct Job {
    TileKey key{};
    // The render.tile payload (engine protocol §6.4).
    QJsonObject request{};
    QSize pixels{};
    // When the canvas first asked for the tile, for the latency figures.
    Clock::time_point wanted;
  };

  struct Result {
    TileKey key{};
    // The pixels, or a null image if the engine refused the tile.
    QImage image{};
    // The last request the engine had handled before it rendered the tile (Engine::lastPostedRequest).
    qint64 renderedAfter = 0;
    Clock::time_point wanted;
    Clock::time_point started;
    Clock::time_point finished;
  };

  // The engine must outlive the renderer.
  explicit TileRenderer(Engine &engine, QObject *parent = nullptr);
  // Stops the thread, after the tile it may be rendering.
  ~TileRenderer() override;
  Q_DISABLE_COPY_MOVE(TileRenderer)

  // Replaces the tiles waiting to be rendered. A tile that is being rendered right now is not repeated.
  void setJobs(std::vector<Job> jobs);
  // Whether a tile is being rendered or waits to be.
  [[nodiscard]] bool busy() const;

signals:
  // Emitted on the thread that owns the renderer (the GUI thread) for every tile rendered.
  void tileRendered(const bayan::TileRenderer::Result &result);

private:
  void run();

  Engine &engine_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::optional<TileKey> rendering_;
  bool stopping_ = false;
  std::thread thread_;
};

} // namespace bayan
