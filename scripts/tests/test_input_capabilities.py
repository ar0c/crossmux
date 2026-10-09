"""Compile the input capability adapter with hardware and pinned-simulator HALs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class InputCapabilitiesTest(unittest.TestCase):
    def test_wheel_layout_matches_hardware_and_explicit_simulator_profile(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        cases = [
            (False, False, False),
            (False, True, True),
            (True, False, False),
            (True, True, True),
        ]
        for emulated, wheel, expected in cases:
            with self.subTest(emulated=emulated, wheel=wheel), tempfile.TemporaryDirectory() as tmp:
                work = Path(tmp)
                # The pinned simulator deliberately has no wheel query: merely
                # compiling both simulator profiles catches the original error.
                method = "" if emulated else (
                    "bool hasWheelAndBootButtons() const { return " + str(wheel).lower() + "; }"
                )
                (work / "HalGPIO.h").write_text("#pragma once\nclass HalGPIO { public: " + method + " };\n")
                (work / "main.cpp").write_text(
                    '#include "platform/InputCapabilities.h"\n'
                    'int main() { HalGPIO gpio; return inputCapabilities::hasWheelAndBootButtons(gpio) == '
                    + str(expected).lower() + ' ? 0 : 1; }\n'
                )
                exe = work / ("test.exe" if os.name == "nt" else "test")
                subprocess.run([
                    compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-DCROSSPOINT_EMULATED=" + str(int(emulated)),
                    "-DCROSSPOINT_SIM_UI_WAVESHARE_397=" + str(int(emulated and wheel)),
                    "-I" + str(work), "-I" + str(ROOT / "src"),
                    str(work / "main.cpp"), "-o", str(exe),
                ], check=True, capture_output=True, text=True)
                subprocess.run([str(exe)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
