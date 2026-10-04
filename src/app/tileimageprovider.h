// Supplies page images to QML (image://tiles/<revision>/<page>) by asking the engine to render them. The revision in the address changes
// when the engine invalidates the page, so QML asks again. A stand-in for DESK-002's tiled document canvas.

#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtQuick/QQuickImageProvider>

namespace bayan {

class DocumentSession;

class TileImageProvider final : public QQuickImageProvider {
public:
  explicit TileImageProvider(DocumentSession &session);

  // Called by QML on the main thread (the page Image is not asynchronous).
  QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
  DocumentSession &session_;
};

} // namespace bayan
