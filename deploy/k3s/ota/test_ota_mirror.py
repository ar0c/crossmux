import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mirror_ota_release as mirror


class MirrorTest(unittest.TestCase):
    def release(self, tag, only=None):
        channel = 'nightly'
        assets = {}
        targets = {}
        for target in sorted(mirror.TARGETS[channel]):
            if only is not None and target not in only:
                continue
            board_tag, slug = mirror.TARGETS[channel][target]
            firmware = f'crossmux-ar0c-{slug}-firmware.bin'
            data = (target * 64).encode()
            assets[firmware] = data
            manifest_assets = [{
                'role': 'firmware', 'name': firmware, 'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
            }]
            variants = {}
            for flavor, suffix in [('global', 'global'), ('zh-CN', 'cn')]:
                filename = f'{slug}-{suffix}-manifest.json'
                url = mirror.SOURCE + tag + '/' + filename
                manifest = {
                    'schemaVersion': 1, 'channel': channel, 'targetId': target,
                    'flavor': flavor, 'boardTag': board_tag, 'version': '1.0',
                    'crossmuxSha': 'a' * 40, 'sdkSha': 'b' * 40,
                    'assets': manifest_assets,
                }
                assets[filename] = json.dumps(manifest).encode()
                variants[flavor] = {
                    'manifestUrl': url, 'version': '1.0',
                    'crossmuxSha': 'a' * 40, 'sdkSha': 'b' * 40,
                }
            targets[target] = {'targetId': target, 'boardTag': board_tag, 'variants': variants}
        index = {'schemaVersion': 1, 'channel': channel, 'buildId': tag, 'targets': targets}
        source = {mirror.SOURCE + tag + '/' + name: data for name, data in assets.items()}
        source[mirror.SOURCE + 'nightly/release-index.json'] = json.dumps(index).encode()
        return index, source

    def test_waveshare_only_transition_and_mixed_historical_rollback(self):
        old_tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        partial_tag = 'nightly-build-' + 'b' * 40 + '-2-1'
        new_tag = 'nightly-build-' + 'c' * 40 + '-3-1'
        old_index, old_source = self.release(old_tag)
        previous_index, previous_source = self.release(partial_tag)
        previous_index['targets']['xteink_x4_pro'] = old_index['targets']['xteink_x4_pro']
        new_index, new_source = self.release(new_tag, only={'waveshare_epaper_397'})
        source = {**old_source, **previous_source, **new_source}
        index_url = mirror.SOURCE + 'nightly/release-index.json'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                for index in (old_index, previous_index):
                    source[index_url] = json.dumps(index).encode()
                    mirror.mirror_channel(root, 'nightly')
                # Model upgrading the old mirror, which had no private history.
                for file in (root / '.verified-indexes').iterdir():
                    file.unlink()
                before = {str(file.relative_to(root)): file.read_bytes()
                          for tag in (old_tag, partial_tag) for file in (root / tag).iterdir()}
                source[index_url] = json.dumps(new_index).encode()
                self.assertEqual(mirror.mirror_channel(root, 'nightly'), (new_tag, 1))
                self.assertEqual(mirror.mirror_channel(root, 'nightly'), (new_tag, 0))
                published = json.loads((root / 'nightly/release-index.json').read_bytes())
                self.assertEqual(set(published['targets']), {'waveshare_epaper_397'})
                source[index_url] = json.dumps(previous_index).encode()
                self.assertEqual(mirror.mirror_channel(root, 'nightly')[0], partial_tag)
                self.assertEqual(mirror.mirror_channel(root, 'nightly'), (partial_tag, 0))
                self.assertEqual(set(json.loads((root / 'nightly/release-index.json').read_bytes())['targets']),
                                 set(mirror.TARGETS['nightly']))
                for name, data in before.items():
                    self.assertEqual((root / name).read_bytes(), data)
                self.assertTrue((root / new_tag).is_dir())

    def test_waveshare_only_corruption_keeps_previous_index(self):
        old_tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        new_tag = 'nightly-build-' + 'c' * 40 + '-2-1'
        _, old_source = self.release(old_tag)
        _, new_source = self.release(new_tag, only={'waveshare_epaper_397'})
        source = {**old_source, **new_source}
        firmware_url = next(url for url in new_source if url.endswith('-firmware.bin'))
        source[firmware_url] = b'wrong'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: old_source[url]):
                mirror.mirror_channel(root, 'nightly')
            before = (root / 'nightly/release-index.json').read_bytes()
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                with self.assertRaisesRegex(ValueError, 'failed SHA-256'):
                    mirror.mirror_channel(root, 'nightly')
            self.assertEqual((root / 'nightly/release-index.json').read_bytes(), before)
            self.assertFalse((root / new_tag).exists())

    def test_unknown_target_or_missing_waveshare_rejected(self):
        tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        for target_set in ({'xteink_x4_pro'}, {'waveshare_epaper_397', 'unknown'}, set()):
            with self.subTest(target_set=target_set), tempfile.TemporaryDirectory() as temp:
                index, source = self.release(tag)
                index['targets'] = {name: {} for name in target_set}
                source[mirror.SOURCE + 'nightly/release-index.json'] = json.dumps(index).encode()
                with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                    with self.assertRaisesRegex(ValueError, 'unexpected target set'):
                        mirror.mirror_channel(Path(temp), 'nightly')

    def test_failed_asset_never_replaces_rolling_index(self):
        channel = 'nightly'
        tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        index, source = self.release(tag)

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            previous = root / 'nightly' / 'release-index.json'
            previous.parent.mkdir()
            previous_bytes = json.dumps(index).encode()
            previous.write_bytes(previous_bytes)
            corrupted = dict(source)
            firmware_url = next(url for url in source if url.endswith('-firmware.bin'))
            corrupted[firmware_url] = b'wrong'
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: corrupted[url]):
                with self.assertRaisesRegex(ValueError, 'failed SHA-256'):
                    mirror.mirror_channel(root, channel)
            self.assertEqual(previous.read_bytes(), previous_bytes)
            self.assertFalse((root / tag).exists())

            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                self.assertEqual(mirror.mirror_channel(root, channel)[0], tag)
                self.assertEqual(mirror.mirror_channel(root, channel), (tag, 0))
            published = json.loads(previous.read_bytes())
            for entry in published['targets'].values():
                for variant in entry['variants'].values():
                    self.assertTrue(variant['manifestUrl'].startswith(mirror.DESTINATION + tag + '/'))

    def test_partial_nightly_preserves_verified_previous_target(self):
        channel = 'nightly'
        old_tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        new_tag = 'nightly-build-' + 'c' * 40 + '-2-1'
        old_index, old_source = self.release(old_tag)
        new_index, new_source = self.release(new_tag)
        new_index['targets']['xteink_x4_pro'] = old_index['targets']['xteink_x4_pro']
        source = {**old_source, **new_source}

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: old_source[url]):
                mirror.mirror_channel(root, channel)
            old_files = {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                         for file in (root / old_tag).iterdir()}

            source[mirror.SOURCE + channel + '/release-index.json'] = json.dumps(new_index).encode()
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                self.assertEqual(mirror.mirror_channel(root, channel)[0], new_tag)
                self.assertEqual(mirror.mirror_channel(root, channel), (new_tag, 0))
            published = json.loads((root / channel / 'release-index.json').read_bytes())
            self.assertEqual(published['buildId'], new_tag)
            for target, tag in [('xteink_x4_pro', old_tag), ('waveshare_epaper_397', new_tag)]:
                for pointer in published['targets'][target]['variants'].values():
                    self.assertTrue(pointer['manifestUrl'].startswith(mirror.DESTINATION + tag + '/'))
            self.assertEqual(old_files, {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                                         for file in (root / old_tag).iterdir()})
            self.assertTrue((root / new_tag).is_dir())

    def test_partial_nightly_rejects_changed_preserved_target(self):
        channel = 'nightly'
        old_tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        new_tag = 'nightly-build-' + 'c' * 40 + '-2-1'
        old_index, old_source = self.release(old_tag)
        new_index, new_source = self.release(new_tag)
        new_index['targets']['xteink_x4_pro'] = old_index['targets']['xteink_x4_pro']
        new_index['targets']['xteink_x4_pro']['variants']['global']['version'] = 'changed'
        source = {**old_source, **new_source,
                  mirror.SOURCE + channel + '/release-index.json': json.dumps(new_index).encode()}

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: old_source[url]):
                mirror.mirror_channel(root, channel)
            before = (root / channel / 'release-index.json').read_bytes()
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                with self.assertRaisesRegex(ValueError, 'preserved target differs'):
                    mirror.mirror_channel(root, channel)
            self.assertEqual((root / channel / 'release-index.json').read_bytes(), before)
            self.assertFalse((root / new_tag).exists())


if __name__ == '__main__':
    unittest.main()
