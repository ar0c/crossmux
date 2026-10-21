"""Keep factory-image failures visible and use the configured Python interpreter."""
import ast
from contextlib import redirect_stdout
import io
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = '''def esp32_create_combined_bin(source, target, env):
    cmd = ["--chip", "esp32s3", "merge-bin", "-o", "firmware.factory.bin"]
    esptool = esptool_binary_path
    try:
        result = subprocess.run(
            [esptool, *cmd], check=False, capture_output=True, text=True
        )
        if result.returncode == 0:
            print("Successfully created combined binary image.")
        else:
            print(f"esptool merge-bin failed (exit code {result.returncode})")
            if result.stderr:
                print(result.stderr)
    except Exception as e:
        print(f"Error creating factory binary: {e}")

#
# Target: Build executable and linkable firmware or FS image
'''


def patcher():
    module = ast.parse((ROOT / 'scripts/patch_pioarduino_cache.py').read_text(encoding='utf-8'))
    function = next(node for node in module.body if isinstance(node, ast.FunctionDef)
                    and node.name == 'patch_factory_source')
    namespace = {}
    exec(compile(ast.Module(body=[function], type_ignores=[]), '<factory patch>', 'exec'), namespace)
    return namespace['patch_factory_source']


class FactoryImageActionTest(unittest.TestCase):
    def action(self, runner):
        namespace = {'PYTHON_EXE': 'configured-python', 'esptool_binary_path': 'launcher.exe',
                     'subprocess': SimpleNamespace(run=runner)}
        exec(compile(patcher()(SOURCE), '<patched factory action>', 'exec'), namespace)
        return namespace['esp32_create_combined_bin']

    def test_same_interpreter_module_and_success(self):
        commands = []

        def run(command, **options):
            commands.append(command)
            return SimpleNamespace(returncode=0, stderr='')

        with redirect_stdout(io.StringIO()):
            self.assertEqual(self.action(run)(None, None, None), 0)
        self.assertEqual(commands[0][:3], ['configured-python', '-m', 'esptool'])
        self.assertIn('merge-bin', commands[0])

    def test_nonzero_and_launch_errors_fail_the_build(self):
        with redirect_stdout(io.StringIO()):
            action = self.action(lambda *args, **kwargs: SimpleNamespace(returncode=101, stderr='launch failed'))
            self.assertEqual(action(None, None, None), 101)

            def unavailable(*args, **kwargs):
                raise OSError('interpreter unavailable')

            self.assertEqual(self.action(unavailable)(None, None, None), 1)

    def test_idempotent_and_unknown_platform_fails_closed(self):
        patch = patcher()
        changed = patch(SOURCE)
        self.assertEqual(patch(changed), changed)
        with self.assertRaises(RuntimeError):
            patch(SOURCE.replace('[esptool, *cmd]', '[unknown_tool, *cmd]'))


if __name__ == '__main__':
    unittest.main()
