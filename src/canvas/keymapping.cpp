#include "keymapping.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringView>

#include <algorithm>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// Keys whose DOM key value does not depend on the layout, with their DOM code where the physical key is the same everywhere.
struct NamedKey {
  int qtKey;
  const char16_t *key;
  const char16_t *code;
};

constexpr std::array kNamedKeys{
    NamedKey{.qtKey = Qt::Key_Backspace, .key = u"Backspace", .code = u"Backspace"},
    NamedKey{.qtKey = Qt::Key_Tab, .key = u"Tab", .code = u"Tab"},
    NamedKey{.qtKey = Qt::Key_Backtab, .key = u"Tab", .code = u"Tab"},
    NamedKey{.qtKey = Qt::Key_Return, .key = u"Enter", .code = u"Enter"},
    NamedKey{.qtKey = Qt::Key_Enter, .key = u"Enter", .code = u"NumpadEnter"},
    NamedKey{.qtKey = Qt::Key_Escape, .key = u"Escape", .code = u"Escape"},
    NamedKey{.qtKey = Qt::Key_Delete, .key = u"Delete", .code = u"Delete"},
    NamedKey{.qtKey = Qt::Key_Insert, .key = u"Insert", .code = u"Insert"},
    NamedKey{.qtKey = Qt::Key_Home, .key = u"Home", .code = u"Home"},
    NamedKey{.qtKey = Qt::Key_End, .key = u"End", .code = u"End"},
    NamedKey{.qtKey = Qt::Key_PageUp, .key = u"PageUp", .code = u"PageUp"},
    NamedKey{.qtKey = Qt::Key_PageDown, .key = u"PageDown", .code = u"PageDown"},
    NamedKey{.qtKey = Qt::Key_Left, .key = u"ArrowLeft", .code = u"ArrowLeft"},
    NamedKey{.qtKey = Qt::Key_Right, .key = u"ArrowRight", .code = u"ArrowRight"},
    NamedKey{.qtKey = Qt::Key_Up, .key = u"ArrowUp", .code = u"ArrowUp"},
    NamedKey{.qtKey = Qt::Key_Down, .key = u"ArrowDown", .code = u"ArrowDown"},
    NamedKey{.qtKey = Qt::Key_Shift, .key = u"Shift", .code = u""},
    NamedKey{.qtKey = Qt::Key_Alt, .key = u"Alt", .code = u""},
    NamedKey{.qtKey = Qt::Key_AltGr, .key = u"AltGraph", .code = u""},
    NamedKey{.qtKey = Qt::Key_Super_L, .key = u"Meta", .code = u"MetaLeft"},
    NamedKey{.qtKey = Qt::Key_Super_R, .key = u"Meta", .code = u"MetaRight"},
    NamedKey{.qtKey = Qt::Key_CapsLock, .key = u"CapsLock", .code = u"CapsLock"},
    NamedKey{.qtKey = Qt::Key_NumLock, .key = u"NumLock", .code = u"NumLock"},
    NamedKey{.qtKey = Qt::Key_ScrollLock, .key = u"ScrollLock", .code = u"ScrollLock"},
    NamedKey{.qtKey = Qt::Key_Menu, .key = u"ContextMenu", .code = u"ContextMenu"},
    NamedKey{.qtKey = Qt::Key_Print, .key = u"PrintScreen", .code = u"PrintScreen"},
    NamedKey{.qtKey = Qt::Key_Pause, .key = u"Pause", .code = u"Pause"},
    NamedKey{.qtKey = Qt::Key_Clear, .key = u"Clear", .code = u""},
    NamedKey{.qtKey = Qt::Key_Help, .key = u"Help", .code = u"Help"},
    // Keys of East Asian input methods (UI Events KeyboardEvent key values, "IME and composition keys").
    NamedKey{.qtKey = Qt::Key_Kanji, .key = u"KanjiMode", .code = u""},
    NamedKey{.qtKey = Qt::Key_Muhenkan, .key = u"NonConvert", .code = u"NonConvert"},
    NamedKey{.qtKey = Qt::Key_Henkan, .key = u"Convert", .code = u"Convert"},
    NamedKey{.qtKey = Qt::Key_Romaji, .key = u"Romaji", .code = u""},
    NamedKey{.qtKey = Qt::Key_Hiragana, .key = u"Hiragana", .code = u""},
    NamedKey{.qtKey = Qt::Key_Katakana, .key = u"Katakana", .code = u""},
    NamedKey{.qtKey = Qt::Key_Hiragana_Katakana, .key = u"HiraganaKatakana", .code = u"KanaMode"},
    NamedKey{.qtKey = Qt::Key_Zenkaku, .key = u"Zenkaku", .code = u""},
    NamedKey{.qtKey = Qt::Key_Hankaku, .key = u"Hankaku", .code = u""},
    NamedKey{.qtKey = Qt::Key_Zenkaku_Hankaku, .key = u"ZenkakuHankaku", .code = u""},
    NamedKey{.qtKey = Qt::Key_Kana_Lock, .key = u"KanaMode", .code = u""},
    NamedKey{.qtKey = Qt::Key_Kana_Shift, .key = u"KanaMode", .code = u""},
    NamedKey{.qtKey = Qt::Key_Eisu_Shift, .key = u"Alphanumeric", .code = u""},
    NamedKey{.qtKey = Qt::Key_Eisu_toggle, .key = u"Alphanumeric", .code = u""},
    NamedKey{.qtKey = Qt::Key_Hangul, .key = u"HangulMode", .code = u"Lang1"},
    NamedKey{.qtKey = Qt::Key_Hangul_Hanja, .key = u"HanjaMode", .code = u"Lang2"},
    NamedKey{.qtKey = Qt::Key_Codeinput, .key = u"CodeInput", .code = u""},
    NamedKey{.qtKey = Qt::Key_SingleCandidate, .key = u"SingleCandidate", .code = u""},
    NamedKey{.qtKey = Qt::Key_MultipleCandidate, .key = u"AllCandidates", .code = u""},
    NamedKey{.qtKey = Qt::Key_PreviousCandidate, .key = u"PreviousCandidate", .code = u""},
    NamedKey{.qtKey = Qt::Key_Mode_switch, .key = u"ModeChange", .code = u""},
    NamedKey{.qtKey = Qt::Key_VolumeUp, .key = u"AudioVolumeUp", .code = u"AudioVolumeUp"},
    NamedKey{.qtKey = Qt::Key_VolumeDown, .key = u"AudioVolumeDown", .code = u"AudioVolumeDown"},
    NamedKey{.qtKey = Qt::Key_VolumeMute, .key = u"AudioVolumeMute", .code = u"AudioVolumeMute"},
    NamedKey{.qtKey = Qt::Key_MediaTogglePlayPause, .key = u"MediaPlayPause", .code = u"MediaPlayPause"},
    NamedKey{.qtKey = Qt::Key_MediaStop, .key = u"MediaStop", .code = u"MediaStop"},
    NamedKey{.qtKey = Qt::Key_MediaPrevious, .key = u"MediaTrackPrevious", .code = u"MediaTrackPrevious"},
    NamedKey{.qtKey = Qt::Key_MediaNext, .key = u"MediaTrackNext", .code = u"MediaTrackNext"},
};

// Keys of the numeric keypad, when Qt marks them with Qt::KeypadModifier.
struct KeypadKey {
  int qtKey;
  const char16_t *code;
};

constexpr std::array kKeypadKeys{
    KeypadKey{.qtKey = Qt::Key_0, .code = u"Numpad0"},
    KeypadKey{.qtKey = Qt::Key_1, .code = u"Numpad1"},
    KeypadKey{.qtKey = Qt::Key_2, .code = u"Numpad2"},
    KeypadKey{.qtKey = Qt::Key_3, .code = u"Numpad3"},
    KeypadKey{.qtKey = Qt::Key_4, .code = u"Numpad4"},
    KeypadKey{.qtKey = Qt::Key_5, .code = u"Numpad5"},
    KeypadKey{.qtKey = Qt::Key_6, .code = u"Numpad6"},
    KeypadKey{.qtKey = Qt::Key_7, .code = u"Numpad7"},
    KeypadKey{.qtKey = Qt::Key_8, .code = u"Numpad8"},
    KeypadKey{.qtKey = Qt::Key_9, .code = u"Numpad9"},
    KeypadKey{.qtKey = Qt::Key_Plus, .code = u"NumpadAdd"},
    KeypadKey{.qtKey = Qt::Key_Minus, .code = u"NumpadSubtract"},
    KeypadKey{.qtKey = Qt::Key_Asterisk, .code = u"NumpadMultiply"},
    KeypadKey{.qtKey = Qt::Key_Slash, .code = u"NumpadDivide"},
    KeypadKey{.qtKey = Qt::Key_Period, .code = u"NumpadDecimal"},
    KeypadKey{.qtKey = Qt::Key_Comma, .code = u"NumpadDecimal"},
    KeypadKey{.qtKey = Qt::Key_Equal, .code = u"NumpadEqual"},
};

const NamedKey *namedKey(int qtKey) {
  for (const NamedKey &named : kNamedKeys) {
    if (named.qtKey == qtKey) {
      return &named;
    }
  }
  return nullptr;
}

bool isFunctionKey(int qtKey) { return qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24; }

QString functionKeyName(int qtKey) { return u"F%1"_s.arg(qtKey - Qt::Key_F1 + 1); }

bool isDeadKey(int qtKey) { return qtKey >= Qt::Key_Dead_Grave && qtKey <= Qt::Key_Dead_Longsolidusoverlay; }

// Whether text consists of characters that type something: valid UTF-16 (no unpaired surrogates) without control characters (C0, DEL, C1).
bool isPrintable(QStringView text) {
  if (text.isEmpty() || !text.isValidUtf16()) {
    return false;
  }
  return std::ranges::none_of(text.toUcs4(),
                              [](char32_t character) { return character < 0x20 || (character >= 0x7f && character <= 0x9f); });
}

// Whether the modifiers make a key press a shortcut rather than typing.
bool isShortcut(Qt::KeyboardModifiers modifiers, const KeyMappingOptions &options) {
  const bool control = modifiers.testFlag(Qt::ControlModifier);
  const bool meta = modifiers.testFlag(Qt::MetaModifier);
  const bool alt = modifiers.testFlag(Qt::AltModifier);
  if (options.commandIsControl) {
    // macOS: Command and Control make shortcuts; Option types special characters.
    return control || meta;
  }
  // The Windows key, and Alt alone (menu accelerators), make shortcuts; so does Control, unless Control+Alt is AltGr (Windows).
  return meta || (alt && !control) || (control && !(alt && options.altGraphIsControlAlt));
}

} // namespace

KeyMappingOptions KeyMappingOptions::forPlatform() {
  KeyMappingOptions options;
#ifdef Q_OS_MACOS
  options.commandIsControl = !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
#endif
#ifdef Q_OS_WIN
  options.altGraphIsControlAlt = true;
#endif
  return options;
}

protocol::Modifiers domModifiers(Qt::KeyboardModifiers modifiers, const KeyMappingOptions &options) {
  const bool control = modifiers.testFlag(Qt::ControlModifier);
  const bool meta = modifiers.testFlag(Qt::MetaModifier);
  return {.shift = modifiers.testFlag(Qt::ShiftModifier),
          .ctrl = options.commandIsControl ? meta : control,
          .alt = modifiers.testFlag(Qt::AltModifier),
          .meta = options.commandIsControl ? control : meta};
}

QString domKey(const QKeyEvent &event, const KeyMappingOptions &options) {
  const int qtKey = event.key();
  if (qtKey == Qt::Key_Control) {
    return options.commandIsControl ? u"Meta"_s : u"Control"_s;
  }
  if (qtKey == Qt::Key_Meta) {
    return options.commandIsControl ? u"Control"_s : u"Meta"_s;
  }
  if (const NamedKey *named = namedKey(qtKey)) {
    return QString::fromUtf16(named->key);
  }
  if (isFunctionKey(qtKey)) {
    return functionKeyName(qtKey);
  }
  if (isDeadKey(qtKey)) {
    return u"Dead"_s;
  }
  if (const QString text = event.text(); isPrintable(text)) {
    return text;
  }
  // With Control held, Qt's text is a control character; the DOM still names the character the key would type.
  if (qtKey >= 0x20 && qtKey < Qt::Key_Escape && (qtKey < 0xd800 || qtKey > 0xdfff)) {
    const auto codePoint = static_cast<char32_t>(qtKey);
    const QString character = QString::fromUcs4(&codePoint, 1);
    return event.modifiers().testFlag(Qt::ShiftModifier) ? character : character.toLower();
  }
  return u"Unidentified"_s;
}

QString domCode(const QKeyEvent &event) {
  const int qtKey = event.key();
  if (const NamedKey *named = namedKey(qtKey)) {
    // Qt::Key_Enter is the keypad's Enter key; Qt::Key_Return the main one.
    return QString::fromUtf16(named->code);
  }
  if (isFunctionKey(qtKey)) {
    return functionKeyName(qtKey);
  }
  if (qtKey == Qt::Key_Space) {
    return u"Space"_s;
  }
  if (event.modifiers().testFlag(Qt::KeypadModifier)) {
    for (const KeypadKey &keypad : kKeypadKeys) {
      if (keypad.qtKey == qtKey) {
        return QString::fromUtf16(keypad.code);
      }
    }
  }
  return {};
}

protocol::KeyInput keyInput(const QKeyEvent &event, bool composing, const KeyMappingOptions &options) {
  return {.key = domKey(event, options),
          .code = domCode(event),
          .modifiers = domModifiers(event.modifiers(), options),
          .repeat = event.isAutoRepeat(),
          .composing = composing};
}

std::optional<QString> typedText(const QKeyEvent &event, const KeyMappingOptions &options) {
  if (!isPrintable(event.text()) || isShortcut(event.modifiers(), options)) {
    return std::nullopt;
  }
  return event.text();
}

} // namespace bayan
