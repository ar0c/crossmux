"""Compile real SHTC3 HAL and overlay against bounded I2C/render test doubles."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def simulator_sensor_sources():
    """Use the actual build hook's registrations, not a test-only source list."""
    sources = []

    class Environment:
        def subst(self, value):
            return {"$PROJECT_LIBDEPS_DIR": str(ROOT / ".pio/libdeps"), "$PIOENV": "simulator"}[value]

        def Append(self, **kwargs):
            pass

        def BuildSources(self, target, source, src_filter):
            for rule in src_filter:
                if rule.startswith("+<"):
                    sources.extend(path for path in (ROOT / "lib/hal").glob(rule[2:-1])
                                   if path.name == "HalEnvironmentSensor.cpp")

    runpy.run_path(str(ROOT / "scripts/simulator_project_hal.py"),
                   init_globals={"Import": lambda name: None, "env": Environment()})
    return list(map(str, sources))


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
                       str(ROOT / "src/components/EnvironmentOverlay.cpp"),
                       str(ROOT / "test/environment/environment.cpp"), "-o", str(binary)]
            if simulator:
                command.append("-DSIMULATOR=1")
                command.extend(simulator_sensor_sources())
            else:
                command.append(str(ROOT / "lib/hal/HalEnvironmentSensor.cpp"))
            subprocess.run(command, check=True, env=env)
            subprocess.run([str(binary)], check=True)
        print("SHTC3 protocol/failure/sleep and overlay/gray-plane/board gates passed")


if __name__ == "__main__":
    run()
