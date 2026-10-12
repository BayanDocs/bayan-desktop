#include "canvasnode.h"

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGImageNode>
#include <QtQuick/QSGRectangleNode>
#include <QtQuick/QSGTexture>

namespace bayan {
namespace {

// Makes a layer hold exactly count rectangle nodes, creating or deleting nodes at its end, and returns them.
std::vector<QSGRectangleNode *> rectangleNodes(QQuickWindow &window, QSGNode &layer, std::size_t count) {
  std::vector<QSGRectangleNode *> nodes;
  for (QSGNode *child = layer.firstChild(); child != nullptr; child = child->nextSibling()) {
    nodes.push_back(static_cast<QSGRectangleNode *>(child)); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast): only these here.
  }
  while (nodes.size() > count) {
    QSGRectangleNode *last = nodes.back();
    layer.removeChildNode(last);
    delete last; // NOLINT(cppcoreguidelines-owning-memory): removed from its parent, so nothing else deletes it.
    nodes.pop_back();
  }
  while (nodes.size() < count) {
    QSGRectangleNode *node = window.createRectangleNode();
    layer.appendChildNode(node);
    nodes.push_back(node);
  }
  return nodes;
}

// Moves a node under another parent, unless it is there already.
void moveNode(QSGNode &node, QSGNode &parent) {
  if (node.parent() == &parent) {
    return;
  }
  if (node.parent() != nullptr) {
    node.parent()->removeChildNode(&node);
  }
  parent.appendChildNode(&node);
}

} // namespace

CanvasNode::CanvasNode() {
  for (QSGNode *layer : {pages_, standIns_, tiles_, overlays_}) {
    appendChildNode(layer);
  }
}

void CanvasNode::setPages(QQuickWindow &window, const std::vector<QRectF> &pages, const QColor &shadow, const QColor &placeholder) {
  const std::vector<QSGRectangleNode *> nodes = rectangleNodes(window, *pages_, pages.size() * 2);
  for (std::size_t i = 0; i < pages.size(); ++i) {
    nodes.at(2 * i)->setRect(pages.at(i).translated(2, 3));
    nodes.at(2 * i)->setColor(shadow);
    nodes.at((2 * i) + 1)->setRect(pages.at(i));
    nodes.at((2 * i) + 1)->setColor(placeholder);
  }
}

CanvasNode::TileUpdate CanvasNode::setTiles(QQuickWindow &window, const std::vector<CanvasTile> &tiles, int maxUploads) {
  TileUpdate update;
  std::set<TileKey> shown;
  for (const CanvasTile &tile : tiles) {
    if (placeTile(window, tile, maxUploads, update)) {
      shown.insert(tile.key);
    }
  }
  removeTilesExcept(shown);
  return update;
}

bool CanvasNode::placeTile(QQuickWindow &window, const CanvasTile &tile, int maxUploads, TileUpdate &update) {
  TileSlot &slot = slots_[tile.key];
  if (slot.node == nullptr || slot.version != tile.version) {
    if (update.uploads >= maxUploads) {
      // Keep showing the old image if there is one; a tile without any waits.
      update.deferred = true;
    } else {
      QSGTexture *texture = window.createTextureFromImage(tile.image, QQuickWindow::TextureHasAlphaChannel);
      if (slot.node == nullptr) {
        slot.node = window.createImageNode();
        slot.node->setOwnsTexture(true);
      }
      slot.node->setTexture(texture); // deletes the node's previous texture
      slot.version = tile.version;
      ++update.uploads;
      update.uploadBytes += tile.image.sizeInBytes();
      if (tile.current) {
        update.newlyShown.push_back(tile.key);
      }
    }
  }
  if (slot.node == nullptr) {
    slots_.erase(tile.key);
    return false;
  }
  moveNode(*slot.node, tile.current ? *tiles_ : *standIns_);
  slot.node->setRect(tile.rect);
  slot.node->setFiltering(tile.exact ? QSGTexture::Nearest : QSGTexture::Linear);
  return true;
}

void CanvasNode::removeTilesExcept(const std::set<TileKey> &shown) {
  for (auto slot = slots_.begin(); slot != slots_.end();) {
    if (shown.contains(slot->first)) {
      ++slot;
      continue;
    }
    if (slot->second.node->parent() != nullptr) {
      slot->second.node->parent()->removeChildNode(slot->second.node);
    }
    delete slot->second.node; // NOLINT(cppcoreguidelines-owning-memory): removed from its parent; deleting it also deletes its texture.
    slot = slots_.erase(slot);
  }
}

void CanvasNode::setOverlays(QQuickWindow &window, const std::vector<QRectF> &selection, const std::optional<QRectF> &caret,
                             const QColor &selectionColor, const QColor &caretColor) {
  const std::vector<QSGRectangleNode *> nodes = rectangleNodes(window, *overlays_, selection.size() + (caret ? 1 : 0));
  for (std::size_t i = 0; i < selection.size(); ++i) {
    nodes.at(i)->setRect(selection.at(i));
    nodes.at(i)->setColor(selectionColor);
  }
  if (caret) {
    nodes.back()->setRect(*caret);
    nodes.back()->setColor(caretColor);
  }
}

} // namespace bayan
