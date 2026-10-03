#!/usr/bin/env python3
"""Build and manually publish one local Waveshare image to the fork's Nightly OTA.

The source identity is a Git tree object, so a staged but uncommitted build can
be named honestly. The source archive stays local; only firmware assets,
manifests, checksums, and the rolling index are uploaded.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from build_nightly_index import build_index
from generate_ota_notes import marker, validate_notes
from nightly_targets import TARGETS, version_for
from package_nightly_target import package_target, sha256
from verify_nightly_release import fetch_bytes, verify_release


ROOT = Path(__file__).resolve().parent.parent
REPO = "ar0c/crossmux"
CHANNEL = "nightly"
TARGET = "waveshare_epaper_397"
ENVIRONMENT = TARGETS[TARGET]["environments"][CHANNEL]
GITHUB_INDEX = f"https://github.com/{REPO}/releases/download/{CHANNEL}/release-index.json"
PUBLIC_INDEX = f"https://ooo.ar0c.com/releases/download/{CHANNEL}/release-index.json"
SHA40 = re.compile(r"[0-9a-f]{40}")
BUILD_TAG = re.compile(r"nightly-build-[0-9a-f]{40}-[0-9]{14}-1")


def run(*command: str, cwd: Path = ROOT, capture: bool = True) -> str:
    environment = os.environ.copy()
    environment.pop("GH_DEBUG", None)
    environment.pop("GH_HTTP_DEBUG", None)
    environment["GH_PROMPT_DISABLED"] = "1"
    result = subprocess.run(
        command, cwd=cwd, env=environment, text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        check=False,
    )
    if result.returncode:
        # gh may include short-lived signed asset URLs in debug output. Do not
        # echo stderr or a command containing private arguments.
        raise RuntimeError(f"{Path(command[0]).name} exited with {result.returncode}")
    return result.stdout.strip() if capture else ""


def git(*args: str, cwd: Path = ROOT) -> str:
    return run("git", *args, cwd=cwd)


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def current_index() -> bytes:
    with tempfile.TemporaryDirectory(prefix="crossmux-nightly-") as temporary:
        run("gh", "release", "download", CHANNEL, "--repo", REPO,
            "--pattern", "release-index.json", "--dir", temporary)
        return (Path(temporary) / "release-index.json").read_bytes()


def source_tree() -> tuple[str, str, str]:
    if git("diff", "--name-only"):
        raise ValueError("tracked files have unstaged edits; stage the intended source snapshot first")
    untracked = [name for name in git("ls-files", "--others", "--exclude-standard").splitlines()
                 if name and not name.startswith("artifacts/")]
    if untracked:
        raise ValueError(f"untracked source is outside the snapshot: {untracked[0]}")
    tree = git("write-tree")
    if not SHA40.fullmatch(tree) or git("cat-file", "-t", tree) != "tree":
        raise ValueError("could not freeze the staged Git tree")
    sdk_tree = git("ls-tree", tree, "freeink-sdk").split()
    sdk = git("rev-parse", "HEAD", cwd=ROOT / "freeink-sdk")
    if len(sdk_tree) != 4 or sdk_tree[1] != "commit" or sdk_tree[2] != sdk:
        raise ValueError("FreeInk SDK checkout does not match the staged source tree")
    return tree, sdk, git("rev-parse", "HEAD")


def image_version(tree: str) -> str:
    config = configparser.ConfigParser()
    config.read(ROOT / "platformio.ini", encoding="utf-8")
    base = config["crosspoint"]["version"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", base):
        raise ValueError("firmware base version is not numeric")
    return version_for(base, TARGET, CHANNEL, "global", tree, build_kind="local")


def check_embedded_version(image: Path, expected: str) -> None:
    data = image.read_bytes()
    if expected.encode() + b"\0" not in data:
        raise ValueError("firmware does not contain the expected local source-tree version")
    found = set(re.findall(rb"[0-9]+\.[0-9]+\.[0-9]+-[0-9a-f]{7}-ws397-(?:rc|local)", data))
    if found != {expected.encode()}:
        raise ValueError("firmware contains a conflicting Waveshare Nightly version")


def verify_prepared(index: bytes, build: Path, tag: str, tree: str) -> None:
    prefix = f"https://github.com/{REPO}/releases/download/{tag}/"

    def fetch(url: str) -> bytes:
        if urlparse_path(url) == urlparse_path(GITHUB_INDEX):
            return index
        if url.startswith(prefix):
            name = url[len(prefix):]
            if "/" in name or "?" in name or "#" in name:
                raise ValueError("unsafe prepared asset URL")
            return (build / name).read_bytes()
        return fetch_bytes(url)

    verify_release(GITHUB_INDEX, tree, CHANNEL, fetch=fetch, only_target=TARGET)


def urlparse_path(url: str) -> str:
    from urllib.parse import urlparse
    parsed = urlparse(url)
    return parsed.netloc + parsed.path


def prepare(args: argparse.Namespace) -> None:
    tree, sdk, target_commit = source_tree()
    version = image_version(tree)
    notes = validate_notes(json.loads(args.notes_json.read_text(encoding="utf-8")))
    previous_bytes = current_index()
    previous = json.loads(previous_bytes)
    if previous.get("schemaVersion") != 1 or previous.get("channel") != CHANNEL or set(previous.get("targets", {})) != set(TARGETS):
        raise ValueError("the current Nightly index is incomplete")
    tag = f"nightly-build-{tree}-{datetime.now(timezone.utc):%Y%m%d%H%M%S}-1"
    if not BUILD_TAG.fullmatch(tag):
        raise ValueError("invalid immutable build tag")
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError("prepared release directory already exists")
    output.mkdir(parents=True)
    build = output / "build"
    previous_dir = output / "previous"
    previous_dir.mkdir()
    (previous_dir / "release-index.json").write_bytes(previous_bytes)
    (output / "ota-notes.md").write_text(marker(notes), encoding="utf-8")

    if args.build:
        environment = os.environ.copy()
        environment["CROSSPOINT_RC_HASH"] = tree[:7]
        environment["CROSSPOINT_BUILD_KIND"] = "local"
        environment["PLATFORMIO_BUILD_DIR"] = str(output / "pio-build")
        result = subprocess.run([sys.executable, "-m", "platformio", "run", "-e", ENVIRONMENT],
                                cwd=ROOT, env=environment, check=False)
        if result.returncode:
            raise RuntimeError(f"PlatformIO build failed with {result.returncode}")
    build_root = output / "pio-build" if args.build else ROOT / ".pio" / "build"
    image = build_root / ENVIRONMENT / "firmware.bin"
    if not image.is_file():
        raise FileNotFoundError(image)
    check_embedded_version(image, version)
    package_target(ROOT, TARGET, CHANNEL, build, source_sha=tree, embedded_version=version,
                   build_root=build_root)
    source_archive = output / f"source-tree-{tree}.zip"
    run("git", "archive", "--format=zip", "--output", str(source_archive), tree)
    updated_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    candidate = build_index(build, "global",
                            f"https://github.com/{REPO}/releases/download/{tag}/",
                            updated_at, tag, CHANNEL, notes, TARGET, previous)
    if candidate["targets"]["xteink_x4_pro"] != previous["targets"]["xteink_x4_pro"]:
        raise ValueError("X4 Pro pointer changed during Waveshare-only packaging")
    candidate_bytes = canonical_json(candidate)
    (output / "release-index.json").write_bytes(candidate_bytes)
    verify_prepared(candidate_bytes, build, tag, tree)
    assets = sorted(path.name for path in build.iterdir() if path.is_file())
    state = {
        "repository": REPO, "channel": CHANNEL, "target": TARGET,
        "buildTag": tag, "sourceKind": "git-tree-local", "sourceTree": tree,
        "sdkSha": sdk, "targetCommit": target_commit, "version": version,
        "imageSha256": sha256(image), "previousIndexSha256": hashlib.sha256(previous_bytes).hexdigest(),
        "candidateIndexSha256": hashlib.sha256(candidate_bytes).hexdigest(),
        "sourceArchiveSha256": sha256(source_archive),
        "assets": {name: sha256(build / name) for name in assets},
    }
    (output / "state.json").write_bytes(canonical_json(state))
    print(f"Prepared {tag}\nVersion: {version}\nFirmware SHA-256: {state['imageSha256']}\nDirectory: {output}")


def load_prepared(path: Path) -> tuple[dict, bytes, bytes]:
    state = json.loads((path / "state.json").read_text(encoding="utf-8"))
    if (state.get("repository"), state.get("channel"), state.get("target")) != (REPO, CHANNEL, TARGET):
        raise ValueError("prepared release has a different destination")
    if not BUILD_TAG.fullmatch(state.get("buildTag", "")) or not SHA40.fullmatch(state.get("sourceTree", "")):
        raise ValueError("prepared release has invalid source identity")
    for name, digest in state["assets"].items():
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]+", name) or sha256(path / "build" / name) != digest:
            raise ValueError(f"prepared asset changed: {name}")
    archive = path / f"source-tree-{state['sourceTree']}.zip"
    if sha256(archive) != state["sourceArchiveSha256"]:
        raise ValueError("local source archive changed")
    previous = (path / "previous" / "release-index.json").read_bytes()
    candidate = (path / "release-index.json").read_bytes()
    if hashlib.sha256(previous).hexdigest() != state["previousIndexSha256"] or hashlib.sha256(candidate).hexdigest() != state["candidateIndexSha256"]:
        raise ValueError("prepared rolling index changed")
    return state, previous, candidate


def wait_public_index(expected: bytes, seconds: int) -> None:
    expected_index = json.loads(expected)
    mirrored_targets = json.loads(json.dumps(expected_index["targets"]))
    github_assets = f"https://github.com/{REPO}/releases/download/"
    public_assets = "https://ooo.ar0c.com/releases/download/"
    for entry in mirrored_targets.values():
        for pointer in entry["variants"].values():
            url = pointer["manifestUrl"]
            if not url.startswith(github_assets):
                raise ValueError("prepared index contains a non-fork manifest URL")
            pointer["manifestUrl"] = public_assets + url[len(github_assets):]
    deadline = time.monotonic() + seconds
    while True:
        try:
            fetched = fetch_bytes(f"{PUBLIC_INDEX}?local={int(time.time())}", attempts=1)
            index = json.loads(fetched)
            if index.get("buildId") == expected_index.get("buildId") and index.get("targets") == mirrored_targets:
                return
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
            pass
        if time.monotonic() >= deadline:
            raise TimeoutError("public K3s mirror did not confirm the expected Nightly index")
        time.sleep(min(20, max(1, deadline - time.monotonic())))


def publish(args: argparse.Namespace) -> None:
    prepared = args.prepared.resolve()
    state, previous, candidate = load_prepared(prepared)
    if current_index() != previous:
        raise ValueError("GitHub Nightly index changed since preparation")
    public = json.loads(fetch_bytes(PUBLIC_INDEX, attempts=1))
    if public.get("buildId") != json.loads(previous)["buildId"]:
        raise ValueError("public mirror is not at the prepared baseline")
    tag = state["buildTag"]
    probe = subprocess.run(["gh", "release", "view", tag, "--repo", REPO],
                           cwd=ROOT, env={**os.environ, "GH_DEBUG": "", "GH_PROMPT_DISABLED": "1"},
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if probe.returncode == 0:
        raise ValueError("immutable release tag already exists; inspect it before retrying")
    notes_file = prepared / "release-body.md"
    notes_file.write_text(
        f"Local Waveshare Nightly test build.\n\nGit source tree: `{state['sourceTree']}`\n"
        f"FreeInk SDK: `{state['sdkSha']}`\nFirmware version: `{state['version']}`\n\n"
        + (prepared / "ota-notes.md").read_text(encoding="utf-8"), encoding="utf-8")
    assets = [str(prepared / "build" / name) for name in state["assets"]]
    run("gh", "release", "create", tag, *assets, "--repo", REPO,
        "--target", state["targetCommit"], "--title", f"CrossMux Nightly Waveshare {state['version']}",
        "--notes-file", str(notes_file), "--prerelease")
    # The immutable release is visible now. Verify every new and preserved
    # manifest and binary before moving the rolling channel pointer.
    def candidate_fetch(url: str) -> bytes:
        return candidate if urlparse_path(url) == urlparse_path(GITHUB_INDEX) else fetch_bytes(url)
    verify_release(GITHUB_INDEX, state["sourceTree"], CHANNEL,
                   fetch=candidate_fetch, only_target=TARGET)
    run("gh", "release", "upload", CHANNEL, str(prepared / "release-index.json"),
        "--repo", REPO, "--clobber")
    if current_index() != candidate:
        raise RuntimeError("GitHub rolling index readback differs from the candidate")
    wait_public_index(candidate, args.wait_seconds)
    verify_release(f"{PUBLIC_INDEX}?local={int(time.time())}", state["sourceTree"], CHANNEL, only_target=TARGET)
    print(f"Published and verified {tag} on GitHub and the public K3s mirror")


def rollback(args: argparse.Namespace) -> None:
    prepared = args.prepared.resolve()
    state, previous, candidate = load_prepared(prepared)
    if current_index() != candidate:
        raise ValueError("GitHub Nightly is no longer this prepared release; refusing rollback")
    run("gh", "release", "upload", CHANNEL, str(prepared / "previous" / "release-index.json"),
        "--repo", REPO, "--clobber")
    if current_index() != previous:
        raise RuntimeError("GitHub rollback index readback differs from the backup")
    wait_public_index(previous, args.wait_seconds)
    old_sha = json.loads(previous)["targets"][TARGET]["variants"]["global"]["crossmuxSha"]
    verify_release(f"{PUBLIC_INDEX}?local={int(time.time())}", old_sha, CHANNEL, only_target=TARGET)
    print(f"Restored the previous Nightly index from before {state['buildTag']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prep = actions.add_parser("prepare", help="freeze, build, and verify a Waveshare-only candidate")
    prep.add_argument("--notes-json", type=Path, required=True)
    prep.add_argument("--output", type=Path, required=True)
    prep.add_argument("--build", action="store_true", help="run the complete PlatformIO build")
    for name in ("publish", "rollback"):
        action = actions.add_parser(name)
        action.add_argument("--prepared", type=Path, required=True)
        action.add_argument("--wait-seconds", type=int, default=600)
    args = parser.parse_args()
    if args.action == "prepare":
        prepare(args)
    elif args.action == "publish":
        publish(args)
    else:
        rollback(args)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"manual Nightly release stopped: {error}", file=sys.stderr)
        raise SystemExit(1) from None
