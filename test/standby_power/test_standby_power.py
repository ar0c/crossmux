"""Exercise the real Waveshare light-sleep HAL with recording GPIO/timer APIs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run():
    with tempfile.TemporaryDirectory(prefix="crossmux-standby-power-") as directory:
        work = Path(directory)
        env = dict(os.environ, CCACHE_DIR=str(work / "ccache"))
        for board, simulator in ((1, False), (0, False), (1, True)):
            binary = work / f"standby-{board}-{simulator}"
            args = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-DFREEINK_DEVICE_WAVESHARE_EPAPER_397={board}", "-DFREEINK_CAP_BLE_HID_HOST=1",
                    "-I" + str(ROOT / "test/standby_power/stubs"), "-I" + str(ROOT / "lib/hal"),
                    "-I" + str(ROOT / "src/activities/apps/standby"),
                    str(ROOT / "lib/hal/Waveshare397Standby.cpp"),
                    str(ROOT / "test/standby_power/standby_power.cpp"), "-o", str(binary)]
            if simulator:
                args.append("-DSIMULATOR=1")
            subprocess.run(args, check=True, env=env)
            subprocess.run([str(binary)], check=True)
        source = (ROOT / "src/activities/apps/standby/StandbyActivity.cpp").read_text(encoding="utf-8")
        start = source.index("bool StandbyActivity::tryLightSleep(")
        method = source[start:source.index("\nvoid StandbyActivity::loop()", start)]
        harness = (ROOT / "test/standby_power/activity.cpp").read_text(encoding="utf-8")
        generated = work / "activity.cpp"
        generated.write_text(harness.replace("// PRODUCTION_METHOD", method), encoding="utf-8")
        binary = work / "activity"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DCROSSPOINT_EMULATED=0",
                        "-DFREEINK_DEVICE_WAVESHARE_EPAPER_397=1", "-DCROSSPOINT_CAP_SOUND_FEEDBACK=1",
                        "-I" + str(ROOT / "src/activities/apps/standby"), str(generated), "-o", str(binary)],
                       check=True, env=env)
        subprocess.run([str(binary)], check=True)
        print("Waveshare timer/key wake, radio gates, input races, cleanup and cadence passed")


if __name__ == "__main__":
    run()
