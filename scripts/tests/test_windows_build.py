import io
import configparser
import runpy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


class WindowsBuildTest(unittest.TestCase):
    def test_pinned_toolchain_is_isolated_from_other_projects(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini", encoding="utf-8")
        self.assertEqual(config["platformio"]["core_dir"], ".platformio")

    def test_middleware_preserves_patterns_and_real_object_return(self):
        module = runpy.run_path(str(ROOT / "scripts/patch_windows_middleware.py"))

        class Node:
            def __init__(self, path):
                self.path = path

            def srcnode(self):
                return self

            def get_path(self):
                return self.path

        class Env:
            def Object(self, node, **kwargs):
                return [Node("compiled/" + node.path)]

        calls = []

        def ble(env, node):
            calls.append(node.path)
            return env.Object(Node("generated/BleKeyboardHost.cpp"))[0]

        def fallback(env, node):
            return env.Object(node)[0]

        dispatch = module["make_middleware"](
            [(ble, "*/BleKeyboardHost.cpp")], fallback
        )
        self.assertEqual(dispatch(Env(), Node("core/main.cpp")).path, "compiled/core/main.cpp")
        self.assertEqual(calls, [])
        self.assertEqual(dispatch(Env(), Node("src/BleKeyboardHost.cpp")).path,
                         "compiled/generated/BleKeyboardHost.cpp")
        self.assertEqual(len(calls), 1)
        skip = module["make_middleware"]([(lambda node: None, "*")], fallback)
        self.assertIsNone(skip(Env(), Node("skip.cpp")))
        self.assertEqual(dispatch(Env(), Node("src\\BleKeyboardHost.cpp")).path,
                         "compiled/generated/BleKeyboardHost.cpp")

    def test_unknown_platform_fails_closed(self):
        module = runpy.run_path(str(ROOT / "scripts/patch_windows_middleware.py"))
        with self.assertRaisesRegex(RuntimeError, "Unsupported pioarduino"):
            module["patch_source"]("# changed upstream implementation\n")

    def test_i18n_logging_with_utf8_capture_and_gbk_outer_console(self):
        module = runpy.run_path(str(ROOT / "scripts/gen_i18n.py"))
        outer = io.TextIOWrapper(io.BytesIO(), encoding="gbk", errors="strict")
        capture = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")

        def forwarded_print(line):
            outer.write(line)

        with patch.object(sys, "stdout", capture), patch.object(sys, "__stdout__", outer), \
                patch("builtins.print", forwarded_print):
            module["_print_language_table"](["AR"], ["العربية"], ["ar"], [set()], ["K"], set(), [2])

    def test_scons_logging_with_hidden_gbk_parent(self):
        module = runpy.run_path(str(ROOT / "scripts/gen_i18n.py"))
        printer = module["_print_language_table"]
        printer.__globals__["Import"] = lambda _: None
        parent = io.TextIOWrapper(io.BytesIO(), encoding="gbk", errors="strict")
        capture = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        with patch.object(sys, "stdout", capture), patch.object(sys, "__stdout__", capture), \
                patch("builtins.print", lambda line: parent.write(line)):
            printer(["AR"], ["العربية"], ["ar"], [set()], ["K"], set(), [2])


if __name__ == "__main__":
    unittest.main()
