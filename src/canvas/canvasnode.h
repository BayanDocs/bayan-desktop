// The document canvas's part of Qt Quick's scene graph (DESK-002): the nodes that draw page backgrounds, tiles and overlays.
//
// The canvas prepares what a frame shows on the GUI thread (DocumentCanvas::updatePolish); the scene graph's render thread then turns it
// into nodes here (DocumentCanvas::updatePaintNode), while the GUI thread waits. Tile nodes are kept by key, so a tile's texture is
// uploaded to the graphics card once and only moved while it stays on screen; a few textures are uploaded per frame at most, so that no
// frame is late.
//
// The node and its layers are transform nodes with the identity matrix rather than plain QSGNodes: Qt's software renderer (the offscreen
// platform in CI) remembers the position only of transform, clip and opacity nodes, and draws children added later under a plain node as if
// the item sat at the window's top-left corner (Qt 6.12, QSGSoftwareRenderableNodeUpdater::updateNodes; the DESK-002 report notes it).

#pragma once

#include "tilegrid.h"

#include <QtCore/QRectF>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtQuick/QSGTransformNode>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

class QQuickWindow;
class QSGImageNode;

namespace bayan {

// A tile to draw in a frame.
struct CanvasTile {
  TileKey key{};
  // Where it goes, in the canvas's coordinates.
  QRectF rect;
  QImage image;
  // Changes whenever the image does, so that a new image is uploaded again.
  std::uint64_t version = 0;
  // A tile of the current zoom; otherwise a stretched stand-in of another zoom, drawn below the current ones.
  bool current = true;
  // Drawn at its own scale on whole device pixels, so it is copied pixel for pixel; a stretched one is smoothed.
  bool exact = true;
};

class CanvasNode final : public QSGTransformNode {
public:
  // What updating the tiles did.
  struct TileUpdate {
    // Some new images wait for a later frame, because this one has uploaded enough.
    bool deferred = false;
    qint64 uploads = 0;
    qint64 uploadBytes = 0;
    // Tiles of the current zoom whose new image this frame shows for the first time.
    std::vector<TileKey> newlyShown;
  };

  CanvasNode();
  ~CanvasNode() override = default;
  Q_DISABLE_COPY_MOVE(CanvasNode)

  // A soft shadow and a neutral placeholder under each page, visible until the page's tiles arrive.
  void setPages(QQuickWindow &window, const std::vector<QRectF> &pages, const QColor &shadow, const QColor &placeholder);
  // Shows exactly these tiles, uploading at most maxUploads new images.
  [[nodiscard]] TileUpdate setTiles(QQuickWindow &window, const std::vector<CanvasTile> &tiles, int maxUploads);
  // The selection's rectangles, and the caret on top of them.
  void setOverlays(QQuickWindow &window, const std::vector<QRectF> &selection, const std::optional<QRectF> &caret,
                   const QColor &selectionColor, const QColor &caretColor);

private:
  struct TileSlot {
    QSGImageNode *node = nullptr;
    std::uint64_t version = 0;
  };

  // Puts one tile's node in place, uploading its image if it is new and the frame still may; returns whether the tile is on screen.
  bool placeTile(QQuickWindow &window, const CanvasTile &tile, int maxUploads, TileUpdate &update);
  // Deletes the nodes of tiles that are no longer shown.
  void removeTilesExcept(const std::set<TileKey> &shown);

  // Owned by this node: QSGNode deletes the children flagged OwnedByParent, the default.
  QSGNode *pages_ = new QSGTransformNode;    // NOLINT(cppcoreguidelines-owning-memory)
  QSGNode *standIns_ = new QSGTransformNode; // NOLINT(cppcoreguidelines-owning-memory)
  QSGNode *tiles_ = new QSGTransformNode;    // NOLINT(cppcoreguidelines-owning-memory)
  QSGNode *overlays_ = new QSGTransformNode; // NOLINT(cppcoreguidelines-owning-memory)
  std::map<TileKey, TileSlot> slots_;
};

} // namespace bayan
