"""Compile the real exporter and display HAL with SD/board/cache fixtures. No device/network I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenDiagnosticExportTest(unittest.TestCase):
    def test_export_image_privacy_retention_and_storage_faults(self):
        for simulator in (False, True):
            with self.subTest(simulator=simulator):
                self.compile_and_run(simulator)

    def compile_and_run(self, simulator):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "screen-diagnostic-test.exe"
            subprocess.run([
                os.environ.get("CXX", "g++"), "-std=c++17", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                *(["-DSIMULATOR"] if simulator else []),
                "-Itest/screen_diagnostic/stubs", "-Isrc/util", "-Ifreeink-sdk/libs/network/JsonSax/include", "-Ilib/WeReadWebApi/src",
                "-Ilib/hal", "-Ifreeink-sdk/libs/hardware/XteinkDetect/include",
                "test/screen_diagnostic/ScreenDiagnosticExportTest.cpp", "src/util/ScreenDiagnosticExport.cpp",
                "lib/hal/HalDisplayDiagnostics.cpp",
                "freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
