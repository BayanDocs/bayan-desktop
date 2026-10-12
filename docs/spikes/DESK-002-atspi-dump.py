#!/usr/bin/env python3
"""Reads the document canvas of a running bayan-desktop through AT-SPI, the way Orca does (DESK-002, AC-3 on Linux).

It finds the app on the accessibility bus, prints its accessible objects, then the canvas's role, name, states, text, caret and character
positions, inserts "hello" at the caret through the editable-text interface, and prints the text-changed and caret-moved events that come
back. It is evidence for the Linux bridge, not a replacement for a screen reader.

On Ubuntu 24.04 it needs python3-pyatspi, installed from Ubuntu's archive frozen at CI's date as the DESK-002 report's "How to reproduce"
shows, and the system's Python, `python3.12`, which the package was built for. On a desktop session, start the app built against the real
engine, then run this script. Without a desktop (a cloud computer), start the accessibility bus and the app under Xvfb first; "How to
reproduce" shows that too.
"""

from __future__ import annotations

import sys
import time

import pyatspi
from gi.repository import GLib

APP_NAME = "BayanDocs"
MAX_TEXT = 400


def find_app(timeout: float = 30.0):
    """Returns the app's accessible object once it is on the bus, or None."""
    deadline = time.monotonic() + timeout
    names: list[str | None] = []
    while time.monotonic() < deadline:
        desktop = pyatspi.Registry.getDesktop(0)
        names = [app.name if app is not None else None for app in desktop]
        for app in desktop:
            if app is not None and app.name == APP_NAME:
                return app
        time.sleep(0.5)
    print(f"applications on the bus: {names}")
    return None


def print_tree(accessible, depth: int = 0, editable: list | None = None) -> list:
    """Prints the accessible objects, and returns those that offer editable text."""
    editable = [] if editable is None else editable
    interfaces = sorted(pyatspi.listInterfaces(accessible))
    print("  " * depth + f"{accessible.getRoleName()!r} name={accessible.name!r} interfaces={interfaces}")
    if "EditableText" in interfaces:
        editable.append(accessible)
    for index in range(min(accessible.childCount, 50)):
        child = accessible.getChildAtIndex(index)
        if child is not None:
            print_tree(child, depth + 1, editable)
    return editable


def print_text(document) -> int:
    """Prints what a screen reader reads from the canvas, and returns the caret offset."""
    states = sorted(pyatspi.stateToString(state) for state in document.getState().getStates())
    text = document.queryText()
    count = text.characterCount
    caret = text.caretOffset
    print(f"\nrole: {document.getRoleName()}\nname: {document.name!r}\nstates: {states}")
    print(f"characters: {count}, caret offset: {caret}")
    print(f"text, the first {MAX_TEXT} characters:\n{text.getText(0, min(count, MAX_TEXT))!r}")
    for offset in (0, 1, caret):
        if 0 <= offset < count:
            print(f"extents of character {offset} (x, y, width, height): {tuple(text.getCharacterExtents(offset, pyatspi.DESKTOP_COORDS))}")
    print(f"first line: {text.getTextAtOffset(0, pyatspi.TEXT_BOUNDARY_LINE_START)[0]!r}")
    print(f"line at the caret: {text.getTextAtOffset(caret, pyatspi.TEXT_BOUNDARY_LINE_START)[0]!r}")
    return caret


def edit(document, caret: int) -> None:
    """Inserts text at the caret through AT-SPI and prints the events and the text that follow."""
    events: list[str] = []

    def record(event) -> None:
        events.append(f"{event.type} detail1={event.detail1} detail2={event.detail2} any_data={event.any_data!r}")

    print("\nediting through AT-SPI: inserting 'hello' at the caret")
    pyatspi.Registry.registerEventListener(record, "object:text-changed", "object:text-caret-moved")
    print(f"insertText returned {document.queryEditableText().insertText(caret, 'hello', 5)}")
    loop = GLib.MainLoop()
    GLib.timeout_add(1500, loop.quit)
    loop.run()
    pyatspi.Registry.deregisterEventListener(record, "object:text-changed", "object:text-caret-moved")
    text = document.queryText()
    print(f"caret offset now: {text.caretOffset}")
    print(f"line at the caret now: {text.getTextAtOffset(text.caretOffset, pyatspi.TEXT_BOUNDARY_LINE_START)[0]!r}")
    print("events received:")
    for entry in events[:20]:
        print(f"  {entry}")


def main() -> int:
    app = find_app()
    if app is None:
        print(f"{APP_NAME} did not appear on the accessibility bus", file=sys.stderr)
        return 1
    print(f"application: {app.name!r}, toolkit: {app.get_toolkit_name()} {app.get_toolkit_version()}")
    editable = print_tree(app)
    if not editable:
        print("no accessible object with editable text", file=sys.stderr)
        return 1
    document = editable[0]
    edit(document, print_text(document))
    return 0


if __name__ == "__main__":
    sys.exit(main())
