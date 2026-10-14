#include "tilerenderer.h"

#include <QtCore/QMetaObject>

#include <algorithm>
#include <utility>

namespace bayan {

TileRenderer::TileRenderer(Engine &engine, QObject *parent) : QObject(parent), engine_(engine) {
  thread_ = std::thread([this] { run(); });
}

TileRenderer::~TileRenderer() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
    jobs_.clear();
  }
  wake_.notify_all();
  thread_.join();
}

void TileRenderer::setJobs(std::vector<Job> jobs) {
  {
    const std::scoped_lock lock(mutex_);
    jobs_.clear();
    for (Job &job : jobs) {
      if (rendering_ != job.key) {
        jobs_.push_back(std::move(job));
      }
    }
  }
  wake_.notify_one();
}

bool TileRenderer::busy() const {
  const std::scoped_lock lock(mutex_);
  return rendering_.has_value() || !jobs_.empty();
}

void TileRenderer::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      rendering_.reset();
      wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
      rendering_ = job.key;
    }
    const Clock::time_point started = Clock::now();
    // Read before rendering: every request posted up to this one is handled before the tile (see Engine::lastPostedRequest).
    const qint64 renderedAfter = engine_.lastPostedRequest();
    QImage image = engine_.renderTile(job.request, job.pixels);
    Result result{.key = job.key,
                  .image = std::move(image),
                  .renderedAfter = renderedAfter,
                  .wanted = job.wanted,
                  .started = started,
                  .finished = Clock::now()};
    // Back to the GUI thread. If the renderer is destroyed first, Qt drops the call together with its receiver.
    QMetaObject::invokeMethod(this, [this, result = std::move(result)] { emit tileRendered(result); }, Qt::QueuedConnection);
  }
}

} // namespace bayan
