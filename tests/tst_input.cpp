// Unit tests for the canvas's input translation (src/canvas/keymapping.h, keybindings.h, imetranslator.h): Qt's key events become the DOM
// key and code values of input.key and the text of input.text, with code from each platform's native key codes (Windows' scan codes,
// macOS's virtual key codes, the XKB keycodes of X11 and Wayland) for presses from US, French, German, Russian and Arabic layouts and with
// AltGr; the matching rule of engine protocol §14.3 decides which presses go to the engine (the conformance vectors of keys.json are run by
// tst_conformance); and the input-method events that Windows, macOS and Linux send become input.composition and input.text. The key events
// and input-method sequences replay what each platform's Qt integration sends (qwindowskeymapper.cpp, qnsview_keys.mm, qxkbcommon.cpp,
// qwindowsinputcontext.cpp, qnsview_complextext.mm, qibusplatforminputcontext.cpp in Qt 6.12).

#include "imetranslator.h"
#include "keybindings.h"
#include "keymapping.h"

#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextCharFormat>
#include <QtTest/QTest>

#include <limits>

using namespace Qt::StringLiterals;
using namespace bayan;
using Kind = ImeAction::Kind;

namespace {

// The options of each platform for events without native key codes, as the tests' offscreen platform delivers them, and with them, as
// Qt's windows, cocoa and xcb or wayland plugins deliver them.
constexpr KeyMappingOptions kLinux{};
constexpr KeyMappingOptions kWindows{.commandIsControl = false, .altGraphIsControlAlt = true};
constexpr KeyMappingOptions kMacOS{.commandIsControl = true, .altGraphIsControlAlt = false};
constexpr KeyMappingOptions kNativeLinux{
    .commandIsControl = false,
    .altGraphIsControlAlt = false,
    .nativeKeyCodes = NativeKeyCodes::XkbKeycodes,
};
constexpr KeyMappingOptions kNativeWindows{
    .commandIsControl = false,
    .altGraphIsControlAlt = true,
    .nativeKeyCodes = NativeKeyCodes::WindowsScanCodes,
};
constexpr KeyMappingOptions kNativeMacOS{
    .commandIsControl = true,
    .altGraphIsControlAlt = false,
    .nativeKeyCodes = NativeKeyCodes::MacVirtualKeys,
};

QKeyEvent press(int key, Qt::KeyboardModifiers modifiers, const QString &text = {}, bool autoRepeat = false) {
  return {QEvent::KeyPress, key, modifiers, text, autoRepeat};
}

// A press as a platform's Qt plugin delivers it: Windows and X11 or Wayland fill the native scan code, macOS the virtual key.
QKeyEvent nativePress(int key, Qt::KeyboardModifiers modifiers, const QString &text, NativeKeyCodes platform, quint32 nativeCode) {
  const bool mac = platform == NativeKeyCodes::MacVirtualKeys;
  return {QEvent::KeyPress, key, modifiers, mac ? 0U : nativeCode, mac ? nativeCode : 0U, 0U, text};
}

// A key entry of the UI manifest (engine protocol §14.3), and the press of input.key.
protocol::KeyEntry binding(const QString &key, bool ctrl, bool shift = false, bool alt = false, bool meta = false) {
  return {.key = key, .code = {}, .ctrl = ctrl, .shift = shift, .alt = alt, .meta = meta, .label = u"label of "_s + key};
}

protocol::KeyInput keyPress(const QString &key, const QString &code, protocol::Modifiers modifiers = protocol::Modifiers(),
                            bool composing = false) {
  return {.key = key, .code = code, .modifiers = modifiers, .repeat = false, .composing = composing};
}

// The attributes of a pre-edit text: a range with a background colour (the selected clause on Windows, the selected range on macOS), an
// underline over the whole text (Linux), and the input method's cursor.
QInputMethodEvent::Attribute background(int start, int length) {
  QTextCharFormat format;
  format.setBackground(Qt::darkBlue);
  return {QInputMethodEvent::TextFormat, start, length, format};
}

QInputMethodEvent::Attribute underline(int start, int length) {
  QTextCharFormat format;
  format.setFontUnderline(true);
  return {QInputMethodEvent::TextFormat, start, length, format};
}

QInputMethodEvent::Attribute cursor(int position) { return {QInputMethodEvent::Cursor, position, 1, QVariant()}; }

QInputMethodEvent preedit(const QString &text, const QList<QInputMethodEvent::Attribute> &attributes) { return {text, attributes}; }

// Translates an event that commits text, and may replace text around the caret (events cannot be returned from a function: Qt forbids
// moving them).
std::vector<ImeAction> translateCommit(ImeTranslator &ime, const QString &text, int replaceFrom = 0, int replaceLength = 0) {
  QInputMethodEvent event;
  event.setCommitString(text, replaceFrom, replaceLength);
  return ime.translate(event);
}

ImeAction update(const QString &text, int start, int end) {
  return {.kind = Kind::Update, .text = text, .selectionStart = start, .selectionEnd = end};
}

} // namespace

// Lets QTest print lists of actions when a comparison fails (found by argument-dependent lookup).
namespace bayan {
char *toString(const std::vector<ImeAction> &actions) {
  QString text;
  for (const ImeAction &action : actions) {
    text += u"[%1 \"%2\" %3-%4 %5] "_s.arg(static_cast<int>(action.kind))
                .arg(action.text)
                .arg(action.selectionStart)
                .arg(action.selectionEnd)
                .arg(action.length);
  }
  return qstrdup(text.toUtf8().constData());
}
} // namespace bayan

class TestInput final : public QObject {
  Q_OBJECT

private slots:
  void lettersTypeTheirText();
  void namedKeysHaveDomNames();
  void shortcutsTypeNothing();
  void macOSSwapsCommandAndControlBack();
  void altGraphTypesOnWindows();
  void keypadAndInputMethodKeys();
  void keyInputCarriesRepeatAndComposition();
  void nativeKeyCodesNameThePhysicalKey();
  void pressesFromOtherLayoutsKeepTheirPosition();
  void bindingsMatchAsTheProtocolSays();
  void reservedKeysAndShellActionsStayWithTheShell();
  void japaneseOnWindowsConvertsAClause();
  void chineseOnMacOSMarksTheSelectedRange();
  void linuxUnderlinesAndShowsTheCursor();
  void koreanCommitsAndStartsTheNextSyllableInOneEvent();
  void escapeCancelsTheComposition();
  void accentMenuReplacesTheCharacterBeforeTheCaret();
  void deleteSurroundingTextBecomesDeletions();
  void replacingDuringACompositionEndsItFirst();
  void selectionNeverSplitsASurrogatePair();
};

void TestInput::lettersTypeTheirText() {
  const QKeyEvent a = press(Qt::Key_A, Qt::NoModifier, u"a"_s);
  QCOMPARE(domKey(a, kLinux), u"a"_s);
  QCOMPARE(domCode(a, kLinux), QString()); // without a native key code, the physical key of a letter is not known
  QCOMPARE(typedText(a, kLinux), std::optional<QString>(u"a"_s));
  const QKeyEvent shifted = press(Qt::Key_A, Qt::ShiftModifier, u"A"_s);
  QCOMPARE(domKey(shifted, kLinux), u"A"_s);
  QCOMPARE(typedText(shifted, kLinux), std::optional<QString>(u"A"_s));
  QVERIFY(domModifiers(Qt::ShiftModifier, kLinux).shift);
  // Non-Latin layouts and characters outside the Basic Multilingual Plane.
  const QKeyEvent cyrillic = press(0x0436, Qt::NoModifier, u"ж"_s);
  QCOMPARE(domKey(cyrillic, kLinux), u"ж"_s);
  const QKeyEvent emoji = press(Qt::Key_unknown, Qt::NoModifier, u"😀"_s);
  QCOMPARE(domKey(emoji, kLinux), u"😀"_s);
  QCOMPARE(typedText(emoji, kLinux), std::optional<QString>(u"😀"_s));
  const QKeyEvent space = press(Qt::Key_Space, Qt::NoModifier, u" "_s);
  QCOMPARE(domKey(space, kLinux), u" "_s);
  QCOMPARE(domCode(space, kLinux), u"Space"_s);
}

void TestInput::namedKeysHaveDomNames() {
  struct Case {
    int key;
    QString text;
    QString domKeyName;
    QString domCodeName;
  };
  const QList<Case> cases{
      {.key = Qt::Key_Backspace, .text = u"\b"_s, .domKeyName = u"Backspace"_s, .domCodeName = u"Backspace"_s},
      {.key = Qt::Key_Return, .text = u"\r"_s, .domKeyName = u"Enter"_s, .domCodeName = u"Enter"_s},
      {.key = Qt::Key_Enter, .text = u"\r"_s, .domKeyName = u"Enter"_s, .domCodeName = u"NumpadEnter"_s},
      {.key = Qt::Key_Escape, .text = u"\x1b"_s, .domKeyName = u"Escape"_s, .domCodeName = u"Escape"_s},
      {.key = Qt::Key_Delete, .text = u"\x7f"_s, .domKeyName = u"Delete"_s, .domCodeName = u"Delete"_s},
      {.key = Qt::Key_Left, .text = QString(), .domKeyName = u"ArrowLeft"_s, .domCodeName = u"ArrowLeft"_s},
      {.key = Qt::Key_PageDown, .text = QString(), .domKeyName = u"PageDown"_s, .domCodeName = u"PageDown"_s},
      {.key = Qt::Key_F5, .text = QString(), .domKeyName = u"F5"_s, .domCodeName = u"F5"_s},
      {.key = Qt::Key_F24, .text = QString(), .domKeyName = u"F24"_s, .domCodeName = u"F24"_s},
      {.key = Qt::Key_Dead_Acute, .text = QString(), .domKeyName = u"Dead"_s, .domCodeName = QString()},
      {.key = Qt::Key_unknown, .text = QString(), .domKeyName = u"Unidentified"_s, .domCodeName = QString()},
  };
  for (const Case &item : cases) {
    const QKeyEvent event = press(item.key, Qt::NoModifier, item.text);
    QCOMPARE(domKey(event, kLinux), item.domKeyName);
    QCOMPARE(domCode(event, kLinux), item.domCodeName);
    // Control characters are keys, never text.
    QCOMPARE(typedText(event, kLinux), std::nullopt);
  }
}

void TestInput::shortcutsTypeNothing() {
  // Control+A: Qt's text is the control character U+0001, the DOM key is still "a".
  const QKeyEvent controlA = press(Qt::Key_A, Qt::ControlModifier, u"\x01"_s);
  QCOMPARE(domKey(controlA, kLinux), u"a"_s);
  QVERIFY(domModifiers(controlA.modifiers(), kLinux).ctrl);
  QCOMPARE(typedText(controlA, kLinux), std::nullopt);
  QCOMPARE(typedText(press(Qt::Key_S, Qt::ControlModifier, u"s"_s), kWindows), std::nullopt);
  // Alt alone opens menus on Windows and Linux; the Windows key makes system shortcuts.
  QCOMPARE(typedText(press(Qt::Key_F, Qt::AltModifier, u"f"_s), kWindows), std::nullopt);
  QCOMPARE(typedText(press(Qt::Key_E, Qt::MetaModifier, u"e"_s), kLinux), std::nullopt);
  // Control+Shift+Z keeps its capital in the DOM.
  QCOMPARE(domKey(press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, u"\x1a"_s), kLinux), u"Z"_s);
}

void TestInput::macOSSwapsCommandAndControlBack() {
  // By default Qt reports Command as Control and Control as Meta on macOS; the DOM calls Command "meta".
  const protocol::Modifiers command = domModifiers(Qt::ControlModifier, kMacOS);
  QVERIFY(command.meta && !command.ctrl);
  const protocol::Modifiers control = domModifiers(Qt::MetaModifier, kMacOS);
  QVERIFY(control.ctrl && !control.meta);
  QCOMPARE(domKey(press(Qt::Key_Control, Qt::ControlModifier), kMacOS), u"Meta"_s);
  QCOMPARE(domKey(press(Qt::Key_Meta, Qt::MetaModifier), kMacOS), u"Control"_s);
  QCOMPARE(domKey(press(Qt::Key_Control, Qt::ControlModifier), kLinux), u"Control"_s);
  // Command+C types nothing; Option+E types the dead key's accent, Option+G "©".
  QCOMPARE(typedText(press(Qt::Key_C, Qt::ControlModifier, u"c"_s), kMacOS), std::nullopt);
  QCOMPARE(typedText(press(Qt::Key_G, Qt::AltModifier, u"©"_s), kMacOS), std::optional<QString>(u"©"_s));
}

void TestInput::altGraphTypesOnWindows() {
  // Windows reports AltGr as Control+Alt: AltGr+Q types "@" on a German keyboard.
  const QKeyEvent altGrQ = press(Qt::Key_At, Qt::ControlModifier | Qt::AltModifier, u"@"_s);
  QCOMPARE(typedText(altGrQ, kWindows), std::optional<QString>(u"@"_s));
  QCOMPARE(domKey(altGrQ, kWindows), u"@"_s);
  // Elsewhere, Control+Alt is a shortcut.
  QCOMPARE(typedText(altGrQ, kLinux), std::nullopt);
}

void TestInput::keypadAndInputMethodKeys() {
  QCOMPARE(domCode(press(Qt::Key_5, Qt::KeypadModifier, u"5"_s), kLinux), u"Numpad5"_s);
  QCOMPARE(domCode(press(Qt::Key_5, Qt::NoModifier, u"5"_s), kLinux), QString());
  QCOMPARE(domCode(press(Qt::Key_Plus, Qt::KeypadModifier, u"+"_s), kLinux), u"NumpadAdd"_s);
  QCOMPARE(domKey(press(Qt::Key_Henkan, Qt::NoModifier), kWindows), u"Convert"_s);
  QCOMPARE(domKey(press(Qt::Key_Hangul, Qt::NoModifier), kLinux), u"HangulMode"_s);
  QCOMPARE(domKey(press(Qt::Key_Zenkaku_Hankaku, Qt::NoModifier), kWindows), u"ZenkakuHankaku"_s);
}

void TestInput::keyInputCarriesRepeatAndComposition() {
  const protocol::KeyInput input = keyInput(press(Qt::Key_Backspace, Qt::NoModifier, u"\b"_s, true), true, kLinux);
  QCOMPARE(input, (protocol::KeyInput{.key = u"Backspace"_s, .code = u"Backspace"_s, .modifiers = {}, .repeat = true, .composing = true}));
}

void TestInput::nativeKeyCodesNameThePhysicalKey() {
  // Each platform's code for the same physical keys: letters and digits, whose characters depend on the layout, and keys whose names do
  // not.
  struct Case {
    QString code;
    quint32 windows;
    quint32 mac;
    quint32 evdev;
  };
  const QList<Case> cases{
      {.code = u"KeyA"_s, .windows = 0x1E, .mac = 0x00, .evdev = 30},
      {.code = u"KeyQ"_s, .windows = 0x10, .mac = 0x0C, .evdev = 16},
      {.code = u"KeyW"_s, .windows = 0x11, .mac = 0x0D, .evdev = 17},
      {.code = u"KeyZ"_s, .windows = 0x2C, .mac = 0x06, .evdev = 44},
      {.code = u"KeyY"_s, .windows = 0x15, .mac = 0x10, .evdev = 21},
      {.code = u"KeyM"_s, .windows = 0x32, .mac = 0x2E, .evdev = 50},
      {.code = u"Digit1"_s, .windows = 0x02, .mac = 0x12, .evdev = 2},
      {.code = u"Digit0"_s, .windows = 0x0B, .mac = 0x1D, .evdev = 11},
      {.code = u"Digit5"_s, .windows = 0x06, .mac = 0x17, .evdev = 6},
      {.code = u"Minus"_s, .windows = 0x0C, .mac = 0x1B, .evdev = 12},
      {.code = u"Equal"_s, .windows = 0x0D, .mac = 0x18, .evdev = 13},
      {.code = u"Semicolon"_s, .windows = 0x27, .mac = 0x29, .evdev = 39},
      {.code = u"IntlBackslash"_s, .windows = 0x56, .mac = 0x0A, .evdev = 86},
      {.code = u"Space"_s, .windows = 0x39, .mac = 0x31, .evdev = 57},
      {.code = u"Enter"_s, .windows = 0x1C, .mac = 0x24, .evdev = 28},
      {.code = u"NumpadEnter"_s, .windows = 0xE01C, .mac = 0x4C, .evdev = 96},
      {.code = u"ArrowLeft"_s, .windows = 0xE04B, .mac = 0x7B, .evdev = 105},
      {.code = u"Delete"_s, .windows = 0xE053, .mac = 0x75, .evdev = 111},
      {.code = u"ControlRight"_s, .windows = 0xE01D, .mac = 0x3E, .evdev = 97},
      {.code = u"F7"_s, .windows = 0x41, .mac = 0x62, .evdev = 65},
      {.code = u"F12"_s, .windows = 0x58, .mac = 0x6F, .evdev = 88},
      {.code = u"Numpad7"_s, .windows = 0x47, .mac = 0x59, .evdev = 71},
      {.code = u"ContextMenu"_s, .windows = 0xE05D, .mac = 0x6E, .evdev = 127},
      {.code = u"MetaLeft"_s, .windows = 0xE05B, .mac = 0x37, .evdev = 125},
  };
  constexpr quint32 kXkbOffset = 8;
  for (const Case &item : cases) {
    QCOMPARE(domCodeForNativeKey(NativeKeyCodes::WindowsScanCodes, item.windows), item.code);
    QCOMPARE(domCodeForNativeKey(NativeKeyCodes::MacVirtualKeys, item.mac), item.code);
    QCOMPARE(domCodeForNativeKey(NativeKeyCodes::XkbKeycodes, item.evdev + kXkbOffset), item.code);
  }
  // Codes that name no key the DOM names, and the absent code of an event without native information, give no code; the numeric keypad's 7
  // is not the 7 of the digit row, and an extended key is not its unextended twin.
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::WindowsScanCodes, 0), QString());
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::WindowsScanCodes, 0xE04C), QString());
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::WindowsScanCodes, 0x4B), u"Numpad4"_s);
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::XkbKeycodes, 0), QString());
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::XkbKeycodes, kXkbOffset), QString());
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::MacVirtualKeys, 0x34), QString());
  QCOMPARE(domCodeForNativeKey(NativeKeyCodes::None, 30), QString());
  // An event with a native code takes its code from it; one without falls back to the keys whose position the layout does not change.
  QCOMPARE(domCode(nativePress(Qt::Key_A, Qt::NoModifier, u"a"_s, NativeKeyCodes::XkbKeycodes, 38), kNativeLinux), u"KeyA"_s);
  QCOMPARE(domCode(press(Qt::Key_A, Qt::NoModifier, u"a"_s), kNativeLinux), QString());
  QCOMPARE(domCode(press(Qt::Key_Left, Qt::NoModifier), kNativeWindows), u"ArrowLeft"_s);
}

void TestInput::pressesFromOtherLayoutsKeepTheirPosition() {
  // The presses of keys.json's layouts, as each platform's Qt plugin delivers them: the key that Qt reports, and the position from the
  // native key code, so that both the binding's character and the fallback to the position work (engine protocol §14.3 item 3). What the
  // platform's Qt plugin reports: Qt's key, the modifiers, the text and the native key code. Qt forbids copying events, so each case holds
  // what makes its event.
  struct Pressed {
    int key;
    Qt::KeyboardModifiers modifiers;
    QString text;
    NativeKeyCodes platform;
    quint32 nativeCode;
  };
  struct Case {
    QString what;
    KeyMappingOptions options;
    Pressed event;
    protocol::KeyInput expected;
    std::optional<QString> text;
  };
  const protocol::Modifiers ctrl{.shift = false, .ctrl = true, .alt = false, .meta = false};
  const protocol::Modifiers altGraph{.shift = false, .ctrl = true, .alt = true, .meta = false};
  const QList<Case> cases{
      {
          .what = u"US Control+B on Windows"_s,
          .options = kNativeWindows,
          .event = Pressed(Qt::Key_B, Qt::ControlModifier, u"\x02"_s, NativeKeyCodes::WindowsScanCodes, 0x30),
          .expected = keyPress(u"b"_s, u"KeyB"_s, ctrl),
          .text = std::nullopt,
      },
      {
          .what = u"French Control+Z on Windows: Z is where US has W"_s,
          .options = kNativeWindows,
          .event = Pressed(Qt::Key_Z, Qt::ControlModifier, u"\x1a"_s, NativeKeyCodes::WindowsScanCodes, 0x11),
          .expected = keyPress(u"z"_s, u"KeyW"_s, ctrl),
          .text = std::nullopt,
      },
      {
          .what = u"French AltGr+E on Windows types the euro sign"_s,
          .options = kNativeWindows,
          .event = Pressed(0x20AC, Qt::ControlModifier | Qt::AltModifier, u"€"_s, NativeKeyCodes::WindowsScanCodes, 0x12),
          .expected = keyPress(u"€"_s, u"KeyE"_s, altGraph),
          .text = u"€"_s,
      },
      {
          .what = u"German AltGr+Q on Windows types the at sign"_s,
          .options = kNativeWindows,
          .event = Pressed(Qt::Key_At, Qt::ControlModifier | Qt::AltModifier, u"@"_s, NativeKeyCodes::WindowsScanCodes, 0x10),
          .expected = keyPress(u"@"_s, u"KeyQ"_s, altGraph),
          .text = u"@"_s,
      },
      {
          .what = u"German Control+Z on Linux: Z is where US has Y"_s,
          .options = kNativeLinux,
          .event = Pressed(Qt::Key_Z, Qt::ControlModifier, u"\x1a"_s, NativeKeyCodes::XkbKeycodes, 21 + 8),
          .expected = keyPress(u"z"_s, u"KeyY"_s, ctrl),
          .text = std::nullopt,
      },
      {
          .what = u"Russian Control+Ш on Linux, without a Latin layout for Qt to fall back to"_s,
          .options = kNativeLinux,
          .event = Pressed(0x0448, Qt::ControlModifier, QString(), NativeKeyCodes::XkbKeycodes, 23 + 8),
          .expected = keyPress(u"ш"_s, u"KeyI"_s, ctrl),
          .text = std::nullopt,
      },
      {
          .what = u"Arabic Control+the key of B on Windows"_s,
          .options = kNativeWindows,
          .event = Pressed(0x0644, Qt::ControlModifier, QString(), NativeKeyCodes::WindowsScanCodes, 0x30),
          .expected = keyPress(u"ل"_s, u"KeyB"_s, ctrl),
          .text = std::nullopt,
      },
      {
          .what = u"Arabic letter on Linux types text"_s,
          .options = kNativeLinux,
          .event = Pressed(0x0628, Qt::NoModifier, u"ب"_s, NativeKeyCodes::XkbKeycodes, 33 + 8),
          .expected = keyPress(u"ب"_s, u"KeyF"_s),
          .text = u"ب"_s,
      },
      {
          .what = u"Command+B on macOS, which Qt reports as Control"_s,
          .options = kNativeMacOS,
          .event = Pressed(Qt::Key_B, Qt::ControlModifier, u"b"_s, NativeKeyCodes::MacVirtualKeys, 0x0B),
          .expected = keyPress(u"b"_s, u"KeyB"_s, {.shift = false, .ctrl = false, .alt = false, .meta = true}),
          .text = std::nullopt,
      },
  };
  // The manifest of Windows and Linux binds Control+B to bold and Control+Z to undo; for the test, Command+B on macOS.
  const KeyBindings bindings(protocol::UiManifest{
      .commands =
          {
              {.id = u"format.bold.toggle"_s, .label = u"Bold"_s, .bindings = {binding(u"b"_s, true)}, .shellAction = std::nullopt},
              {.id = u"edit.undo"_s, .label = u"Undo"_s, .bindings = {binding(u"z"_s, true)}, .shellAction = std::nullopt},
              {.id = u"edit.redo"_s, .label = u"Redo"_s, .bindings = {binding(u"y"_s, true)}, .shellAction = std::nullopt},
              {.id = u"format.italic.toggle"_s, .label = u"Italic"_s, .bindings = {binding(u"i"_s, true)}, .shellAction = std::nullopt},
              {
                  .id = u"mac.bold"_s,
                  .label = u"Bold"_s,
                  .bindings = {binding(u"b"_s, false, false, false, true)},
                  .shellAction = std::nullopt,
              },
          },
      .documentKeys = {},
      .reservedKeys = {},
  });
  for (const Case &item : cases) {
    const QKeyEvent event = nativePress(item.event.key, item.event.modifiers, item.event.text, item.event.platform, item.event.nativeCode);
    const protocol::KeyInput input = keyInput(event, false, item.options);
    QVERIFY2(input == item.expected, qPrintable(item.what + u": "_s + input.key + u" "_s + input.code));
    QVERIFY2(typedText(event, item.options) == item.text, qPrintable(item.what));
    // Typed text never goes as input.key; every shortcut here does, by its character or by its position.
    QVERIFY2(bindings.sends(input) == !item.text.has_value(), qPrintable(item.what));
  }
}

void TestInput::bindingsMatchAsTheProtocolSays() {
  // Engine protocol §14.3 item 3: modifiers given must be equal, those left out match either state; ASCII letters without regard to case;
  // the position instead of the character only for a press that types no ASCII letter or digit, and never with AltGr.
  const protocol::Modifiers ctrl{.shift = false, .ctrl = true, .alt = false, .meta = false};
  const protocol::Modifiers ctrlShift{.shift = true, .ctrl = true, .alt = false, .meta = false};
  QVERIFY(KeyBindings::matches(binding(u"b"_s, true), keyPress(u"b"_s, u"KeyB"_s, ctrl)));
  QVERIFY(KeyBindings::matches(binding(u"b"_s, true), keyPress(u"B"_s, u"KeyB"_s, ctrl)));
  QVERIFY(!KeyBindings::matches(binding(u"b"_s, true), keyPress(u"B"_s, u"KeyB"_s, ctrlShift)));
  QVERIFY(!KeyBindings::matches(binding(u"b"_s, true), keyPress(u"b"_s, u"KeyB"_s)));
  // Other letters are compared exactly: no case folding beyond ASCII.
  QVERIFY(!KeyBindings::matches(binding(u"é"_s, true), keyPress(u"É"_s, u"Digit2"_s, ctrl)));
  // The position: an ASCII letter or digit of the binding against a press that types another script, or a French é on the key of 2.
  QVERIFY(KeyBindings::matches(binding(u"i"_s, true), keyPress(u"ш"_s, u"KeyI"_s, ctrl)));
  QVERIFY(KeyBindings::matches(binding(u"2"_s, true), keyPress(u"é"_s, u"Digit2"_s, ctrl)));
  QVERIFY(KeyBindings::matches(binding(u"b"_s, true), keyPress(u"لا"_s, u"KeyB"_s, ctrl)));
  // Not when the press types an ASCII letter of its own (French Control+W is not undo), with AltGr, or without a known position.
  QVERIFY(!KeyBindings::matches(binding(u"z"_s, true), keyPress(u"w"_s, u"KeyZ"_s, ctrl)));
  QVERIFY(!KeyBindings::matches(binding(u"e"_s, true, false, true),
                                keyPress(u"€"_s, u"KeyE"_s, {.shift = false, .ctrl = true, .alt = true, .meta = false})));
  QVERIFY(!KeyBindings::matches(binding(u"i"_s, true), keyPress(u"ш"_s, QString(), ctrl)));
  // A binding's key that is no single letter or digit never falls back to the position.
  QVERIFY(!KeyBindings::matches(binding(u"F7"_s, true), keyPress(u"ш"_s, u"F7"_s, ctrl)));
  // Entries with code compare codes; entries that leave modifiers out match them in either state.
  const protocol::KeyEntry keyTip{.key = {}, .code = u"KeyF"_s, .ctrl = false, .shift = false, .alt = true, .meta = false, .label = {}};
  QVERIFY(KeyBindings::matches(keyTip, keyPress(u"f"_s, u"KeyF"_s, {.shift = false, .ctrl = false, .alt = true, .meta = false})));
  QVERIFY(!KeyBindings::matches(keyTip, keyPress(u"f"_s, u"KeyG"_s, {.shift = false, .ctrl = false, .alt = true, .meta = false})));
  const protocol::KeyEntry anyModifiers{.key = u"Backspace"_s, .code = {}, .ctrl = {}, .shift = {}, .alt = {}, .meta = {}, .label = {}};
  QVERIFY(KeyBindings::matches(anyModifiers, keyPress(u"Backspace"_s, u"Backspace"_s, ctrlShift)));
  QVERIFY(KeyBindings::matches(anyModifiers, keyPress(u"Backspace"_s, u"Backspace"_s)));
}

void TestInput::reservedKeysAndShellActionsStayWithTheShell() {
  const protocol::KeyEntry tab{.key = u"Tab"_s, .code = {}, .ctrl = false, .shift = {}, .alt = false, .meta = false, .label = {}};
  const protocol::KeyEntry backspace{.key = u"Backspace"_s, .code = {}, .ctrl = {}, .shift = {}, .alt = {}, .meta = {}, .label = {}};
  const protocol::KeyEntry tabAsDocumentKey{.key = u"Tab"_s, .code = {}, .ctrl = {}, .shift = {}, .alt = {}, .meta = {}, .label = {}};
  const KeyBindings bindings(protocol::UiManifest{
      .commands =
          {
              {.id = u"edit.copy"_s, .label = u"Copy"_s, .bindings = {binding(u"c"_s, true)}, .shellAction = u"copy"_s},
              {.id = u"later.zoom"_s, .label = u"Zoom in"_s, .bindings = {binding(u"="_s, true)}, .shellAction = u"zoom_in"_s},
              {.id = u"format.bold.toggle"_s, .label = u"Bold"_s, .bindings = {binding(u"b"_s, true)}, .shellAction = std::nullopt},
          },
      .documentKeys = {backspace, tabAsDocumentKey},
      .reservedKeys = {tab},
  });
  const protocol::Modifiers ctrl{.shift = false, .ctrl = true, .alt = false, .meta = false};
  QVERIFY(bindings.sends(keyPress(u"b"_s, u"KeyB"_s, ctrl)));
  QVERIFY(bindings.sends(keyPress(u"Backspace"_s, u"Backspace"_s, ctrl)));
  // A reserved key wins over a document key; a command with shell_action, known or not, never sends its keys; composing sends nothing.
  QVERIFY(!bindings.sends(keyPress(u"Tab"_s, u"Tab"_s)));
  QVERIFY(bindings.sends(keyPress(u"Tab"_s, u"Tab"_s, ctrl)));
  QVERIFY(!bindings.sends(keyPress(u"c"_s, u"KeyC"_s, ctrl)));
  QVERIFY(!bindings.sends(keyPress(u"="_s, u"Equal"_s, ctrl)));
  QVERIFY(!bindings.sends(keyPress(u"Backspace"_s, u"Backspace"_s, {}, true)));
  // Without key data, nothing goes to the engine.
  QVERIFY(!KeyBindings().sends(keyPress(u"Backspace"_s, u"Backspace"_s)));
  // The labels are the engine's; a command whose shell_action the shell does not know is not offered, so it shows none.
  QCOMPARE(bindings.labels(u"format.bold.toggle"_s), QStringList{u"label of b"_s});
  QCOMPARE(bindings.labels(u"edit.copy"_s), QStringList{u"label of c"_s});
  QCOMPARE(bindings.labels(u"later.zoom"_s), QStringList());
  QCOMPARE(bindings.labels(u"no.such.command"_s), QStringList());
  QVERIFY(KeyBindings::knowsShellAction(u"paste"_s) && !KeyBindings::knowsShellAction(u"zoom_in"_s));
}

void TestInput::japaneseOnWindowsConvertsAClause() {
  ImeTranslator ime;
  // Typing "nihon" with Microsoft IME: kana appear as pre-edit text with the cursor at the end.
  auto actions = ime.translate(preedit(u"に"_s, {underline(0, 1), cursor(1)}));
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Start}, update(u"に"_s, 1, 1)}));
  QVERIFY(ime.composing());
  actions = ime.translate(preedit(u"にほん"_s, {underline(0, 3), cursor(3)}));
  QCOMPARE(actions, (std::vector<ImeAction>{update(u"にほん"_s, 3, 3)}));
  // Space converts: the converted clause is highlighted, and becomes the composition's selection.
  actions = ime.translate(preedit(u"日本"_s, {background(0, 2), cursor(0)}));
  QCOMPARE(actions, (std::vector<ImeAction>{update(u"日本"_s, 0, 2)}));
  // The same again (Windows repeats events) sends nothing.
  QVERIFY(ime.translate(preedit(u"日本"_s, {background(0, 2), cursor(0)})).empty());
  // Enter commits; input.text ends the composition by itself.
  actions = translateCommit(ime, u"日本"_s);
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Commit, .text = u"日本"_s}}));
  QVERIFY(!ime.composing());
  QVERIFY(ime.composition().isEmpty());
}

void TestInput::chineseOnMacOSMarksTheSelectedRange() {
  ImeTranslator ime;
  // Pinyin with two syllables, the second being edited: macOS marks the selected range with a background colour.
  auto actions = ime.translate(preedit(u"zhong wen"_s, {underline(0, 9), background(6, 3), cursor(9)}));
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Start}, update(u"zhong wen"_s, 6, 9)}));
  actions = translateCommit(ime, u"中文"_s);
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Commit, .text = u"中文"_s}}));
}

void TestInput::linuxUnderlinesAndShowsTheCursor() {
  ImeTranslator ime;
  // IBus: the pre-edit text is underlined and the cursor is inside it; the selection is the cursor.
  const auto actions = ime.translate(preedit(u"にほんご"_s, {underline(0, 4), cursor(2)}));
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Start}, update(u"にほんご"_s, 2, 2)}));
  // Without any attribute, the selection is at the end.
  QCOMPARE(ImeTranslator::selectionOf(u"abc"_s, {}), std::pair(3, 3));
}

void TestInput::koreanCommitsAndStartsTheNextSyllableInOneEvent() {
  ImeTranslator ime;
  auto actions = ime.translate(preedit(u"한"_s, {background(0, 1), cursor(1)}));
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Start}, update(u"한"_s, 0, 1)}));
  // The next consonant finishes 한 and starts a new syllable: one event with both a commit and a pre-edit text.
  QInputMethodEvent next(u"ㄱ"_s, {background(0, 1), cursor(1)});
  next.setCommitString(u"한"_s);
  actions = ime.translate(next);
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::Commit, .text = u"한"_s}, {.kind = Kind::Start}, update(u"ㄱ"_s, 0, 1)}));
  QVERIFY(ime.composing());
}

void TestInput::escapeCancelsTheComposition() {
  ImeTranslator ime;
  (void)ime.translate(preedit(u"か"_s, {underline(0, 1), cursor(1)}));
  // An empty event while composing: the composition is discarded.
  QCOMPARE(ime.translate(QInputMethodEvent()), (std::vector<ImeAction>{{.kind = Kind::End}}));
  QVERIFY(!ime.composing());
  // An empty event otherwise means nothing.
  QVERIFY(ime.translate(QInputMethodEvent()).empty());
  // reset() forgets a composition without any action.
  (void)ime.translate(preedit(u"か"_s, {}));
  ime.reset();
  QVERIFY(!ime.composing());
}

void TestInput::accentMenuReplacesTheCharacterBeforeTheCaret() {
  ImeTranslator ime;
  // macOS: hold "e" (already typed), choose "é" in the accent menu.
  const auto actions = translateCommit(ime, u"é"_s, -1, 1);
  QCOMPARE(actions, (std::vector<ImeAction>{{.kind = Kind::DeleteBefore, .length = 1}, {.kind = Kind::Commit, .text = u"é"_s}}));
}

void TestInput::deleteSurroundingTextBecomesDeletions() {
  ImeTranslator ime;
  // IBus's delete-surrounding-text across the caret: two code units before it and one after.
  QCOMPARE(translateCommit(ime, QString(), -2, 3),
           (std::vector<ImeAction>{{.kind = Kind::DeleteBefore, .length = 2}, {.kind = Kind::DeleteAfter, .length = 1}}));
  // Only after the caret.
  QCOMPARE(translateCommit(ime, QString(), 0, 2), (std::vector<ImeAction>{{.kind = Kind::DeleteAfter, .length = 2}}));
  // A range that starts after the caret cannot be sent to the engine at the pinned core and is left out; the commit still happens.
  QCOMPARE(translateCommit(ime, u"x"_s, 1, 1), (std::vector<ImeAction>{{.kind = Kind::Commit, .text = u"x"_s}}));
  // The input method decides the numbers: the extremes of int pass through without overflowing (the canvas deletes only the text there
  // is).
  constexpr int kMin = std::numeric_limits<int>::min();
  constexpr int kMax = std::numeric_limits<int>::max();
  QCOMPARE(translateCommit(ime, QString(), kMin, 3), (std::vector<ImeAction>{{.kind = Kind::DeleteBefore, .length = 3}}));
  QCOMPARE(translateCommit(ime, QString(), kMin, kMax), (std::vector<ImeAction>{{.kind = Kind::DeleteBefore, .length = kMax}}));
  QCOMPARE(translateCommit(ime, QString(), -1, kMax),
           (std::vector<ImeAction>{{.kind = Kind::DeleteBefore, .length = 1}, {.kind = Kind::DeleteAfter, .length = kMax - 1}}));
}

void TestInput::replacingDuringACompositionEndsItFirst() {
  ImeTranslator ime;
  (void)ime.translate(preedit(u"e"_s, {underline(0, 1), cursor(1)}));
  const auto actions = translateCommit(ime, u"é"_s, -1, 1);
  QCOMPARE(actions, (std::vector<ImeAction>{
                        {.kind = Kind::End}, {.kind = Kind::DeleteBefore, .length = 1}, {.kind = Kind::Commit, .text = u"é"_s}}));
  QVERIFY(!ime.composing());
}

void TestInput::selectionNeverSplitsASurrogatePair() {
  // "a😀b": the emoji is two UTF-16 code units, 1 and 2. Offset 2 lies between them and moves back to 1.
  const QString text = u"a😀b"_s;
  QCOMPARE(ImeTranslator::selectionOf(text, {cursor(2)}), std::pair(1, 1));
  QCOMPARE(ImeTranslator::selectionOf(text, {background(0, 2)}), std::pair(0, 1));
  QCOMPARE(ImeTranslator::selectionOf(text, {background(2, 2)}), std::pair(1, 4));
  // Offsets outside the text are clamped.
  QCOMPARE(ImeTranslator::selectionOf(text, {cursor(99)}), std::pair(4, 4));
  QCOMPARE(ImeTranslator::selectionOf(text, {cursor(-3)}), std::pair(0, 0));
  // A background range that collapses is ignored in favour of the cursor.
  QCOMPARE(ImeTranslator::selectionOf(text, {background(2, 0), cursor(4)}), std::pair(4, 4));
}

QTEST_MAIN(TestInput)
#include "tst_input.moc"
