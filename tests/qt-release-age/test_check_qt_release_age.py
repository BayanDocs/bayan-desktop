#!/usr/bin/env python3
"""Tests for scripts/check-qt-release-age.py, run by CTest (tests/CMakeLists.txt).

They make small Git repositories with deps/qt.json committed at chosen times, so they need no network and do not depend on this repository's
history. Environment: BAYAN_CHECK_QT_RELEASE_AGE (the script).
"""

from __future__ import annotations

import contextlib
import datetime
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(os.environ.get("BAYAN_CHECK_QT_RELEASE_AGE", Path(__file__).resolve().parents[2] / "scripts" / "check-qt-release-age.py"))

sys.dont_write_bytecode = True  # do not leave a __pycache__ directory next to the script
spec = importlib.util.spec_from_file_location("check_qt_release_age", SCRIPT)
check_qt_release_age = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check_qt_release_age  # dataclasses look their module up there
spec.loader.exec_module(check_qt_release_age)

UTC = datetime.timezone.utc


def at(text: str) -> datetime.datetime:
    return datetime.datetime.fromisoformat(text).replace(tzinfo=UTC)


class Repository:
    """A temporary Git repository, isolated from the configuration of the machine running the tests."""

    def __init__(self, folder: Path) -> None:
        self.root = folder / "repository"
        self.root.mkdir()
        config = folder / "gitconfig"
        config.write_text("", encoding="utf-8")
        self.environment = dict(os.environ)
        self.environment.update(
            GIT_CONFIG_GLOBAL=str(config),
            GIT_CONFIG_NOSYSTEM="1",
            GIT_AUTHOR_NAME="Test",
            GIT_AUTHOR_EMAIL="test@bayandocs.invalid",
            GIT_COMMITTER_NAME="Test",
            GIT_COMMITTER_EMAIL="test@bayandocs.invalid",
        )
        self.git("init", "--quiet")

    def git(self, *arguments: str, when: datetime.datetime | None = None) -> str:
        environment = dict(self.environment)
        if when is not None:
            environment["GIT_AUTHOR_DATE"] = environment["GIT_COMMITTER_DATE"] = f"@{int(when.timestamp())} +0000"
        result = subprocess.run(
            ["git", "-C", str(self.root), "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false", *arguments],
            capture_output=True,
            text=True,
            env=environment,
        )
        if result.returncode != 0:
            raise AssertionError(f"git {arguments} failed: {result.stderr}")
        return result.stdout

    def pin(self, version: str, released: str, archives: tuple[str, ...] = ("qtbase",)) -> None:
        path = self.root / "deps" / "qt.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps({"qt": version, "qt_released": released, "hosts": {"linux": {"archives": list(archives)}}}, indent=2) + "\n", encoding="utf-8")

    def commit(self, message: str, when: datetime.datetime) -> str:
        self.git("add", "--all")
        self.git("commit", "--quiet", "--allow-empty", "--message", message, when=when)
        return self.git("rev-parse", "HEAD").strip()

    def check(self, now: datetime.datetime) -> tuple[bool, str]:
        return check_qt_release_age.check(self.root, self.root / "deps" / "qt.json", now)


class QtReleaseAgeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._folder = tempfile.TemporaryDirectory()
        self.folder = Path(self._folder.name)
        self.repository = Repository(self.folder)

    def tearDown(self) -> None:
        self._folder.cleanup()

    def test_passes_a_release_pinned_days_later(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        commit = self.repository.commit("pin Qt 6.12.0", at("2026-10-04T22:18:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertTrue(ok, message)
        self.assertIn("counted as 2026-10-01 00:00 UTC", message)
        self.assertIn(f"commit {commit[:12]}", message)
        self.assertIn("3 d 22 h before it was pinned", message)

    def test_fails_a_release_pinned_on_the_next_day(self) -> None:
        # Released on 2026-09-30, so counted as released at 2026-10-01 00:00 UTC: a pin at 15:00 that day is 15 hours too early.
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-10-01T15:00:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertFalse(ok)
        self.assertIn("only 15 h 0 min before it was pinned", message)
        self.assertIn("it may be pinned from 2026-10-02 00:00 UTC on", message)

    def test_fails_a_pin_made_before_the_release_date_ended(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-09-30T20:00:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertFalse(ok)
        self.assertIn("4 h 0 min after it was pinned", message)

    def test_accepts_exactly_one_day(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-10-02T00:00:00"))
        self.assertTrue(self.repository.check(at("2026-10-07T03:00:00"))[0])

    def test_measures_from_the_commit_that_pinned_the_version(self) -> None:
        self.repository.pin("6.11.2", "2026-08-20")
        self.repository.commit("pin Qt 6.11.2", at("2026-08-25T10:00:00"))
        self.repository.pin("6.12.0", "2026-09-30")
        first = self.repository.commit("pin Qt 6.12.0", at("2026-10-01T12:00:00"))
        # A later change of another field, and a later correction of the date, do not make the version older than it was when it was pinned.
        self.repository.pin("6.12.0", "2026-09-30", archives=("qtbase", "qtsvg"))
        self.repository.commit("add qtsvg", at("2026-10-05T10:00:00"))
        self.repository.pin("6.12.0", "2026-09-29", archives=("qtbase", "qtsvg"))
        self.repository.commit("correct the release date", at("2026-10-06T10:00:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertTrue(ok, message)
        self.assertIn(f"commit {first[:12]}", message)
        # With the recorded date, the same first commit would have been too early.
        self.repository.pin("6.12.0", "2026-09-30", archives=("qtbase", "qtsvg"))
        self.repository.commit("restore the date", at("2026-10-06T11:00:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertFalse(ok, message)
        self.assertIn(f"commit {first[:12]}", message)

    def test_measures_an_uncommitted_pin_against_now(self) -> None:
        self.repository.pin("6.11.2", "2026-08-20")
        self.repository.commit("pin Qt 6.11.2", at("2026-08-25T10:00:00"))
        self.repository.pin("6.12.0", "2026-09-30")
        ok, message = self.repository.check(at("2026-10-01T20:00:00"))
        self.assertFalse(ok)
        self.assertIn("now (the pin is not committed yet)", message)
        self.assertTrue(self.repository.check(at("2026-10-02T00:00:00"))[0])

    def test_measures_a_commit_dated_in_the_future_against_now(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-12-01T00:00:00"))
        ok, message = self.repository.check(at("2026-10-01T12:00:00"))
        self.assertFalse(ok)
        self.assertIn("is dated in the future", message)

    def test_takes_the_earlier_of_author_and_committer_time(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-10-01T10:00:00"))
        # Amending later (as a rebase does) gives the commit a new committer time but keeps its author time.
        self.repository.git("commit", "--quiet", "--amend", "--no-edit", "--date", "@1790848800 +0000", when=at("2026-10-06T00:00:00"))
        ok, message = self.repository.check(at("2026-10-07T03:00:00"))
        self.assertFalse(ok, message)
        self.assertIn("2026-10-01 10:00 UTC", message)

    def test_refuses_a_shallow_clone(self) -> None:
        self.repository.pin("6.12.0", "2026-09-30")
        self.repository.commit("pin Qt 6.12.0", at("2026-10-04T00:00:00"))
        self.repository.pin("6.12.0", "2026-09-30", archives=("qtbase", "qtsvg"))
        self.repository.commit("add qtsvg", at("2026-10-05T00:00:00"))
        clone = self.folder / "clone"
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", self.repository.root.as_uri(), str(clone)], check=True, capture_output=True, env=self.repository.environment)
        with self.assertRaisesRegex(check_qt_release_age.CheckError, "shallow"):
            check_qt_release_age.check(clone, clone / "deps" / "qt.json", at("2026-10-07T03:00:00"))

    def test_rejects_unreadable_pins(self) -> None:
        for text in ['{"qt": "6.12.0"}', '{"qt": "6.12.0", "qt_released": "30.09.2026"}', '{"qt": "6.12.0", "qt_released": "20260930"}', "not json"]:
            with self.subTest(text=text):
                with self.assertRaises(check_qt_release_age.CheckError):
                    check_qt_release_age.read_pin(text, "deps/qt.json")

    def test_skips_a_source_tree_without_git(self) -> None:
        folder = self.folder / "export"
        (folder / "deps").mkdir(parents=True)
        (folder / "deps" / "qt.json").write_text('{"qt": "6.12.0", "qt_released": "2026-09-30"}', encoding="utf-8")
        if check_qt_release_age.in_git(folder):
            self.skipTest("the temporary folder is inside a Git working tree")
        with contextlib.redirect_stdout(io.StringIO()) as output:
            status = check_qt_release_age.main(["--root", str(folder)])
        self.assertEqual(status, check_qt_release_age.NOT_IN_GIT)
        self.assertIn("not in a Git checkout", output.getvalue())

    def test_this_repository_pins_qt_by_its_own_rules(self) -> None:
        pins = json.loads((SCRIPT.parents[1] / "deps" / "qt.json").read_text(encoding="utf-8"))
        pin = check_qt_release_age.read_pin(json.dumps(pins), "deps/qt.json")
        self.assertRegex(pin.version, r"^\d+\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
