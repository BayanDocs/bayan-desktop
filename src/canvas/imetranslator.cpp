#include "imetranslator.h"

#include <QtGui/QBrush>
#include <QtGui/QTextFormat>

#include <algorithm>

namespace bayan {

std::pair<int, int> ImeTranslator::selectionOf(const QString &preedit, const QList<QInputMethodEvent::Attribute> &attributes) {
  const auto length = static_cast<int>(preedit.size());
  // The protocol's offsets never fall inside a surrogate pair (engine protocol §7); an offset that would moves back to the pair's start.
  const auto boundary = [&preedit, length](int offset) {
    offset = std::clamp(offset, 0, length);
    if (offset > 0 && offset < length && preedit.at(offset).isLowSurrogate() && preedit.at(offset - 1).isHighSurrogate()) {
      --offset;
    }
    return offset;
  };
  for (const QInputMethodEvent::Attribute &attribute : attributes) {
    if (attribute.type != QInputMethodEvent::TextFormat || attribute.length <= 0) {
      continue;
    }
    // Windows (the converted clause) and macOS (the input method's selected range) mark the selection with a background colour.
    if (attribute.value.value<QTextFormat>().background().style() != Qt::NoBrush) {
      const int start = boundary(attribute.start);
      const int end = boundary(attribute.start + attribute.length);
      if (start < end) {
        return {start, end};
      }
    }
  }
  for (const QInputMethodEvent::Attribute &attribute : attributes) {
    if (attribute.type == QInputMethodEvent::Cursor) {
      const int at = boundary(attribute.start);
      return {at, at};
    }
  }
  return {length, length};
}

std::vector<ImeAction> ImeTranslator::translate(const QInputMethodEvent &event) {
  std::vector<ImeAction> actions;
  const QString &preedit = event.preeditString();
  const QString &commit = event.commitString();
  const int replaceFrom = event.replacementStart();
  const int replaceLength = event.replacementLength();

  // Text around the caret to replace. Qt counts from the caret (negative: before it). A range that starts after the caret cannot be
  // reached with Backspace and Delete alone; protocol v0 has no way to say it, so it is left out (the DESK-002 report lists the gap).
  if (replaceLength > 0 && replaceFrom <= 0) {
    if (composing_) {
      actions.push_back({.kind = ImeAction::Kind::End});
      reset();
    }
    const int before = std::min(replaceLength, -replaceFrom);
    const int after = replaceLength - before;
    if (before > 0) {
      actions.push_back({.kind = ImeAction::Kind::DeleteBefore, .length = before});
    }
    if (after > 0) {
      actions.push_back({.kind = ImeAction::Kind::DeleteAfter, .length = after});
    }
  }

  if (!commit.isEmpty()) {
    // input.text replaces an active composition and ends it (§6.5).
    actions.push_back({.kind = ImeAction::Kind::Commit, .text = commit});
    reset();
  }

  if (!preedit.isEmpty()) {
    const auto [start, end] = selectionOf(preedit, event.attributes());
    if (!composing_) {
      actions.push_back({.kind = ImeAction::Kind::Start});
      composing_ = true;
      selectionStart_ = -1;
      selectionEnd_ = -1;
    }
    if (preedit != text_ || start != selectionStart_ || end != selectionEnd_) {
      actions.push_back({.kind = ImeAction::Kind::Update, .text = preedit, .selectionStart = start, .selectionEnd = end});
      text_ = preedit;
      selectionStart_ = start;
      selectionEnd_ = end;
    }
  } else if (composing_) {
    // An empty pre-edit without a commit: the user cancelled the composition (for example with Escape).
    actions.push_back({.kind = ImeAction::Kind::End});
    reset();
  }
  return actions;
}

void ImeTranslator::reset() {
  composing_ = false;
  text_.clear();
  selectionStart_ = 0;
  selectionEnd_ = 0;
}

} // namespace bayan
