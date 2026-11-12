#!/usr/bin/env python3
"""Checks deps/ubuntu-packages.lock and the workflows that install it (work package X-005), run by CTest (tests/CMakeLists.txt).

The Linux jobs of CI and of the CodeQL workflow run in a container from the base image that the lock's header names, and install
exactly the package files that the lock lists, with scripts/ubuntu-packages.sh. This test checks that the lock is well formed, as the
script's lock command writes it; that its snapshot date is not before its base image was created (a -dev package of an older date could
need an older version of its library than the image has); that every package it unpacks is configured afterwards, at the same version;
and that every job that installs it runs in that base image, so that the plan apt made for that image is the one that dpkg replays. It
also checks that no workflow runs apt or names a snapshot date: the lock is the only place where the packages are chosen. It replaces
tests/workflows/test_ubuntu_packages.py, which kept the snapshot date and package list of two workflows equal (X-002). Each check is
also tested on a broken example, so that a check that can no longer fail is noticed. It reads the files as text, with the Python
standard library only. Environment: BAYAN_SOURCE_DIR (the repository; by default the one this file is in).
"""

from __future__ import annotations

import os
import re
import unittest
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(os.environ.get("BAYAN_SOURCE_DIR", Path(__file__).resolve().parents[2]))
LOCK = ROOT / "deps" / "ubuntu-packages.lock"
WORKFLOWS = ROOT / ".github" / "workflows"
INSTALL_COMMAND = "scripts/ubuntu-packages.sh install"

IMAGE = re.compile(r"[a-z0-9][a-z0-9._/:-]*@sha256:[0-9a-f]{64}")
CREATED = re.compile(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?Z")
SNAPSHOT = re.compile(r"\d{8}T\d{6}Z")
PACKAGE = re.compile(r"[a-z0-9][a-z0-9.+-]+")
VERSION = re.compile(r"[0-9][0-9A-Za-z.+~:-]*")
POOL = re.compile(r"pool/[a-z0-9-]+/[a-z0-9+.-]+/[a-z0-9+.-]+/[A-Za-z0-9+.~_-]+\.deb")
SHA256 = re.compile(r"[0-9a-f]{64}")


@dataclass
class Lock:
    image: str
    created: datetime
    architecture: str
    snapshot: datetime
    requests: list[str]
    # ("unpack", package, version, architecture, pool path, size, sha256) or ("configure", package, version), in apt's order.
    steps: list[tuple[str, ...]] = field(default_factory=list)


def parse_lock(text: str) -> Lock:
    """Reads a lock as scripts/ubuntu-packages.sh writes it; raises ValueError, naming the line, for anything else."""
    lines = [(number, line) for number, line in enumerate(text.splitlines(), start=1) if line.strip() and not line.startswith("#")]
    header: dict[str, str] = {}
    requests: list[str] = []
    steps: list[tuple[str, ...]] = []
    expected = ["format", "image", "created", "architecture", "snapshot"]
    for number, line in lines:
        words = line.split(" ")
        if expected:
            key = expected.pop(0)
            if len(words) != 2 or words[0] != key:
                raise ValueError(f"line {number}: expected '{key} <value>', found {line!r}")
            header[key] = words[1]
            continue
        if words[0] == "request" and not steps:
            if len(words) != 2 or not PACKAGE.fullmatch(words[1]):
                raise ValueError(f"line {number}: not a requested package: {line!r}")
            requests.append(words[1])
        elif words[0] == "unpack" and len(words) == 7:
            _, package, version, architecture, pool, size, sha256 = words
            if not (PACKAGE.fullmatch(package) and VERSION.fullmatch(version) and architecture in ("amd64", "all")):
                raise ValueError(f"line {number}: malformed package, version or architecture: {line!r}")
            plain_version = version.split(":", 1)[1] if ":" in version else version
            if not POOL.fullmatch(pool) or pool.rsplit("/", 1)[1] != f"{package}_{plain_version}_{architecture}.deb":
                raise ValueError(f"line {number}: not the pool path of {package} {version}: {pool}")
            if not (size.isdigit() and int(size) > 0 and not size.startswith("0") and SHA256.fullmatch(sha256)):
                raise ValueError(f"line {number}: malformed size or SHA-256: {line!r}")
            steps.append(tuple(words))
        elif words[0] == "configure" and len(words) == 3 and requests:
            steps.append(tuple(words))
        else:
            raise ValueError(f"line {number}: unexpected line {line!r}")
    if expected:
        raise ValueError(f"the header has no '{expected[0]}' line")
    if header["format"] != "1":
        raise ValueError(f"unknown format {header['format']}")
    if not IMAGE.fullmatch(header["image"]):
        raise ValueError(f"not an image with a digest: {header['image']}")
    created = CREATED.fullmatch(header["created"])
    if not created:
        raise ValueError(f"not a UTC time: {header['created']}")
    if not SNAPSHOT.fullmatch(header["snapshot"]):
        raise ValueError(f"not a snapshot ID: {header['snapshot']}")
    if not requests:
        raise ValueError("no requested packages")
    if requests != sorted(set(requests)):
        raise ValueError("the requested packages must be sorted and named once")
    if not steps:
        raise ValueError("no plan")
    fraction = (created.group(2) or "").ljust(6, "0")[:6]  # Python keeps microseconds; enough to order against whole-second dates
    return Lock(
        image=header["image"],
        created=datetime.strptime(created.group(1), "%Y-%m-%dT%H:%M:%S").replace(microsecond=int(fraction), tzinfo=timezone.utc),
        architecture=header["architecture"],
        snapshot=datetime.strptime(header["snapshot"], "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc),
        requests=requests,
        steps=steps,
    )


def lock_problems(lock: Lock) -> list[str]:
    """What is wrong with a lock that parses: its dates, its architecture, and the order of its plan."""
    problems = []
    if lock.architecture != "amd64":
        problems.append(f"the architecture is {lock.architecture}, not amd64")
    if lock.snapshot < lock.created:
        problems.append(f"the snapshot date {lock.snapshot:%Y%m%dT%H%M%SZ} is before the base image was created ({lock.created.isoformat()})")
    unpacked: dict[str, str] = {}
    configured: set[str] = set()
    files: set[str] = set()
    for step in lock.steps:
        if step[0] == "unpack":
            _, package, version, _, pool, _, _ = step
            if package in unpacked:
                problems.append(f"{package} is unpacked twice")
            if pool in files:
                problems.append(f"{pool} is listed twice")
            unpacked[package] = version
            files.add(pool)
        else:
            _, package, version = step
            if unpacked.get(package) != version:
                problems.append(f"{package} {version} is configured, but not unpacked before at that version")
            if package in configured:
                problems.append(f"{package} is configured twice")
            configured.add(package)
    problems.extend(f"{package} is unpacked but never configured" for package in sorted(set(unpacked) - configured))
    return problems


def strip_comment(value: str) -> str:
    """A YAML scalar without its trailing comment and quotes (enough for the plain values these workflows use)."""
    return re.sub(r"\s+#.*$", "", value).strip().strip("'\"")


def installing_jobs(workflow: str) -> dict[str, str | None]:
    """The jobs of a workflow that run the install command, each with the image of its container (None if it has none).

    It reads the workflow as text: jobs are the keys one level below the top-level "jobs:", and a job's container image is either
    "container: <image>" or the "image:" key of its "container:" mapping. Comment lines are ignored."""
    jobs: dict[str, list[str]] = {}
    job_indent = None
    current = None
    in_jobs = False
    for line in workflow.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent == 0:
            in_jobs = line.rstrip() == "jobs:"
            current = None
            continue
        if not in_jobs:
            continue
        if job_indent is None:
            job_indent = indent
        if indent == job_indent:
            match = re.fullmatch(r"([A-Za-z0-9_-]+):\s*", line.strip() + " ")
            current = match.group(1) if match else None
            if current:
                jobs[current] = []
            continue
        if current:
            jobs[current].append(line)
    result: dict[str, str | None] = {}
    for job, body in jobs.items():
        if not any(INSTALL_COMMAND in line for line in body):
            continue
        image = None
        for index, line in enumerate(body):
            match = re.fullmatch(r"(\s*)container:\s*(.*)", line)
            if not match:
                continue
            if strip_comment(match.group(2)):
                image = strip_comment(match.group(2))
                break
            for inner in body[index + 1 :]:
                if len(inner) - len(inner.lstrip(" ")) <= len(match.group(1)):
                    break
                image_match = re.fullmatch(r"\s*image:\s*(.*)", inner)
                if image_match:
                    image = strip_comment(image_match.group(1))
                    break
            break
        result[job] = image
    return result


APT_COMMAND = re.compile(r"\bapt(?:-get)?\s+(?:-\S+\s+)*(?:install|update|upgrade|dist-upgrade|full-upgrade)\b|--snapshot\b|\bapt-get\b")


def apt_lines(workflow: str) -> list[str]:
    """The lines of a workflow, comments aside, that run apt or name a snapshot date."""
    return [line.strip() for line in workflow.splitlines() if not line.lstrip().startswith("#") and APT_COMMAND.search(strip_comment(line))]


def workflows() -> dict[str, str]:
    return {path.name: path.read_text(encoding="utf-8") for path in sorted(WORKFLOWS.glob("*.yml"))}


class TheLock(unittest.TestCase):
    def test_the_lock_is_well_formed_and_its_plan_installs_every_package_it_unpacks(self) -> None:
        lock = parse_lock(LOCK.read_text(encoding="utf-8"))
        self.assertEqual(lock_problems(lock), [])

    def test_every_job_that_installs_the_lock_runs_in_its_base_image(self) -> None:
        lock = parse_lock(LOCK.read_text(encoding="utf-8"))
        found = {}
        for name, text in workflows().items():
            for job, image in installing_jobs(text).items():
                found[f"{name}: {job}"] = image
                with self.subTest(job=f"{name}: {job}"):
                    self.assertEqual(image, lock.image, f"{name}: job {job} installs {LOCK.name} but runs in another image than its header names")
        # The jobs X-005 moved into containers; if the reader stopped finding them, the check above would pass without checking anything.
        self.assertLessEqual({"ci.yml: linux", "ci.yml: sanitizers", "codeql.yml: cpp-analysis"}, set(found))

    def test_no_workflow_runs_apt_or_names_a_snapshot(self) -> None:
        for name, text in workflows().items():
            with self.subTest(workflow=name):
                self.assertEqual(apt_lines(text), [], f"{name} runs apt or names a snapshot date; install from {LOCK.name} instead")


EXAMPLE_LOCK = """\
# comment
format 1
image docker.io/library/buildpack-deps:noble-scm@sha256:94ed690dd634b8b2f531eaa742fde9b1264077cd8ea695a3b960bd104ae4c481
created 2026-10-26T23:50:00.169696607Z
architecture amd64
snapshot 20261111T000000Z
request hello
unpack libhello1 1:1.0-1 amd64 pool/main/h/hello/libhello1_1.0-1_amd64.deb 1234 0000000000000000000000000000000000000000000000000000000000000001
unpack hello 2.10-3build2 amd64 pool/main/h/hello/hello_2.10-3build2_amd64.deb 5678 0000000000000000000000000000000000000000000000000000000000000002
configure libhello1 1:1.0-1
configure hello 2.10-3build2
"""

EXAMPLE_WORKFLOW = """\
name: Example
on:
  pull_request:
jobs:
  inside:
    runs-on: ubuntu-24.04
    container:
      image: docker.io/library/buildpack-deps:noble-scm@sha256:94ed690dd634b8b2f531eaa742fde9b1264077cd8ea695a3b960bd104ae4c481 # created 2026-10-26
    steps:
      - run: sh scripts/ubuntu-packages.sh install deps/ubuntu-packages.lock "$RUNNER_TEMP/ubuntu-packages"
  inline:
    runs-on: ubuntu-24.04
    container: docker.io/library/ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
    steps:
      - name: Install
        run: |
          sh scripts/ubuntu-packages.sh install deps/ubuntu-packages.lock "$RUNNER_TEMP/ubuntu-packages"
  outside:
    runs-on: ubuntu-24.04
    steps:
      - run: sh scripts/ubuntu-packages.sh install deps/ubuntu-packages.lock /tmp/cache
  other:
    runs-on: ubuntu-24.04
    steps:
      # sh scripts/ubuntu-packages.sh install is only mentioned here
      - run: echo hello
"""


class TheChecksThemselves(unittest.TestCase):
    """Each check on a broken example, so that a check that can no longer fail is noticed (AC-4 of X-005)."""

    def test_a_lock_as_the_script_writes_it_passes(self) -> None:
        lock = parse_lock(EXAMPLE_LOCK)
        self.assertEqual(lock_problems(lock), [])
        self.assertEqual(lock.requests, ["hello"])
        self.assertEqual(len(lock.steps), 4)

    def test_a_snapshot_before_the_image_was_created_is_found(self) -> None:
        lock = parse_lock(EXAMPLE_LOCK.replace("snapshot 20261111T000000Z", "snapshot 20261026T000000Z"))
        self.assertEqual(len(lock_problems(lock)), 1)
        self.assertIn("before the base image was created", lock_problems(lock)[0])
        # The same second as the creation, but the image was created a fraction of a second later.
        lock = parse_lock(EXAMPLE_LOCK.replace("snapshot 20261111T000000Z", "snapshot 20261026T235000Z"))
        self.assertIn("before the base image was created", lock_problems(lock)[0])

    def test_a_package_that_is_unpacked_but_not_configured_is_found(self) -> None:
        lock = parse_lock(EXAMPLE_LOCK.replace("configure hello 2.10-3build2\n", ""))
        self.assertEqual(lock_problems(lock), ["hello is unpacked but never configured"])
        lock = parse_lock(EXAMPLE_LOCK.replace("configure hello 2.10-3build2", "configure hello 2.10-3"))
        self.assertIn("hello 2.10-3 is configured, but not unpacked before at that version", lock_problems(lock))

    def test_a_package_configured_before_it_is_unpacked_is_found(self) -> None:
        lines = EXAMPLE_LOCK.splitlines()
        lines[7:9], lines[9:10] = [lines[9]], lines[7:9]  # configure libhello1 first, then the two unpack lines
        self.assertIn("libhello1 1:1.0-1 is configured, but not unpacked before at that version", lock_problems(parse_lock("\n".join(lines))))

    def test_malformed_locks_are_refused(self) -> None:
        broken = {
            "no digest": EXAMPLE_LOCK.replace("@sha256:94ed690dd634b8b2f531eaa742fde9b1264077cd8ea695a3b960bd104ae4c481", ""),
            "unsorted requests": EXAMPLE_LOCK.replace("request hello\n", "request hello\nrequest abc\n"),
            "a pool path of another package": EXAMPLE_LOCK.replace("pool/main/h/hello/hello_2.10", "pool/main/h/hello/other_2.10"),
            "a path outside the pool": EXAMPLE_LOCK.replace("pool/main/h/hello/hello_", "pool/../h/hello/hello_"),
            "a short SHA-256": EXAMPLE_LOCK.replace("0000000000000000000000000000000000000000000000000000000000000002", "02"),
            "an unknown line": EXAMPLE_LOCK + "remove hello\n",
            "a missing header line": EXAMPLE_LOCK.replace("architecture amd64\n", ""),
            "no plan": "\n".join(EXAMPLE_LOCK.splitlines()[:7]) + "\n",
        }
        for name, text in broken.items():
            with self.subTest(name), self.assertRaises(ValueError):
                parse_lock(text)

    def test_a_job_in_another_image_or_without_a_container_is_found(self) -> None:
        jobs = installing_jobs(EXAMPLE_WORKFLOW)
        self.assertEqual(
            jobs,
            {
                "inside": "docker.io/library/buildpack-deps:noble-scm@sha256:94ed690dd634b8b2f531eaa742fde9b1264077cd8ea695a3b960bd104ae4c481",
                "inline": "docker.io/library/ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852",
                "outside": None,
            },
        )
        image = parse_lock(EXAMPLE_LOCK).image
        self.assertEqual([job for job, job_image in jobs.items() if job_image != image], ["inline", "outside"])

    def test_a_workflow_that_runs_apt_is_found(self) -> None:
        self.assertEqual(apt_lines(EXAMPLE_WORKFLOW), [])
        self.assertEqual(len(apt_lines("      - run: sudo apt-get install -y libgl-dev\n")), 1)
        self.assertEqual(len(apt_lines("      - run: apt -y install zstd\n")), 1)
        self.assertEqual(len(apt_lines("  UBUNTU_SNAPSHOT: 20261027T000000Z # for apt --snapshot\n")), 0)
        self.assertEqual(len(apt_lines("      run: dpkg-query --snapshot 1\n")), 1)
        self.assertEqual(len(apt_lines("      # sudo apt-get update --snapshot 20261027T000000Z\n")), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
