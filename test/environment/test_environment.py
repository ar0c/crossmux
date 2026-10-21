"""Compile real SHTC3 HAL and overlay against bounded I2C/render test doubles."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run():
    with tempfile.TemporaryDirectory(prefix="crossmux-environment-") as directory:
        root = Path(directory)
        env = dict(os.environ, CCACHE_DIR=str(root / "ccache"))
        for board, simulator in ((1, False), (0, False), (1, True)):
            binary = root / f"environment-{board}-{simulator}"
            command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       f"-DFREEINK_DEVICE_WAVESHARE_EPAPER_397={board}",
                       "-I" + str(ROOT / "test/environment/stubs"),
                       "-I" + str(ROOT / "lib/hal"), "-I" + str(ROOT / "src"),
                       str(ROOT / "lib/hal/HalEnvironmentSensor.cpp"),
                       str(ROOT / "src/components/EnvironmentOverlay.cpp"),
                       str(ROOT / "test/environment/environment.cpp"), "-o", str(binary)]
            if simulator:
                command.append("-DSIMULATOR=1")
            subprocess.run(command, check=True, env=env)
            subprocess.run([str(binary)], check=True)
        print("SHTC3 protocol/failure/sleep and overlay/gray-plane/board gates passed")


if __name__ == "__main__":
    run()
