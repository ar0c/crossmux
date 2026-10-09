import configparser
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import build_nightly_index
import nightly_retention
import nightly_targets
import package_nightly_target
import verify_nightly_release


class NightlyTargetTest(unittest.TestCase):
    def test_boot_app0_uses_the_active_platformio_core(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / 'home'
            custom_core = root / 'isolated-core'
            relative = Path('packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin')
            default_asset = root / '.platformio' / relative
            custom_asset = custom_core / relative
            for asset in (default_asset, custom_asset):
                asset.parent.mkdir(parents=True)
                asset.write_bytes(b'boot_app0')
            with mock.patch.object(Path, 'home', return_value=home):
                for override, expected in (('', default_asset), (str(custom_core), custom_asset)):
                    with self.subTest(core_dir=override), mock.patch.dict(
                        'os.environ', {'PLATFORMIO_CORE_DIR': override}
                    ):
                        self.assertEqual(package_nightly_target.find_boot_app0(root), expected)
                with mock.patch.dict('os.environ', {}, clear=True):
                    self.assertEqual(package_nightly_target.find_boot_app0(root), default_asset)
                custom_asset.unlink()
                with mock.patch.dict('os.environ', {'PLATFORMIO_CORE_DIR': str(custom_core)}):
                    with self.assertRaises(SystemExit) as error:
                        package_nightly_target.find_boot_app0(root)
                    self.assertIn(str(custom_asset), str(error.exception))

    def test_fetch_retries_incomplete_reads(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.side_effect = [
            verify_nightly_release.http.client.IncompleteRead(b'partial', 1),
            b'complete',
        ]
        with mock.patch.object(
            verify_nightly_release.urllib.request, 'urlopen', return_value=response
        ) as urlopen, mock.patch.object(verify_nightly_release.time, 'sleep'):
            self.assertEqual(
                verify_nightly_release.fetch_bytes('https://assets.crossmux.cn/asset.bin'),
                b'complete',
            )
        self.assertEqual(urlopen.call_count, 2)

    def test_matrix_has_only_waveshare_s3_firmware(self):
        self.assertEqual(package_nightly_target.matrix('nightly')['include'], [
            {'targetId': 'waveshare_epaper_397', 'deviceSlug': 'waveshare-epaper-397',
             'environment': 'waveshare_epaper_397_nightly'}])
        self.assertEqual(package_nightly_target.matrix('stable')['include'], [])
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / 'platformio.ini', encoding='utf-8')
        self.assertEqual(config['platformio']['default_envs'], 'waveshare_epaper_397')
        self.assertFalse(any(name.startswith('env:x4pro') for name in config.sections()))


    def test_runtime_models_and_board_tags_are_explicit(self):
        self.assertEqual(set(nightly_targets.TARGETS), {'waveshare_epaper_397'})
        self.assertEqual(nightly_targets.TARGETS['waveshare_epaper_397']['boardTag'], 'waveshare_epaper_397')


    def test_versions_are_nightly_release_candidates(self):
        self.assertEqual(
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'global', '12345678'),
            '1.5.7-1234567-ws397-rc',
        )
        self.assertEqual(
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'zh-CN', '12345678'),
            '1.5.7-1234567-ws397-rc',
        )
        self.assertNotIn(
            'beta',
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'global', '1234567'),
        )
        self.assertLess(len(nightly_targets.version_for(
            '1.6.0', 'waveshare_epaper_397', 'nightly', 'global', 'abcdef0', 'local'
        )), 32)
        for revision in ('unknown', '123456', '123456g'):
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                nightly_targets.version_for('1.6.0', 'waveshare_epaper_397', 'nightly', 'global', revision)

    def test_workflow_packages_one_binary_set(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        hardware = (ROOT / '.github/workflows/hardware-ci.yml').read_text()
        self.assertIn('find artifacts -type f -print', workflow)
        self.assertEqual(workflow.count('python3 scripts/package_nightly_target.py "${{ matrix.targetId }}"'), 1)
        self.assertIn('for target in waveshare_epaper_397;', hardware)
        self.assertNotIn('x4pro', hardware)
        self.assertNotIn('xteink_x4_pro', workflow)
        self.assertNotIn('gh release delete', workflow)
        self.assertNotIn('gh release delete-asset', workflow)


    def test_dispatch_rejects_retired_firmware_and_selects_waveshare(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('auto|waveshare_epaper_397)', workflow)
        self.assertIn('target_args=(--only-target waveshare_epaper_397)', workflow)
        self.assertNotIn('publish_stable_release:', workflow)
        self.assertNotIn('  push:', workflow)


    def test_workflow_publishes_only_to_fork_github_releases(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('group: firmware-nightly-publish', workflow)
        self.assertNotIn('COS_SECRET_', workflow)
        self.assertNotIn('assets.crossmux.cn', workflow)
        self.assertIn('needs: [prepare, publish_github]', workflow)
        self.assertIn('needs: [prepare, verify_publish]', workflow)
        self.assertIn('python3 scripts/notify_ota_mirror.py', workflow)
        self.assertNotIn('cleanup_github:', workflow)


    def test_package_contains_one_binary_set_and_two_compatible_manifests(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / '.pio/build/waveshare_epaper_397_nightly'
            build.mkdir(parents=True)
            (build / 'bootloader.bin').write_bytes(b'bootloader')
            (build / 'partitions.bin').write_bytes(b'partitions')
            (build / 'firmware.bin').write_bytes(self.write_image(board='waveshare_epaper_397').read_bytes())
            boot_app0 = root / 'boot_app0.bin'
            boot_app0.write_bytes(b'boot_app0')
            (root / 'platformio.ini').write_text('[crosspoint]\nversion = 1.5.7\n')
            output = root / 'dist/waveshare_epaper_397'

            def git_value(_root, *args):
                return 'a' * (7 if '--short=7' in args else 40)

            with (
                mock.patch.object(package_nightly_target, 'verify_partition_csv'),
                mock.patch.object(package_nightly_target, 'find_boot_app0', return_value=boot_app0),
                mock.patch.object(package_nightly_target, 'git_value', side_effect=git_value),
            ):
                package_nightly_target.package_target(root, 'waveshare_epaper_397', 'nightly', output)

            self.assertEqual(
                {path.name for path in output.iterdir()},
                {
                    'crossmux-ar0c-waveshare-epaper-397-bootloader.bin',
                    'crossmux-ar0c-waveshare-epaper-397-partitions.bin',
                    'crossmux-ar0c-waveshare-epaper-397-boot_app0.bin',
                    'crossmux-ar0c-waveshare-epaper-397-firmware.bin',
                    'waveshare-epaper-397-global-manifest.json',
                    'waveshare-epaper-397-cn-manifest.json',
                    'waveshare-epaper-397-SHA256SUMS',
                },
            )
            manifests = [
                json.loads((output / nightly_targets.manifest_name('waveshare_epaper_397', flavor)).read_text())
                for flavor in nightly_targets.FLAVOR_TOKENS
            ]
            self.assertEqual(manifests[0]['assets'], manifests[1]['assets'])
            self.assertEqual(
                {key: value for key, value in manifests[0].items() if key != 'flavor'},
                {key: value for key, value in manifests[1].items() if key != 'flavor'},
            )

            local_output = root / 'dist/local-waveshare'
            local_tree = 'b' * 40
            local_version = '1.5.7-bbbbbbb-ws397-local'
            with (
                mock.patch.object(package_nightly_target, 'verify_partition_csv'),
                mock.patch.object(package_nightly_target, 'find_boot_app0', return_value=boot_app0),
                mock.patch.object(package_nightly_target, 'git_value', side_effect=git_value),
            ):
                package_nightly_target.package_target(
                    root, 'waveshare_epaper_397', 'nightly', local_output,
                    source_sha=local_tree, embedded_version=local_version,
                )
            for flavor in nightly_targets.FLAVOR_TOKENS:
                local = json.loads((local_output / nightly_targets.manifest_name('waveshare_epaper_397', flavor)).read_text())
                self.assertEqual(local['crossmuxSha'], local_tree)
                self.assertEqual(local['version'], local_version)
                self.assertEqual(local['sourceKind'], 'git-tree-local')

    def test_retired_target_cannot_be_packaged(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(KeyError):
                package_nightly_target.package_target(Path(temp), 'xteink_x4_pro', 'nightly', Path(temp) / 'out')


    def test_boot_app0_uses_isolated_platformio_core(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            expected = root / '.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
            expected.parent.mkdir(parents=True)
            expected.write_bytes(b'isolated')
            with mock.patch.dict('os.environ', {'PLATFORMIO_CORE_DIR': ''}):
                self.assertEqual(package_nightly_target.find_boot_app0(root), expected)
            custom = root / 'custom-core/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
            custom.parent.mkdir(parents=True)
            custom.write_bytes(b'custom')
            with mock.patch.dict('os.environ', {'PLATFORMIO_CORE_DIR': 'custom-core'}):
                self.assertEqual(package_nightly_target.find_boot_app0(root), custom)

    def test_github_publish_keeps_credentials_scoped(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        github_job = workflow.split('  publish_github:\n')[1].split('  verify_publish:\n')[0]
        self.assertIn('runs-on: ubuntu-latest', github_job)
        self.assertNotIn('COS_SECRET_', github_job)
        self.assertIn('GH_TOKEN: ${{ github.token }}', github_job)

    def test_nightly_retains_previous_evidence_without_cleanup(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('Load previous global Nightly index', workflow)
        self.assertIn('previous/global/release-index.json', workflow)
        self.assertNotIn('nightly_retention.py', workflow)


    def write_image(self, chip_id=0x0009, board='x4pro'):
        image = bytearray(24)
        image[0] = 0xE9
        image[12:14] = chip_id.to_bytes(2, 'little')
        image.extend(f'CROSSPOINT-BOARD-V1:{board};'.encode())
        temp = tempfile.NamedTemporaryFile(delete=False)
        temp.write(image)
        temp.close()
        self.addCleanup(Path(temp.name).unlink)
        return Path(temp.name)

    def test_waveshare_image_rejects_other_boards(self):
        package_nightly_target.verify_firmware(self.write_image(board='waveshare_epaper_397'), 0x0009, 'waveshare_epaper_397')
        with self.assertRaises(SystemExit):
            package_nightly_target.verify_firmware(self.write_image(board='x4pro'),
                                                  0x0009, 'waveshare_epaper_397')

    def test_rejects_wrong_chip_or_board(self):
        package_nightly_target.verify_firmware(self.write_image(), 0x0009, 'x4pro')
        with self.assertRaises(SystemExit):
            package_nightly_target.verify_firmware(self.write_image(chip_id=5), 0x0009, 'x4pro')
        with self.assertRaises(SystemExit):
            package_nightly_target.verify_firmware(self.write_image(board='waveshare_epaper_397'), 0x0009, 'x4pro')


class NightlyIndexTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write_pair(
        self, target_id, revision='a' * 40, sdk_revision='b' * 40, channel='nightly'
    ):
        target = nightly_targets.TARGETS[target_id]
        for flavor in nightly_targets.FLAVOR_TOKENS:
            manifest = {
                'schemaVersion': 1,
                'channel': channel,
                'targetId': target_id,
                'models': target['models'],
                'deviceSlug': target['deviceSlug'],
                'boardTag': target['boardTag'],
                'supportedChannels': target['supportedChannels'],
                'environment': nightly_targets.environment_for(target_id, channel, flavor),
                'chip': target['chip'],
                'flavor': flavor,
                'version': '1.5.8' if channel == 'stable' else f'1.5.7-rc+{revision[:7]}',
                'crossmuxSha': revision,
                'sdkSha': sdk_revision,
                'assets': [{
                    'role': 'firmware',
                    'name': nightly_targets.asset_name(target_id, 'firmware.bin'),
                    'sha256': 'd' * 64,
                }],
            }
            (self.root / nightly_targets.manifest_name(target_id, flavor)).write_text(json.dumps(manifest))

    def write_all_pairs(self, revision='a' * 40, sdk_revision='b' * 40, channel='nightly'):
        for target_id in nightly_targets.targets_for(channel):
            self.write_pair(target_id, revision, sdk_revision, channel)

    def test_waveshare_scope_ignores_retired_pointers_without_modifying_previous(self):
        self.write_all_pairs(revision='c' * 40)
        old = build_nightly_index.build_index(self.root, 'global', 'https://example.com/', 'old', 'old', 'nightly')
        old['targets']['xteink_x4_pro'] = {'targetId': 'xteink_x4_pro', 'historical': True}
        snapshot = json.dumps(old, sort_keys=True)
        self.write_pair('waveshare_epaper_397')
        new = build_nightly_index.build_index(self.root, 'global', 'https://example.com/', 'new', 'new', 'nightly',
                                              only_target='waveshare_epaper_397', previous=old)
        self.assertEqual(set(new['targets']), {'waveshare_epaper_397'})
        self.assertEqual(json.dumps(old, sort_keys=True), snapshot)


    def test_retired_partial_release_is_rejected(self):
        self.write_all_pairs()
        with self.assertRaisesRegex(ValueError, 'supported target'):
            build_nightly_index.build_index(self.root, 'global', 'https://example.com/', 'now', 'new', 'nightly',
                                            only_target='xteink_x4_pro')


    def test_builds_complete_index(self):
        self.write_all_pairs()
        index = build_nightly_index.build_index(
            self.root,
            'global',
            'https://example.com/nightly/',
            '2026-08-26T00:00:00Z',
            'nightly-test',
            'nightly',
        )
        self.assertEqual(set(index['targets']), set(nightly_targets.TARGETS))
        self.assertEqual(
            index['targets']['waveshare_epaper_397']['variants']['zh-CN']['manifestUrl'],
            'https://example.com/nightly/waveshare-epaper-397-cn-manifest.json',
        )

    def test_china_variants_share_one_target_directory_and_binary(self):
        self.write_all_pairs()
        index = build_nightly_index.build_index(
            self.root, 'cn', 'https://assets.example/firmware/builds/test/', 'now', 'test', 'nightly'
        )
        variants = index['targets']['waveshare_epaper_397']['variants']
        self.assertEqual(
            variants['global']['manifestUrl'],
            'https://assets.example/firmware/builds/test/waveshare_epaper_397/waveshare-epaper-397-global-manifest.json',
        )
        self.assertEqual(
            variants['zh-CN']['manifestUrl'],
            'https://assets.example/firmware/builds/test/waveshare_epaper_397/waveshare-epaper-397-cn-manifest.json',
        )
        manifests = [
            json.loads((self.root / nightly_targets.manifest_name('waveshare_epaper_397', flavor)).read_text())
            for flavor in nightly_targets.FLAVOR_TOKENS
        ]
        self.assertEqual(manifests[0]['assets'], manifests[1]['assets'])

    def test_stable_index_has_no_maintained_firmware_target(self):
        with self.assertRaisesRegex(ValueError, 'no maintained firmware targets'):
            build_nightly_index.build_index(self.root, 'global', 'https://example.com/', 'now', 'new', 'stable')


    def test_rejects_incomplete_target_set(self):
        self.write_all_pairs()
        (self.root / nightly_targets.manifest_name('waveshare_epaper_397', 'zh-CN')).unlink()
        with self.assertRaisesRegex(ValueError, 'expected one waveshare_epaper_397/zh-CN manifest'):
            build_nightly_index.build_index(
                self.root, 'cn', 'https://assets.example/firmware/builds/test/', 'now', 'test', 'nightly'
            )

    def test_rejects_mismatched_sdk_pair(self):
        self.write_all_pairs()
        chinese = self.root / nightly_targets.manifest_name('waveshare_epaper_397', 'zh-CN')
        manifest = json.loads(chinese.read_text())
        manifest['sdkSha'] = 'c' * 40
        chinese.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'SDK revisions do not match'):
            build_nightly_index.build_index(
                self.root, 'global', 'https://example.com/', 'now', 'test', 'nightly'
            )

    def test_rejects_mismatched_asset_pair(self):
        self.write_all_pairs()
        chinese = self.root / nightly_targets.manifest_name('waveshare_epaper_397', 'zh-CN')
        manifest = json.loads(chinese.read_text())
        manifest['assets'][0]['sha256'] = 'e' * 64
        chinese.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'assets do not match'):
            build_nightly_index.build_index(
                self.root, 'global', 'https://example.com/', 'now', 'test', 'nightly'
            )

    def test_rejects_mixed_manifest_revisions(self):
        self.write_all_pairs()
        chinese = self.root / nightly_targets.manifest_name('waveshare_epaper_397', 'zh-CN')
        manifest = json.loads(chinese.read_text())
        manifest['crossmuxSha'] = 'c' * 40
        chinese.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'flavor revisions do not match'):
            build_nightly_index.build_index(self.root, 'global', 'https://example.com/', 'now', 'new', 'nightly')



class NightlyRetentionTest(unittest.TestCase):
    def previous_index(self, storage, first_build, second_build):
        targets = {}
        for index, target_id in enumerate(('xteink_x4_pro', 'waveshare_epaper_397')):
            build = second_build if index == 0 else first_build
            if storage == 'github':
                base = f'https://github.com/0x1abin/crossmux/releases/download/{build}/'
            else:
                base = f'https://assets.crossmux.cn/firmware/builds/{build}/{target_id}/'
            targets[target_id] = {
                'targetId': target_id,
                'variants': {
                    flavor: {
                        'manifestUrl': base + (f'x4pro-{nightly_targets.FLAVOR_TOKENS[flavor]}-manifest.json'
                                               if target_id == 'xteink_x4_pro' else nightly_targets.manifest_name(target_id, flavor))
                    }
                    for flavor in nightly_targets.FLAVOR_TOKENS
                },
            }
        return {'schemaVersion': 1, 'channel': 'nightly', 'targets': targets}

    def test_github_keeps_current_and_every_build_in_previous_index(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        previous_fallback = f'nightly-build-{"c" * 40}-8-1'
        obsolete = f'nightly-build-{"d" * 40}-7-1'
        candidates = '\n'.join((current, previous, previous_fallback, obsolete, 'v1.5.7'))
        self.assertEqual(
            nightly_retention.obsolete_builds(
                'github',
                current,
                self.previous_index('github', previous, previous_fallback),
                candidates,
            ),
            [obsolete],
        )

    def test_fork_cleanup_accepts_only_its_own_previous_urls(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        index = self.previous_index('github', previous, previous)
        for entry in index['targets'].values():
            for pointer in entry['variants'].values():
                pointer['manifestUrl'] = pointer['manifestUrl'].replace('0x1abin/crossmux', 'ar0c/crossmux')
        self.assertEqual(nightly_retention.obsolete_builds(
            'github', current, index, '\n'.join((current, previous)), 'ar0c/crossmux'), [])
        with self.assertRaisesRegex(ValueError, 'unexpected previous'):
            nightly_retention.obsolete_builds(
                'github', current, index, '\n'.join((current, previous)), '0x1abin/crossmux')

    def test_cos_extracts_build_directories_from_listing(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'{"b" * 40}-9-1'
        previous_fallback = f'nightly-build-{"c" * 40}-8-1'
        obsolete = f'nightly-build-{"d" * 40}-7-1'
        candidates = '\n'.join(
            f'firmware/builds/{build}/ | DIR'
            for build in (current, previous, previous_fallback, obsolete)
        )
        self.assertEqual(
            nightly_retention.obsolete_builds(
                'cos',
                current,
                self.previous_index('cos', previous, previous_fallback),
                candidates,
            ),
            [obsolete],
        )

    def test_keeps_historical_builds_when_targets_change(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        previous_fallback = f'nightly-build-{"c" * 40}-8-1'
        obsolete = f'nightly-build-{"d" * 40}-7-1'
        for storage in ('github', 'cos'):
            candidates = '\n'.join((current, previous, previous_fallback, obsolete, 'stable', 'nightly'))
            for change in ('added', 'retired'):
                with self.subTest(storage=storage, change=change):
                    index = self.previous_index(storage, previous, previous_fallback)
                    if change == 'added':
                        del index['targets']['waveshare_epaper_397']
                        # The earlier one-board index references only the
                        # fallback build; the other candidate has no owner.
                        expected_obsolete = sorted((previous, obsolete))
                    else:
                        target = index['targets'].pop('xteink_x4_pro')
                        target['targetId'] = 'retired_device'
                        index['targets']['retired_device'] = target
                        expected_obsolete = [obsolete]
                    self.assertEqual(
                        nightly_retention.obsolete_builds(storage, current, index, candidates),
                        expected_obsolete,
                    )

    def test_rejects_malformed_historical_indexes(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        cases = (
            (('schemaVersion',), 2),
            (('channel',), 'stable'),
            (('targets',), None),
            (('targets',), []),
            (('targets',), {}),
            (('targets', 'xteink_x4_pro', 'targetId'), 'wrong_target'),
            (('targets', 'xteink_x4_pro', 'variants'), {'global': {}}),
            (('targets', 'xteink_x4_pro', 'variants', 'global'), {}),
            (('targets', 'xteink_x4_pro', 'variants', 'global', 'manifestUrl'), 'https://example.com/firmware.bin'),
        )
        for storage in ('github', 'cos'):
            for path, value in cases:
                with self.subTest(storage=storage, path=path, value=value):
                    index = self.previous_index(storage, previous, previous)
                    entry = index
                    for key in path[:-1]:
                        entry = entry[key]
                    entry[path[-1]] = value
                    with self.assertRaises(ValueError):
                        nightly_retention.obsolete_builds(storage, current, index, current)

    def test_rejects_empty_target_identity(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        for storage in ('github', 'cos'):
            index = self.previous_index(storage, previous, previous)
            index['targets'][''] = index['targets'].pop('xteink_x4_pro')
            index['targets']['']['targetId'] = ''
            with self.subTest(storage=storage), self.assertRaisesRegex(ValueError, 'variant set'):
                nightly_retention.obsolete_builds(storage, current, index, current)

    def test_rejects_unexpected_previous_url_and_incomplete_listing(self):
        current = f'nightly-build-{"a" * 40}-10-1'
        previous = f'nightly-build-{"b" * 40}-9-1'
        index = self.previous_index('github', previous, previous)
        index['targets']['waveshare_epaper_397']['variants']['global']['manifestUrl'] = (
            'https://example.com/firmware.bin'
        )
        with self.assertRaisesRegex(ValueError, 'unexpected previous waveshare_epaper_397/global'):
            nightly_retention.obsolete_builds('github', current, index, current)
        with self.assertRaisesRegex(ValueError, 'missing from the candidate list'):
            nightly_retention.obsolete_builds(
                'github', current, self.previous_index('github', previous, previous), previous
            )


class PublishedNightlyTest(unittest.TestCase):
    def setUp(self):
        self.index_url = (
            'https://github.com/0x1abin/crossmux/releases/download/nightly/release-index.json'
        )
        self.release_url = (
            'https://github.com/0x1abin/crossmux/releases/download/nightly-build-test/'
        )
        self.current_sha = 'a' * 40
        self.old_sha = 'c' * 40
        self.sdk_sha = 'b' * 40
        self.store = {}
        self.fetches = {}
        targets = {}
        for target_id, target in nightly_targets.TARGETS.items():
            revision = self.current_sha
            assets = []
            for role, name, offset in verify_nightly_release.expected_assets(target_id, 'nightly'):
                data = f'{target_id}/{role}'.encode()
                self.store[self.release_url + name] = data
                assets.append({
                    'role': role,
                    'name': name,
                    'offset': offset,
                    'size': len(data),
                    'sha256': hashlib.sha256(data).hexdigest(),
                })
            variants = {}
            for flavor in nightly_targets.FLAVOR_TOKENS:
                version = f'1.5.7-rc+{revision[:7]}'
                manifest = {
                    'schemaVersion': 1,
                    'channel': 'nightly',
                    'targetId': target_id,
                    'models': target['models'],
                    'deviceSlug': target['deviceSlug'],
                    'boardTag': target['boardTag'],
                    'supportedChannels': target['supportedChannels'],
                    'environment': nightly_targets.environment_for(target_id, 'nightly', flavor),
                    'flavor': flavor,
                    'version': version,
                    'crossmuxSha': revision,
                    'sdkSha': self.sdk_sha,
                    'assets': assets,
                }
                url = self.release_url + nightly_targets.manifest_name(target_id, flavor)
                self.store[url] = json.dumps(manifest).encode()
                variants[flavor] = {
                    'version': version,
                    'crossmuxSha': revision,
                    'sdkSha': self.sdk_sha,
                    'publishedAt': 'now',
                    'manifestUrl': url,
                }
            targets[target_id] = {
                'targetId': target_id,
                'models': target['models'],
                'deviceSlug': target['deviceSlug'],
                'boardTag': target['boardTag'],
                'supportedChannels': target['supportedChannels'],
                'variants': variants,
            }
        self.index = {
            'schemaVersion': 1,
            'channel': 'nightly',
            'updatedAt': 'now',
            'buildId': 'test',
            'targets': targets,
        }
        self.write_index()

    def write_index(self):
        self.store[self.index_url] = json.dumps(self.index).encode()

    def fetch(self, url):
        self.fetches[url] = self.fetches.get(url, 0) + 1
        return self.store[url]

    def test_scoped_publish_verifies_only_waveshare_assets(self):
        result = verify_nightly_release.verify_release(self.index_url, self.current_sha, 'nightly', self.fetch,
                                                       only_target='waveshare_epaper_397')
        self.assertEqual(result['targets'], 1)
        self.assertEqual(result['currentTargets'], 1)
        self.assertEqual(result['assets'], 4)


    def test_verifies_complete_current_release_with_one_asset_fetch(self):
        result = verify_nightly_release.verify_release(
            self.index_url, self.current_sha, 'nightly', self.fetch
        )
        self.assertEqual(result['targets'], len(nightly_targets.TARGETS))
        self.assertEqual(result['currentTargets'], len(nightly_targets.TARGETS))
        for url in self.store:
            if url.endswith('.bin'):
                self.assertEqual(self.fetches[url], 1)

    def test_github_assets_stay_in_the_index_repository(self):
        index_url = 'https://github.com/ar0c/crossmux/releases/download/nightly/release-index.json'
        verify_nightly_release.validate_url(
            'https://github.com/ar0c/crossmux/releases/download/nightly-build-test/crossmux-ar0c-x4pro-firmware.bin',
            index_url, 'nightly',
        )
        with self.assertRaisesRegex(ValueError, 'outside the expected release path'):
            verify_nightly_release.validate_url(
                'https://github.com/0x1abin/crossmux/releases/download/nightly-build-test/crossmux-ar0c-x4pro-firmware.bin',
                index_url, 'nightly',
            )

    def test_k3s_mirror_assets_stay_in_the_immutable_channel_build(self):
        index_url = 'https://ooo.ar0c.com/releases/download/nightly/release-index.json'
        build = 'nightly-build-' + 'a' * 40 + '-1-1'
        verify_nightly_release.validate_url(
            f'https://ooo.ar0c.com/releases/download/{build}/crossmux-ar0c-x4pro-firmware.bin',
            index_url, 'nightly',
        )
        for url in (
            f'https://github.com/ar0c/crossmux/releases/download/{build}/crossmux-ar0c-x4pro-firmware.bin',
            f'https://ooo.ar0c.com/releases/download/{build}/../stable/release-index.json',
            f'http://ooo.ar0c.com/releases/download/{build}/crossmux-ar0c-x4pro-firmware.bin',
        ):
            with self.assertRaises(ValueError):
                verify_nightly_release.validate_url(url, index_url, 'nightly')

    def test_rejects_target_from_previous_revision(self):
        target_id = 'waveshare_epaper_397'
        for flavor in nightly_targets.FLAVOR_TOKENS:
            url = self.release_url + nightly_targets.manifest_name(target_id, flavor)
            manifest = json.loads(self.store[url])
            manifest['crossmuxSha'] = self.old_sha
            self.store[url] = json.dumps(manifest).encode()
            self.index['targets'][target_id]['variants'][flavor]['crossmuxSha'] = self.old_sha
        self.write_index()
        with self.assertRaisesRegex(ValueError, 'does not point to the current revision'):
            verify_nightly_release.verify_release(self.index_url, self.current_sha, 'nightly', self.fetch)

    def test_rejects_missing_target(self):
        self.index['targets'].pop('waveshare_epaper_397')
        self.write_index()
        with self.assertRaisesRegex(ValueError, 'canonical target set'):
            verify_nightly_release.verify_release(self.index_url, self.current_sha, 'nightly', self.fetch)

    def test_rejects_manifest_difference(self):
        url = self.release_url + nightly_targets.manifest_name('waveshare_epaper_397', 'zh-CN')
        manifest = json.loads(self.store[url])
        manifest['unexpected'] = True
        self.store[url] = json.dumps(manifest).encode()
        with self.assertRaisesRegex(ValueError, 'differ beyond flavor'):
            verify_nightly_release.verify_release(self.index_url, self.current_sha, 'nightly', self.fetch)

    def test_rejects_corrupt_asset(self):
        url = self.release_url + nightly_targets.asset_name('waveshare_epaper_397', 'firmware.bin')
        self.store[url] += b'corrupt'
        with self.assertRaisesRegex(ValueError, 'size or SHA-256'):
            verify_nightly_release.verify_release(self.index_url, self.current_sha, 'nightly', self.fetch)

if __name__ == '__main__':
    unittest.main()
