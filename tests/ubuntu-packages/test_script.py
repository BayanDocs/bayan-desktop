#!/usr/bin/env python3
"""Tests scripts/ubuntu-packages.sh (work package X-005): its fetch command against a local HTTPS server, and the checks of its lock
command with a stand-in for Docker. Run by CTest on Linux (tests/CMakeLists.txt), where CI runs the script.

fetch: every source is the same local server under a different path, with a certificate made for the test run by openssl, which curl
trusts through CURL_CA_BUNDLE; the stall limit is shortened through UBUNTU_PACKAGES_STALL_SECONDS. The tests show that a failed download
moves to the next source, that a source that accepts the connection and then sends nothing moves to the next one within the stall
limit, that the job stops and names the file and every source's error when all of them fail, that a size or SHA-256 mismatch stops the
job without trying another source, that a redirect to an http:// address is refused, and that a file already in the cache folder is
checked and, if it does not match, stops the job with the message that says how to recover.

lock: a stand-in for the Docker client (the DOCKER variable) reports an image and prints, for `docker run`, what apt would print in the
image. The tests show that the command writes the same lock for the same inputs, in whatever order the packages are given and again from
the lock itself (--from), and that it refuses a snapshot date before the image's creation or not before the cutoff, a cutoff less than 24
hours ago, a plan that would remove a package, and a package that the verified index does not have, without writing a lock.

Needs sh, curl, openssl and the usual command-line tools of Linux. Environment: BAYAN_UBUNTU_PACKAGES (the script; by default the one in
this repository).
"""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

SCRIPT = Path(os.environ.get("BAYAN_UBUNTU_PACKAGES", Path(__file__).resolve().parents[2] / "scripts" / "ubuntu-packages.sh"))
IMAGE = "docker.io/library/buildpack-deps:noble-scm@sha256:94ed690dd634b8b2f531eaa742fde9b1264077cd8ea695a3b960bd104ae4c481"
SNAPSHOT = "20261111T000000Z"


def clean_environment(**extra: str) -> dict[str, str]:
    """The environment for the script: without proxies (the server is local) and with the given variables."""
    environment = {
        name: value
        for name, value in os.environ.items()
        if name.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy", "curl_ca_bundle", "github_step_summary")
    }
    environment.update(extra)
    return environment


class Package:
    def __init__(self, name: str, version: str, architecture: str, pool_folder: str, content: bytes) -> None:
        self.name, self.version, self.architecture, self.content = name, version, architecture, content
        plain_version = version.split(":", 1)[-1]
        self.file = f"{name}_{plain_version}_{architecture}.deb"
        self.pool = f"{pool_folder}/{self.file}"
        self.sha256 = hashlib.sha256(content).hexdigest()


PACKAGES = [
    Package("libhello1", "1:1.0-1ubuntu0.1", "amd64", "pool/main/h/hello", b"first package file\n" * 1000),
    Package("hello-data", "2.10-3build2", "all", "pool/universe/h/hello", b"second package file\n" * 1500),
]


def lock_text(packages: list[Package] = PACKAGES) -> str:
    lines = ["# A lock for the tests", "format 1", f"image {IMAGE}", "created 2026-10-26T23:50:00.169696607Z", "architecture amd64"]
    lines += [f"snapshot {SNAPSHOT}", "request hello"]
    lines += [f"unpack {p.name} {p.version} {p.architecture} {p.pool} {len(p.content)} {p.sha256}" for p in packages]
    lines += [f"configure {p.name} {p.version}" for p in packages]
    return "\n".join(lines) + "\n"


class Server:
    """One HTTPS server for the three sources (paths /snapshot, /launchpad and /archive) and one plain HTTP server, both local.

    `behaviour` maps a source to what it does for every request: "serve" (the right file), "missing" (404), "stall" (accept the request
    and send nothing), "wrong-size", "wrong-hash", or "redirect-http" (302 to the plain HTTP server). Every request is recorded."""

    def __init__(self, folder: Path) -> None:
        self.behaviour: dict[str, str] = {}
        self.requests: list[tuple[str, str]] = []
        self.http_requests: list[str] = []
        self.release = threading.Event()
        certificate, key = folder / "server.crt", folder / "server.key"
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", certificate, "-days", "1",
             "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
            check=True, capture_output=True,
        )
        self.certificate = certificate
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: object) -> None:
                pass

            def do_GET(self) -> None:
                source, _, rest = self.path.lstrip("/").partition("/")
                name = rest.rsplit("/", 1)[-1]
                server.requests.append((source, name))
                package = next((p for p in PACKAGES if p.file == name), None)
                expected = {"snapshot": f"{SNAPSHOT}/{package.pool}" if package else None, "launchpad": name, "archive": package.pool if package else None}
                what = server.behaviour.get(source, "missing")
                if package is None or rest != expected.get(source):
                    what = "missing"
                if what == "stall":
                    server.release.wait(30)
                    return
                if what == "redirect-http":
                    # A fixed address, nothing taken from the request.
                    self.send_response(302)
                    self.send_header("Location", f"http://127.0.0.1:{server.http_port}/redirected.deb")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if what == "missing":
                    self.send_error(404)
                    return
                body = package.content
                if what == "wrong-size":
                    body = body + b"!"
                elif what == "wrong-hash":
                    body = b"X" + body[1:]
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        class PlainHandler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: object) -> None:
                pass

            def do_GET(self) -> None:
                server.http_requests.append(self.path)
                self.send_error(404)

        self.https = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.https.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2  # as the script's curl requires (--tlsv1.2)
        context.load_cert_chain(certificate, key)
        self.https.socket = context.wrap_socket(self.https.socket, server_side=True)
        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PlainHandler)
        self.http.daemon_threads = True
        self.port, self.http_port = self.https.server_address[1], self.http.server_address[1]
        for instance in (self.https, self.http):
            threading.Thread(target=instance.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.release.set()
        for instance in (self.https, self.http):
            instance.shutdown()
            instance.server_close()

    def url(self, source: str) -> str:
        return f"https://127.0.0.1:{self.port}/{source}"


@unittest.skipUnless(sys.platform.startswith("linux"), "the script runs on Linux")
class Fetch(unittest.TestCase):
    server: Server
    folder: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.folder = Path(tempfile.mkdtemp(prefix="ubuntu-packages-test-"))
        cls.server = Server(cls.folder)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.close()
        shutil.rmtree(cls.folder, ignore_errors=True)

    def setUp(self) -> None:
        self.server.requests.clear()
        self.server.http_requests.clear()
        self.server.behaviour = {}
        self.work = Path(tempfile.mkdtemp(dir=self.folder))
        self.lock = self.work / "ubuntu-packages.lock"
        self.lock.write_text(lock_text(), encoding="utf-8")
        self.cache = self.work / "cache"
        self.report = self.work / "report"

    def fetch(self, **behaviour: str) -> subprocess.CompletedProcess[str]:
        self.server.behaviour = behaviour
        environment = clean_environment(
            CURL_CA_BUNDLE=str(self.server.certificate),
            UBUNTU_PACKAGES_SNAPSHOT_URL=self.server.url("snapshot"),
            UBUNTU_PACKAGES_LAUNCHPAD_URL=self.server.url("launchpad"),
            UBUNTU_PACKAGES_ARCHIVE_URL=self.server.url("archive"),
            UBUNTU_PACKAGES_STALL_SECONDS="2",
        )
        return subprocess.run(
            ["sh", str(SCRIPT), "fetch", "--report", str(self.report), str(self.lock), str(self.cache)],
            env=environment, capture_output=True, text=True, timeout=120,
        )

    def report_counts(self) -> dict[str, int]:
        return {key: int(value) for key, value in (line.split() for line in self.report.read_text(encoding="utf-8").splitlines()) if key != "seconds"}

    def assertCacheHoldsEveryFile(self) -> None:
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), sorted(p.file for p in PACKAGES))
        for package in PACKAGES:
            self.assertEqual((self.cache / package.file).read_bytes(), package.content)

    def test_downloads_every_file_from_the_first_source_and_checks_it(self) -> None:
        result = self.fetch(snapshot="serve", launchpad="serve", archive="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertCacheHoldsEveryFile()
        self.assertEqual(sorted(self.server.requests), sorted(("snapshot", p.file) for p in PACKAGES))
        counts = self.report_counts()
        self.assertEqual((counts["files"], counts["snapshot"], counts["launchpad"], counts["archive"], counts["cache"]), (2, 2, 0, 0, 0))
        self.assertEqual(counts["bytes"], sum(len(p.content) for p in PACKAGES))

    def test_a_failed_download_moves_to_the_next_source(self) -> None:
        result = self.fetch(snapshot="missing", launchpad="serve", archive="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertCacheHoldsEveryFile()
        self.assertEqual(self.report_counts()["launchpad"], 2)
        # Each failed attempt is in the log with its error, so a summary that counts files from a later source can be explained.
        for package in PACKAGES:
            self.assertIn(
                f"{package.file}: could not download from {self.server.url('snapshot')}/{SNAPSHOT}/{package.pool} (curl: (22)", result.stderr
            )
            self.assertIn("404); trying the next source", result.stderr)
        result = self.fetch(snapshot="missing", launchpad="missing", archive="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.report_counts()["cache"], 2)  # the files of the first run are now in the cache folder
        shutil.rmtree(self.cache)
        result = self.fetch(snapshot="missing", launchpad="missing", archive="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.report_counts()["archive"], 2)

    def test_a_stalled_source_moves_to_the_next_one_within_the_time_limit(self) -> None:
        started = time.monotonic()
        result = self.fetch(snapshot="stall", launchpad="serve", archive="serve")
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertCacheHoldsEveryFile()
        self.assertEqual(self.report_counts()["launchpad"], 2)
        # The stall limit is 2 seconds here (30 by default); both files download at the same time.
        self.assertGreaterEqual(elapsed, 2)
        self.assertLess(elapsed, 20)

    def test_when_every_source_fails_the_job_stops_and_names_the_file_and_each_error(self) -> None:
        result = self.fetch(snapshot="missing", launchpad="stall", archive="redirect-http")
        self.assertNotEqual(result.returncode, 0)
        for package in PACKAGES:
            message = next((line for line in result.stderr.splitlines() if f"could not download {package.file} from any source" in line), "")
            self.assertTrue(message, result.stderr)
            self.assertIn(f"{self.server.url('snapshot')}/{SNAPSHOT}/{package.pool}: curl: (22)", message)
            self.assertIn("404", message)
            self.assertIn(f"{self.server.url('launchpad')}/{package.file}: curl: (28)", message)  # 28: the stall limit
            self.assertIn(f"{self.server.url('archive')}/{package.pool}: curl: (1)", message)  # 1: http:// is not allowed
        self.assertFalse(any(self.cache.iterdir()))

    def test_a_size_or_sha256_mismatch_stops_without_trying_another_source(self) -> None:
        for wrong in ("wrong-size", "wrong-hash"):
            with self.subTest(wrong):
                self.setUp()
                result = self.fetch(snapshot=wrong, launchpad="serve", archive="serve")
                self.assertNotEqual(result.returncode, 0)
                for package in PACKAGES:
                    self.assertIn(f"{package.file} from {self.server.url('snapshot')}/{SNAPSHOT}/{package.pool} has", result.stderr)
                self.assertIn("no other source is tried", result.stderr)
                self.assertIn("SHA-256" if wrong == "wrong-hash" else "bytes", result.stderr)
                self.assertEqual({source for source, _ in self.server.requests}, {"snapshot"})
                self.assertFalse(any(self.cache.iterdir()))

    def test_a_redirect_to_http_is_refused(self) -> None:
        result = self.fetch(snapshot="redirect-http", launchpad="serve", archive="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.http_requests, [])
        self.assertCacheHoldsEveryFile()
        self.assertEqual(self.report_counts()["launchpad"], 2)

    def test_a_cached_file_is_checked_and_used(self) -> None:
        self.cache.mkdir()
        (self.cache / PACKAGES[0].file).write_bytes(PACKAGES[0].content)
        result = self.fetch(snapshot="serve")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertCacheHoldsEveryFile()
        self.assertEqual(self.server.requests, [("snapshot", PACKAGES[1].file)])
        counts = self.report_counts()
        self.assertEqual((counts["cache"], counts["snapshot"]), (1, 1))

    def test_a_bad_cached_file_stops_the_job_with_the_recovery_message(self) -> None:
        self.cache.mkdir()
        bad = b"X" + PACKAGES[0].content[1:]
        (self.cache / PACKAGES[0].file).write_bytes(bad)
        result = self.fetch(snapshot="serve", launchpad="serve", archive="serve")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{PACKAGES[0].file} in the cache has the SHA-256 {hashlib.sha256(bad).hexdigest()}, but the lock pins {PACKAGES[0].sha256}", result.stderr)
        self.assertIn("change the version in the cache key", result.stderr)
        self.assertNotIn(PACKAGES[0].file, [name for _, name in self.server.requests])
        self.assertEqual((self.cache / PACKAGES[0].file).read_bytes(), bad)  # left as it was, for whoever investigates

    def test_only_https_sources_are_accepted(self) -> None:
        environment = clean_environment(UBUNTU_PACKAGES_LAUNCHPAD_URL=f"http://127.0.0.1:{self.server.http_port}")
        result = subprocess.run(
            ["sh", str(SCRIPT), "fetch", str(self.lock), str(self.cache)], env=environment, capture_output=True, text=True, timeout=60
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("every source address must start with https://", result.stderr)
        self.assertEqual((self.server.requests, self.server.http_requests), ([], []))

    def test_a_pool_path_with_a_dot_or_dot_dot_directory_is_refused(self) -> None:
        for directories in ("pool/main/../..", "pool/main/h/."):
            with self.subTest(directories):
                path = f"{directories}/{PACKAGES[0].file}"
                self.lock.write_text(lock_text().replace(PACKAGES[0].pool, path), encoding="utf-8")
                result = self.fetch(snapshot="serve", launchpad="serve", archive="serve")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f'a pool path with a "." or ".." directory: {path}', result.stderr)
                self.assertEqual(self.server.requests, [])

    def test_a_lock_that_was_edited_by_hand_is_refused(self) -> None:
        self.lock.write_text(lock_text().replace("configure hello-data 2.10-3build2\n", ""), encoding="utf-8")
        result = self.fetch(snapshot="serve")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unpacked but never configured", result.stderr)
        self.assertEqual(self.server.requests, [])


# What plan_in_image prints in the image for `docker run`: the architecture, apt's simulated plan and the verified index's fields.
APT_OUTPUT = """\
architecture amd64
Inst libhello1 [1:1.0-1] (1:1.0-1ubuntu0.1 Ubuntu:24.04/noble-updates, Ubuntu:24.04/noble-security [amd64]) [hello:amd64 ]
Conf libhello1 (1:1.0-1ubuntu0.1 Ubuntu:24.04/noble-updates, Ubuntu:24.04/noble-security [amd64]) []
Inst hello-data (2.10-3build2 Ubuntu:24.04/noble [all])
Conf hello-data (2.10-3build2 Ubuntu:24.04/noble [all])
index
Package: libhello1
Architecture: amd64
Version: 1:1.0-1ubuntu0.1
Filename: {pool0}
Size: {size0}
SHA256: {sha0}
Package: hello-data
Architecture: all
Version: 2.10-3build2
Filename: {pool1}
Size: {size1}
SHA256: {sha1}
""".format(
    pool0=PACKAGES[0].pool, size0=len(PACKAGES[0].content), sha0=PACKAGES[0].sha256,
    pool1=PACKAGES[1].pool, size1=len(PACKAGES[1].content), sha1=PACKAGES[1].sha256,
)

FAKE_DOCKER = """\
#!{python}
import json, os, sys
arguments = sys.argv[1:]
with open(os.environ["FAKE_DOCKER_LOG"], "a", encoding="utf-8") as log:
    # For `run`, the script's text (the argument after "-c") is left out of the log.
    log.write(json.dumps([a for i, a in enumerate(arguments) if i == 0 or arguments[i - 1] != "-c"]) + "\\n")
if arguments[0] == "image":
    print(os.environ["FAKE_DOCKER_CREATED"] + " linux amd64")
elif arguments[0] == "run":
    sys.stdout.write(open(os.environ["FAKE_DOCKER_APT_OUTPUT"], encoding="utf-8").read())
elif arguments[0] != "pull":
    sys.exit(2)
"""

EXPECTED_LOCK_BODY = f"""\
format 1
image {IMAGE}
created 2026-10-26T23:50:00.169696607Z
architecture amd64
snapshot {SNAPSHOT}
request g++
request hello
request libfoo-dev
unpack libhello1 1:1.0-1ubuntu0.1 amd64 {PACKAGES[0].pool} {len(PACKAGES[0].content)} {PACKAGES[0].sha256}
configure libhello1 1:1.0-1ubuntu0.1
unpack hello-data 2.10-3build2 all {PACKAGES[1].pool} {len(PACKAGES[1].content)} {PACKAGES[1].sha256}
configure hello-data 2.10-3build2
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "the script runs on Linux")
class Lock(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="ubuntu-packages-lock-test-"))
        self.addCleanup(shutil.rmtree, self.work, True)
        self.docker = self.work / "docker"
        self.docker.write_text(FAKE_DOCKER.format(python=sys.executable), encoding="utf-8")
        self.docker.chmod(0o755)
        self.log = self.work / "docker.log"
        self.apt_output = self.work / "apt-output"
        self.apt_output.write_text(APT_OUTPUT, encoding="utf-8")
        self.output = self.work / "ubuntu-packages.lock"

    def lock(self, *arguments: str, created: str = "2026-10-26T23:50:00.169696607Z", apt_output: str | None = None) -> subprocess.CompletedProcess[str]:
        if apt_output is not None:
            self.apt_output.write_text(apt_output, encoding="utf-8")
        environment = clean_environment(
            DOCKER=str(self.docker), FAKE_DOCKER_LOG=str(self.log), FAKE_DOCKER_CREATED=created, FAKE_DOCKER_APT_OUTPUT=str(self.apt_output)
        )
        return subprocess.run(["sh", str(SCRIPT), "lock", *arguments], env=environment, capture_output=True, text=True, timeout=60)

    def docker_runs(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [call for call in map(json.loads, self.log.read_text(encoding="utf-8").splitlines()) if call[0] == "run"]

    def standard(self, *packages: str, snapshot: str = SNAPSHOT, cutoff: str = "2026-11-11T15:00:00Z") -> tuple[str, ...]:
        return ("--image", IMAGE, "--snapshot", snapshot, "--cutoff", cutoff, "--output", str(self.output), *(packages or ("libfoo-dev", "hello", "g++")))

    def test_writes_the_lock_from_apt_s_plan_and_the_verified_index(self) -> None:
        result = self.lock(*self.standard())
        self.assertEqual(result.returncode, 0, result.stderr)
        text = self.output.read_text(encoding="utf-8")
        self.assertEqual("\n".join(line for line in text.splitlines() if not line.startswith("#")) + "\n", EXPECTED_LOCK_BODY)
        runs = self.docker_runs()
        self.assertEqual(len(runs), 1)
        self.assertIn("--platform", runs[0])
        self.assertEqual(runs[0][runs[0].index("__plan") :], ["__plan", SNAPSHOT, "", "", "g++", "hello", "libfoo-dev"])

    def test_the_same_inputs_give_the_same_file_byte_for_byte(self) -> None:
        self.assertEqual(self.lock(*self.standard()).returncode, 0)
        first = self.output.read_bytes()
        self.assertEqual(self.lock(*self.standard("hello", "g++", "libfoo-dev", "hello")).returncode, 0)
        self.assertEqual(self.output.read_bytes(), first)
        again = self.work / "again.lock"
        result = self.lock("--from", str(self.output), "--cutoff", "2026-11-11T15:00:00Z", "--output", str(again))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(again.read_bytes(), first)
        result = self.lock("--from", str(self.output), "--cutoff", "2026-11-11T15:00:00Z")
        self.assertEqual(result.stdout.encode(), first)

    def test_refuses_a_snapshot_before_the_image_was_created(self) -> None:
        result = self.lock(*self.standard(snapshot="20261026T000000Z"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is before the image was created", result.stderr)
        # The same second as the creation, which was a fraction of a second later.
        result = self.lock(*self.standard(snapshot="20261026T235000Z"))
        self.assertIn("is before the image was created", result.stderr)
        self.assertEqual(self.docker_runs(), [])
        self.assertFalse(self.output.exists())
        result = self.lock(*self.standard(snapshot="20261026T235001Z"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_refuses_a_snapshot_that_is_not_before_the_cutoff(self) -> None:
        for snapshot in ("20261111T150000Z", "20261112T000000Z"):
            with self.subTest(snapshot):
                result = self.lock(*self.standard(snapshot=snapshot))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"the snapshot {snapshot} is not before the cutoff 2026-11-11T15:00:00Z", result.stderr)
        self.assertEqual(self.docker_runs(), [])

    def test_refuses_a_cutoff_less_than_24_hours_ago(self) -> None:
        cutoff = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 23 * 3600))
        result = self.lock(*self.standard(cutoff=cutoff))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is less than 24 hours ago", result.stderr)
        self.assertEqual(self.docker_runs(), [])

    def test_refuses_a_plan_that_would_remove_a_package(self) -> None:
        output = APT_OUTPUT.replace("index\n", "Remv oldhello [1.0-1]\nindex\n")
        result = self.lock(*self.standard(), apt_output=output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("it would remove oldhello", result.stderr)
        self.assertFalse(self.output.exists())

    def test_refuses_a_package_that_the_verified_index_does_not_have(self) -> None:
        output = APT_OUTPUT.replace("Version: 2.10-3build2", "Version: 2.10-3build3")
        result = self.lock(*self.standard(), apt_output=output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("the verified index has no record of hello-data 2.10-3build2 (all)", result.stderr)
        self.assertFalse(self.output.exists())

    def test_refuses_inputs_that_are_not_pinned_or_not_dates(self) -> None:
        cases = {
            "the image must be named with its digest": ("--image", "docker.io/library/buildpack-deps:noble-scm"),
            "not a snapshot ID": ("--snapshot", "2026-11-11"),
            "not a UTC time": ("--cutoff", "yesterday"),
        }
        for message, (option, value) in cases.items():
            with self.subTest(option):
                arguments = list(self.standard())
                arguments[arguments.index(option) + 1] = value
                result = self.lock(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
        self.assertEqual(self.docker_runs(), [])


# Stand-ins for install's tools, put first on PATH: dpkg records its arguments and answers --audit with $FAKE_DPKG_AUDIT; dpkg-query
# prints each queried package's line ("<package> <status> <version>") from $FAKE_DPKG_STATE; id says root, so the tests run as any user.
FAKE_DPKG = """\
#!/bin/sh
printf '%s\\n' "$*" >>"$FAKE_DPKG_LOG"
if [ "$*" = --audit ]; then printf '%s' "${FAKE_DPKG_AUDIT:-}"; fi
"""

FAKE_DPKG_QUERY = """\
#!/bin/sh
shift 3
status=0
for package in "$@"; do
  line=$(awk -v p="$package" '$1 == p { print; exit }' "$FAKE_DPKG_STATE")
  if [ -n "$line" ]; then printf '%s\\n' "$line"; else echo "dpkg-query: no packages found matching $package" >&2; status=1; fi
done
exit $status
"""

FAKE_ID = """\
#!/bin/sh
echo 0
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "the script runs on Linux")
class Install(unittest.TestCase):
    """install's checks of the result, with stand-ins for dpkg, dpkg-query and id (as the Lock tests stand in for Docker), so that the
    tests change nothing on the machine and pass whether or not the gate runs as root."""

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="ubuntu-packages-install-test-"))
        self.addCleanup(shutil.rmtree, self.work, True)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        for name, text in (("dpkg", FAKE_DPKG), ("dpkg-query", FAKE_DPKG_QUERY), ("id", FAKE_ID)):
            (self.bin / name).write_text(text, encoding="utf-8")
            (self.bin / name).chmod(0o755)
        self.lock = self.work / "ubuntu-packages.lock"
        self.lock.write_text(lock_text(), encoding="utf-8")
        self.folder = self.work / "cache"
        self.folder.mkdir()
        for package in PACKAGES:
            (self.folder / package.file).write_bytes(package.content)
        self.log = self.work / "dpkg.log"
        self.state = self.work / "dpkg-state"
        self.state.write_text("".join(f"{p.name} ii {p.version}\n" for p in PACKAGES), encoding="utf-8")

    def install(self, audit: str = "") -> subprocess.CompletedProcess[str]:
        environment = clean_environment(
            PATH=f"{self.bin}{os.pathsep}{os.environ.get('PATH', '')}",
            FAKE_DPKG_LOG=str(self.log),
            FAKE_DPKG_AUDIT=audit,
            FAKE_DPKG_STATE=str(self.state),
        )
        return subprocess.run(
            ["sh", str(SCRIPT), "install", str(self.lock), str(self.folder)], env=environment, capture_output=True, text=True, timeout=60
        )

    def test_replays_the_plan_and_succeeds_when_dpkg_reports_everything_installed(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("installed the 2 packages", result.stderr)
        options = "--no-triggers --force-confdef --force-confold"
        self.assertEqual(
            self.log.read_text(encoding="utf-8").splitlines(),
            [
                f"--unpack --auto-deconfigure {options} {PACKAGES[0].file} {PACKAGES[1].file}",
                f"--configure {options} {PACKAGES[0].name} {PACKAGES[1].name}",
                "--configure --pending",
                "--audit",
            ],
        )

    def test_fails_when_dpkg_audit_reports_a_problem(self) -> None:
        audit = "The following packages are only half configured, probably due to problems\nconfiguring them the first time.\n hello-data\n"
        result = self.install(audit=audit)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dpkg --audit reports unfinished packages: The following packages are only half configured", result.stderr)
        self.assertNotIn("installed the", result.stderr)

    def test_fails_when_a_locked_package_is_not_installed_at_its_locked_version(self) -> None:
        first = f"{PACKAGES[0].name} ii {PACKAGES[0].version}\n"
        cases = {
            "another version": (first + f"{PACKAGES[1].name} ii 2.10-3build1\n", "ii 2.10-3build1"),
            "unpacked, not configured": (first + f"{PACKAGES[1].name} iU {PACKAGES[1].version}\n", f"iU {PACKAGES[1].version}"),
            "not installed": (first, "not installed"),
        }
        for name, (state, shown) in cases.items():
            with self.subTest(name):
                self.state.write_text(state, encoding="utf-8")
                result = self.install()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    f"these packages are not installed at their locked versions: {PACKAGES[1].name} {PACKAGES[1].version}: {shown};", result.stderr
                )
                self.assertNotIn("installed the", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
