#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys

VERSION_FILE = Path("openevse-tft.yaml")
VERSION_RE = re.compile(r'(^\s*version:\s*")(\d+\.\d+\.\d+)(".*$)', re.MULTILINE)
BREAKING_RE = re.compile(r"^([a-z]+)(?:\([^)]+\))?!:", re.MULTILINE)
TYPE_RE = re.compile(r"^([a-z]+)(?:\([^)]+\))?:", re.MULTILINE)
BREAKING_FOOTER_RE = re.compile(r"^BREAKING[ -]CHANGE:\s", re.MULTILINE)
BUMP_ORDER = {"none": 0, "patch": 1, "minor": 2, "major": 3}


@dataclass
class ReleaseInfo:
    current_version: str
    latest_tag: str | None
    bump: str
    next_version: str
    changed: bool


def run_git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        check=check,
        capture_output=True,
        text=True,
    )


def read_current_version() -> str:
    text = VERSION_FILE.read_text()
    match = VERSION_RE.search(text)
    if match is None:
        raise SystemExit(f"Could not find firmware version in {VERSION_FILE}.")
    return match.group(2)


def write_version(version: str) -> None:
    text = VERSION_FILE.read_text()
    updated, replacements = VERSION_RE.subn(rf'\g<1>{version}\g<3>', text, count=1)
    if replacements != 1:
        raise SystemExit(f"Could not update firmware version in {VERSION_FILE}.")
    VERSION_FILE.write_text(updated)


def latest_tag() -> str | None:
    result = run_git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", check=False)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def commits_since(tag: str | None) -> list[tuple[str, str]]:
    revision_range = f"{tag}..HEAD" if tag else "HEAD"
    result = run_git("log", "--format=%s%x1f%b%x1e", revision_range)
    commits: list[tuple[str, str]] = []
    for record in result.stdout.split("\x1e"):
        record = record.strip()
        if not record:
            continue
        subject, _, body = record.partition("\x1f")
        commits.append((subject.strip(), body.strip()))
    return commits


def classify_bump(commits: list[tuple[str, str]]) -> str:
    bump = "none"
    for subject, body in commits:
        if BREAKING_RE.match(subject) or BREAKING_FOOTER_RE.search(body):
            return "major"
        match = TYPE_RE.match(subject)
        if match is None:
            continue
        commit_type = match.group(1)
        if commit_type == "feat":
            bump = max(bump, "minor", key=BUMP_ORDER.get)
        elif commit_type in {"fix", "perf"}:
            bump = max(bump, "patch", key=BUMP_ORDER.get)
    return bump


def bump_version(version: str, bump: str) -> str:
    major, minor, patch = (int(part) for part in version.split("."))
    if bump == "major":
        return f"{major + 1}.0.0"
    if bump == "minor":
        return f"{major}.{minor + 1}.0"
    if bump == "patch":
        return f"{major}.{minor}.{patch + 1}"
    return version


def build_release_info() -> ReleaseInfo:
    current = read_current_version()
    tag = latest_tag()
    if tag is not None and tag.removeprefix("v") != current:
        raise SystemExit(
            f"Latest tag {tag} does not match firmware version {current} in {VERSION_FILE}."
        )

    bump = classify_bump(commits_since(tag))
    next_version = bump_version(current, bump)
    return ReleaseInfo(
        current_version=current,
        latest_tag=tag,
        bump=bump,
        next_version=next_version,
        changed=next_version != current,
    )


def write_github_output(path: str, info: ReleaseInfo) -> None:
    output = Path(path)
    with output.open("a") as handle:
        handle.write(f"current_version={info.current_version}\n")
        handle.write(f"latest_tag={info.latest_tag or ''}\n")
        handle.write(f"bump={info.bump}\n")
        handle.write(f"next_version={info.next_version}\n")
        handle.write(f"changed={'true' if info.changed else 'false'}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compute and optionally apply the next firmware version from conventional commits."
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="Update openevse-tft.yaml when a version bump is required.",
    )
    parser.add_argument(
        "--github-output",
        help="Optional path to the GitHub Actions output file.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    info = build_release_info()

    print(f"Current firmware version: {info.current_version}")
    print(f"Latest release tag: {info.latest_tag or '<none>'}")
    print(f"Required bump: {info.bump}")
    print(f"Next firmware version: {info.next_version}")

    if args.write and info.changed:
        write_version(info.next_version)
        print(f"Updated {VERSION_FILE} to {info.next_version}.")

    if args.github_output:
        write_github_output(args.github_output, info)

    return 0


if __name__ == "__main__":
    sys.exit(main())
