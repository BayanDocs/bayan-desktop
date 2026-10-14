// Turns Qt's input-method events into the engine's composition messages (engine protocol §6.5, DESK-002).
//
// An input method (IME) lets people type languages with more characters than keys: in Japanese, "nihongo" becomes にほんご and then 日本語;
// in Korean, keys combine into syllables such as 한. While the user composes, Qt sends QInputMethodEvents with the pre-edit text (shown
// underlined at the caret, not yet part of the document) and finally a commit string (the chosen text). The engine wants the same story as
// input.composition start, update and end, plus input.text for the committed text, which replaces an active composition.
//
// Qt's events carry details the protocol has no message for yet, which this translator turns into the closest thing the protocol can say:
// - The selected part of the composition (the clause being converted) arrives as a styled range with a background colour on Windows and
//   macOS; it becomes the composition's selection.
// - Some input methods replace text next to the caret: macOS's accent menu (hold "e", choose "é"), Japanese reconversion, IBus's
//   delete-surrounding-text. Qt says "replace length code units from start relative to the caret". Protocol v0 cannot express that, so the
//   translator asks for deletions before and after the caret, which the canvas sends as Backspace and Delete keys. The DESK-002 report
//   recommends a replacement range on input.text instead.
//
// Pure logic without Qt Quick, so the tests can replay what each platform's input method sends.

#pragma once

#include <QtCore/QString>
#include <QtGui/QInputMethodEvent>

#include <cstdint>
#include <vector>

namespace bayan {

struct ImeAction {
  enum class Kind : std::uint8_t {
    Start,        // input.composition start
    Update,       // input.composition update with text and selection
    End,          // input.composition end: the composition is discarded
    Commit,       // input.text: committed text, which also ends a composition
    DeleteBefore, // delete length UTF-16 code units before the caret
    DeleteAfter,  // delete length UTF-16 code units after the caret
  };

  Kind kind = Kind::End;
  QString text{};
  int selectionStart = 0;
  int selectionEnd = 0;
  int length = 0;
  friend bool operator==(const ImeAction &, const ImeAction &) = default;
};

class ImeTranslator {
public:
  // The protocol actions for one event, in the order they must be sent.
  [[nodiscard]] std::vector<ImeAction> translate(const QInputMethodEvent &event);

  [[nodiscard]] bool composing() const { return composing_; }
  [[nodiscard]] const QString &composition() const { return text_; }
  // Forgets the composition without sending anything (the caller decides whether to commit or discard it).
  void reset();

  // The composition's selection described by Qt's attributes: the range styled with a background colour, otherwise the input method's
  // cursor, otherwise the end of the text. Never splits a surrogate pair.
  [[nodiscard]] static std::pair<int, int> selectionOf(const QString &preedit, const QList<QInputMethodEvent::Attribute> &attributes);

private:
  bool composing_ = false;
  QString text_;
  int selectionStart_ = 0;
  int selectionEnd_ = 0;
};

} // namespace bayan
