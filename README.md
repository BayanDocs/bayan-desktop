# bayan-desktop

The native desktop application of [BayanDocs](https://github.com/BayanDocs/docs) for Windows, macOS and Linux: a free, open-source word processor that matches Microsoft Word's document fidelity, works fully offline, starts instantly and uses little memory.

This repository is a deliberately **thin shell**: windows, menus, the ribbon and dialogs (rendered from the shared UI manifest), the document canvas, input methods, the bridge to operating-system screen readers, printing, file associations, installers and updates. Every behavior that concerns documents lives in [bayan-core](https://github.com/BayanDocs/bayan-core), which this app links through a small C interface, so the desktop and [web](https://github.com/BayanDocs/bayan-web) apps lay out documents identically.

Built with C++20 and Qt 6 (Qt Quick), using only LGPL-licensed Qt modules.

> **Status: Phase 0 (Foundations).** The scaffold from [DESK-001](https://github.com/BayanDocs/docs/blob/HEAD/workpackages/phase-0/DESK-001-desktop-scaffold.md) is in place: a Qt Quick window with a placeholder ribbon and a document area that shows a page rendered by a stub engine, the engine integration layer, tests and CI. The real engine arrives with bayan-core's C SDK (CORE-007) and the document canvas with DESK-002.

## Building

Everything is built with CMake presets. One script installs the exact tool versions the project pins (CMake, Ninja, clang-format, clang-tidy and Qt 6.12.0, each checked against a published hash), so that your build matches CI:

```sh
scripts/dev-setup.sh                               # once; installs into ~/.local/share/bayandocs/desktop-tools
. ~/.local/share/bayandocs/desktop-tools/env.sh    # in every new shell
cmake --workflow --preset dev                      # Debug build and tests
cmake --workflow --preset verify                   # the full verification gate (adds lint), as in CI
./build/dev/src/bayan-desktop                      # run the app (on macOS: open build/dev/src/bayan-desktop.app)
```

What each operating system needs before running the script:

- **Linux (Ubuntu 24.04 or similar):** a C++20 compiler (GCC 13 or Clang 18), Python 3.11 or newer with `venv`, and the development packages Qt needs: `sudo apt install build-essential python3-venv libgl-dev libegl-dev libvulkan-dev libxkbcommon-dev libfontconfig-dev libdbus-1-dev`.
- **macOS (Apple silicon):** the Xcode command line tools (`xcode-select --install`) and Python 3.11 or newer.
- **Windows:** Visual Studio 2022 with the "Desktop development with C++" workload, Git for Windows (run the script in Git Bash) and Python 3.11 or newer. Then open an "x64 Native Tools" developer prompt, put the tools folder and Qt's `bin` folder that the script printed at the front of `PATH`, set `CMAKE_PREFIX_PATH` to the Qt folder, and run `cmake --workflow --preset msvc`. Note: the pinned aqtinstall 3.3.0 cannot yet install Qt 6.12 for Windows, because Qt changed its download layout; until that is resolved, install Qt 6.12.0 for MSVC 2022 64-bit with Qt's own installer (LGPL modules only) and set `BAYAN_QT_DIR` to that folder before running the script, which then uses it instead of installing Qt.

The sanitizer build (`cmake --workflow --preset asan`, Linux) runs the tests with AddressSanitizer and UndefinedBehaviorSanitizer. `bayan-desktop --smoke-test` starts the app, checks that the engine's page is displayed and exits; CI runs it without a screen (`QT_QPA_PLATFORM=offscreen`).

## Where things are decided

- Master plan: [docs/plan](https://github.com/BayanDocs/docs/tree/HEAD/plan)
- Desktop decision: [ADR-0013](https://github.com/BayanDocs/docs/blob/HEAD/adr/0013-desktop-shell.md); engine boundary: [ADR-0012](https://github.com/BayanDocs/docs/blob/HEAD/adr/0012-engine-boundary.md) and the [engine protocol](https://github.com/BayanDocs/docs/blob/HEAD/specs/engine-protocol.md)
- Work packages: [docs/workpackages](https://github.com/BayanDocs/docs/tree/HEAD/workpackages)

Contributors and agents: start with [AGENTS.md](AGENTS.md).

## License

Licensed under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)) with the [BayanDocs App Store Permission](LICENSES/LicenseRef-BayanDocs-App-Store-Permission.txt), an additional permission under GPLv3 section 7 that allows distribution through app stores as long as the source code stays freely available to everyone. SPDX: `GPL-3.0-or-later WITH LicenseRef-BayanDocs-App-Store-Permission`. [REUSE.toml](REUSE.toml) records which license applies to which files, and [LICENSES/](LICENSES) holds the full texts. See the [licensing FAQ](https://github.com/BayanDocs/docs/blob/HEAD/LICENSING.md) and [ADR-0003](https://github.com/BayanDocs/docs/blob/HEAD/adr/0003-licensing-and-contribution-model.md).
