"""CI-to-release provenance, integrity, fail-closed selection and ZIP boundary tests."""

from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_firmware_package as package
import package_nightly_target as packager
import promote_ci_firmware as promotion

ROOT = Path(__file__).resolve().parents[2]
REPO, REF, SHA, SDK = 'ar0c/crossmux', 'refs/heads/main', 'a' * 40, 'b' * 40
VERSION = '1.6.5-aaaaaaa-ws397-rc'


def good_run():
    return {'id': 123, 'run_attempt': 1, 'status': 'completed', 'conclusion': 'success',
            'event': 'push', 'path': promotion.WORKFLOW, 'head_sha': SHA, 'head_branch': 'main',
            'repository': {'full_name': REPO, 'id': 42}, 'head_repository': {'full_name': REPO, 'id': 42}}


def good_artifact(data=b'archive'):
    return {'id': 456, 'name': promotion.artifact_name(1), 'size_in_bytes': len(data), 'expired': False,
            'digest': 'sha256:' + hashlib.sha256(data).hexdigest(),
            'expires_at': (datetime.now(timezone.utc) + timedelta(days=14)).isoformat(),
            'workflow_run': {'id': 123, 'head_sha': SHA, 'head_branch': 'main',
                             'repository_id': 42, 'head_repository_id': 42}}


class FakeAPI:
    def __init__(self, archive):
        self.archive = archive
        self.run = good_run()
        self.artifact = good_artifact(archive.read_bytes())
        self.downloads = 0
        self.race = None

    def json(self, path):
        if '/workflows/ci.yml/runs?' in path:
            return {'workflow_runs': [deepcopy(self.run)]}
        if '/artifacts?' in path:
            return {'artifacts': [deepcopy(self.artifact)]}
        if path.endswith('/artifacts/456'):
            return deepcopy(self.artifact)
        if path.endswith('/runs/123'):
            if self.downloads and self.race:
                return self.race()
            return deepcopy(self.run)
        raise AssertionError(path)

    def download(self, repository, artifact, destination):
        self.downloads += 1
        shutil.copyfile(self.archive, destination)


class FirmwareReuseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / 'package'
        build = self.root / '.pio/build/waveshare_epaper_397_nightly'
        build.mkdir(parents=True)
        image = bytearray(24)
        image[0], image[12] = 0xE9, 9
        image.extend(b'CROSSPOINT-BOARD-V1:waveshare_epaper_397;\0' + VERSION.encode() + b'\0')
        (build / 'firmware.bin').write_bytes(image)
        (build / 'bootloader.bin').write_bytes(b'bootloader')
        (build / 'partitions.bin').write_bytes(b'partition table')
        boot = self.root / 'boot_app0.bin'
        boot.write_bytes(b'boot app0')
        (self.root / 'platformio.ini').write_text('[crosspoint]\nversion=1.6.5\n')
        shutil.copyfile(ROOT / 'partitions.csv', self.root / 'partitions.csv')

        def git_value(root, *args):
            if 'HEAD:freeink-sdk' in args or Path(root).name == 'freeink-sdk':
                return SDK
            return SHA[:7] if '--short=7' in args else SHA

        for module in (package, packager):
            patcher = mock.patch.object(module, 'git_value', side_effect=git_value)
            patcher.start()
            self.addCleanup(patcher.stop)
        with mock.patch.object(packager, 'find_boot_app0', return_value=boot):
            packager.package_target(self.root, package.TARGET, 'nightly', self.out)
        package.seal(self.root, self.out, REPO, REF, 123, 1)

    def zip(self, mutate=None):
        path = self.root / 'package.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for source in self.out.iterdir():
                archive.write(source, source.name)
            if mutate:
                mutate(archive)
        return path

    def test_complete_package_promotes_without_changing_any_bytes(self):
        api = FakeAPI(self.zip())
        destination = self.root / 'promoted'
        result = promotion.promote(api, self.root, REPO, REF, destination)
        self.assertEqual((result['runId'], result['runAttempt'], result['artifactId']), (123, 1, 456))
        self.assertEqual(result['version'], VERSION)
        for source in self.out.iterdir():
            self.assertEqual(source.read_bytes(), (destination / source.name).read_bytes())
        self.assertEqual(api.downloads, 1)
        with self.assertRaises(FileExistsError):
            promotion.promote(api, self.root, REPO, REF, destination, '123')

    def test_rejects_tampered_or_incomplete_packages(self):
        mutations = [
            lambda: (self.out / 'unexpected.bin').write_bytes(b'extra'),
            lambda: (self.out / package.manifest_name(package.TARGET, 'global')).unlink(),
            lambda: (self.out / package.asset_name(package.TARGET, 'firmware.bin')).write_bytes(b'wrong chip'),
            lambda: (self.out / (package.TARGETS[package.TARGET]['deviceSlug'] + '-SHA256SUMS')).write_text(''),
            lambda: (self.out / package.provenance_name()).write_text('{}'),
        ]
        original = {path.name: path.read_bytes() for path in self.out.iterdir()}
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                mutation()
                with self.assertRaises((ValueError, SystemExit)):
                    package.verify_seal(self.root, self.out, REPO, REF, 123, 1)
                for path in self.out.iterdir():
                    if path.name not in original:
                        path.unlink()
                for name, data in original.items():
                    (self.out / name).write_bytes(data)

    def test_rejects_manifest_identity_changes_even_if_files_are_rehashed(self):
        path = self.out / package.manifest_name(package.TARGET, 'global')
        original = json.loads(path.read_text())
        for key, value in [('sdkSha', 'c' * 40), ('crossmuxSha', 'c' * 40), ('version', '1.6.4-old'),
                           ('environment', 'debug'), ('sourceKind', 'git-tree-local'), ('boardTag', 'wrong')]:
            with self.subTest(key=key):
                changed = dict(original, **{key: value})
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    package.verify_package(self.root, self.out)
        path.write_text(json.dumps(original))

    def test_provenance_binds_ci_attempt_repository_and_ref(self):
        for repo, ref, run_id, attempt in [(REPO, REF, 124, 1), (REPO, REF, 123, 2),
                                          ('other/crossmux', REF, 123, 1), (REPO, 'refs/heads/other', 123, 1)]:
            with self.subTest(repo=repo, ref=ref, run_id=run_id, attempt=attempt), self.assertRaises(ValueError):
                package.verify_seal(self.root, self.out, repo, ref, run_id, attempt)

    def test_image_checks_survive_consistent_manifest_and_checksum_rewrites(self):
        firmware = self.out / package.asset_name(package.TARGET, 'firmware.bin')
        original = firmware.read_bytes()
        wrong_chip = bytearray(original)
        wrong_chip[12] = 5
        from debug_build_policy import DEBUG_MARKER
        variants = [bytes(wrong_chip), original.replace(b'waveshare_epaper_397;', b'another_board_name_;'),
                    original + DEBUG_MARKER, original.replace(VERSION.encode(), b'other-version'),
                    original + b'\0' * package.EXPECTED_PARTITIONS['app0'][1]]
        for data in variants:
            with self.subTest(size=len(data), digest=hashlib.sha256(data).hexdigest()):
                firmware.write_bytes(data)
                for flavor in package.FLAVOR_TOKENS:
                    path = self.out / package.manifest_name(package.TARGET, flavor)
                    manifest = json.loads(path.read_text())
                    asset = next(asset for asset in manifest['assets'] if asset['role'] == 'firmware')
                    asset.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
                    path.write_text(json.dumps(manifest))
                checksum = package.TARGETS[package.TARGET]['deviceSlug'] + '-SHA256SUMS'
                (self.out / checksum).write_text(''.join(
                    f'{package.sha256(self.out / name)}  {name}\n'
                    for name in sorted(package.package_names() - {checksum})))
                with self.assertRaises((ValueError, SystemExit)):
                    package.verify_package(self.root, self.out)

    def test_pull_requests_can_upload_but_cannot_be_promoted(self):
        (self.out / package.provenance_name()).unlink()
        package.seal(self.root, self.out, REPO, 'refs/pull/7/merge', 123, 1)
        package.verify_seal(self.root, self.out, REPO, 'refs/pull/7/merge', 123, 1)
        run = dict(good_run(), event='pull_request')
        with self.assertRaises(ValueError):
            promotion.validate_run(run, REPO, REF, SHA)

    def test_manual_branch_ci_remains_supported(self):
        run = dict(good_run(), event='workflow_dispatch', head_branch='feature')
        promotion.validate_run(run, REPO, 'refs/heads/feature', SHA)

    def test_rejects_untrusted_or_incomplete_runs(self):
        mutations = [{'status': 'in_progress'}, {'conclusion': 'failure'}, {'conclusion': 'cancelled'},
                     {'event': 'pull_request'}, {'head_sha': 'c' * 40}, {'head_branch': 'other'},
                     {'path': '.github/workflows/other.yml'}, {'head_repository': {'full_name': 'other/fork', 'id': 7}},
                     {'repository': {'full_name': 'other/repo', 'id': 42}}]
        for changed in mutations:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                promotion.validate_run(dict(good_run(), **changed), REPO, REF, SHA)

    def test_rejects_wrong_attempt_expired_or_unbound_artifacts(self):
        mutations = [{'expired': True}, {'digest': None}, {'digest': 'sha256:bad'}, {'size_in_bytes': 0},
                     {'size_in_bytes': promotion.MAX_ARCHIVE + 1}, {'name': promotion.artifact_name(2)},
                     {'workflow_run': {}}, {'expires_at': '2000-01-01T00:00:00Z'}]
        for changed in mutations:
            with self.subTest(changed=changed), self.assertRaises((ValueError, TypeError)):
                promotion.validate_artifact(dict(good_artifact(), **changed), good_run())

    def test_missing_package_does_not_use_legacy_bin_or_rebuild(self):
        api = FakeAPI(self.zip())
        api.artifact['name'] = 'crossmux-ar0c-waveshare-epaper-397-firmware.bin'
        with self.assertRaises(ValueError):
            promotion.promote(api, self.root, REPO, REF, self.root / 'never-written')
        self.assertEqual(api.downloads, 0)
        self.assertFalse((self.root / 'never-written').exists())

    def test_no_matching_successful_ci_has_no_download_side_effects(self):
        api = FakeAPI(self.zip())
        api.run['conclusion'] = 'failure'
        with self.assertRaises(ValueError):
            promotion.select_source(api, REPO, REF, SHA)
        with self.assertRaises(ValueError):
            promotion.select_source(api, REPO, REF, SHA, '123')
        self.assertEqual(api.downloads, 0)

    def test_rerun_during_download_prevents_promotion(self):
        api = FakeAPI(self.zip())
        api.race = lambda: dict(api.run, run_attempt=2)
        with self.assertRaises(ValueError):
            promotion.promote(api, self.root, REPO, REF, self.root / 'never-written')
        self.assertFalse((self.root / 'never-written').exists())

    def test_selection_ignores_pr_runs_and_finds_a_later_page(self):
        api = FakeAPI(self.zip())
        original = api.json
        def paginated(path):
            if '/workflows/ci.yml/runs?' in path:
                if path.endswith('&page=1'):
                    return {'workflow_runs': [dict(good_run(), event='pull_request')] * 100}
                return {'workflow_runs': [good_run()]}
            return original(path)
        with mock.patch.object(api, 'json', side_effect=paginated):
            run, artifact = promotion.select_source(api, REPO, REF, SHA)
        self.assertEqual((run['id'], artifact['id']), (123, 456))

    def test_duplicate_artifact_names_are_not_selected_arbitrarily(self):
        api = FakeAPI(self.zip())
        original = api.json
        def duplicate(path):
            if '/artifacts?' in path:
                return {'artifacts': [api.artifact, dict(api.artifact, id=457)]}
            return original(path)
        with mock.patch.object(api, 'json', side_effect=duplicate), self.assertRaises(ValueError):
            promotion.select_source(api, REPO, REF, SHA)
        self.assertEqual(api.downloads, 0)

    def test_zip_expansion_is_bounded_before_extraction(self):
        path = self.root / 'large.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for index, name in enumerate(sorted(package.package_names() | {package.provenance_name()})):
                archive.writestr(name, b'\0' * (promotion.MAX_ARCHIVE + 1) if index == 0 else b'')
        with self.assertRaises(ValueError):
            promotion.extract_package(path, self.root / 'extract')
        self.assertFalse((self.root / 'extract').exists())

    def test_zip_rejects_extra_nested_duplicate_and_symlink_entries(self):
        for name in ['../escape', 'nested/file', package.provenance_name()]:
            with self.subTest(name=name):
                path = self.zip(lambda archive: archive.writestr(name, b'invalid'))
                with self.assertRaises(ValueError):
                    promotion.extract_package(path, self.root / 'extract')
        path = self.root / 'symlink.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            for source in self.out.iterdir():
                entry = zipfile.ZipInfo(source.name)
                entry.create_system = 3
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(entry, b'link')
        with self.assertRaises(ValueError):
            promotion.extract_package(path, self.root / 'extract')

    def test_download_verifies_archive_digest_and_never_forwards_token(self):
        data = b'archive bytes'
        client = promotion.GitHub('secret-test-token')
        for valid in (True, False):
            artifact = good_artifact(data)
            if not valid:
                artifact['digest'] = 'sha256:' + '0' * 64
            redirect = urllib.error.HTTPError('https://api.github.com/path', 302, 'redirect',
                                              {'Location': 'https://blob.example/package?signature=secret'}, None)
            with mock.patch.object(client.opener, 'open', side_effect=[redirect, io.BytesIO(data)]) as opener:
                if valid:
                    client.download(REPO, artifact, self.root / 'download.zip')
                else:
                    with self.assertRaises(ValueError):
                        client.download(REPO, artifact, self.root / 'download.zip')
                self.assertEqual(opener.call_args_list[0].args[0].get_header('Authorization'), 'Bearer secret-test-token')
                self.assertIsNone(opener.call_args_list[1].args[0].get_header('Authorization'))


if __name__ == '__main__':
    unittest.main()
