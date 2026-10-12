// Turns Qt's keyboard events into the engine's input messages (engine protocol §6.5): input.key with the DOM's key and code values, and
// input.text for the text a key types.
//
// The protocol uses the web's names so that both shells send the same thing: key is the key's meaning ("a", "Backspace", "ArrowLeft"),
// code the physical key ("KeyA"). Qt reports the meaning, but not the physical key in a portable way, so code is filled in only for keys
// whose position does not depend on the keyboard layout (arrows, Enter, function keys, the numeric keypad and so on) and left empty
// otherwise (the DESK-002 report discusses this). Modifiers follow the DOM too: on macOS Qt reports Command as Control by default, which
// the DOM calls meta, so the two are swapped back.
//
// Typing a character sends both: input.key for the key press (the engine may treat it as a shortcut) and input.text for the character.

#pragma once

#include "protocol.h"

#include <QtCore/QString>
#include <QtGui/QKeyEvent>

#include <optional>

namespace bayan {

struct KeyMappingOptions {
  // Whether Qt reports Command as Control and Control as Meta: the default on macOS (see Qt::AA_MacDontSwapCtrlAndMeta).
  bool commandIsControl = false;
  // Whether AltGr arrives as Control+Alt, as on Windows; then Control+Alt+key may type a character.
  bool altGraphIsControlAlt = false;

  // The options for the platform the application runs on.
  [[nodiscard]] static KeyMappingOptions forPlatform();
};

// The DOM modifier flags for Qt's modifiers.
[[nodiscard]] protocol::Modifiers domModifiers(Qt::KeyboardModifiers modifiers, const KeyMappingOptions &options);
// The DOM key value for a key event ("a", "A", "Backspace", "Dead", "Unidentified" and so on).
[[nodiscard]] QString domKey(const QKeyEvent &event, const KeyMappingOptions &options);
// The DOM code value for keys whose physical position is known regardless of the layout, or an empty string.
[[nodiscard]] QString domCode(const QKeyEvent &event);
// input.key for a key press.
[[nodiscard]] protocol::KeyInput keyInput(const QKeyEvent &event, bool composing, const KeyMappingOptions &options);
// The text a key press types, if any: printable characters without a shortcut modifier.
[[nodiscard]] std::optional<QString> typedText(const QKeyEvent &event, const KeyMappingOptions &options);

} // namespace bayan
