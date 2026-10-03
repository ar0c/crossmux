import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import manual_nightly_release as manual


class ManualNightlyReleaseTest(unittest.TestCase):
    def test_embedded_version_must_match_the_source_tree(self):
        version = "1.6.0-1234567-ws397-local"
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "firmware.bin"
            image.write_bytes(b"header\0" + version.encode() + b"\0")
            manual.check_embedded_version(image, version)
            with self.assertRaisesRegex(ValueError, "expected local"):
                manual.check_embedded_version(image, version.replace("1234567", "abcdef0"))
            image.write_bytes(image.read_bytes() + b"1.5.8-abcdef0-ws397-rc\0")
            with self.assertRaisesRegex(ValueError, "conflicting"):
                manual.check_embedded_version(image, version)

    def test_public_readback_requires_both_targets_and_rewritten_urls(self):
        github = "https://github.com/ar0c/crossmux/releases/download/"
        public = "https://ooo.ar0c.com/releases/download/"
        targets = {
            name: {"variants": {
                "global": {"manifestUrl": github + tag + "/global-manifest.json"},
                "zh-CN": {"manifestUrl": github + tag + "/cn-manifest.json"},
            }}
            for name, tag in (("xteink_x4_pro", "nightly-build-" + "a" * 40 + "-1-1"),
                              ("waveshare_epaper_397", "nightly-build-" + "b" * 40 + "-2-1"))
        }
        expected = {"buildId": "nightly-build-" + "b" * 40 + "-2-1", "targets": targets}
        mirrored = json.loads(json.dumps(expected))
        for entry in mirrored["targets"].values():
            for pointer in entry["variants"].values():
                pointer["manifestUrl"] = pointer["manifestUrl"].replace(github, public)
        with mock.patch.object(manual, "fetch_bytes", return_value=json.dumps(mirrored).encode()):
            manual.wait_public_index(manual.canonical_json(expected), 0)
        mirrored["targets"]["xteink_x4_pro"]["variants"]["global"]["version"] = "changed"
        with mock.patch.object(manual, "fetch_bytes", return_value=json.dumps(mirrored).encode()):
            with self.assertRaises(TimeoutError):
                manual.wait_public_index(manual.canonical_json(expected), 0)

    def test_rollback_refuses_to_replace_a_newer_channel(self):
        state = {"buildTag": "nightly-build-" + "a" * 40 + "-1-1"}
        with (mock.patch.object(manual, "load_prepared", return_value=(state, b"old", b"candidate")),
              mock.patch.object(manual, "current_index", return_value=b"newer"),
              mock.patch.object(manual, "run") as command):
            with self.assertRaisesRegex(ValueError, "no longer this prepared release"):
                manual.rollback(SimpleNamespace(prepared=Path("."), wait_seconds=0))
            command.assert_not_called()

    def test_prepared_checksum_filename_is_accepted_and_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / "build").mkdir()
            (path / "previous").mkdir()
            checksum_name = "waveshare-epaper-397-SHA256SUMS"
            (path / "build" / checksum_name).write_bytes(b"checksum")
            (path / "previous" / "release-index.json").write_bytes(b"old")
            (path / "release-index.json").write_bytes(b"new")
            tree = "a" * 40
            (path / f"source-tree-{tree}.zip").write_bytes(b"archive")
            digest = lambda data: manual.hashlib.sha256(data).hexdigest()
            state = {
                "repository": manual.REPO, "channel": manual.CHANNEL, "target": manual.TARGET,
                "buildTag": f"nightly-build-{tree}-20261002080000-1", "sourceTree": tree,
                "assets": {checksum_name: digest(b"checksum")},
                "sourceArchiveSha256": digest(b"archive"),
                "previousIndexSha256": digest(b"old"),
                "candidateIndexSha256": digest(b"new"),
            }
            (path / "state.json").write_text(json.dumps(state), encoding="utf-8")
            self.assertEqual(manual.load_prepared(path)[1:], (b"old", b"new"))
            (path / "build" / checksum_name).write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "prepared asset changed"):
                manual.load_prepared(path)


if __name__ == "__main__":
    unittest.main()
