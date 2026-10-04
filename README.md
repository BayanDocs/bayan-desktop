# bayan-desktop

The native desktop application of [BayanDocs](https://github.com/BayanDocs/docs) for Windows, macOS and Linux: a free, open-source word processor that matches Microsoft Word's document fidelity, works fully offline, starts instantly and uses little memory.

This repository is a deliberately **thin shell**: windows, menus, the ribbon and dialogs (rendered from the shared UI manifest), the document canvas, input methods, the bridge to operating-system screen readers, printing, file associations, installers and updates. Every behavior that concerns documents lives in [bayan-core](https://github.com/BayanDocs/bayan-core), which this app links through a small C interface, so the desktop and [web](https://github.com/BayanDocs/bayan-web) apps lay out documents identically.

Built with C++20 and Qt 6 (Qt Quick), using only LGPL-licensed Qt modules.

> **Status: Phase 0 (Foundations).** No code yet. The first work package is [DESK-001](https://github.com/BayanDocs/docs/blob/HEAD/workpackages/phase-0/DESK-001-desktop-scaffold.md).

## Where things are decided

- Master plan: [docs/plan](https://github.com/BayanDocs/docs/tree/HEAD/plan)
- Desktop decision: [ADR-0013](https://github.com/BayanDocs/docs/blob/HEAD/adr/0013-desktop-shell.md); engine boundary: [ADR-0012](https://github.com/BayanDocs/docs/blob/HEAD/adr/0012-engine-boundary.md) and the [engine protocol](https://github.com/BayanDocs/docs/blob/HEAD/specs/engine-protocol.md)
- Work packages: [docs/workpackages](https://github.com/BayanDocs/docs/tree/HEAD/workpackages)

Contributors and agents: start with [AGENTS.md](AGENTS.md).

## License

Licensed under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)) with the [BayanDocs App Store Permission](LICENSES/LicenseRef-BayanDocs-App-Store-Permission.txt), an additional permission under GPLv3 section 7 that allows distribution through app stores as long as the source code stays freely available to everyone. SPDX: `GPL-3.0-or-later WITH LicenseRef-BayanDocs-App-Store-Permission`. [REUSE.toml](REUSE.toml) records which license applies to which files, and [LICENSES/](LICENSES) holds the full texts. See the [licensing FAQ](https://github.com/BayanDocs/docs/blob/HEAD/LICENSING.md) and [ADR-0003](https://github.com/BayanDocs/docs/blob/HEAD/adr/0003-licensing-and-contribution-model.md).
