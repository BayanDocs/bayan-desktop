#include "keymapping.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringView>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <array>
#include <cstddef>
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

// The physical keys that the DOM names (UI Events KeyboardEvent code values), by each platform's native code for them. A key missing here
// gets its code from Qt's key, if its position does not depend on the layout (domCode).
using NativeKey = std::pair<unsigned, const char16_t *>;

// Windows: scan codes of set 1, as Qt 6 reports them, with 0xE000 added for the extended keys.
constexpr std::array kWindowsScanCodes{
    NativeKey{0x01U, u"Escape"},
    NativeKey{0x02U, u"Digit1"},
    NativeKey{0x03U, u"Digit2"},
    NativeKey{0x04U, u"Digit3"},
    NativeKey{0x05U, u"Digit4"},
    NativeKey{0x06U, u"Digit5"},
    NativeKey{0x07U, u"Digit6"},
    NativeKey{0x08U, u"Digit7"},
    NativeKey{0x09U, u"Digit8"},
    NativeKey{0x0AU, u"Digit9"},
    NativeKey{0x0BU, u"Digit0"},
    NativeKey{0x0CU, u"Minus"},
    NativeKey{0x0DU, u"Equal"},
    NativeKey{0x0EU, u"Backspace"},
    NativeKey{0x0FU, u"Tab"},
    NativeKey{0x10U, u"KeyQ"},
    NativeKey{0x11U, u"KeyW"},
    NativeKey{0x12U, u"KeyE"},
    NativeKey{0x13U, u"KeyR"},
    NativeKey{0x14U, u"KeyT"},
    NativeKey{0x15U, u"KeyY"},
    NativeKey{0x16U, u"KeyU"},
    NativeKey{0x17U, u"KeyI"},
    NativeKey{0x18U, u"KeyO"},
    NativeKey{0x19U, u"KeyP"},
    NativeKey{0x1AU, u"BracketLeft"},
    NativeKey{0x1BU, u"BracketRight"},
    NativeKey{0x1CU, u"Enter"},
    NativeKey{0x1DU, u"ControlLeft"},
    NativeKey{0x1EU, u"KeyA"},
    NativeKey{0x1FU, u"KeyS"},
    NativeKey{0x20U, u"KeyD"},
    NativeKey{0x21U, u"KeyF"},
    NativeKey{0x22U, u"KeyG"},
    NativeKey{0x23U, u"KeyH"},
    NativeKey{0x24U, u"KeyJ"},
    NativeKey{0x25U, u"KeyK"},
    NativeKey{0x26U, u"KeyL"},
    NativeKey{0x27U, u"Semicolon"},
    NativeKey{0x28U, u"Quote"},
    NativeKey{0x29U, u"Backquote"},
    NativeKey{0x2AU, u"ShiftLeft"},
    NativeKey{0x2BU, u"Backslash"},
    NativeKey{0x2CU, u"KeyZ"},
    NativeKey{0x2DU, u"KeyX"},
    NativeKey{0x2EU, u"KeyC"},
    NativeKey{0x2FU, u"KeyV"},
    NativeKey{0x30U, u"KeyB"},
    NativeKey{0x31U, u"KeyN"},
    NativeKey{0x32U, u"KeyM"},
    NativeKey{0x33U, u"Comma"},
    NativeKey{0x34U, u"Period"},
    NativeKey{0x35U, u"Slash"},
    NativeKey{0x36U, u"ShiftRight"},
    NativeKey{0x37U, u"NumpadMultiply"},
    NativeKey{0x38U, u"AltLeft"},
    NativeKey{0x39U, u"Space"},
    NativeKey{0x3AU, u"CapsLock"},
    NativeKey{0x3BU, u"F1"},
    NativeKey{0x3CU, u"F2"},
    NativeKey{0x3DU, u"F3"},
    NativeKey{0x3EU, u"F4"},
    NativeKey{0x3FU, u"F5"},
    NativeKey{0x40U, u"F6"},
    NativeKey{0x41U, u"F7"},
    NativeKey{0x42U, u"F8"},
    NativeKey{0x43U, u"F9"},
    NativeKey{0x44U, u"F10"},
    NativeKey{0x45U, u"Pause"},
    NativeKey{0x46U, u"ScrollLock"},
    NativeKey{0x47U, u"Numpad7"},
    NativeKey{0x48U, u"Numpad8"},
    NativeKey{0x49U, u"Numpad9"},
    NativeKey{0x4AU, u"NumpadSubtract"},
    NativeKey{0x4BU, u"Numpad4"},
    NativeKey{0x4CU, u"Numpad5"},
    NativeKey{0x4DU, u"Numpad6"},
    NativeKey{0x4EU, u"NumpadAdd"},
    NativeKey{0x4FU, u"Numpad1"},
    NativeKey{0x50U, u"Numpad2"},
    NativeKey{0x51U, u"Numpad3"},
    NativeKey{0x52U, u"Numpad0"},
    NativeKey{0x53U, u"NumpadDecimal"},
    NativeKey{0x56U, u"IntlBackslash"},
    NativeKey{0x57U, u"F11"},
    NativeKey{0x58U, u"F12"},
    NativeKey{0x59U, u"NumpadEqual"},
    NativeKey{0x64U, u"F13"},
    NativeKey{0x65U, u"F14"},
    NativeKey{0x66U, u"F15"},
    NativeKey{0x67U, u"F16"},
    NativeKey{0x68U, u"F17"},
    NativeKey{0x69U, u"F18"},
    NativeKey{0x6AU, u"F19"},
    NativeKey{0x6BU, u"F20"},
    NativeKey{0x6CU, u"F21"},
    NativeKey{0x6DU, u"F22"},
    NativeKey{0x6EU, u"F23"},
    NativeKey{0x70U, u"KanaMode"},
    NativeKey{0x71U, u"Lang2"},
    NativeKey{0x72U, u"Lang1"},
    NativeKey{0x73U, u"IntlRo"},
    NativeKey{0x76U, u"F24"},
    NativeKey{0x79U, u"Convert"},
    NativeKey{0x7BU, u"NonConvert"},
    NativeKey{0x7DU, u"IntlYen"},
    NativeKey{0x7EU, u"NumpadComma"},
    NativeKey{0xE01CU, u"NumpadEnter"},
    NativeKey{0xE01DU, u"ControlRight"},
    NativeKey{0xE035U, u"NumpadDivide"},
    NativeKey{0xE037U, u"PrintScreen"},
    NativeKey{0xE038U, u"AltRight"},
    NativeKey{0xE045U, u"NumLock"},
    NativeKey{0xE047U, u"Home"},
    NativeKey{0xE048U, u"ArrowUp"},
    NativeKey{0xE049U, u"PageUp"},
    NativeKey{0xE04BU, u"ArrowLeft"},
    NativeKey{0xE04DU, u"ArrowRight"},
    NativeKey{0xE04FU, u"End"},
    NativeKey{0xE050U, u"ArrowDown"},
    NativeKey{0xE051U, u"PageDown"},
    NativeKey{0xE052U, u"Insert"},
    NativeKey{0xE053U, u"Delete"},
    NativeKey{0xE05BU, u"MetaLeft"},
    NativeKey{0xE05CU, u"MetaRight"},
    NativeKey{0xE05DU, u"ContextMenu"},
};

// macOS: virtual key codes (Carbon's kVK_ constants, which NSEvent's keyCode carries).
constexpr std::array kMacVirtualKeys{
    NativeKey{0x00U, u"KeyA"},
    NativeKey{0x01U, u"KeyS"},
    NativeKey{0x02U, u"KeyD"},
    NativeKey{0x03U, u"KeyF"},
    NativeKey{0x04U, u"KeyH"},
    NativeKey{0x05U, u"KeyG"},
    NativeKey{0x06U, u"KeyZ"},
    NativeKey{0x07U, u"KeyX"},
    NativeKey{0x08U, u"KeyC"},
    NativeKey{0x09U, u"KeyV"},
    NativeKey{0x0AU, u"IntlBackslash"},
    NativeKey{0x0BU, u"KeyB"},
    NativeKey{0x0CU, u"KeyQ"},
    NativeKey{0x0DU, u"KeyW"},
    NativeKey{0x0EU, u"KeyE"},
    NativeKey{0x0FU, u"KeyR"},
    NativeKey{0x10U, u"KeyY"},
    NativeKey{0x11U, u"KeyT"},
    NativeKey{0x12U, u"Digit1"},
    NativeKey{0x13U, u"Digit2"},
    NativeKey{0x14U, u"Digit3"},
    NativeKey{0x15U, u"Digit4"},
    NativeKey{0x16U, u"Digit6"},
    NativeKey{0x17U, u"Digit5"},
    NativeKey{0x18U, u"Equal"},
    NativeKey{0x19U, u"Digit9"},
    NativeKey{0x1AU, u"Digit7"},
    NativeKey{0x1BU, u"Minus"},
    NativeKey{0x1CU, u"Digit8"},
    NativeKey{0x1DU, u"Digit0"},
    NativeKey{0x1EU, u"BracketRight"},
    NativeKey{0x1FU, u"KeyO"},
    NativeKey{0x20U, u"KeyU"},
    NativeKey{0x21U, u"BracketLeft"},
    NativeKey{0x22U, u"KeyI"},
    NativeKey{0x23U, u"KeyP"},
    NativeKey{0x24U, u"Enter"},
    NativeKey{0x25U, u"KeyL"},
    NativeKey{0x26U, u"KeyJ"},
    NativeKey{0x27U, u"Quote"},
    NativeKey{0x28U, u"KeyK"},
    NativeKey{0x29U, u"Semicolon"},
    NativeKey{0x2AU, u"Backslash"},
    NativeKey{0x2BU, u"Comma"},
    NativeKey{0x2CU, u"Slash"},
    NativeKey{0x2DU, u"KeyN"},
    NativeKey{0x2EU, u"KeyM"},
    NativeKey{0x2FU, u"Period"},
    NativeKey{0x30U, u"Tab"},
    NativeKey{0x31U, u"Space"},
    NativeKey{0x32U, u"Backquote"},
    NativeKey{0x33U, u"Backspace"},
    NativeKey{0x35U, u"Escape"},
    NativeKey{0x36U, u"MetaRight"},
    NativeKey{0x37U, u"MetaLeft"},
    NativeKey{0x38U, u"ShiftLeft"},
    NativeKey{0x39U, u"CapsLock"},
    NativeKey{0x3AU, u"AltLeft"},
    NativeKey{0x3BU, u"ControlLeft"},
    NativeKey{0x3CU, u"ShiftRight"},
    NativeKey{0x3DU, u"AltRight"},
    NativeKey{0x3EU, u"ControlRight"},
    NativeKey{0x40U, u"F17"},
    NativeKey{0x41U, u"NumpadDecimal"},
    NativeKey{0x43U, u"NumpadMultiply"},
    NativeKey{0x45U, u"NumpadAdd"},
    NativeKey{0x47U, u"NumLock"},
    NativeKey{0x4BU, u"NumpadDivide"},
    NativeKey{0x4CU, u"NumpadEnter"},
    NativeKey{0x4EU, u"NumpadSubtract"},
    NativeKey{0x4FU, u"F18"},
    NativeKey{0x50U, u"F19"},
    NativeKey{0x51U, u"NumpadEqual"},
    NativeKey{0x52U, u"Numpad0"},
    NativeKey{0x53U, u"Numpad1"},
    NativeKey{0x54U, u"Numpad2"},
    NativeKey{0x55U, u"Numpad3"},
    NativeKey{0x56U, u"Numpad4"},
    NativeKey{0x57U, u"Numpad5"},
    NativeKey{0x58U, u"Numpad6"},
    NativeKey{0x59U, u"Numpad7"},
    NativeKey{0x5AU, u"F20"},
    NativeKey{0x5BU, u"Numpad8"},
    NativeKey{0x5CU, u"Numpad9"},
    NativeKey{0x5DU, u"IntlYen"},
    NativeKey{0x5EU, u"IntlRo"},
    NativeKey{0x5FU, u"NumpadComma"},
    NativeKey{0x60U, u"F5"},
    NativeKey{0x61U, u"F6"},
    NativeKey{0x62U, u"F7"},
    NativeKey{0x63U, u"F3"},
    NativeKey{0x64U, u"F8"},
    NativeKey{0x65U, u"F9"},
    NativeKey{0x66U, u"Lang2"},
    NativeKey{0x67U, u"F11"},
    NativeKey{0x68U, u"Lang1"},
    NativeKey{0x69U, u"F13"},
    NativeKey{0x6AU, u"F16"},
    NativeKey{0x6BU, u"F14"},
    NativeKey{0x6DU, u"F10"},
    NativeKey{0x6EU, u"ContextMenu"},
    NativeKey{0x6FU, u"F12"},
    NativeKey{0x71U, u"F15"},
    NativeKey{0x72U, u"Insert"},
    NativeKey{0x73U, u"Home"},
    NativeKey{0x74U, u"PageUp"},
    NativeKey{0x75U, u"Delete"},
    NativeKey{0x76U, u"F4"},
    NativeKey{0x77U, u"End"},
    NativeKey{0x78U, u"F2"},
    NativeKey{0x79U, u"PageDown"},
    NativeKey{0x7AU, u"F1"},
    NativeKey{0x7BU, u"ArrowLeft"},
    NativeKey{0x7CU, u"ArrowRight"},
    NativeKey{0x7DU, u"ArrowDown"},
    NativeKey{0x7EU, u"ArrowUp"},
};

// Linux: the kernel's evdev key codes (linux/input-event-codes.h); XKB keycodes, which Qt reports on X11 and Wayland, are these plus 8.
constexpr unsigned kXkbKeycodeOffset = 8;
constexpr std::array kEvdevCodes{
    NativeKey{1U, u"Escape"},
    NativeKey{2U, u"Digit1"},
    NativeKey{3U, u"Digit2"},
    NativeKey{4U, u"Digit3"},
    NativeKey{5U, u"Digit4"},
    NativeKey{6U, u"Digit5"},
    NativeKey{7U, u"Digit6"},
    NativeKey{8U, u"Digit7"},
    NativeKey{9U, u"Digit8"},
    NativeKey{10U, u"Digit9"},
    NativeKey{11U, u"Digit0"},
    NativeKey{12U, u"Minus"},
    NativeKey{13U, u"Equal"},
    NativeKey{14U, u"Backspace"},
    NativeKey{15U, u"Tab"},
    NativeKey{16U, u"KeyQ"},
    NativeKey{17U, u"KeyW"},
    NativeKey{18U, u"KeyE"},
    NativeKey{19U, u"KeyR"},
    NativeKey{20U, u"KeyT"},
    NativeKey{21U, u"KeyY"},
    NativeKey{22U, u"KeyU"},
    NativeKey{23U, u"KeyI"},
    NativeKey{24U, u"KeyO"},
    NativeKey{25U, u"KeyP"},
    NativeKey{26U, u"BracketLeft"},
    NativeKey{27U, u"BracketRight"},
    NativeKey{28U, u"Enter"},
    NativeKey{29U, u"ControlLeft"},
    NativeKey{30U, u"KeyA"},
    NativeKey{31U, u"KeyS"},
    NativeKey{32U, u"KeyD"},
    NativeKey{33U, u"KeyF"},
    NativeKey{34U, u"KeyG"},
    NativeKey{35U, u"KeyH"},
    NativeKey{36U, u"KeyJ"},
    NativeKey{37U, u"KeyK"},
    NativeKey{38U, u"KeyL"},
    NativeKey{39U, u"Semicolon"},
    NativeKey{40U, u"Quote"},
    NativeKey{41U, u"Backquote"},
    NativeKey{42U, u"ShiftLeft"},
    NativeKey{43U, u"Backslash"},
    NativeKey{44U, u"KeyZ"},
    NativeKey{45U, u"KeyX"},
    NativeKey{46U, u"KeyC"},
    NativeKey{47U, u"KeyV"},
    NativeKey{48U, u"KeyB"},
    NativeKey{49U, u"KeyN"},
    NativeKey{50U, u"KeyM"},
    NativeKey{51U, u"Comma"},
    NativeKey{52U, u"Period"},
    NativeKey{53U, u"Slash"},
    NativeKey{54U, u"ShiftRight"},
    NativeKey{55U, u"NumpadMultiply"},
    NativeKey{56U, u"AltLeft"},
    NativeKey{57U, u"Space"},
    NativeKey{58U, u"CapsLock"},
    NativeKey{59U, u"F1"},
    NativeKey{60U, u"F2"},
    NativeKey{61U, u"F3"},
    NativeKey{62U, u"F4"},
    NativeKey{63U, u"F5"},
    NativeKey{64U, u"F6"},
    NativeKey{65U, u"F7"},
    NativeKey{66U, u"F8"},
    NativeKey{67U, u"F9"},
    NativeKey{68U, u"F10"},
    NativeKey{69U, u"NumLock"},
    NativeKey{70U, u"ScrollLock"},
    NativeKey{71U, u"Numpad7"},
    NativeKey{72U, u"Numpad8"},
    NativeKey{73U, u"Numpad9"},
    NativeKey{74U, u"NumpadSubtract"},
    NativeKey{75U, u"Numpad4"},
    NativeKey{76U, u"Numpad5"},
    NativeKey{77U, u"Numpad6"},
    NativeKey{78U, u"NumpadAdd"},
    NativeKey{79U, u"Numpad1"},
    NativeKey{80U, u"Numpad2"},
    NativeKey{81U, u"Numpad3"},
    NativeKey{82U, u"Numpad0"},
    NativeKey{83U, u"NumpadDecimal"},
    NativeKey{86U, u"IntlBackslash"},
    NativeKey{87U, u"F11"},
    NativeKey{88U, u"F12"},
    NativeKey{89U, u"IntlRo"},
    NativeKey{90U, u"Lang3"},
    NativeKey{91U, u"Lang4"},
    NativeKey{92U, u"Convert"},
    NativeKey{93U, u"KanaMode"},
    NativeKey{94U, u"NonConvert"},
    NativeKey{96U, u"NumpadEnter"},
    NativeKey{97U, u"ControlRight"},
    NativeKey{98U, u"NumpadDivide"},
    NativeKey{99U, u"PrintScreen"},
    NativeKey{100U, u"AltRight"},
    NativeKey{102U, u"Home"},
    NativeKey{103U, u"ArrowUp"},
    NativeKey{104U, u"PageUp"},
    NativeKey{105U, u"ArrowLeft"},
    NativeKey{106U, u"ArrowRight"},
    NativeKey{107U, u"End"},
    NativeKey{108U, u"ArrowDown"},
    NativeKey{109U, u"PageDown"},
    NativeKey{110U, u"Insert"},
    NativeKey{111U, u"Delete"},
    NativeKey{117U, u"NumpadEqual"},
    NativeKey{119U, u"Pause"},
    NativeKey{121U, u"NumpadComma"},
    NativeKey{122U, u"Lang1"},
    NativeKey{123U, u"Lang2"},
    NativeKey{124U, u"IntlYen"},
    NativeKey{125U, u"MetaLeft"},
    NativeKey{126U, u"MetaRight"},
    NativeKey{127U, u"ContextMenu"},
    NativeKey{183U, u"F13"},
    NativeKey{184U, u"F14"},
    NativeKey{185U, u"F15"},
    NativeKey{186U, u"F16"},
    NativeKey{187U, u"F17"},
    NativeKey{188U, u"F18"},
    NativeKey{189U, u"F19"},
    NativeKey{190U, u"F20"},
    NativeKey{191U, u"F21"},
    NativeKey{192U, u"F22"},
    NativeKey{193U, u"F23"},
    NativeKey{194U, u"F24"},
};

template <std::size_t Size> QString lookUp(const std::array<NativeKey, Size> &table, unsigned nativeCode) {
  const auto found = std::ranges::find(table, nativeCode, &NativeKey::first);
  return found == table.end() ? QString() : QString::fromUtf16(found->second);
}

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
  // The native key codes come from Qt's platform plugin, not from the operating system alone: the offscreen platform of the tests reports
  // none, and on Linux only the X11 and Wayland plugins report XKB keycodes.
  const QString platform = QGuiApplication::platformName();
  if (platform == u"windows"_s) {
    options.nativeKeyCodes = NativeKeyCodes::WindowsScanCodes;
  } else if (platform == u"cocoa"_s) {
    options.nativeKeyCodes = NativeKeyCodes::MacVirtualKeys;
  } else if (platform == u"xcb"_s || platform.startsWith(u"wayland"_s)) {
    options.nativeKeyCodes = NativeKeyCodes::XkbKeycodes;
  }
  return options;
}

protocol::Modifiers domModifiers(Qt::KeyboardModifiers modifiers, const KeyMappingOptions &options) {
  const bool control = modifiers.testFlag(Qt::ControlModifier);
  const bool meta = modifiers.testFlag(Qt::MetaModifier);
  return {
      .shift = modifiers.testFlag(Qt::ShiftModifier),
      .ctrl = options.commandIsControl ? meta : control,
      .alt = modifiers.testFlag(Qt::AltModifier),
      .meta = options.commandIsControl ? control : meta,
  };
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

QString domCodeForNativeKey(NativeKeyCodes kind, quint32 nativeCode) {
  switch (kind) {
  case NativeKeyCodes::WindowsScanCodes:
    return nativeCode == 0 ? QString() : lookUp(kWindowsScanCodes, nativeCode);
  case NativeKeyCodes::MacVirtualKeys:
    // 0 is the A key's code on macOS.
    return lookUp(kMacVirtualKeys, nativeCode);
  case NativeKeyCodes::XkbKeycodes:
    return nativeCode <= kXkbKeycodeOffset ? QString() : lookUp(kEvdevCodes, nativeCode - kXkbKeycodeOffset);
  case NativeKeyCodes::None:
    break;
  }
  return {};
}

QString domCode(const QKeyEvent &event, const KeyMappingOptions &options) {
  const quint32 nativeCode = options.nativeKeyCodes == NativeKeyCodes::MacVirtualKeys ? event.nativeVirtualKey() : event.nativeScanCode();
  if (QString code = domCodeForNativeKey(options.nativeKeyCodes, nativeCode); !code.isEmpty()) {
    return code;
  }
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
  return {
      .key = domKey(event, options),
      .code = domCode(event, options),
      .modifiers = domModifiers(event.modifiers(), options),
      .repeat = event.isAutoRepeat(),
      .composing = composing,
  };
}

std::optional<QString> typedText(const QKeyEvent &event, const KeyMappingOptions &options) {
  if (!isPrintable(event.text()) || isShortcut(event.modifiers(), options)) {
    return std::nullopt;
  }
  return event.text();
}

} // namespace bayan
