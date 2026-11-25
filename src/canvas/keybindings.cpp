#include "keybindings.h"

#include <QtCore/QStringView>

#include <algorithm>
#include <optional>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

bool isAsciiLetterOrDigit(QChar character) {
  const char16_t unit = character.unicode();
  return (unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z') || (unit >= u'0' && unit <= u'9');
}

// Whether a key value is a single ASCII letter or digit.
bool isOneAsciiLetterOrDigit(QStringView key) { return key.size() == 1 && isAsciiLetterOrDigit(key.front()); }

// ASCII letters in lower case, every other character unchanged: Rust, Qt and JavaScript change the case of other letters differently.
QString foldAscii(QStringView key) {
  QString folded = key.toString();
  for (QChar &character : folded) {
    if (character.unicode() >= u'A' && character.unicode() <= u'Z') {
      character = QChar(static_cast<char16_t>(character.unicode() - u'A' + u'a'));
    }
  }
  return folded;
}

// The lower-case letter or the digit of a code from KeyA to KeyZ or Digit0 to Digit9, or an empty string.
QString letterOrDigitOf(QStringView code) {
  if (code.size() == 4 && code.startsWith(u"Key") && code.back().unicode() >= u'A' && code.back().unicode() <= u'Z') {
    return foldAscii(code.last(1));
  }
  if (code.size() == 6 && code.startsWith(u"Digit") && code.back().unicode() >= u'0' && code.back().unicode() <= u'9') {
    return code.last(1).toString();
  }
  return {};
}

bool agrees(const std::optional<bool> &wanted, bool held) { return !wanted.has_value() || *wanted == held; }

} // namespace

bool KeyBindings::matches(const protocol::KeyEntry &entry, const protocol::KeyInput &press) {
  const protocol::Modifiers &held = press.modifiers;
  if (!agrees(entry.ctrl, held.ctrl) || !agrees(entry.shift, held.shift) || !agrees(entry.alt, held.alt) ||
      !agrees(entry.meta, held.meta)) {
    return false;
  }
  if (!entry.code.isEmpty()) {
    return entry.code == press.code;
  }
  const QString wanted = foldAscii(entry.key);
  if (wanted == foldAscii(press.key)) {
    return true;
  }
  // The key's position instead of the character it types, for a press that types no ASCII letter or digit (another script), unless AltGr
  // types it: then it stays text.
  if (!isOneAsciiLetterOrDigit(entry.key) || isOneAsciiLetterOrDigit(press.key) || (held.ctrl && held.alt)) {
    return false;
  }
  const QString position = letterOrDigitOf(press.code);
  return !position.isEmpty() && position == wanted;
}

bool KeyBindings::sends(const protocol::KeyInput &press) const {
  if (press.composing) {
    return false;
  }
  const auto matching = [&press](const protocol::KeyEntry &entry) { return matches(entry, press); };
  if (std::ranges::any_of(manifest_.reservedKeys, matching)) {
    return false;
  }
  // A command with shell_action is the shell's to carry out, whether the shell knows the action or not: its keys never go to the engine.
  const bool bound = std::ranges::any_of(manifest_.commands, [&matching](const protocol::ManifestCommand &command) {
    return !command.shellAction.has_value() && std::ranges::any_of(command.bindings, matching);
  });
  return bound || std::ranges::any_of(manifest_.documentKeys, matching);
}

bool KeyBindings::knowsShellAction(const QString &action) { return action == u"copy"_s || action == u"cut"_s || action == u"paste"_s; }

QStringList KeyBindings::labels(const QString &commandId) const {
  QStringList result;
  const auto command = std::ranges::find(manifest_.commands, commandId, &protocol::ManifestCommand::id);
  if (command == manifest_.commands.end() || (command->shellAction && !knowsShellAction(*command->shellAction))) {
    return result;
  }
  for (const protocol::KeyEntry &binding : command->bindings) {
    result.append(binding.label);
  }
  return result;
}

} // namespace bayan
