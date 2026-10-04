#!/usr/bin/env python3
"""Tests for scripts/install-qt.py, run by CTest (tests/CMakeLists.txt).

They build a small fake Qt repository on disk (an index, archives made with CMake and their published SHA-256 files) and run the installer
against it through file:// URLs, or through a small HTTP server on 127.0.0.1, so they need no network. Environment: BAYAN_INSTALL_QT (the
script) and BAYAN_TEST_CMAKE (a CMake).
"""

from __future__ import annotations

import contextlib
import functools
import hashlib
import http.server
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

INSTALLER = Path(os.environ.get("BAYAN_INSTALL_QT", Path(__file__).resolve().parents[2] / "scripts" / "install-qt.py"))
CMAKE = os.environ.get("BAYAN_TEST_CMAKE", "cmake")
REPOSITORY = "linux_x64/desktop/qt6_6120/qt6_6120"
PACKAGE = "qt.qt6.6120.linux_gcc_64"
VERSION = "6.12.0-0-202609280346"

sys.dont_write_bytecode = True  # do not leave a __pycache__ directory next to the script
spec = importlib.util.spec_from_file_location("install_qt", INSTALLER)
install_qt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_qt)


def can_make_links() -> bool:
    # Qt's Windows archives contain no links, and Windows resolves ".." in link targets differently, so the link tests run elsewhere.
    if sys.platform == "win32":
        return False
    with tempfile.TemporaryDirectory() as directory:
        try:
            os.symlink("target", Path(directory, "link"))
        except (OSError, NotImplementedError):
            return False
    return True


# The installer runs without proxy settings, so that requests to the local test server stay local.
INSTALLER_ENVIRONMENT = {name: value for name, value in os.environ.items() if not name.lower().endswith("_proxy")}
INSTALLER_ENVIRONMENT["NO_PROXY"] = INSTALLER_ENVIRONMENT["no_proxy"] = "127.0.0.1,localhost"


class QuietFileHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *arguments) -> None:
        pass


class RedirectingHandler(http.server.BaseHTTPRequestHandler):
    """Answers every request with a redirect, as download.qt.io does for archives it hands over to mirrors."""

    def do_GET(self) -> None:
        self.send_response(302)
        self.send_header("Location", "http://mirror.invalid/" + self.path.lstrip("/"))
        self.end_headers()

    def log_message(self, *arguments) -> None:
        pass


@contextlib.contextmanager
def http_server(handler):
    """Runs an HTTP server on 127.0.0.1 in a thread and yields its base URL."""
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class FakeRepository:
    """A Qt repository in a temporary directory: each archive is a dictionary of path -> file content, or "->target" for a link."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.archives: dict[str, tuple[str, dict[str, str]]] = {}

    def add(self, short_name: str, files: dict[str, str], destination: str = "@TargetDir@/6.12.0/gcc_64") -> str:
        name = f"{short_name}-Linux-Fake-X86_64.7z"
        self.archives[name] = (destination, files)
        return name

    def publish(self, *, wrong_index_hash: bool = False, wrong_hash_for: str | None = None, drop_operation_for: str | None = None) -> str:
        package_dir = self.root / REPOSITORY / PACKAGE
        package_dir.mkdir(parents=True, exist_ok=True)
        operations = ""
        for name, (destination, files) in self.archives.items():
            with tempfile.TemporaryDirectory() as staging:
                for path, content in files.items():
                    target = Path(staging, path)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if content.startswith("->"):
                        os.symlink(content[2:], target)
                    else:
                        target.write_text(content, encoding="utf-8")
                archive = package_dir / (VERSION + name)
                top_level = sorted({path.split("/")[0] for path in files})
                subprocess.run([CMAKE, "-E", "tar", "cf", str(archive), "--format=7zip", *top_level], cwd=staging, check=True)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            if name == wrong_hash_for:
                digest = "0" * 64
            Path(f"{archive}.sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
            if name != drop_operation_for:
                operations += f'<Operation name="Extract"><Argument>{destination}</Argument><Argument>{name}</Argument></Operation>'
        index = (
            "<Updates><ApplicationName>{AnyApplication}</ApplicationName>"
            f"<PackageUpdate><Name>qt.qt6.6120.linux_gcc_64.debug_info</Name><Version>{VERSION}</Version></PackageUpdate>"
            f"<PackageUpdate><Name>{PACKAGE}</Name><Version>{VERSION}</Version>"
            f"<DownloadableArchives>{', '.join(self.archives)}</DownloadableArchives>"
            f"<Operations>{operations}</Operations></PackageUpdate></Updates>"
        ).encode("utf-8")
        index_path = self.root / REPOSITORY / "Updates.xml"
        index_path.write_bytes(index)
        digest = "1" * 64 if wrong_index_hash else hashlib.sha256(index).hexdigest()
        Path(f"{index_path}.sha256").write_text(f"{digest}  Updates.xml\n", encoding="ascii")
        return self.root.as_uri()


class InstallerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="install-qt-test-"))
        self.repository = FakeRepository(self.work / "repository")
        self.prefix = self.work / "Qt"
        self.target = self.prefix / "6.12.0" / "gcc_64"

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def run_installer(self, url: str, archives: list[str], *extra: str) -> subprocess.CompletedProcess:
        pins = self.work / "qt.json"
        host = {"repository": REPOSITORY, "package": PACKAGE, "directory": "gcc_64", "archives": archives}
        pins.write_text(json.dumps({"qt": "6.12.0", "hosts": {"linux": host}}), encoding="utf-8")
        command = [sys.executable, str(INSTALLER), "--prefix", str(self.prefix), "--cmake", CMAKE, "--pins", str(pins), "--host", "linux",
                   "--index-base", url, "--archive-base", url, *extra]
        return subprocess.run(command, capture_output=True, text=True, check=False, env=INSTALLER_ENVIRONMENT)

    def assert_fails(self, result: subprocess.CompletedProcess, message: str) -> None:
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stdout)
        self.assertFalse(self.target.exists(), "nothing may be installed after a failure")
        self.assertEqual([path.name for path in self.prefix.glob(".*")], [], "temporary directories must be removed")

    def test_installs_archives_where_the_index_says(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base", "lib/cmake/Qt6Core/version.cmake": "6.12.0"})
        self.repository.add("icu", {"libicuuc.so.73": "icu"}, destination="@TargetDir@/6.12.0/gcc_64/lib")
        result = self.run_installer(self.repository.publish(), ["qtbase", "icu"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], str(self.target))
        self.assertEqual((self.target / "bin" / "qtbase.txt").read_text(encoding="utf-8"), "base")
        self.assertEqual((self.target / "lib" / "libicuuc.so.73").read_text(encoding="utf-8"), "icu")
        self.assertEqual((self.target / "bin" / "qt.conf").read_text(encoding="utf-8"), "[Paths]\nPrefix=..\n")
        marker = json.loads((self.target / ".bayandocs-qt.json").read_text(encoding="utf-8"))
        self.assertEqual(marker["package_version"], VERSION)
        self.assertEqual(sorted(marker["sha256"]), sorted(self.repository.archives))

    def test_second_run_does_nothing(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        url = self.repository.publish()
        self.assertEqual(self.run_installer(url, ["qtbase"]).returncode, 0)
        shutil.rmtree(self.repository.root)  # a second run must not need the repository
        result = self.run_installer(url, ["qtbase"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("already installed", result.stdout)

    def test_replaces_the_installation_when_the_pins_change(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        self.repository.add("qtsvg", {"lib/qtsvg.txt": "svg"})
        url = self.repository.publish()
        self.assertEqual(self.run_installer(url, ["qtbase"]).returncode, 0)
        self.assertFalse((self.target / "lib" / "qtsvg.txt").exists())
        result = self.run_installer(url, ["qtbase", "qtsvg"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.target / "lib" / "qtsvg.txt").exists())
        self.assertEqual([path.name for path in self.target.parent.iterdir()], ["gcc_64"], "the previous installation must be removed")

    def test_refuses_an_archive_with_the_wrong_hash(self) -> None:
        name = self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        result = self.run_installer(self.repository.publish(wrong_hash_for=name), ["qtbase"])
        self.assert_fails(result, "not installing it")

    def test_refuses_an_index_with_the_wrong_hash(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        result = self.run_installer(self.repository.publish(wrong_index_hash=True), ["qtbase"])
        self.assert_fails(result, "does not match the published one")

    def test_refuses_an_archive_that_the_index_does_not_list(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        result = self.run_installer(self.repository.publish(), ["qtbase", "qtcharts"])
        self.assert_fails(result, "archives named qtcharts-*.7z instead of one")

    def test_refuses_an_archive_without_a_destination(self) -> None:
        name = self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        result = self.run_installer(self.repository.publish(drop_operation_for=name), ["qtbase"])
        self.assert_fails(result, "does not say where")

    def test_refuses_a_destination_outside_the_installation(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"}, destination="@TargetDir@/6.12.0/gcc_64/../../escape")
        result = self.run_installer(self.repository.publish(), ["qtbase"])
        self.assert_fails(result, "unsafe path")

    def test_refuses_an_oversized_archive(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base" * 100})
        result = self.run_installer(self.repository.publish(), ["qtbase"], "--max-archive-bytes", "64")
        self.assert_fails(result, "larger than the limit")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_installs_links_inside_the_installation(self) -> None:
        self.repository.add("qtbase", {"lib/libQt6Core.so.6.12.0": "core", "lib/libQt6Core.so.6": "->libQt6Core.so.6.12.0"})
        result = self.run_installer(self.repository.publish(), ["qtbase"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.target / "lib" / "libQt6Core.so.6").read_text(encoding="utf-8"), "core")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_refuses_a_link_that_points_outside(self) -> None:
        self.repository.add("qtbase", {"lib/escape": "->../../../../etc"})
        result = self.run_installer(self.repository.publish(), ["qtbase"])
        self.assert_fails(result, "link points outside the installation")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_refuses_a_chain_of_links_that_points_outside(self) -> None:
        # Each link stays inside on its own ("lib/up" is the root, "escape" is "lib"), but followed together they leave the installation.
        self.repository.add("qtbase", {"lib/up": "->..", "escape": "->lib/up/.."})
        result = self.run_installer(self.repository.publish(), ["qtbase"])
        self.assert_fails(result, "link points outside the installation")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_refuses_a_later_archive_that_writes_through_an_escaping_link(self) -> None:
        # The case from the review: each link stays inside on its own, but "esc" leads two levels above the temporary directory, and a later
        # archive writes through it. Nothing may be written outside the prefix, which sits deep inside a sandbox so that any escape lands
        # somewhere the test can see.
        sandbox = self.work / "sandbox"
        self.prefix = sandbox / "one" / "two" / "Qt"
        self.target = self.prefix / "6.12.0" / "gcc_64"
        self.repository.add("qtbase", {"lib/a/up": "->../..", "esc": "->lib/a/up/../.."})
        self.repository.add("qtsvg", {"esc/evil.txt": "evil"})
        result = self.run_installer(self.repository.publish(), ["qtbase", "qtsvg"])
        self.assert_fails(result, "link points outside the installation")
        self.assertEqual([str(path.relative_to(self.work)) for path in self.work.rglob("evil.txt")], [])
        self.assertEqual([str(path.relative_to(sandbox)) for path in sandbox.rglob("*") if not path.is_relative_to(self.prefix)],
                         ["one", "one/two"], "nothing may be created outside the prefix")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_refuses_a_later_archive_that_writes_through_an_earlier_link(self) -> None:
        self.repository.add("qtbase", {"lib/inner": "->other", "lib/other/core.txt": "core"})
        self.repository.add("qtsvg", {"lib/inner/svg.txt": "svg"})
        result = self.run_installer(self.repository.publish(), ["qtbase", "qtsvg"])
        self.assert_fails(result, "archive path passes through a link")

    @unittest.skipUnless(can_make_links(), "links are tested on Linux and macOS")
    def test_refuses_a_destination_that_is_a_link(self) -> None:
        self.repository.add("qtbase", {"lib": "->other", "other/core.txt": "core"})
        self.repository.add("icu", {"libicuuc.so.73": "icu"}, destination="@TargetDir@/6.12.0/gcc_64/lib")
        result = self.run_installer(self.repository.publish(), ["qtbase", "icu"])
        self.assert_fails(result, "is or passes through a link")

    def test_installs_over_http(self) -> None:
        self.repository.add("qtbase", {"bin/qtbase.txt": "base"})
        self.repository.publish()
        with http_server(functools.partial(QuietFileHandler, directory=str(self.repository.root))) as url:
            result = self.run_installer(url, ["qtbase"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.target / "bin" / "qtbase.txt").read_text(encoding="utf-8"), "base")

    def test_refuses_a_redirect(self) -> None:
        with http_server(RedirectingHandler) as url:
            result = self.run_installer(url, ["qtbase"])
        self.assert_fails(result, "redirected to http://mirror.invalid/")


class EntryCheckTest(unittest.TestCase):
    """The checks applied to every archive entry before anything is extracted."""

    def test_accepts_ordinary_entries(self) -> None:
        install_qt.check_entries([
            ("bin/", "d", None),
            ("bin/moc", "-", None),
            ("lib/QtCore.framework/Versions/Current", "l", "A"),
            ("lib/QtCore.framework/QtCore", "l", "Versions/Current/QtCore"),
            ("lib/libQt6Core.so", "l", "libQt6Core.so.6"),
        ])

    def test_refuses_unsafe_paths(self) -> None:
        for name in ("../escape", "/etc/passwd", "lib/../../escape", "C:/Windows/evil.dll", "lib\\evil", "./lib", "lib//x", ""):
            with self.subTest(name=name), self.assertRaises(install_qt.InstallError):
                install_qt.check_entries([(name, "-", None)])

    def test_refuses_other_entry_types(self) -> None:
        for kind in ("h", "c", "b", "p", "s"):
            with self.subTest(kind=kind), self.assertRaises(install_qt.InstallError):
                install_qt.check_entries([("lib/device", kind, None)])

    def test_refuses_links_that_leave_the_installation(self) -> None:
        for target in ("/etc", "../../escape", "C:\\Windows", "a/../../../escape", ""):
            with self.subTest(target=target), self.assertRaises(install_qt.InstallError):
                install_qt.check_entries([("lib/link", "l", target)])

    def test_refuses_paths_through_a_link(self) -> None:
        with self.assertRaises(install_qt.InstallError):
            install_qt.check_entries([("lib/inner", "l", "sub"), ("lib/inner/file", "-", None)])

    def test_refuses_a_path_through_a_link_that_differs_only_in_case(self) -> None:
        # On macOS's default file system "ESC" is "esc", so this file would be written through the link.
        entries = [("lib/a/up", "l", "../.."), ("esc", "l", "lib/a/up/../.."), ("ESC/evil.txt", "-", None)]
        with self.assertRaisesRegex(install_qt.InstallError, "passes through a link"):
            install_qt.check_entries(entries)

    def test_refuses_paths_through_a_link_written_differently(self) -> None:
        # Unicode canonical equivalents (macOS) and trailing dots and spaces (Windows) name the same file.
        for path in ("lib/cafe\u0301/file", "lib/caf\u00e9./file", "lib/CAF\u00c9 /file"):
            with self.subTest(path=path), self.assertRaisesRegex(install_qt.InstallError, "passes through a link"):
                install_qt.check_entries([("lib/caf\u00e9", "l", "sub"), (path, "-", None)])

    def test_refuses_names_that_differ_only_in_case(self) -> None:
        for first, second in ((("lib/Foo.txt", "-"), ("lib/foo.txt", "-")), (("lib/x/", "d"), ("lib/X", "-")),
                              (("lib/caf\u00e9", "-"), ("lib/cafe\u0301", "-")), (("lib/link", "l"), ("lib/LINK/", "d"))):
            with self.subTest(names=(first[0], second[0])), self.assertRaisesRegex(install_qt.InstallError, "same name"):
                install_qt.check_entries([(first[0], first[1], "target" if first[1] == "l" else None), (second[0], second[1], None)])

    def test_accepts_directories_that_differ_only_in_case(self) -> None:
        install_qt.check_entries([("lib/Dir/", "d", None), ("lib/dir/", "d", None), ("lib/dir", "d", None)])

    def test_refuses_names_made_only_of_dots_and_spaces(self) -> None:
        for name in ("lib/.../x", "lib/ /x", "lib/. ./x"):
            with self.subTest(name=name), self.assertRaisesRegex(install_qt.InstallError, "unsafe path"):
                install_qt.check_entries([(name, "-", None)])

    def test_carries_links_across_archives(self) -> None:
        links = install_qt.check_entries([("lib/Inner", "l", "other"), ("lib/other/", "d", None)])
        self.assertEqual(links, {"lib/inner"})
        with self.assertRaisesRegex(install_qt.InstallError, "passes through a link"):
            install_qt.check_entries([("lib/inner/file", "-", None)], known_links=links)
        with self.assertRaisesRegex(install_qt.InstallError, "would replace a link"):
            install_qt.check_entries([("lib/INNER", "-", None)], known_links=links)
        with self.assertRaisesRegex(install_qt.InstallError, "passes through a link"):
            install_qt.check_entries([("inner/file", "-", None)], subdirectory="lib", known_links=links)

    def test_resolves_links_relative_to_the_installation(self) -> None:
        # An archive extracted into lib/ may link to its parent, which is still inside the installation, but not further.
        self.assertEqual(install_qt.check_entries([("current", "l", "../bin")], subdirectory="lib"), {"lib/current"})
        with self.assertRaisesRegex(install_qt.InstallError, "points outside"):
            install_qt.check_entries([("escape", "l", "../../bin")], subdirectory="lib")

    def test_refuses_a_destination_through_a_known_link(self) -> None:
        with tempfile.TemporaryDirectory() as staging:
            install_qt.check_destination(Path(staging), "lib", set())
            with self.assertRaisesRegex(install_qt.InstallError, "is or passes through a link"):
                install_qt.check_destination(Path(staging), "lib/sub", {"lib"})
            with self.assertRaisesRegex(install_qt.InstallError, "is or passes through a link"):
                install_qt.check_destination(Path(staging), "LIB", {"lib"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
