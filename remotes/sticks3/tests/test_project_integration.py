"""Check relocated tool defaults and the generic flash rejection boundary."""
import argparse
from pathlib import Path
import runpy
import sys
import types
import unittest
from unittest.mock import patch


PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[1]
sys.path.insert(0, str(PROJECT / 'tools'))


class DefaultsCaptured(Exception):
    pass


class IntegrationTests(unittest.TestCase):
    def test_receiver_tools_find_enclosing_repository(self):
        self.assertTrue((REPO / 'platformio.ini').is_file())
        for name in ('closed_loop', 'global_controls', 'learn_map',
                     'observe_joystick', 'reader_control', 'usb_stress'):
            with self.subTest(tool=name):
                def capture(parser, *args, **kwargs):
                    self.assertEqual(parser.get_default('repo'), REPO)
                    raise DefaultsCaptured
                with patch.object(argparse.ArgumentParser, 'parse_args', capture):
                    with self.assertRaises(DefaultsCaptured):
                        runpy.run_path(str(PROJECT / 'tools' / (name + '.py')))

    def run_guard(self, targets):
        script = types.ModuleType('SCons.Script')
        script.COMMAND_LINE_TARGETS = targets
        with patch.dict(sys.modules, {'SCons': types.ModuleType('SCons'), 'SCons.Script': script}):
            runpy.run_path(str(PROJECT / 'tools/reject_unsafe_upload.py'))

    def test_plain_build_remains_available(self):
        self.run_guard([])
        self.run_guard(['buildprog'])
        self.run_guard(['clean'])

    def test_generic_flash_targets_fail_before_upload(self):
        for target in ('upload', 'uploadfs', 'uploadfsota', 'erase'):
            with self.subTest(target=target):
                with self.assertRaisesRegex(RuntimeError, 'verified full backup'):
                    self.run_guard([target])


if __name__ == '__main__':
    unittest.main()
