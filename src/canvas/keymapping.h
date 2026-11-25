// Turns Qt's keyboard events into the engine's input messages (engine protocol §6.5, §14.3): the press as input.key describes it, with the
// DOM's key and code values, and the text a key types, for input.text.
//
// The protocol uses the web's names so that both shells describe presses alike: key is the key's meaning ("a", "Backspace", "ArrowLeft"),
// code the physical key ("KeyA"), named after its place on a US keyboard whatever the layout. Qt reports the meaning, and each platform's
// own code for the physical key: Windows' scan codes (set 1, with 0xE000 for the extended keys), macOS's virtual key codes, and on Linux
// the XKB keycodes of X11 and Wayland (the kernel's evdev codes plus 8). code is filled from those for every key whose position the DOM
// names, letters and digits included, so that the engine and the shell can fall back to a key's position when it types a letter of another
// script (§14.3 item 3); for an event without them, such as one a test makes, it is filled only for keys whose position does not depend on
// the layout (arrows, Enter, function keys, the numeric keypad and so on), and left empty otherwise. Modifiers follow the DOM too: on macOS
// Qt reports Command as Control by default, which the DOM calls meta, so the two are swapped back.
//
// Which presses go to the engine as input.key is decided by KeyBindings; typed text goes as input.text only
// (DocumentCanvas::keyPressEvent).

#pragma once

#include "protocol.h"

#include <QtCore/QString>
#include <QtGui/QKeyEvent>

#include <cstdint>
#include <optional>

namespace bayan {

// The native key codes that Qt's key events carry, which name the physical key.
enum class NativeKeyCodes : std::uint8_t {
  // None that the shell reads (the offscreen platform of the tests, and platforms without a map).
  None,
  // QKeyEvent::nativeScanCode() on Windows: the scan code of set 1, with 0xE000 added for the extended keys.
  WindowsScanCodes,
  // QKeyEvent::nativeVirtualKey() on macOS: the virtual key code (kVK_ANSI_A and so on).
  MacVirtualKeys,
  // QKeyEvent::nativeScanCode() on Linux with X11 or Wayland: the XKB keycode, the kernel's evdev code plus 8.
  XkbKeycodes,
};

struct KeyMappingOptions {
  // Whether Qt reports Command as Control and Control as Meta: the default on macOS (see Qt::AA_MacDontSwapCtrlAndMeta).
  bool commandIsControl = false;
  // Whether AltGr arrives as Control+Alt, as on Windows; then Control+Alt+key may type a character.
  bool altGraphIsControlAlt = false;
  NativeKeyCodes nativeKeyCodes = NativeKeyCodes::None;

  // The options for the platform the application runs on: the operating system, and Qt's platform plugin for the native key codes.
  [[nodiscard]] static KeyMappingOptions forPlatform();
};

// The DOM modifier flags for Qt's modifiers.
[[nodiscard]] protocol::Modifiers domModifiers(Qt::KeyboardModifiers modifiers, const KeyMappingOptions &options);
// The DOM key value for a key event ("a", "A", "Backspace", "Dead", "Unidentified" and so on).
[[nodiscard]] QString domKey(const QKeyEvent &event, const KeyMappingOptions &options);
// The DOM code value of the key's physical position, from the event's native key code or, without one, for keys whose position does not
// depend on the layout; otherwise an empty string.
[[nodiscard]] QString domCode(const QKeyEvent &event, const KeyMappingOptions &options);
// The DOM code value for a native key code of the given kind, or an empty string if the code names no key the DOM names.
[[nodiscard]] QString domCodeForNativeKey(NativeKeyCodes kind, quint32 nativeCode);
// The press as input.key describes it.
[[nodiscard]] protocol::KeyInput keyInput(const QKeyEvent &event, bool composing, const KeyMappingOptions &options);
// The text a key press types, if any: printable characters without a shortcut modifier.
[[nodiscard]] std::optional<QString> typedText(const QKeyEvent &event, const KeyMappingOptions &options);

} // namespace bayan
