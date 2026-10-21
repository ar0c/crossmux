"""Compile the production OTA allowlist and exercise hostile authorities/paths."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaReleaseUrlTest(unittest.TestCase):
    def test_exact_host_channel_and_immutable_path(self):
        compiler = shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.cpp'
            executable = Path(directory) / 'test.exe'
            source.write_text(r'''
#include "src/network/OtaReleaseUrl.h"
#include <cassert>
#include <string>
int main() {
  const std::string build = "nightly-build-" + std::string(40, 'a') + "-123-1/firmware.bin";
  const std::string base(ota_release::BASE);
  assert(ota_release::isReleaseUrl(base + "nightly/release-index.json"));
  assert(ota_release::isReleaseUrl(base + "stable/release-index.json"));
  assert(ota_release::isBuildUrl(base + build, "nightly"));
  assert(!ota_release::isBuildUrl(base + build, "stable"));
  for (const auto host : {"http://release.ar0c.com", "https://ooo.ar0c.com",
       "https://release.ar0c.com.evil.test", "https://user@release.ar0c.com",
       "https://release.ar0c.com:443", "https://release.ar0c.com@evil.test"}) {
    assert(!ota_release::isReleaseUrl(std::string(host) + "/releases/download/" + build));
  }
  for (const auto suffix : {"?x=y", "#fragment", "/other", "%2f", "\\other"}) {
    assert(!ota_release::isReleaseUrl(base + build + suffix));
  }
  for (const auto path : {"nightly/../stable/release-index.json", "nightly/release-index.json?x=y",
       "nightly-build-short-1-1/firmware.bin", "nightly-build-/firmware.bin", "hooks/release"}) {
    assert(!ota_release::isReleaseUrl(base + path));
  }
  assert(!ota_release::isReleaseUrl(base + build.substr(0, build.rfind('/') + 1) + "..bin"));
}
''', encoding='utf-8')
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                            str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
