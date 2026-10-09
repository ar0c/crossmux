from pathlib import Path
import runpy
import tempfile
import unittest


class WolfsslTargetScopeTest(unittest.TestCase):
    def test_patch_only_changes_the_selected_environment(self):
        script = Path(__file__).resolve().parents[1] / 'patch_wolfssl.py'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = {}
            for target in ('waveshare_epaper_397', 'x4pro'):
                path = root / '.pio/libdeps' / target / 'Arduino-wolfSSL/src/user_settings.h'
                path.parent.mkdir(parents=True)
                path.write_text('/* existing settings */\n')
                paths[target] = path
            class Env(dict):
                def subst(self, value):
                    self.assert_subst = value
                    return str(root)
            env = Env(PIOENV='waveshare_epaper_397')
            runpy.run_path(str(script), init_globals={'env': env, 'Import': lambda _: None})
            self.assertIn('CrossPoint wolfSSL compatibility overrides', paths['waveshare_epaper_397'].read_text())
            self.assertEqual(paths['x4pro'].read_text(), '/* existing settings */\n')


if __name__ == '__main__':
    unittest.main()
