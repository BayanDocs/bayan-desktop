#include "canvasaccessible.h"

#include "accessibletext.h"
#include "documentcanvas.h"
#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QWindow>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// A rectangle in an item's coordinates, in screen coordinates as accessibility reports them.
QRect screenRect(const QQuickItem &item, const QRectF &itemRect) {
  const QQuickWindow *window = item.window();
  if (window == nullptr) {
    return {};
  }
  QRectF rect = item.mapRectToScene(itemRect).translated(window->mapToGlobal(QPointF(0, 0)));
  // A caret position has no width; give it one pixel so that every bridge reports a usable rectangle.
  rect.setWidth(std::max<qreal>(rect.width(), 1.0));
  return rect.toAlignedRect();
}

} // namespace

QAccessibleInterface *canvasAccessibleFactory(const QString &className, QObject *object) {
  if (className == QLatin1StringView(DocumentCanvas::staticMetaObject.className())) {
    if (auto *canvas = qobject_cast<DocumentCanvas *>(object)) {
      return new CanvasAccessible(canvas); // NOLINT(cppcoreguidelines-owning-memory): Qt's accessibility cache takes ownership.
    }
  }
  return nullptr;
}

void installCanvasAccessibleFactory() { QAccessible::installFactory(&canvasAccessibleFactory); }

CanvasAccessible::CanvasAccessible(DocumentCanvas *canvas) : QAccessibleObject(canvas) {
  // From now on a screen reader (or a test) is looking: the canvas starts reporting text and caret changes.
  canvas->setAccessibleInterfaceActive(true);
}

DocumentCanvas *CanvasAccessible::canvas() const { return qobject_cast<DocumentCanvas *>(object()); }

bool CanvasAccessible::isValid() const { return canvas() != nullptr; }

QWindow *CanvasAccessible::window() const {
  const DocumentCanvas *canvas = this->canvas();
  return canvas == nullptr ? nullptr : canvas->window();
}

QAccessibleInterface *CanvasAccessible::parent() const {
  const DocumentCanvas *canvas = this->canvas();
  QQuickWindow *window = canvas == nullptr ? nullptr : canvas->window();
  if (window == nullptr) {
    return nullptr;
  }
  // The nearest ancestor that is in the accessibility tree, as Qt Quick's own items report it (QAccessibleQuickItem::parent).
  for (QQuickItem *ancestor = canvas->parentItem(); ancestor != nullptr; ancestor = ancestor->parentItem()) {
    if (ancestor == window->contentItem()) {
      break;
    }
    if (QAccessibleInterface *interface = QAccessible::queryAccessibleInterface(ancestor)) {
      return interface;
    }
  }
  if (QAccessibleInterface *root = window->accessibleRoot()) {
    return root;
  }
  return QAccessible::queryAccessibleInterface(window);
}

QAccessibleInterface *CanvasAccessible::child(int /*index*/) const { return nullptr; }

int CanvasAccessible::childCount() const { return 0; }

int CanvasAccessible::indexOfChild(const QAccessibleInterface * /*child*/) const { return -1; }

QAccessibleInterface *CanvasAccessible::childAt(int /*x*/, int /*y*/) const { return nullptr; }

QString CanvasAccessible::text(QAccessible::Text type) const {
  const DocumentCanvas *canvas = this->canvas();
  if (canvas == nullptr) {
    return {};
  }
  switch (type) {
  case QAccessible::Name:
    // The document's own name from the engine; the shell's label until the engine has sent its tree.
    return canvas->accessibleText().name().isEmpty() ? QCoreApplication::translate("DocumentCanvas", "Document")
                                                     : canvas->accessibleText().name();
  case QAccessible::Value:
    return canvas->accessibleText().text();
  default:
    return {};
  }
}

void CanvasAccessible::setText(QAccessible::Text /*type*/, const QString & /*text*/) {
  // Replacing the whole document's text is not something a screen reader should do, and protocol v0 has no message for it.
}

QRect CanvasAccessible::rect() const {
  const DocumentCanvas *canvas = this->canvas();
  return canvas == nullptr ? QRect() : screenRect(*canvas, canvas->boundingRect());
}

QAccessible::Role CanvasAccessible::role() const {
  // An editable multi-line text: macOS shows it as a text area (AXTextArea), Windows as an edit control with UI Automation's text pattern,
  // and AT-SPI as an editable text. "Document" would be the semantically closer role, but macOS gives document roles no text value.
  return QAccessible::EditableText;
}

QAccessible::State CanvasAccessible::state() const {
  QAccessible::State state;
  const DocumentCanvas *canvas = this->canvas();
  if (canvas == nullptr) {
    state.invalid = true;
    return state;
  }
  state.focusable = true;
  state.focused = canvas->hasActiveFocus();
  state.editable = true;
  state.multiLine = true;
  // Not selectableText: protocol v0 has no selection to offer (the DESK-002 report, section 6).
  state.invisible = !canvas->isVisible() || canvas->window() == nullptr || !canvas->window()->isVisible();
  return state;
}

void *CanvasAccessible::interface_cast(QAccessible::InterfaceType type) {
  switch (type) {
  case QAccessible::TextInterface:
    return static_cast<QAccessibleTextInterface *>(&text_);
  case QAccessible::EditableTextInterface:
    return static_cast<QAccessibleEditableTextInterface *>(&editableText_);
  case QAccessible::ActionInterface:
    return static_cast<QAccessibleActionInterface *>(&action_);
  default:
    return nullptr;
  }
}

int CanvasAccessible::cursorPosition() const {
  const DocumentCanvas *canvas = this->canvas();
  return canvas == nullptr ? 0 : canvas->accessibleCursorPosition();
}

bool CanvasAccessible::deletableAtCaret(int startOffset, int endOffset) const {
  const DocumentCanvas *canvas = this->canvas();
  if (canvas == nullptr || startOffset >= endOffset) {
    return false;
  }
  const int caret = cursorPosition();
  const AccessibleText::Segment *segment = canvas->accessibleText().segmentAt(caret);
  return segment != nullptr && segment->editable && startOffset >= segment->start && endOffset <= segment->end() &&
         (startOffset == caret || endOffset == caret);
}

void CanvasAccessible::Text::selection(int /*selectionIndex*/, int *startOffset, int *endOffset) const {
  // No selections in protocol v0's accessibility tree.
  *startOffset = 0;
  *endOffset = 0;
}

int CanvasAccessible::Text::selectionCount() const { return 0; }

void CanvasAccessible::Text::addSelection(int /*startOffset*/, int /*endOffset*/) {}

void CanvasAccessible::Text::removeSelection(int /*selectionIndex*/) {}

void CanvasAccessible::Text::setSelection(int /*selectionIndex*/, int /*startOffset*/, int /*endOffset*/) {}

int CanvasAccessible::Text::cursorPosition() const { return owner_.cursorPosition(); }

void CanvasAccessible::Text::setCursorPosition(int /*position*/) {
  // Protocol v0 cannot move the caret to an offset (the DESK-002 report recommends a message for it).
}

QString CanvasAccessible::Text::text(int startOffset, int endOffset) const {
  const DocumentCanvas *canvas = owner_.canvas();
  if (canvas == nullptr) {
    return {};
  }
  const QString &all = canvas->accessibleText().text();
  const auto start = std::clamp<qsizetype>(startOffset, 0, all.size());
  const auto end = std::clamp<qsizetype>(endOffset, start, all.size());
  return all.mid(start, end - start);
}

int CanvasAccessible::Text::characterCount() const {
  const DocumentCanvas *canvas = owner_.canvas();
  return canvas == nullptr ? 0 : static_cast<int>(canvas->accessibleText().text().size());
}

QRect CanvasAccessible::Text::characterRect(int offset) const {
  const DocumentCanvas *canvas = owner_.canvas();
  if (canvas == nullptr) {
    return {};
  }
  const auto box = canvas->accessibleText().characterBox(offset);
  const auto itemRect = box ? canvas->itemRectFor(box->page, box->rect) : std::nullopt;
  return itemRect ? screenRect(*canvas, *itemRect) : QRect();
}

int CanvasAccessible::Text::offsetAtPoint(const QPoint & /*point*/) const {
  // Which character is under a point is hit-testing, which belongs to the engine (engine protocol §7); protocol v0 has no query for it.
  return -1;
}

void CanvasAccessible::Text::scrollToSubstring(int startIndex, int /*endIndex*/) {
  DocumentCanvas *canvas = owner_.canvas();
  if (canvas == nullptr) {
    return;
  }
  const auto box = canvas->accessibleText().characterBox(startIndex);
  if (const auto itemRect = box ? canvas->itemRectFor(box->page, box->rect) : std::nullopt) {
    canvas->revealItemRect(*itemRect);
  }
}

QString CanvasAccessible::Text::attributes(int offset, int *startOffset, int *endOffset) const {
  // The v0 tree has no text attributes (fonts, styles, languages); report one run per line.
  const DocumentCanvas *canvas = owner_.canvas();
  const AccessibleText::Segment *segment = canvas == nullptr ? nullptr : canvas->accessibleText().segmentAt(offset);
  *startOffset = segment == nullptr ? offset : segment->start;
  *endOffset = segment == nullptr ? offset : segment->end();
  return {};
}

void CanvasAccessible::EditableText::deleteText(int startOffset, int endOffset) {
  // Protocol v0 edits only at the caret, with keys; text elsewhere cannot be deleted from here.
  DocumentCanvas *canvas = owner_.canvas();
  if (canvas == nullptr || !owner_.deletableAtCaret(startOffset, endOffset)) {
    return;
  }
  const int caret = owner_.cursorPosition();
  canvas->deleteAroundCaret(caret - startOffset, endOffset - caret);
}

void CanvasAccessible::EditableText::insertText(int offset, const QString &text) {
  const DocumentCanvas *canvas = owner_.canvas();
  if (canvas == nullptr || offset != owner_.cursorPosition() || text.isEmpty()) {
    return;
  }
  const AccessibleText::Segment *segment = canvas->accessibleText().segmentAt(offset);
  if (segment != nullptr && segment->editable && canvas->session() != nullptr) {
    canvas->session()->sendText(text);
  }
}

void CanvasAccessible::EditableText::replaceText(int startOffset, int endOffset, const QString &text) {
  // Replacing an empty range inserts.
  if (startOffset == endOffset) {
    insertText(startOffset, text);
    return;
  }
  if (!owner_.deletableAtCaret(startOffset, endOffset)) {
    return;
  }
  deleteText(startOffset, endOffset);
  // The engine handles messages in order, so the text lands where the deleted text was.
  if (const DocumentCanvas *canvas = owner_.canvas(); canvas != nullptr && canvas->session() != nullptr && !text.isEmpty()) {
    canvas->session()->sendText(text);
  }
}

QStringList CanvasAccessible::Action::actionNames() const { return {setFocusAction()}; }

void CanvasAccessible::Action::doAction(const QString &actionName) {
  if (DocumentCanvas *canvas = owner_.canvas(); canvas != nullptr && actionName == setFocusAction()) {
    canvas->forceActiveFocus(Qt::OtherFocusReason);
  }
}

QStringList CanvasAccessible::Action::keyBindingsForAction(const QString & /*actionName*/) const { return {}; }

} // namespace bayan
