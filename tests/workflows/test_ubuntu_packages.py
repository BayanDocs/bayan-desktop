#!/usr/bin/env python3
"""Keeps the Ubuntu package pins of the two workflows that install them equal (work package X-002), run by CTest (tests/CMakeLists.txt).

.github/workflows/ci.yml and the C++ job of .github/workflows/codeql.yml both install the Ubuntu packages that Qt needs to build, from the
Ubuntu archive frozen at a date: UBUNTU_SNAPSHOT and UBUNTU_PACKAGES. CodeQL has to analyse the same build that CI tests, and the monthly
dependency session moves the snapshot in both files, so this test fails as soon as the two differ or either file stops setting one of them.
It reads the workflows as text, with the Python standard library only. Environment: BAYAN_SOURCE_DIR (the repository; by default the one
this file is in).
"""

from __future__ import annotations

import os
import re
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("BAYAN_SOURCE_DIR", Path(__file__).resolve().parents[2]))
WORKFLOWS = ROOT / ".github" / "workflows"
NAMES = ("UBUNTU_SNAPSHOT", "UBUNTU_PACKAGES")

# A YAML mapping line such as "  UBUNTU_SNAPSHOT: 20261003T000000Z", with an optional comment after the value.
SETTING = re.compile(r"\s*(UBUNTU_SNAPSHOT|UBUNTU_PACKAGES):[ \t]*(?P<value>[^#]*?)[ \t]*(?:#.*)?")


def settings(workflow: Path) -> dict[str, list[str]]:
    """Every value that the workflow gives each name, in the order in which they appear."""
    found: dict[str, list[str]] = {name: [] for name in NAMES}
    for line in workflow.read_text(encoding="utf-8").splitlines():
        match = SETTING.fullmatch(line)
        if match:
            found[match.group(1)].append(match.group("value"))
    return found


class UbuntuPackagePins(unittest.TestCase):
    def test_ci_and_codeql_install_the_same_ubuntu_packages(self) -> None:
        ci = settings(WORKFLOWS / "ci.yml")
        codeql = settings(WORKFLOWS / "codeql.yml")
        for name in NAMES:
            with self.subTest(name=name):
                self.assertEqual(len(ci[name]), 1, f"ci.yml must set {name} exactly once")
                self.assertEqual(len(codeql[name]), 1, f"codeql.yml must set {name} exactly once")
                self.assertNotEqual(ci[name][0], "", f"{name} is empty in ci.yml")
                self.assertEqual(codeql[name], ci[name], f"{name} differs between ci.yml and codeql.yml; keep them equal")

    def test_the_reader_finds_values_with_and_without_a_comment(self) -> None:
        self.assertEqual(SETTING.fullmatch("  UBUNTU_SNAPSHOT: 20261003T000000Z").group("value"), "20261003T000000Z")
        self.assertEqual(SETTING.fullmatch("      UBUNTU_PACKAGES: a b # c").group("value"), "a b")
        self.assertIsNone(SETTING.fullmatch("  # UBUNTU_SNAPSHOT: 20261003T000000Z"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
