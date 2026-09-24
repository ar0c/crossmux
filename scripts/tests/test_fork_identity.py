from pathlib import Path
import runpy
import unittest
import tempfile
import hashlib


class ForkIdentityTest(unittest.TestCase):
    def test_version_and_filename_identify_owner_device_and_revision(self):
        module = runpy.run_path(str(Path(__file__).resolve().parents[1] / "git_branch.py"))
        version, filename = module["x4pro_identity"]("1.5.8", "408dfcd", "260915-181930")
        self.assertEqual(version, "260915-181930-ar0c-1.5.8-x4pro")
        self.assertEqual(filename, "crossmux-ar0c-260915-181930-408dfcd-x4pro-1.5.8.bin")
        self.assertLess(len(version.encode()), 32)

    def test_x4pro_build_exports_exact_image_with_matching_version(self):
        root = Path(__file__).resolve().parents[2]
        module = runpy.run_path(str(root / "scripts/git_branch.py"))

        class Env(dict):
            def Append(self, **values):
                self.update(values)

            def AddPostAction(self, target, action):
                self.action = action

        env = Env(PIOENV='x4pro', PROJECT_DIR=str(root))
        module['inject_version'](env)
        sha = module['get_git_short_sha'](str(root))
        injected = env['CPPDEFINES'][0][1]
        stamp = injected.split('ar0c')[0].strip('\\"').rstrip('-')
        version, _ = module['x4pro_identity']('1.5.8', sha, stamp)
        self.assertIn(version, env['CPPDEFINES'][0][1])
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'firmware.bin'
            image.write_bytes(b'test image bytes')

            class Node:
                def get_abspath(self):
                    return str(image)

            env.action([Node()], [], env)
            filename = module['x4pro_artifact_name'](stamp, image)
            self.assertTrue(filename.startswith('crossmux-ar0c-' + stamp + '-' +
                                                hashlib.sha256(image.read_bytes()).hexdigest()[:8]))
            self.assertEqual((image.parent / filename).read_bytes(), image.read_bytes())
            image.write_bytes(b'different uncommitted build')
            self.assertNotEqual(filename, module['x4pro_artifact_name'](stamp, image))

    def test_other_development_targets_export_branded_images(self):
        root = Path(__file__).resolve().parents[2]
        module = runpy.run_path(str(root / 'scripts/git_branch.py'))

        class Env(dict):
            def Append(self, **values):
                self.update(values)

            def AddPostAction(self, target, action):
                self.action = action

        env = Env(PIOENV='waveshare_epaper_397', PROJECT_DIR=str(root))
        module['inject_version'](env)
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'firmware.bin'
            image.write_bytes(b'test image bytes')

            class Node:
                def get_abspath(self):
                    return str(image)

            env.action([Node()], [], env)
            filename = module['dev_artifact_name']('1.5.8', 'waveshare_epaper_397',
                                                   module['get_git_short_sha'](str(root)), image)
            self.assertTrue(filename.startswith('crossmux-ar0c-1.5.8-waveshare-epaper-397-'))
            self.assertEqual((image.parent / filename).read_bytes(), image.read_bytes())

    def test_unsafe_or_untraceable_versions_are_rejected(self):
        module = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'git_branch.py'))
        for base, sha in [('1.5.8', 'unknown'), ('../1.5.8', '408dfcd'), ('1.5.8', 'xyz1234')]:
            with self.subTest(base=base, sha=sha), self.assertRaises(ValueError):
                module['x4pro_identity'](base, sha, '260915-181930')
        for stamp in ('../260915', '260231-120000', '260915-250000', '260915-1200'):
            with self.subTest(stamp=stamp), self.assertRaises(ValueError):
                module['x4pro_identity']('1.5.8', '408dfcd', stamp)


if __name__ == "__main__":
    unittest.main()
