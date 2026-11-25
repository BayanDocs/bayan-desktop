// Which key presses the document canvas sends to the engine (engine protocol §14.3).
//
// The engine publishes the keys it handles as data, in the UI manifest, resolved for the session's platform and runtime: each command's
// bindings, the document keys (the twelve editing keys, with any modifiers) and the reserved keys, the presses that the shell keeps for
// itself or the operating system (Tab, F6, F10, the context menu key, KeyTips, the clipboard keys and others). A press goes to the engine
// as input.key if and only if it matches a binding of a command without shell_action, or a document key, matches no reserved key, and no
// input method is composing with it; every other press stays with Qt and the shell's own interface. Typed text never goes as input.key: it
// reaches the engine as input.text (DocumentCanvas::keyPressEvent).
//
// One way to match, for bindings, document keys and reserved keys alike (§14.3 item 3): a modifier that an entry gives must equal the
// press's, one that it leaves out matches either state; an entry with code matches the press's code; an entry with key matches the press's
// key, ASCII letters without regard to case and every other character exactly; and if they differ, the entry's key is an ASCII letter or
// digit, the press's key is not, and the press does not hold Control and Alt together (Windows' AltGr), the letter or digit of the press's
// code (KeyA to KeyZ, Digit0 to Digit9) is compared instead, so that shortcuts keep working with Arabic, Hebrew, Cyrillic or Greek layouts.
// The shell decides nothing about what a key means: the engine decides from the same bindings, matched the same way.

#pragma once

#include "protocol.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <utility>

namespace bayan {

class KeyBindings {
public:
  // No key data yet (before the manifest arrived, or if the engine refused it): no press goes to the engine.
  KeyBindings() = default;
  explicit KeyBindings(protocol::UiManifest manifest) : manifest_(std::move(manifest)) {}

  // Whether the canvas sends this press to the engine as input.key (§14.3 item 2).
  [[nodiscard]] bool sends(const protocol::KeyInput &press) const;
  // Whether a press matches an entry of the key data (§14.3 item 3).
  [[nodiscard]] static bool matches(const protocol::KeyEntry &entry, const protocol::KeyInput &press);
  // Whether the shell carries out a command with this shell_action: copy, cut and paste in v1 (DESK-005 wires them to the clipboard). A
  // command whose shell_action the shell does not know is neither offered nor are its keys sent (§14.3 item 1).
  [[nodiscard]] static bool knowsShellAction(const QString &action);
  // The labels of a command's bindings, as the engine writes them for the platform ("Ctrl+B", "⌘B"), for wherever the interface shows the
  // command's shortcut: the shell never builds that text itself. Empty for an unknown command and for one the shell does not offer.
  [[nodiscard]] QStringList labels(const QString &commandId) const;

  [[nodiscard]] const protocol::UiManifest &manifest() const { return manifest_; }

private:
  protocol::UiManifest manifest_;
};

} // namespace bayan
