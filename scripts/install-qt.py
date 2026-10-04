#!/usr/bin/env python3
"""Install the Qt release pinned in deps/qt.json from Qt's official repository, verifying every download.

bayan-desktop's own replacement for aqtinstall (ADR-0017, amendment of 2026-10-04). It uses the Python standard library only, so no
third-party code runs while Qt is installed. What it does:

1. Downloads the repository index (Updates.xml) for this platform from download.qt.io and checks it against the SHA-256 published next to it.
   Every download refuses HTTP redirects, so files come only from the hosts named here and never from mirrors.
2. Finds the pinned Qt package in the index, the archives that deps/qt.json lists (qtbase, qtdeclarative and so on), and where the index says
   each one belongs (most go to the root of the installation; ICU on Linux goes to lib/).
3. Downloads each archive from Qt's master server and checks it against the SHA-256 that download.qt.io publishes for it, the same check
   aqtinstall makes. A file whose hash does not match is never extracted.
4. Lists every entry of each archive before extracting it, and refuses the archive if any entry
   - has an unsafe name: absolute, with a "." or ".." component or one made only of dots and spaces, or with Windows separators or a drive;
   - is anything other than a plain file, a directory or a symbolic link;
   - has the same name as another entry when names are compared the way macOS and Windows file systems compare them (ignoring case,
     treating Unicode canonical equivalents as equal and ignoring trailing dots and spaces), unless both are directories;
   - is a link whose target, resolved inside the installation, points outside it;
   - passes through, or would replace, a link of this archive or of an archive extracted before it (compared the same way).
5. Refuses an archive whose destination directory is or passes through a link, extracts it with the pinned CMake (cmake -E tar) into a
   temporary directory next to the target, and then checks that no link in that directory resolves outside it (this also catches chains of
   links) before the next archive is extracted. Only a complete installation is moved into place, so an interrupted run never leaves a
   half-installed Qt behind.
6. Writes bin/qt.conf as Qt's installer does, and records what it installed in .bayandocs-qt.json, so running it again with the same pins
   does nothing. (Unlike aqtinstall, it does not rewrite the build machine's paths in the pkg-config and qmake files, which a CMake build
   never reads.)

Usage: install-qt.py --prefix DIR --cmake CMAKE [--pins deps/qt.json] [--host linux|macos|windows]
Qt ends up in DIR/<version>/<directory>, a path printed on the last line of the output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path

INDEX_BASE = "https://download.qt.io/online/qtsdkrepository"
ARCHIVE_BASE = "https://master.qt.io/online/qtsdkrepository"
MAX_INDEX_BYTES = 16 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MARKER = ".bayandocs-qt.json"
USER_AGENT = "bayan-desktop-install-qt/1"
ATTEMPTS = 3

SHA256_LINE = re.compile(r"([0-9a-f]{64})\s+\*?(\S+)\s*")
SAFE_FILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]*")
SAFE_PACKAGE_VERSION = re.compile(r"[0-9][0-9A-Za-z.+-]*")
WINDOWS_DRIVE = re.compile(r"[A-Za-z]:")


class InstallError(Exception):
    """A problem that stops the installation; the message explains it."""


def say(message: str) -> None:
    print(f"install-qt: {message}", flush=True)


def ssl_context() -> ssl.SSLContext:
    """The default TLS settings, plus the system's certificate bundle where Python's own build finds none (as on some macOS builds)."""
    context = ssl.create_default_context()
    if not os.environ.get("SSL_CERT_FILE") and not context.get_ca_certs():
        for bundle in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"):
            if os.path.isfile(bundle):
                context.load_verify_locations(cafile=bundle)
                break
    return context


class _RefuseRedirects(urllib.request.HTTPRedirectHandler):
    """Turns every HTTP redirect into an error: download.qt.io, for example, redirects archive downloads to third-party mirrors."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise InstallError(f"{req.full_url} redirected to {newurl} (HTTP {code}); refusing it, because files must come from the server "
                           "named in the URL")


def make_opener() -> urllib.request.OpenerDirector:
    """A URL opener that verifies TLS certificates, uses the proxy settings from the environment and refuses redirects."""
    return urllib.request.build_opener(_RefuseRedirects(), urllib.request.HTTPSHandler(context=ssl_context()))


def open_url(url: str, opener: urllib.request.OpenerDirector):
    """Opens a URL, retrying temporary network failures; a missing file (HTTP 4xx) and a redirect fail at once."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(1, ATTEMPTS + 1):
        try:
            return opener.open(request, timeout=60)
        except urllib.error.HTTPError as error:
            if error.code < 500 or attempt == ATTEMPTS:
                raise InstallError(f"could not download {url}: HTTP {error.code}") from None
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            if attempt == ATTEMPTS:
                raise InstallError(f"could not download {url}: {error}") from None
        time.sleep(2**attempt)
    raise AssertionError("unreachable")


def download(url: str, limit: int, opener: urllib.request.OpenerDirector, destination: Path | None = None) -> tuple[bytes, str]:
    """Downloads at most <limit> bytes, into <destination> if given (then the returned bytes are empty). Returns the bytes and their SHA-256."""
    digest = hashlib.sha256()
    chunks: list[bytes] = []
    size = 0
    with open_url(url, opener) as response:
        output = open(destination, "wb") if destination else None
        try:
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > limit:
                    raise InstallError(f"{url} is larger than the limit of {limit} bytes")
                digest.update(chunk)
                if output:
                    output.write(chunk)
                else:
                    chunks.append(chunk)
        finally:
            if output:
                output.close()
    return b"".join(chunks), digest.hexdigest()


def published_sha256(url: str, file_name: str, opener: urllib.request.OpenerDirector) -> str:
    """Reads the SHA-256 that the repository publishes for a file (<url>.sha256, in the format "<hash>  <file name>")."""
    text, _ = download(url + ".sha256", 4096, opener)
    match = SHA256_LINE.fullmatch(text.decode("ascii", errors="replace").strip())
    if not match or match.group(2) != file_name:
        raise InstallError(f"{url}.sha256 is not a SHA-256 line for {file_name}")
    return match.group(1)


def find_archives(index: bytes, package: str, qt_version: str, directory: str, wanted: list[str]) -> tuple[str, list[tuple[str, str]]]:
    """Finds the pinned package in the repository index. Returns its full version and, for each wanted archive, its file name and the
    subdirectory it is extracted to, taken from the package's "Extract" operations (for example "lib" for ICU on Linux, "" for most)."""
    try:
        root = ElementTree.fromstring(index)
    except ElementTree.ParseError as error:
        raise InstallError(f"the repository index is not valid XML: {error}") from None
    updates = [update for update in root.iter("PackageUpdate") if update.findtext("Name") == package]
    if len(updates) != 1:
        raise InstallError(f"the repository index lists the package {package} {len(updates)} times instead of once")
    update = updates[0]
    version = (update.findtext("Version") or "").strip()
    if not SAFE_PACKAGE_VERSION.fullmatch(version) or not version.startswith(qt_version + "-"):
        raise InstallError(f"the package {package} has version '{version}', which is not Qt {qt_version}")

    destinations: dict[str, str] = {}
    for operation in update.iter("Operation"):
        arguments = [(argument.text or "").strip() for argument in operation.findall("Argument")]
        if operation.get("name") == "Extract" and len(arguments) == 2:
            destinations[arguments[1]] = arguments[0]
    available = [name.strip() for name in (update.findtext("DownloadableArchives") or "").split(",") if name.strip()]

    root_destination = f"@TargetDir@/{qt_version}/{directory}"
    chosen = []
    for name in wanted:
        matches = [archive for archive in available if archive.startswith(name + "-") and archive.endswith(".7z")]
        if len(matches) != 1:
            raise InstallError(f"the package {package} has {len(matches)} archives named {name}-*.7z instead of one")
        archive = matches[0]
        if not SAFE_FILE_NAME.fullmatch(archive):
            raise InstallError(f"the archive name {archive!r} contains unexpected characters")
        destination = destinations.get(archive)
        if destination == root_destination:
            subdirectory = ""
        elif destination is not None and destination.startswith(root_destination + "/"):
            subdirectory = destination[len(root_destination) + 1 :]
            check_relative_path(subdirectory)
        else:
            raise InstallError(f"the repository index does not say where {archive} belongs inside {qt_version}/{directory}")
        chosen.append((archive, subdirectory))
    return version, chosen


def fold(path: str) -> str:
    """The form in which file systems may treat two paths as the same: macOS and Windows ignore case by default, macOS treats Unicode
    canonical equivalents as the same name, and Windows ignores trailing dots and spaces. All checks between names compare folded paths."""
    folded = unicodedata.normalize("NFD", unicodedata.normalize("NFD", path).casefold())
    return "/".join(component.rstrip(". ") for component in folded.split("/"))


def check_relative_path(name: str) -> None:
    """Refuses archive paths that are absolute, have a component that is empty, "." or "..", or made only of dots and spaces (which Windows
    would shorten), or use Windows separators or drive letters."""
    stripped = name[:-1] if name.endswith("/") else name
    if not stripped or stripped.startswith("/") or "\\" in name or "\0" in name or WINDOWS_DRIVE.match(stripped):
        raise InstallError(f"unsafe path in archive: {name!r}")
    if any(not part.rstrip(". ") for part in stripped.split("/")):
        raise InstallError(f"unsafe path in archive: {name!r}")


def install_path(subdirectory: str, name: str) -> str:
    """The path of an archive entry inside the installation, for an archive extracted into <subdirectory> ("" for the root)."""
    name = name.rstrip("/")
    return f"{subdirectory}/{name}" if subdirectory else name


def check_entries(entries: list[tuple[str, str, str | None]], subdirectory: str = "", known_links: set[str] | None = None) -> set[str]:
    """Checks one archive's (name, kind, link target) entries before it is extracted into <subdirectory> of the installation. Kind is "-"
    (file), "d" (directory) or "l" (symbolic link). <known_links> holds the folded installation paths of the links that archives extracted
    earlier created. Returns the folded installation paths of this archive's links, to be added to <known_links>."""
    known = known_links or set()
    kinds: dict[str, str] = {}
    links: set[str] = set()
    for name, kind, target in entries:
        check_relative_path(name)
        if kind not in ("-", "d", "l"):
            raise InstallError(f"unsupported entry type '{kind}' in archive: {name!r}")
        path = install_path(subdirectory, name)
        key = fold(path)
        if key in kinds and not (kinds[key] == "d" and kind == "d"):
            raise InstallError(f"two archive entries have the same name when case is ignored: {name!r}")
        kinds[key] = kind
        if kind == "l":
            if not target or target.startswith("/") or "\\" in target or "\0" in target or WINDOWS_DRIVE.match(target):
                raise InstallError(f"unsafe link in archive: {name!r} -> {target!r}")
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(path), target))
            if resolved == ".." or resolved.startswith("../"):
                raise InstallError(f"link points outside the installation: {name!r} -> {target!r}")
            links.add(key)
    all_links = known | links
    for name, _, _ in entries:
        parts = fold(install_path(subdirectory, name)).split("/")
        for depth in range(1, len(parts)):
            if "/".join(parts[:depth]) in all_links:
                raise InstallError(f"archive path passes through a link: {name!r}")
        if "/".join(parts) in known:
            raise InstallError(f"archive entry would replace a link of an earlier archive: {name!r}")
    return links


def check_destination(staging: Path, subdirectory: str, known_links: set[str]) -> None:
    """Refuses an extraction directory that is, or passes through, a link: an earlier archive's link (compared folded), or any link on disk."""
    if not subdirectory:
        return
    parts = subdirectory.split("/")
    folded = fold(subdirectory).split("/")
    for depth in range(1, len(parts) + 1):
        if "/".join(folded[:depth]) in known_links or staging.joinpath(*parts[:depth]).is_symlink():
            raise InstallError(f"the extraction directory {subdirectory!r} is or passes through a link")


def run_cmake(cmake: str, arguments: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run([cmake, *arguments], cwd=cwd, capture_output=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise InstallError(f"cmake {' '.join(arguments[:3])} failed: {detail}")
    try:
        return result.stdout.decode("utf-8")
    except UnicodeDecodeError:
        raise InstallError("the archive listing is not valid UTF-8") from None


def list_entries(cmake: str, archive: Path) -> list[tuple[str, str, str | None]]:
    """Lists an archive's entries with CMake: names from "tar tf", types and link targets from "tar tvf" (the same entries in order)."""
    names = run_cmake(cmake, ["-E", "tar", "tf", str(archive)]).splitlines()
    details = run_cmake(cmake, ["-E", "tar", "tvf", str(archive)]).splitlines()
    if len(names) != len(details):
        raise InstallError(f"could not list {archive.name} consistently")
    entries = []
    for name, line in zip(names, details):
        # Like "ls -l": mode, links, owner, group, size, three date fields, then the name (and " -> target" for links).
        fields = line.split(None, 8)
        if len(fields) != 9 or not fields[0]:
            raise InstallError(f"could not read the archive listing line {line!r}")
        kind, rest, target = fields[0][0], fields[8], None
        if kind == "l":
            if not rest.startswith(name + " -> "):
                raise InstallError(f"could not read the link entry {line!r}")
            target = rest[len(name) + 4 :]
        elif rest != name:
            raise InstallError(f"the archive listing does not match for {name!r}")
        entries.append((name, kind, target))
    return entries


def check_extracted_links(root: Path) -> None:
    """After extraction, makes sure that no symbolic link resolves outside the directory (including through chains of links)."""
    resolved_root = root.resolve()
    for directory, subdirectories, files in os.walk(root):
        for name in subdirectories + files:
            path = Path(directory, name)
            if path.is_symlink() and not path.resolve().is_relative_to(resolved_root):
                raise InstallError(f"link points outside the installation: {path.relative_to(root)}")


def read_marker(path: Path) -> dict | None:
    try:
        return json.loads((path / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def install(pins_path: Path, host: str, prefix: Path, cmake: str, index_base: str, archive_base: str, max_archive_bytes: int) -> Path:
    try:
        pins = json.loads(pins_path.read_text(encoding="utf-8"))
        qt_version = pins["qt"]
        host_pins = pins["hosts"][host]
        repository, package, directory = host_pins["repository"], host_pins["package"], host_pins["directory"]
        wanted = list(host_pins["archives"])
    except (KeyError, TypeError) as error:
        raise InstallError(f"{pins_path} lacks the entry {error} for {host}") from None
    target = prefix / qt_version / directory
    request = {"qt": qt_version, "repository": repository, "package": package, "archives": wanted}

    marker = read_marker(target)
    if marker is not None and marker.get("request") == request:
        say(f"Qt {qt_version} ({' '.join(wanted)}) is already installed in {target}")
        return target

    opener = make_opener()
    index_url = f"{index_base}/{repository}/Updates.xml"
    say(f"reading the repository index {index_url}")
    index, index_sha256 = download(index_url, MAX_INDEX_BYTES, opener)
    if index_sha256 != published_sha256(index_url, "Updates.xml", opener):
        raise InstallError(f"the SHA-256 of {index_url} does not match the published one")
    version, archives = find_archives(index, package, qt_version, directory, wanted)

    prefix.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=prefix))
    downloads = Path(tempfile.mkdtemp(prefix=".downloads-", dir=prefix))
    try:
        installed = {}
        links: set[str] = set()  # folded installation paths of every link extracted so far
        for archive, subdirectory in archives:
            path = f"{repository}/{package}/{version}{archive}"
            expected = published_sha256(f"{index_base}/{path}", version + archive, opener)
            say(f"downloading {archive}")
            file = downloads / archive
            _, actual = download(f"{archive_base}/{path}", max_archive_bytes, opener, file)
            if actual != expected:
                raise InstallError(f"the SHA-256 of {archive} is {actual}, but Qt publishes {expected}; not installing it")
            check_destination(staging, subdirectory, links)
            links |= check_entries(list_entries(cmake, file), subdirectory, links)
            destination = staging / subdirectory
            destination.mkdir(parents=True, exist_ok=True)
            run_cmake(cmake, ["-E", "tar", "xf", str(file)], cwd=destination)
            file.unlink()
            # Before the next archive is extracted: chains of links can leave the installation even when each link stays inside on its own.
            check_extracted_links(staging)
            installed[archive] = actual
        # Like aqtinstall and Qt's own installer: tell Qt's tools (qmake, qtpaths) that the installation's root is the parent of bin/.
        (staging / "bin").mkdir(exist_ok=True)
        (staging / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
        (staging / MARKER).write_text(json.dumps({"request": request, "package_version": version, "sha256": installed}, indent=2) + "\n",
                                      encoding="utf-8")

        # Move the finished installation into place, replacing an older one only now that the new one is complete.
        target.parent.mkdir(parents=True, exist_ok=True)
        previous = None
        if target.exists():
            previous = target.with_name(f"{target.name}.previous-{os.getpid()}")
            target.rename(previous)
        staging.rename(target)
        if previous is not None:
            shutil.rmtree(previous)
    finally:
        shutil.rmtree(downloads, ignore_errors=True)
        shutil.rmtree(staging, ignore_errors=True)
    say(f"installed Qt {qt_version} ({' '.join(wanted)}) in {target}")
    return target


def default_host() -> str:
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform in ("win32", "cygwin", "msys"):
        return "windows"
    raise InstallError(f"unsupported platform {sys.platform}")


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--prefix", required=True, type=Path, help="directory to install into (Qt goes to PREFIX/<version>/<directory>)")
    parser.add_argument("--cmake", required=True, help="the pinned CMake, used to read and extract the archives")
    parser.add_argument("--pins", type=Path, default=Path(__file__).resolve().parent.parent / "deps" / "qt.json", help="the pin file")
    parser.add_argument("--host", choices=("linux", "macos", "windows"), help="platform to install for (default: this one)")
    parser.add_argument("--index-base", default=INDEX_BASE, help=argparse.SUPPRESS)  # for tests
    parser.add_argument("--archive-base", default=ARCHIVE_BASE, help=argparse.SUPPRESS)  # for tests
    parser.add_argument("--max-archive-bytes", type=int, default=MAX_ARCHIVE_BYTES, help=argparse.SUPPRESS)  # for tests
    options = parser.parse_args(arguments)
    try:
        target = install(options.pins, options.host or default_host(), options.prefix.absolute(), options.cmake, options.index_base.rstrip("/"),
                         options.archive_base.rstrip("/"), options.max_archive_bytes)
    except (InstallError, OSError, ValueError) as error:
        say(f"error: {error}")
        return 1
    print(target, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
