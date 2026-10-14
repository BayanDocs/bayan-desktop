#include "pixelprobe.h"

#include "documentcanvas.h"
#include "documentsession.h"
#include "tilegrid.h"
#include "viewgeometry.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointF>
#include <QtGui/QColor>
#include <QtQuick/QQuickWindow>

#include <array>
#include <cmath>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// Colour management and the window grab can shift a channel slightly; anything closer than this counts as a match.
constexpr int kTolerance = 3;

bool close(const QColor &actual, const QColor &expected) {
  return std::abs(actual.red() - expected.red()) <= kTolerance && std::abs(actual.green() - expected.green()) <= kTolerance &&
         std::abs(actual.blue() - expected.blue()) <= kTolerance;
}

// Where the probes go inside the visible part of the page, as fractions of its width and height.
constexpr std::array kProbeFractions{0.17, 0.5, 0.83};

} // namespace

std::optional<QString> compareWithEnginePixels(const DocumentCanvas &canvas, const QImage &frame, int page) {
  const DocumentSession *session = canvas.session();
  const QQuickWindow *window = canvas.window();
  if (session == nullptr || window == nullptr || page < 0 || std::cmp_greater_equal(page, session->pages().size())) {
    return u"no such page"_s;
  }
  if (frame.isNull() || window->width() <= 0) {
    return u"no frame"_s;
  }
  const ViewGeometry view = canvas.geometry();
  const protocol::Page &target = session->pages().at(static_cast<std::size_t>(page));
  const QRectF pageRect = view.pageItemRect(target);
  const QRectF visible = pageRect.intersected(canvas.boundingRect());
  if (visible.width() < 4 || visible.height() < 4) {
    return u"the page is not visible"_s;
  }
  const TileGrid grid(view.scale(), TileGrid::kDefaultTileSize);
  // The grab holds device pixels, but on the offscreen platform it does not say so in its devicePixelRatio; its size tells.
  const double ratio = static_cast<double>(frame.width()) / static_cast<double>(window->width());
  // The page's top-left corner in the window's device pixels (on a whole pixel, see ViewGeometry).
  const QPointF pageCorner = canvas.mapToScene(pageRect.topLeft()) * ratio;
  for (const double fx : kProbeFractions) {
    for (const double fy : kProbeFractions) {
      const QPointF scenePoint = canvas.mapToScene(visible.topLeft() + QPointF(visible.width() * fx, visible.height() * fy)) * ratio;
      const auto x = static_cast<int>(std::floor(scenePoint.x()));
      const auto y = static_cast<int>(std::floor(scenePoint.y()));
      if (!frame.valid(x, y)) {
        return u"a probe is outside the window"_s;
      }
      // The same pixel, rendered by the engine on its own: the page interval of device pixel (column, row).
      const auto column = static_cast<qint64>(x - std::lround(pageCorner.x()));
      const auto row = static_cast<qint64>(y - std::lround(pageCorner.y()));
      const qint64 left = grid.bluAt(column);
      const qint64 top = grid.bluAt(row);
      const protocol::BluRect pixel{.x = left, .y = top, .width = grid.bluAt(column + 1) - left, .height = grid.bluAt(row + 1) - top};
      const QJsonObject request{{u"doc_id"_s, session->documentId()},
                                {u"page"_s, target.index},
                                {u"rect"_s, protocol::toJson(pixel)},
                                {u"width"_s, 1},
                                {u"height"_s, 1}};
      const QImage expected = session->engine().renderTile(request, QSize(1, 1));
      if (expected.isNull()) {
        return u"the engine refused to render a probe pixel"_s;
      }
      const QColor actual = frame.pixelColor(x, y);
      const QColor wanted = expected.pixelColor(0, 0);
      if (!close(actual, wanted)) {
        return u"pixel (%1, %2) has colour %3 instead of the engine's %4"_s.arg(x).arg(y).arg(actual.name(), wanted.name());
      }
    }
  }
  return std::nullopt;
}

} // namespace bayan
