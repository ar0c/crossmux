"""Ensure the saved-image helper rejects corruption and path escapes before I/O."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from flash_saved import main, verified_images


class SavedFlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.manifest = {'format': 1}
        for key, payload in (('application', b'\xe9test'), ('original_flash', b'\xff' * 0x800000)):
            name = key + '.bin'
            (self.folder / name).write_bytes(payload)
            self.manifest[key] = {'file': name, 'bytes': len(payload),
                                  'sha256': hashlib.sha256(payload).hexdigest()}
        self.save_manifest()

    def save_manifest(self):
        (self.folder / 'manifest.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def test_valid_retained_images(self):
        _, images = verified_images(self.folder)
        self.assertEqual(images['application'], self.folder / 'application.bin')

    def test_corrupted_image_rejected(self):
        (self.folder / 'application.bin').write_bytes(b'\xe9oops')
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            verified_images(self.folder)

    def test_image_path_cannot_escape_folder(self):
        # Resolve to an existing file outside the firmware directory.
        self.manifest['application']['file'] = str(Path(__file__).resolve())
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'inside the saved firmware folder'):
            verified_images(self.folder)

    def test_check_does_not_start_flasher(self):
        with patch('flash_saved.verified_images', return_value=({}, {})), \
                patch('flash_saved.version', side_effect=['5.3.0', '3.5']), \
                patch('flash_saved.subprocess.run') as run:
            self.assertEqual(main(['--check']), 0)
            run.assert_not_called()

    def test_failed_flash_is_not_retried(self):
        manifest, images = verified_images(self.folder)
        with patch('flash_saved.verified_images', return_value=(manifest, images)), \
                patch('flash_saved.version', side_effect=['5.3.0', '3.5']), \
                patch('flash_saved.subprocess.run', return_value=SimpleNamespace(returncode=1)) as run:
            self.assertEqual(main([]), 1)
            run.assert_called_once()
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--previous-firmware') + 1], str(images['application']))
            self.assertNotIn('erase-flash', command)
