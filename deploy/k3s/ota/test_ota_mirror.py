import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mirror_ota_release as mirror


class MirrorTest(unittest.TestCase):
    def test_failed_asset_never_replaces_rolling_index(self):
        channel = 'nightly'
        tag = 'nightly-build-' + 'a' * 40 + '-1-1'
        assets = {}
        targets = {}
        for target in sorted(mirror.TARGETS[channel]):
            board_tag, slug = mirror.TARGETS[channel][target]
            revision = ('c' if target == 'xteink_x4_pro' else 'a') * 40
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
                    'crossmuxSha': revision, 'sdkSha': 'b' * 40,
                    'assets': manifest_assets,
                }
                assets[filename] = json.dumps(manifest).encode()
                variants[flavor] = {
                    'manifestUrl': url, 'version': '1.0',
                    'crossmuxSha': revision, 'sdkSha': 'b' * 40,
                }
            targets[target] = {'targetId': target, 'boardTag': board_tag, 'variants': variants}
        index = {'schemaVersion': 1, 'channel': channel, 'buildId': tag,
                 'updatedTargets': ['waveshare_epaper_397'], 'targets': targets}
        source = {mirror.SOURCE + tag + '/' + name: data for name, data in assets.items()}
        source[mirror.SOURCE + 'nightly/release-index.json'] = json.dumps(index).encode()

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            previous = root / 'nightly' / 'release-index.json'
            previous.parent.mkdir()
            previous.write_bytes(b'previous index')
            corrupted = dict(source)
            firmware_url = next(url for url in source if url.endswith('-firmware.bin'))
            corrupted[firmware_url] = b'wrong'
            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: corrupted[url]):
                with self.assertRaisesRegex(ValueError, 'failed SHA-256'):
                    mirror.mirror_channel(root, channel)
            self.assertEqual(previous.read_bytes(), b'previous index')
            self.assertFalse((root / tag).exists())

            with patch.object(mirror, 'fetch', side_effect=lambda url, _limit: source[url]):
                self.assertEqual(mirror.mirror_channel(root, channel)[0], tag)
                self.assertEqual(mirror.mirror_channel(root, channel), (tag, 0))
            published = json.loads(previous.read_bytes())
            for entry in published['targets'].values():
                for variant in entry['variants'].values():
                    self.assertTrue(variant['manifestUrl'].startswith(mirror.DESTINATION + tag + '/'))


if __name__ == '__main__':
    unittest.main()
