"""Exercise production SDMMC I/O bodies with a deterministic sector device."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_sd_space import method

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "freeink-sdk/libs/hardware/SDCardManager/src"


def main():
    source = (DRIVER / "SdmmcBlockDevice.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="crossmux-sdmmc-") as directory:
        temp = Path(directory)
        (temp / "BoardConfig.h").write_text(
            "#define FREEINK_SD_SDMMC 1\nnamespace BoardConfig { struct SdmmcPins {}; }\n")
        (temp / "SdFat.h").write_text("""
#pragma once
#include <cstddef>
#include <cstdint>
using Sector_t = uint32_t;
struct FsBlockDeviceInterface {
  virtual ~FsBlockDeviceInterface() = default;
  virtual void end() = 0;
  virtual bool isBusy() = 0;
  virtual bool readSector(Sector_t, uint8_t*) = 0;
  virtual bool readSectors(Sector_t, uint8_t*, size_t) = 0;
  virtual bool writeSector(Sector_t, const uint8_t*) = 0;
  virtual bool writeSectors(Sector_t, const uint8_t*, size_t) = 0;
  virtual Sector_t sectorCount() = 0;
  virtual bool syncDevice() = 0;
};
""")
        (temp / "methods.inc").write_text("\n".join(method(source, signature) for signature in (
            "void SdmmcBlockDevice::end(", "bool SdmmcBlockDevice::readSectors(",
            "bool SdmmcBlockDevice::writeSectors(", "Sector_t SdmmcBlockDevice::sectorCount(")))
        exe = temp / "sdmmc_test.exe"
        subprocess.run(shlex.split(os.environ.get("CXX", "g++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{temp}", f"-I{DRIVER}",
            str(ROOT / "test/sd_space/sdmmc_read_ahead_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


class SdmmcReadAheadTest(unittest.TestCase):
    def test_io_contract(self):
        main()


if __name__ == "__main__":
    unittest.main()
