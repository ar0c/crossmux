from pathlib import Path
import runpy
import unittest
import tempfile


class ForkIdentityTest(unittest.TestCase):
    def test_version_and_filename_identify_owner_device_and_revision(self):
        module = runpy.run_path(str(Path(__file__).resolve().parents[1] / "git_branch.py"))
        version, filename = module["x4pro_identity"]("1.5.8", "408dfcd")
        self.assertEqual(version, "1.5.8-ar0c-x4pro+408dfcd")
        self.assertEqual(filename, "crossmux-ar0c-1.5.8-x4pro-408dfcd.bin")
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
        version, filename = module['x4pro_identity']('1.5.8', sha)
        self.assertIn(version, env['CPPDEFINES'][0][1])
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'firmware.bin'
            image.write_bytes(b'test image bytes')

            class Node:
                def get_abspath(self):
                    return str(image)

            env.action([Node()], [], env)
            self.assertEqual((image.parent / filename).read_bytes(), image.read_bytes())

    def test_unsafe_or_untraceable_versions_are_rejected(self):
        module = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'git_branch.py'))
        for base, sha in [('1.5.8', 'unknown'), ('../1.5.8', '408dfcd'), ('1.5.8', 'xyz1234')]:
            with self.subTest(base=base, sha=sha), self.assertRaises(ValueError):
                module['x4pro_identity'](base, sha)


if __name__ == "__main__":
    unittest.main()
