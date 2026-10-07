#!/usr/bin/env python3
"""Check that the pinned Qt release was at least 24 hours old when it was pinned (ADR-0017 rule 4; work package X-003).

deps/qt.json records the exact Qt version ("qt") and the date it was released ("qt_released", as YYYY-MM-DD). Qt has no release-age gate of
its own, so this script enforces the rule:

1. It finds the commit that pinned the version: going back from HEAD through the commits that changed deps/qt.json, the oldest one whose
   file already names this version, before an older commit names another. A later commit that changes only other fields (an archive, the
   release date) keeps the version's age. A version that is not committed yet is measured against now.
2. A release date has no time of day, so the release is taken to have happened at the end of that day in UTC: Qt 6.12.0, released on
   2026-09-30, counts as released at 2026-10-01 00:00 UTC and may be pinned from 2026-10-02 00:00 UTC on.
3. It fails if the release was less than 24 hours before the commit that pinned it (the earlier of the commit's author and committer times,
   so that rebasing cannot make a pin look younger), or less than 24 hours before now, so that a commit dated in the future cannot pass.

The check reads the Git history, so it needs the full history: a shallow clone fails (in GitHub Actions, check out with fetch-depth: 0). In a
source tree without Git, there is nothing to check, and the script exits with status 77, which CTest reports as a skipped test.

Usage: check-qt-release-age.py [--root DIR] [--pins FILE]   (default: this repository and its deps/qt.json)
Exit status: 0 when the pin is old enough, 1 when it is too young, 2 when the check could not run, 77 when the source tree is not in Git.
"""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINS = Path("deps") / "qt.json"
MINIMUM_AGE = datetime.timedelta(hours=24)
NOT_IN_GIT = 77


class CheckError(Exception):
    """The check could not run (exit status 2)."""


@dataclass(frozen=True)
class Pin:
    version: str
    released: datetime.date

    @property
    def released_at(self) -> datetime.datetime:
        """The latest moment of the release day, in UTC: the start of the next day."""
        return datetime.datetime.combine(self.released + datetime.timedelta(days=1), datetime.time(), datetime.timezone.utc)


@dataclass(frozen=True)
class Commit:
    id: str
    time: datetime.datetime


def read_pin(text: str, where: str) -> Pin:
    """The Qt version and release date of a deps/qt.json."""
    try:
        pins = json.loads(text)
        version, released = pins["qt"], pins["qt_released"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise CheckError(f"{where} has no readable \"qt\" and \"qt_released\": {error}") from error
    if not isinstance(version, str) or not isinstance(released, str):
        raise CheckError(f"{where}: \"qt\" and \"qt_released\" must be strings")
    try:
        if len(released) != 10:
            raise ValueError(released)
        date = datetime.date.fromisoformat(released)
    except ValueError as error:
        raise CheckError(f"{where}: \"qt_released\" must be a date written as YYYY-MM-DD, not {released!r}") from error
    return Pin(version, date)


def git(root: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, text=True)
    except OSError as error:
        raise CheckError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        raise CheckError(f"git {' '.join(arguments)} failed: {result.stderr.strip()}")
    return result.stdout


def in_git(root: Path) -> bool:
    try:
        result = subprocess.run(["git", "-C", str(root), "rev-parse", "--is-inside-work-tree"], capture_output=True, text=True)
    except OSError:
        return False
    return result.returncode == 0 and result.stdout.strip() == "true"


def file_at(root: Path, commit: str, path: str) -> str | None:
    """The text of `path` (relative to the repository's top folder) at `commit`, or None if it did not exist there."""
    exists = subprocess.run(["git", "-C", str(root), "cat-file", "-e", f"{commit}:{path}"], capture_output=True)
    if exists.returncode != 0:
        return None
    return git(root, "cat-file", "blob", f"{commit}:{path}")


def pinning_commit(root: Path, pins: Path, version: str) -> Commit | None:
    """The commit that pinned `version`, as described in the module documentation, or None if HEAD does not pin it yet."""
    if git(root, "rev-parse", "--is-shallow-repository").strip() == "true":
        raise CheckError("this clone is shallow, so it lacks the commit that pinned Qt. Fetch the full history (git fetch --unshallow); in GitHub Actions, check out with fetch-depth: 0.")
    top = Path(git(root, "rev-parse", "--show-toplevel").strip())
    try:
        path = pins.resolve().relative_to(top.resolve()).as_posix()
    except ValueError as error:
        raise CheckError(f"{pins} is not inside the repository {top}") from error
    found: Commit | None = None
    for line in git(root, "log", "--topo-order", "--format=%H %at %ct", "--", path).splitlines():
        commit, author, committer = line.split()
        text = file_at(root, commit, path)
        if text is None or read_pin(text, f"{path} at {commit[:12]}").version != version:
            break
        found = Commit(commit, datetime.datetime.fromtimestamp(min(int(author), int(committer)), datetime.timezone.utc))
    if found is None:
        return None
    head = file_at(root, "HEAD", path)
    if head is None or read_pin(head, f"{path} at HEAD").version != version:
        return None
    return found


def show(moment: datetime.datetime) -> str:
    return moment.strftime("%Y-%m-%d %H:%M UTC")


def length(delta: datetime.timedelta) -> str:
    minutes = int(abs(delta).total_seconds()) // 60
    days, minutes = divmod(minutes, 24 * 60)
    hours, minutes = divmod(minutes, 60)
    return f"{days} d {hours} h" if days else f"{hours} h {minutes} min"


def check(root: Path, pins: Path, now: datetime.datetime) -> tuple[bool, str]:
    """Whether the Qt pin in `pins` is old enough, with the explanation. `now` is the current time (UTC)."""
    pin = read_pin(pins.read_text(encoding="utf-8"), str(pins))
    commit = pinning_commit(root, pins, pin.version)
    if commit is None:
        reference, when = now, "now (the pin is not committed yet)"
    elif commit.time > now:
        reference, when = now, f"now (the commit {commit.id[:12]} that pinned it is dated in the future, {show(commit.time)})"
    else:
        reference, when = commit.time, f"it was pinned (commit {commit.id[:12]}, {show(commit.time)})"
    age = reference - pin.released_at
    counted = f"Qt {pin.version}, released on {pin.released.isoformat()} (counted as {show(pin.released_at)}, the end of that day)"
    if age >= MINIMUM_AGE:
        return True, f"{counted}, was released {length(age)} before {when}: at least 24 hours (ADR-0017 rule 4)."
    earliest = show(pin.released_at + MINIMUM_AGE)
    if age < datetime.timedelta(0):
        return False, f"{counted}, was released {length(age)} after {when}. ADR-0017 rule 4 requires 24 hours; it may be pinned from {earliest} on."
    return False, f"{counted}, was released only {length(age)} before {when}. ADR-0017 rule 4 requires 24 hours; it may be pinned from {earliest} on."


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check that the pinned Qt release was at least 24 hours old when it was pinned (ADR-0017 rule 4).")
    parser.add_argument("--root", type=Path, default=ROOT, help="the repository (default: the one this script is in)")
    parser.add_argument("--pins", type=Path, help="the pin file (default: deps/qt.json in the repository)")
    options = parser.parse_args(arguments)
    pins = options.pins or options.root / PINS
    if not in_git(options.root):
        print(f"skipped: {options.root} is not in a Git checkout, so there is no history to check the Qt pin against")
        return NOT_IN_GIT
    try:
        ok, message = check(options.root, pins, datetime.datetime.now(datetime.timezone.utc))
    except (CheckError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(message, file=sys.stdout if ok else sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
