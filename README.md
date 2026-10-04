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

GPL-3.0-or-later. See the [licensing FAQ](https://github.com/BayanDocs/docs/blob/HEAD/LICENSING.md) and [ADR-0003](https://github.com/BayanDocs/docs/blob/HEAD/adr/0003-licensing-and-contribution-model.md). The license file is being added by work package X-001; until then, all rights are reserved.
