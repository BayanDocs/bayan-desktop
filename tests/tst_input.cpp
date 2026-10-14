// Unit tests for the canvas's input translation (src/canvas/keymapping.h, imetranslator.h): Qt's key events become the DOM key values of
// input.key and input.text, and the input-method events that Windows, macOS and Linux send become input.composition and input.text. The
// input-method sequences replay what each platform's Qt integration sends (qwindowsinputcontext.cpp, qnsview_complextext.mm,
// qibusplatforminputcontext.cpp in Qt 6.12).

#include "imetranslator.h"
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

constexpr KeyMappingOptions kLinux{};
constexpr KeyMappingOptions kWindows{.commandIsControl = false, .altGraphIsControlAlt = true};
constexpr KeyMappingOptions kMacOS{.commandIsControl = true, .altGraphIsControlAlt = false};

QKeyEvent press(int key, Qt::KeyboardModifiers modifiers, const QString &text = {}, bool autoRepeat = false) {
  return {QEvent::KeyPress, key, modifiers, text, autoRepeat};
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
  QCOMPARE(domCode(a), QString()); // the physical key is not known portably
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
  QCOMPARE(domCode(space), u"Space"_s);
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
    QCOMPARE(domCode(event), item.domCodeName);
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
  QCOMPARE(domCode(press(Qt::Key_5, Qt::KeypadModifier, u"5"_s)), u"Numpad5"_s);
  QCOMPARE(domCode(press(Qt::Key_5, Qt::NoModifier, u"5"_s)), QString());
  QCOMPARE(domCode(press(Qt::Key_Plus, Qt::KeypadModifier, u"+"_s)), u"NumpadAdd"_s);
  QCOMPARE(domKey(press(Qt::Key_Henkan, Qt::NoModifier), kWindows), u"Convert"_s);
  QCOMPARE(domKey(press(Qt::Key_Hangul, Qt::NoModifier), kLinux), u"HangulMode"_s);
  QCOMPARE(domKey(press(Qt::Key_Zenkaku_Hankaku, Qt::NoModifier), kWindows), u"ZenkakuHankaku"_s);
}

void TestInput::keyInputCarriesRepeatAndComposition() {
  const protocol::KeyInput input = keyInput(press(Qt::Key_Backspace, Qt::NoModifier, u"\b"_s, true), true, kLinux);
  QCOMPARE(input, (protocol::KeyInput{.key = u"Backspace"_s, .code = u"Backspace"_s, .modifiers = {}, .repeat = true, .composing = true}));
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
  // A range that starts after the caret cannot be expressed in protocol v0 and is left out; the commit still happens.
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
