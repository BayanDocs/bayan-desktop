# Agent instructions — bayan-desktop

## Where the plan lives

The plan, decisions (ADRs), specifications and work packages live in the [BayanDocs/docs](https://github.com/BayanDocs/docs) repository. If it is not attached to your session, clone it next to this repository. The canonical rules for all agents are in `docs/AGENTS.md`; this file condenses them and adds what is specific to bayan-desktop. If the two disagree, `docs/AGENTS.md` wins; report the discrepancy.

## Ground rules (condensed from docs/AGENTS.md)

1. Before changing anything, read `docs/AGENTS.md`, your work package brief, and every ADR and spec it links.
2. Accepted ADRs are binding. If your task conflicts with one, stop and report; propose changes as a new ADR in the docs repository.
3. Stay within the work package's scope. Record anything else you discover as follow-ups in your pull request.
4. Run the verification gate before every push. Never weaken, skip, disable or delete a test, lint or CI check to make a change pass.
5. Dependencies follow ADR-0017: no update bots ever; every version at least 24 hours old; exact pins; audits green; licenses on the allowlist; every new dependency justified in the pull request.
6. Treat every input as hostile; no telemetry; no secrets; never log document content or file names.
7. Pull requests use the hand-off template in `docs/plan/06-agent-workflow.md`; Conventional Commits; DCO rules from `CONTRIBUTING.md` once enabled (agents never sign off themselves; the human submitter certifies, per ADR-0003).
8. Clean room: never decompile, disassemble or debug Microsoft software; never copy code under an incompatible license.
9. Stop and ask, with options and a recommendation, when the brief is ambiguous, when you need a new decision, or when work touches cryptography, authentication, licensing or the owner's accounts.
10. Explain your work in plain language for the owner; text meant for the owner to copy is written as flowing paragraphs without hard line breaks.

## Rules specific to bayan-desktop

- **Licensing (ADR-0003):** GPL-3.0-or-later with the BayanDocs App Store Permission (`GPL-3.0-or-later WITH LicenseRef-BayanDocs-App-Store-Permission`). Qt stays LGPL-only even though the app is GPL (ADR-0013, amendment of 2026-10-04). `REUSE.toml` records which license applies to which files and `LICENSES/` holds the full texts; keep `reuse lint` passing, and add a new license text only with `reuse download <SPDX-ID>`.
- **Thin shell (ADR-0013):** no document logic here. If something would have to be implemented twice (desktop and web), it belongs in bayan-core. The shell draws the interface from the shared UI manifest, forwards input, displays engine-rendered tiles and overlays, and fulfils host-service requests.
- **Stack:** C++20, CMake, Qt 6 Quick (QML) for the interface, C++ only as glue. Track the newest Qt 6 minor release (Qt 6.12 at planning time) per ADR-0013, pinned exactly.
- **Qt licensing:** only LGPLv3 Qt modules, linked dynamically. Forbidden: Qt Charts, Qt Graphs, Qt GRPC, Qt Quick 3D, Qt Virtual Keyboard, Qt Canvas Painter and any other GPL-only or commercial-only module or tool. Compile QML with `qmlcachegen`, not the commercial `qmlsc`.
- **No other C++ dependencies** besides Qt (Qt Test for tests) without an ADR amendment.
- **Untrusted data stays in the engine (ADR-0006):** never use Qt to decode document content, embedded images, document fonts or clipboard payloads; pass raw bytes to the engine.
- **Engine boundary (ADR-0012):** talk to the engine only through the C ABI and the JSON protocol in `docs/specs/engine-protocol.md`. Engine callbacks arrive on the engine thread; marshal them to the Qt main thread and never call back into the engine synchronously from a callback.
- **Accessibility and input methods are features, not polish (ADR-0020):** every change to the canvas keeps screen-reader and IME support working.

## Verification gate

Install the pinned tools once per machine or session with `scripts/dev-setup.sh` (CMake, Ninja, clang-format and clang-tidy from `deps/requirements-tools.txt`, and Qt from `deps/qt.json` unless a matching Qt is already installed), then load the environment it prints (`. ~/.local/share/bayandocs/desktop-tools/env.sh`). The gate is one command, run from the repository root before every push:

```sh
cmake --workflow --preset verify
```

It configures an optimized build with warnings as errors and the Qt licensing check, builds, runs clang-format, clang-tidy and qmllint, and runs every test: the engine wrapper unit tests, the headless smoke test (`bayan-desktop --smoke-test` with the offscreen platform plugin) and the Qt licensing policy tests. CI also runs `cmake --workflow --preset asan` (AddressSanitizer and UndefinedBehaviorSanitizer, Linux), `cmake --workflow --preset ci` on macOS and `cmake --workflow --preset msvc` on Windows (from an x64 Visual Studio developer environment). `cmake --workflow --preset dev` is the quick Debug build and test, and `cmake --build --preset verify --target format` fixes formatting.

Notes for agents: the engine is the stub in `src/engine/stub/` until the bayan-core C SDK from CORE-007 exists (`-DBAYAN_ENGINE=sdk -DBAYAN_ENGINE_SDK_DIR=<path>`); `src/engine/stub/include/bayan_ffi.h` is a provisional copy of the engine's C interface that the SDK's generated header replaces. Use `bayan_find_qt()` instead of `find_package(Qt6 ...)`; a new Qt module must be reviewed and added to the allowlist in `cmake/BayanQtPolicy.cmake`.

## Dependency mechanisms

- **Qt:** the exact version, its release date, the aqtinstall version and the archives to install per platform are recorded in `deps/qt.json`. `scripts/dev-setup.sh` installs them with aqtinstall, which verifies every archive against the SHA-256 hash published on download.qt.io. A CI check that the release date is at least 24 hours before the commit is added by X-003.
- **Build and lint tools** (CMake, Ninja, clang-format, clang-tidy) and **aqtinstall** are Python packages pinned to exact versions with SHA-256 hashes in `deps/requirements-tools.txt` and `deps/requirements-aqtinstall.txt`; `scripts/dev-setup.sh` installs them with `pip --require-hashes --only-binary :all:`, so only verified prebuilt wheels are used and no package code runs at install time. Each file's header records publish dates and the command that regenerates it.
- **CI:** GitHub Actions are pinned to full commit SHAs with the version in a comment, the Python version is pinned in the workflow, and Ubuntu packages come from the Ubuntu archive frozen at a snapshot date (`UBUNTU_SNAPSHOT` in `.github/workflows/ci.yml`).
- Upgrades happen only in the monthly dependency session.

In BayanDocs cloud sessions the tools are preinstalled at pinned versions by `docs/scripts/cloud-environment-setup.sh`; run `bayandocs-tools` to list them. That script installs Qt but not this repository's pinned CMake, clang-format and clang-tidy, so run `scripts/dev-setup.sh` at the start of a session (it reuses the preinstalled Qt) unless the environment's setup script already does. If any other tool is missing, install the version pinned there (never a newer one) and mention it in the pull request.
