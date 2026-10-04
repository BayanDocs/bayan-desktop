#include "tileimageprovider.h"

#include "documentsession.h"
#include "engine.h"

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace bayan {

TileImageProvider::TileImageProvider(DocumentSession &session) : QQuickImageProvider(QQuickImageProvider::Image), session_(session) {}

QImage TileImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
  // The address is <revision>/<page>; only the page index matters here.
  bool isNumber = false;
  const int pageIndex = id.section(u'/', -1).toInt(&isNumber);
  const auto page = isNumber ? session_.page(pageIndex) : std::nullopt;
  if (!page) {
    return {};
  }

  // Render at the size QML asks for (the displayed size times the screen's pixel ratio), or at 100 % if it does not say.
  const QSizeF pageDips = session_.pageSize();
  QSize pixels = requestedSize;
  if (pixels.width() <= 0 || pixels.height() <= 0) {
    pixels = QSize(static_cast<int>(std::lround(pageDips.width())), static_cast<int>(std::lround(pageDips.height())));
  }
  pixels = pixels.boundedTo(QSize(Engine::kMaxTileSide, Engine::kMaxTileSide));

  const QJsonObject rect{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, page->width}, {u"height"_s, page->height}};
  const QJsonObject request{{u"doc_id"_s, session_.documentId()},
                            {u"page"_s, pageIndex},
                            {u"rect"_s, rect},
                            {u"zoom"_s, pageDips.width() > 0 ? pixels.width() / pageDips.width() : 1.0},
                            {u"device_scale"_s, 1.0},
                            {u"mode"_s, u"interactive"_s}};
  QImage image = session_.engine().renderTile(request, pixels);
  if (size != nullptr) {
    *size = image.size();
  }
  return image;
}

} // namespace bayan
