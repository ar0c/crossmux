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

    def test_matrix_has_only_two_s3_targets(self):
        matrix = package_nightly_target.matrix('nightly')['include']
        self.assertEqual(len(matrix), 2)
        self.assertEqual(
            {entry['targetId'] for entry in matrix},
            set(nightly_targets.TARGETS),
        )
        environments = {entry['environment'] for entry in matrix}
        self.assertEqual(len(environments), 2)
        self.assertTrue(all(target['chip'] == 'ESP32-S3' for target in nightly_targets.TARGETS.values()))
        self.assertEqual(
            package_nightly_target.matrix('stable')['include'],
            [{'targetId': 'xteink_x4_pro', 'deviceSlug': 'x4pro', 'environment': 'x4pro-gh_release'}],
        )

    def test_runtime_models_and_board_tags_are_explicit(self):
        targets = nightly_targets.TARGETS
        self.assertEqual(targets['xteink_x4_pro']['boardTag'], 'x4pro')
        self.assertEqual(targets['waveshare_epaper_397']['boardTag'], 'waveshare_epaper_397')
        self.assertEqual(set(targets), {'xteink_x4_pro', 'waveshare_epaper_397'})

    def test_versions_are_nightly_release_candidates(self):
        self.assertEqual(
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'global', '12345678'),
            '1.5.7-waveshare-epaper-397-rc+1234567',
        )
        self.assertEqual(
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'zh-CN', '12345678'),
            '1.5.7-waveshare-epaper-397-rc+1234567',
        )
        self.assertNotIn(
            'beta',
            nightly_targets.version_for('1.5.7', 'waveshare_epaper_397', 'nightly', 'global', '1234567'),
        )
        self.assertEqual(
            nightly_targets.version_for('1.5.8', 'xteink_x4_pro', 'stable', 'global', '1234567'),
            '1.5.8',
        )

    def test_workflow_packages_one_binary_set(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        hardware_workflow = (ROOT / '.github/workflows/hardware-ci.yml').read_text()
        self.assertIn("find artifacts -type f -print", workflow)
        self.assertNotIn("find artifacts -path '*/global/*'", workflow)
        self.assertEqual(
            workflow.count('python3 scripts/package_nightly_target.py "${{ matrix.targetId }}"'), 1
        )
        self.assertIn('--channel "${{ needs.prepare.outputs.channel }}"', workflow)
        self.assertNotIn('for flavor in global cn', workflow)
        self.assertIn('assets=(artifacts/crossmux-ar0c-x4pro-firmware.bin', workflow)
        self.assertIn('for legacy_asset in firmware.bin firmware-cn.bin', workflow)
        self.assertNotIn('--flavor', hardware_workflow)
        self.assertIn('(cd "dist/nightly/$target" && sha256sum --check *-SHA256SUMS)', hardware_workflow)

    def test_workflow_publishes_only_to_fork_github_releases(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn("group: firmware-${{ github.event_name == 'push'", workflow)
        self.assertIn("if: needs.prepare.outputs.channel == 'stable'", workflow)
        self.assertNotIn('  publish_cn:', workflow)
        self.assertNotIn('  cleanup_cn:', workflow)
        self.assertNotIn('COS_SECRET_', workflow)
        self.assertNotIn('assets.crossmux.cn', workflow)
        rolling = workflow.split('- name: Publish rolling global index last', 1)[1].split('  verify_publish:', 1)[0]
        self.assertGreater(rolling.index('legacy_assets='), rolling.index('gh release upload "$CHANNEL" release-index.json'))
        verify = workflow.split('  verify_publish:', 1)[1]
        self.assertIn('needs: [prepare, publish_github]', verify)
        self.assertEqual(verify.count('python3 scripts/verify_nightly_release.py'), 1)
        self.assertIn("needs.publish_github.outputs.has_previous == 'true'", workflow)
        self.assertEqual(workflow.count('python3 scripts/nightly_retention.py'), 1)
        self.assertIn('gh release delete "$build_tag"', workflow)
        self.assertIn('--cleanup-tag --yes', workflow)

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

    def test_stable_package_has_release_version_and_full_s3_install(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / '.pio/build/x4pro-gh_release'
            build.mkdir(parents=True)
            (build / 'bootloader.bin').write_bytes(b'bootloader')
            (build / 'partitions.bin').write_bytes(b'partitions')
            (build / 'firmware.bin').write_bytes(
                self.write_image(chip_id=0x0009, board='x4pro').read_bytes()
            )
            boot_app0 = root / 'boot_app0.bin'
            boot_app0.write_bytes(b'boot_app0')
            (root / 'platformio.ini').write_text('[crosspoint]\nversion = 1.5.8\n')
            output = root / 'dist/xteink_x4_pro'

            def git_value(_root, *args):
                return 'a' * (7 if '--short=7' in args else 40)

            with (
                mock.patch.object(package_nightly_target, 'verify_partition_csv'),
                mock.patch.object(package_nightly_target, 'find_boot_app0', return_value=boot_app0),
                mock.patch.object(package_nightly_target, 'git_value', side_effect=git_value),
            ):
                package_nightly_target.package_target(root, 'xteink_x4_pro', 'stable', output)

            manifest = json.loads((output / 'x4pro-global-manifest.json').read_text())
            self.assertEqual(manifest['channel'], 'stable')
            self.assertEqual(manifest['version'], '1.5.8')
            self.assertEqual(manifest['environment'], 'x4pro-gh_release')
            self.assertEqual(
                [asset['role'] for asset in manifest['assets']],
                ['bootloader', 'partitions', 'boot_app0', 'firmware'],
            )

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

    def test_first_nightly_publish_skips_cleanup_without_previous_index(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('BUILD_TAG: ${{ needs.prepare.outputs.build_tag }}', workflow)
        self.assertIn('if gh release view nightly --repo "$GITHUB_REPOSITORY"', workflow)
        self.assertIn("echo 'present=false' >> \"$GITHUB_OUTPUT\"", workflow)
        self.assertIn("needs.publish_github.outputs.has_previous == 'true'", workflow)
        self.assertIn('--repository "$GITHUB_REPOSITORY"', workflow)

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

    def test_stable_index_requires_notes_and_contains_only_x4_pro(self):
        self.write_all_pairs(channel='stable')
        with self.assertRaisesRegex(ValueError, 'Stable release notes are required'):
            build_nightly_index.build_index(
                self.root, 'global', 'https://example.com/', 'now', 'test', 'stable'
            )
        notes = {'en': ['One', 'Two'], 'zh': ['一', '二']}
        index = build_nightly_index.build_index(
            self.root, 'global', 'https://example.com/', 'now', 'test', 'stable', notes
        )
        self.assertEqual(set(index['targets']), {'xteink_x4_pro'})
        self.assertEqual(index['releaseNotes'], {'global': notes['en'], 'zh-CN': notes['zh']})

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

    def test_rejects_mixed_target_revisions(self):
        self.write_all_pairs()
        self.write_pair('waveshare_epaper_397', revision='c' * 40)
        with self.assertRaisesRegex(ValueError, 'target CrossMux revisions do not match'):
            build_nightly_index.build_index(
                self.root, 'global', 'https://example.com/', 'now', 'test', 'nightly'
            )


class NightlyRetentionTest(unittest.TestCase):
    def previous_index(self, storage, first_build, second_build):
        targets = {}
        for index, target_id in enumerate(nightly_targets.TARGETS):
            build = second_build if index == 0 else first_build
            if storage == 'github':
                base = f'https://github.com/0x1abin/crossmux/releases/download/{build}/'
            else:
                base = f'https://assets.crossmux.cn/firmware/builds/{build}/{target_id}/'
            targets[target_id] = {
                'targetId': target_id,
                'variants': {
                    flavor: {
                        'manifestUrl': base + nightly_targets.manifest_name(target_id, flavor)
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

    def test_rejects_target_from_previous_revision(self):
        target_id = 'xteink_x4_pro'
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
