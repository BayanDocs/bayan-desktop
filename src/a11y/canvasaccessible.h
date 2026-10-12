// The document canvas's accessibility interface: what screen readers see of the document (DESK-002, ADR-0013 §6, ADR-0020 §3).
//
// The canvas draws pixels, so Qt cannot know its text by itself. When a screen reader (through UI Automation on Windows, NSAccessibility on
// macOS or AT-SPI on Linux) asks about the canvas, Qt asks the factories installed with QAccessible::installFactory; ours answers with this
// interface. It presents the engine's accessibility tree as one multi-line editable text (AccessibleText): screen readers read it by
// character, word, line and paragraph, follow the caret, and learn where each character is on the screen. Editing through accessibility
// (inserting or deleting text next to the caret) is relayed to the engine as input, because the engine decides what the edit does.
//
// What protocol v0 cannot answer is left out on purpose and listed in the DESK-002 report: moving the caret to an offset
// (setCursorPosition), selections, and finding the character at a screen point (offsetAtPoint), which would need the engine's hit-testing
// (engine protocol §7).

#pragma once

#include <QtGui/QAccessible>
#include <QtGui/QAccessibleActionInterface>
#include <QtGui/QAccessibleEditableTextInterface>
#include <QtGui/QAccessibleObject>
#include <QtGui/QAccessibleTextInterface>

namespace bayan {

class DocumentCanvas;

// Answers Qt's question "which accessibility interface does this object have?" for document canvases, and nothing else.
QAccessibleInterface *canvasAccessibleFactory(const QString &className, QObject *object);
// Installs the factory (installing it again does nothing).
void installCanvasAccessibleFactory();

// The interface of one canvas. Qt's accessibility interfaces (text, editable text, actions) are classes with virtual functions; each is
// implemented by a member object of its own, which interface_cast hands out, so that no class inherits more than one of them.
class CanvasAccessible final : public QAccessibleObject {
public:
  explicit CanvasAccessible(DocumentCanvas *canvas);

  // QAccessibleInterface
  [[nodiscard]] bool isValid() const override;
  [[nodiscard]] QWindow *window() const override;
  [[nodiscard]] QAccessibleInterface *parent() const override;
  [[nodiscard]] QAccessibleInterface *child(int index) const override;
  [[nodiscard]] int childCount() const override;
  [[nodiscard]] int indexOfChild(const QAccessibleInterface *child) const override;
  [[nodiscard]] QAccessibleInterface *childAt(int x, int y) const override;
  [[nodiscard]] QString text(QAccessible::Text type) const override;
  void setText(QAccessible::Text type, const QString &text) override;
  [[nodiscard]] QRect rect() const override;
  [[nodiscard]] QAccessible::Role role() const override;
  [[nodiscard]] QAccessible::State state() const override;
  [[nodiscard]] void *interface_cast(QAccessible::InterfaceType type) override;

private:
  // The document's text: reading by character, word and line, the caret, and where characters are on the screen.
  class Text final : public QAccessibleTextInterface {
  public:
    explicit Text(const CanvasAccessible &owner) : owner_(owner) {}
    void selection(int selectionIndex, int *startOffset, int *endOffset) const override;
    [[nodiscard]] int selectionCount() const override;
    void addSelection(int startOffset, int endOffset) override;
    void removeSelection(int selectionIndex) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    [[nodiscard]] int cursorPosition() const override;
    void setCursorPosition(int position) override;
    [[nodiscard]] QString text(int startOffset, int endOffset) const override;
    [[nodiscard]] int characterCount() const override;
    [[nodiscard]] QRect characterRect(int offset) const override;
    [[nodiscard]] int offsetAtPoint(const QPoint &point) const override;
    void scrollToSubstring(int startIndex, int endIndex) override;
    [[nodiscard]] QString attributes(int offset, int *startOffset, int *endOffset) const override;

  private:
    const CanvasAccessible &owner_;
  };

  // Editing through accessibility (dictation, assistive input), relayed to the engine as input next to the caret.
  class EditableText final : public QAccessibleEditableTextInterface {
  public:
    explicit EditableText(const CanvasAccessible &owner) : owner_(owner) {}
    void deleteText(int startOffset, int endOffset) override;
    void insertText(int offset, const QString &text) override;
    void replaceText(int startOffset, int endOffset, const QString &text) override;

  private:
    const CanvasAccessible &owner_;
  };

  // The one action: moving the keyboard focus to the document.
  class Action final : public QAccessibleActionInterface {
  public:
    explicit Action(const CanvasAccessible &owner) : owner_(owner) {}
    [[nodiscard]] QStringList actionNames() const override;
    void doAction(const QString &actionName) override;
    [[nodiscard]] QStringList keyBindingsForAction(const QString &actionName) const override;

  private:
    const CanvasAccessible &owner_;
  };

  [[nodiscard]] DocumentCanvas *canvas() const;
  // The caret's offset in the whole text.
  [[nodiscard]] int cursorPosition() const;
  // Whether text next to the caret, from startOffset to endOffset, can be deleted with Backspace or Delete in an editable node.
  [[nodiscard]] bool deletableAtCaret(int startOffset, int endOffset) const;

  Text text_{*this};
  EditableText editableText_{*this};
  Action action_{*this};
};

} // namespace bayan
