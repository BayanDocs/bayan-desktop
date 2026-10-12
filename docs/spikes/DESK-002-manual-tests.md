# DESK-002 manual tests: input methods, screen readers and frame times

These are the tests of [DESK-002](https://github.com/BayanDocs/docs/blob/HEAD/workpackages/phase-0/DESK-002-spike-qt-canvas.md) that need a person and real computers: acceptance criteria AC-1 (frame times on the reference laptops), AC-2 (Japanese, Chinese and Korean input methods on Windows and macOS) and AC-3 (screen readers on Windows and macOS, and Linux if possible). Everything else is tested automatically; the [report](DESK-002-report.md) has the results.

Each test lists what to do and what should happen. Fill in the result tables at the end (or paste them into the pull request), and attach the screenshots, short screen recordings and benchmark files. "Passed", "Failed" or "Partly" with a sentence is enough; when something differs from the expected result, describe what you saw.

## What you will see

The engine is still CORE-007's mock document: three pages with a heading, paragraphs, a list, a table and one **editable line** (the yellow box on the first page). The mock engine draws text as grey bars, one bar per character, instead of letters. While you compose with an input method, the characters being composed are blue and underlined; committed characters are dark. To check which characters arrived, the screen reader test reads the editable line aloud.

## Preparation: build the app with the real engine

The reference laptops are the plan's reference hardware ([requirements](https://github.com/BayanDocs/docs/blob/HEAD/plan/02-requirements.md)): a 2020-era mid-range laptop with a 4-core x86-64 processor and Windows 11, and an Apple M1 Mac with a current macOS, each with 8 GB of memory or more. Use the laptop's own screen, plugged in, with other apps closed.

### macOS (Apple silicon)

1. Install the Xcode command line tools (`xcode-select --install` in Terminal) and Python 3.11 or newer from [python.org](https://www.python.org/downloads/macos/).
2. Get the code of the pull request and the pinned tools (this downloads Qt, about 1 GB, and checks every file's hash):
   ```sh
   git clone https://github.com/BayanDocs/bayan-desktop.git && cd bayan-desktop
   git checkout claude/desk-002-qt-canvas
   scripts/dev-setup.sh
   . ~/.local/share/bayandocs/desktop-tools/env.sh
   ```
3. Get the engine, either way:
   - **Download** (no Rust needed): on [bayan-core's CI run for the pinned commit](https://github.com/BayanDocs/bayan-core/actions/runs/38177097534), signed in to GitHub, download the artifact `bayan-core-sdk-aarch64-apple-darwin` (it stays available until 2027-01-09). Check it and unpack it:
     ```sh
     shasum -a 256 ~/Downloads/bayan-core-sdk-aarch64-apple-darwin.zip
     # must print 9f947a39f001771ba07bf499cfd848130452d90826d8bc90b470d8d53c22a011
     mkdir -p build/bayan-core-sdk && unzip -q ~/Downloads/bayan-core-sdk-aarch64-apple-darwin.zip -d build/bayan-core-sdk
     (cd build/bayan-core-sdk && shasum -a 256 -c SHA256SUMS)   # every line must end in OK
     xattr -dr com.apple.quarantine build/bayan-core-sdk         # the browser marked the files as downloaded; they were just checked
     ```
     If the browser unpacked the zip by itself (Safari does), copy the folder's contents into `build/bayan-core-sdk` instead and run the last two commands.
   - **Or build it** with Rust: install rustup from [rustup.rs](https://rustup.rs), then run `scripts/build-engine-sdk.sh`.
4. Build the app against it and run its tests (all must pass):
   ```sh
   BAYAN_ENGINE_SDK_DIR="$PWD/build/bayan-core-sdk" cmake --workflow --preset engine-sdk
   ```
5. Start it: `build/engine-sdk/src/bayan-desktop.app/Contents/MacOS/bayan-desktop`. The window shows the first page of the mock document; the status bar says "Ready".

### Windows 11 (x86-64)

1. Install Visual Studio 2022 Community with the "Desktop development with C++" workload, [Git for Windows](https://git-scm.com/download/win) and Python 3.11 or newer from [python.org](https://www.python.org/downloads/windows/) (tick "Add python.exe to PATH").
2. In **Git Bash**, get the code and the pinned tools:
   ```sh
   git clone https://github.com/BayanDocs/bayan-desktop.git && cd bayan-desktop
   git checkout claude/desk-002-qt-canvas
   scripts/dev-setup.sh
   ```
   The script prints the folders of the tools and of Qt; keep them for step 4.
3. Get the engine, either way:
   - **Download** (no Rust needed): from [the same CI run](https://github.com/BayanDocs/bayan-core/actions/runs/38177097534), download `bayan-core-sdk-x86_64-pc-windows-msvc`. In Git Bash:
     ```sh
     sha256sum ~/Downloads/bayan-core-sdk-x86_64-pc-windows-msvc.zip
     # must print f59cfa6f86c2765432de18752d36adba64754ccd73b97306f183e786c1110d6c
     mkdir -p build/bayan-core-sdk && unzip -q ~/Downloads/bayan-core-sdk-x86_64-pc-windows-msvc.zip -d build/bayan-core-sdk
     (cd build/bayan-core-sdk && sha256sum -c SHA256SUMS)   # every line must end in OK
     ```
   - **Or build it** with Rust: install rustup from [rustup.rs](https://rustup.rs), then run `scripts/build-engine-sdk.sh` in Git Bash (not in the Visual Studio prompt of step 4).
4. Open "x64 Native Tools Command Prompt for VS 2022" from the Start menu, go to the `bayan-desktop` folder, and build, with the two folders that step 2 printed in place of `<tools>` and `<qt>`:
   ```bat
   set PATH=<tools>;<qt>\bin;%PATH%
   set CMAKE_PREFIX_PATH=<qt>
   set BAYAN_ENGINE_SDK_DIR=%CD%\build\bayan-core-sdk
   cmake --workflow --preset engine-sdk-msvc
   ```
5. Start it from the same prompt: `build\engine-sdk-msvc\src\bayan-desktop.exe`.

### Linux (optional)

As in the README, then `scripts/build-engine-sdk.sh` (or the artifact `bayan-core-sdk-x86_64-unknown-linux-gnu`, SHA-256 `a5153f7fe6b1b444067cae4cab54f42fd933e95522f02367fd42598c626ced05`) and `BAYAN_ENGINE_SDK_DIR="$PWD/build/bayan-core-sdk" cmake --workflow --preset engine-sdk`. Qt's X11 support needs the packages `libxcb-cursor0`, `libxcb-icccm4` and `libxcb-keysyms1`.

## Test 1: smoothness and frame times (AC-1)

1. Note the screen's refresh rate (macOS: System Settings → Displays; Windows: Settings → System → Display → Advanced display) and its scaling (Windows: Settings → System → Display → Scale).
2. Run the benchmark. The window scrolls, zooms and types by itself for about 20 seconds, closes, and prints three lines. Don't touch the computer meanwhile.
   - macOS: `build/engine-sdk/src/bayan-desktop.app/Contents/MacOS/bayan-desktop --benchmark ~/desk002-macos.json`
   - Windows: `build\engine-sdk-msvc\src\bayan-desktop.exe --benchmark %USERPROFILE%\desk002-windows-100.json`
3. On Windows, repeat step 2 at 125 % and 150 % scaling (change the scale, then run again with a new file name).
4. By hand, in the app:
   - Scroll with two fingers on the trackpad, with the mouse wheel, by dragging the scroll bar, and with Page Down and Page Up. Expected: smooth, no stutter, no blank or grey areas on the pages for more than a moment, no lines between tiles.
   - Zoom with a pinch, with Control and the mouse wheel (Command on macOS), and with Control+plus and Control+minus (Command on macOS); Control+0 (Command+0) goes back to 100 %. Expected: while zooming, the page stretches smoothly (slightly blurry); a moment after you stop, it becomes sharp again. The point under the pointer stays under the pointer.
   - Move the window to a second screen with another scaling, if you have one. Expected: sharp text on both.

**Passes when:** in the printed lines, the p95 interval while scrolling and while zooming is at most 1.5 times the refresh interval (25 ms at 60 Hz, 12.5 ms at 120 Hz), "late" is at most 1 % of the frames, and nothing looked wrong by hand. Attach the JSON files.

## Test 2: input methods (AC-2)

Click into the yellow editable line first; a thin caret blinks there. For each input method, take a screenshot while composing (step b) and after committing (step d).

**Set up** the input methods:

- Windows: Settings → Time & language → Language & region → Add a language: Japanese, Chinese (Simplified, China) and Korean. Switch between them with Windows+Space.
- macOS: System Settings → Keyboard → Text Input → Edit → +: Japanese – Romaji, Chinese, Simplified – Pinyin – Simplified, and Korean – 2-Set Korean. Switch with Control+Space or the Globe key.

**Japanese** (Microsoft IME on Windows; Japanese – Romaji on macOS). On Windows make sure the input mode is Hiragana (the taskbar shows あ; press the Half-width/Full-width key or Alt+` if it shows A).

| Step | Do | Expected |
|---|---|---|
| a | Type `a` in English, then switch to Japanese | One dark bar in the line |
| b | Type `nihon` | Blue underlined bars appear after it as you type (3 for にほん); a candidate or suggestion window opens next to the line, near the caret, not in a corner of the screen |
| c | Press Space | The bars change to 2 (日本), shown as the clause being converted; the candidate window stays next to the line |
| d | Press Enter | The 2 bars turn dark: 3 dark bars in all, the caret after them |
| e | Type `ka`, then press Escape (Windows: twice if needed) | The blue bar appears, then disappears; still 3 dark bars |

**Chinese** (Microsoft Pinyin on Windows; Pinyin – Simplified on macOS).

| Step | Do | Expected |
|---|---|---|
| a | Type `zhongwen` | Blue underlined bars for the letters as you type; candidates next to the line |
| b | Choose 中文 with Space or 1 | The letters are replaced by 2 dark bars |

**Korean** (Microsoft IME on Windows; 2-Set Korean on macOS). On Windows make sure the mode is Hangul (press the Han/Eng key or Right Alt).

| Step | Do | Expected |
|---|---|---|
| a | Type `gks` | One bar that changes as you type (ㅎ, 하, 한), highlighted as composing |
| b | Type `rmf` | The first syllable becomes dark (한) and a new composing one appears (글) |
| c | Press Space | 2 dark bars (한글) followed by a space |

**macOS only:** switch to ABC (or U.S.), hold `e` until the accent menu appears, press 2. Expected: the bar for "e" is replaced by one bar (é), not two. Then press Control+Command+Space and pick an emoji. Expected: one more bar.

**Linux (optional):** IBus with Mozc, Pinyin and Hangul, or Fcitx 5 with the same; the same steps.

**Passes when** composition appears in the editable line as blue underlined bars, committing turns it into the right number of dark bars, the candidate window opens next to the caret, and nothing is lost or doubled, for all three languages on Windows and on macOS.

## Test 3: screen readers (AC-3)

The document is presented as one multi-line text: every heading, paragraph, list item and table cell is one line. The caret stays in the editable line (the mock engine has no caret anywhere else), so the arrow keys move within that line; to read the rest of the document, use the screen reader's own reading commands below.

**Windows: NVDA** (free, [nvaccess.org](https://www.nvaccess.org/download/); the "NVDA" key is Insert, or Caps Lock in the laptop layout). Narrator may be used as well; Narrator key+F1 lists its commands.

| Step | Do | Expected |
|---|---|---|
| a | Start NVDA, then the app; click on the page | NVDA announces "BayanDocs mock document, edit, multi line" and the current line (blank: the editable line is empty) |
| b | Move NVDA's review cursor to the first line: Shift+Numpad 7 (laptop layout: NVDA+Control+Home). Then go down line by line: Numpad 9 (laptop: NVDA+Down Arrow) | It reads the heading "BayanDocs mock document", then each paragraph, the editable line (blank), the list items, the table's header and cells one by one, and finally "This is the end of the mock document." |
| c | Type `hello` | NVDA echoes each character as you type |
| d | Press Left Arrow three times, then Right Arrow | NVDA says each character the caret passes |
| e | Type in Japanese as in Test 2 and read the line with NVDA+Up Arrow (laptop: NVDA+L) | NVDA reads the characters you committed (日本) |

**macOS: VoiceOver** (Command+F5 turns it on and off; "VO" means Control+Option).

| Step | Do | Expected |
|---|---|---|
| a | Start the app, turn VoiceOver on, click on the page | VoiceOver announces "BayanDocs mock document, edit text" or "text area" |
| b | Press VO+A (read all) | VoiceOver reads the document's text. Note where it starts (at the top or at the editable line) and whether it reaches "This is the end of the mock document." |
| c | Press VO+Shift+Down Arrow to interact with the document, then read the current line with VO+L and explore with VoiceOver's text navigation | Each heading, paragraph, list item and table cell is read as a line. Note which commands worked |
| d | Type `hello`, then Left Arrow three times | Typing is echoed; the characters the caret passes are read |
| e | Type in Japanese as in Test 2 and press VO+L | VoiceOver reads 日本 |

**Linux: Orca (optional).** Start Orca (Super+Alt+S on GNOME), then the app, and click on the page. Use Orca's say-all command (Numpad Plus; laptop layout: Orca+Semicolon) and type a few characters. Report what it reads and whether it echoes typing.

**Passes when** the screen reader reads the mock document's text on Windows and on macOS, echoes typing, follows the caret, and reads committed East Asian text. A short screen recording with sound for each platform is the best evidence.

## Results

Fill in one row per check; add the machine (model, processor, memory, screen and refresh rate) and the versions (Windows build or macOS version, screen reader version).

| Test | Check | Windows 11 | macOS | Linux |
|---|---|---|---|---|
| 1 | Benchmark at 100 % (paste the three lines) | | | |
| 1 | Benchmark at 125 % and 150 % | | n/a | |
| 1 | Scrolling, zooming, second screen by hand | | | |
| 2 | Japanese a–e | | | |
| 2 | Chinese a–b | | | |
| 2 | Korean a–c | | | |
| 2 | Accent menu and emoji | n/a | | n/a |
| 3 | Announcement on focus (a) | | | |
| 3 | Reading the whole document (b, c) | | | |
| 3 | Typing echo and caret (c, d) | | | |
| 3 | East Asian text read aloud (e) | | | |
| | Machine and versions | | | |
