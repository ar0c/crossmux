"""Run the production TLS guard with internal-only and PSRAM-backed heaps."""
from pathlib import Path
import re
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class TlsMemoryPreflightTest(unittest.TestCase):
    def test_external_tls_allocator_is_installed_before_activities(self):
        source = (ROOT / 'src/main.cpp').read_text()
        setup = method(source, 'void setup()')
        # Process-wide hooks must be installed on boot, never in a worker.
        # A standalone expression remains active in builds that compile logs out.
        install = 'const bool externalTls = HalTlsMemory::initialize();'
        self.assertEqual(source.count('HalTlsMemory::initialize()'), 1)
        self.assertIn(install, setup)
        self.assertLess(setup.index(install), setup.index('= setupDisplayAndFonts('))
        sync = (ROOT / 'src/activities/apps/weread/webapi/WeReadTimeSync.cpp').read_text()
        self.assertIn('HalTlsMemory::usesPsram() &&', sync)

    def test_allocator_capacity_and_boundaries(self):
        source = (ROOT / 'src/network/HttpDownloader.cpp').read_text()
        header = (ROOT / 'src/network/HttpDownloader.h').read_text()
        constants = '\n'.join(re.findall(r'  static constexpr uint32_t MIN_TLS_.*?;', header))
        run_cpp(r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
template <typename... Args> void logError(Args...) {}
#define LOG_ERR(...) logError(__VA_ARGS__)
struct HalMemory {
 struct HeapStats { size_t freeBytes, totalBytes, minFreeBytes, largestBlockBytes; };
 static inline HeapStats available{}, internal{}, psram{};
 static HeapStats getDefaultHeap() { return available; }
 static HeapStats getInternalHeap() { return internal; }
 static HeapStats getPsramHeap() { return psram; }
};
struct HttpDownloader {
''' + constants + r'''
 static bool hasMemoryForTls();
};
''' + method(source, 'bool HttpDownloader::hasMemoryForTls()') + r'''
int main() {
 HalMemory::internal = {30411, 287611, 0, 7156};
 HalMemory::psram = {6252576, 8373520, 0, 6160372};
 HalMemory::available = {6282987, 8661131, 0, 6160372};
 assert(HttpDownloader::hasMemoryForTls());
 // Even reported PSRAM must not bypass insufficient default-capability memory.
 HalMemory::available = {39999, 100000, 0, 20000};
 assert(!HttpDownloader::hasMemoryForTls());
 HalMemory::available = {40000, 100000, 0, 19999};
 assert(!HttpDownloader::hasMemoryForTls());
 HalMemory::available = {40000, 100000, 0, 20000};
 assert(HttpDownloader::hasMemoryForTls());
 // No PSRAM: identical thresholds, and insufficient internal memory stays blocked.
 HalMemory::psram = {};
 HalMemory::available = HalMemory::internal;
 assert(!HttpDownloader::hasMemoryForTls());
 HalMemory::available = HalMemory::internal = {40000, 100000, 0, 20000};
 assert(HttpDownloader::hasMemoryForTls());
}
''')

    def test_callers_use_shared_guard(self):
        for name in ('settings/FontDownloadActivity.cpp', 'CatalogActivity.cpp'):
            source = (ROOT / 'src/activities' / name).read_text()
            self.assertIn('if (!HttpDownloader::hasMemoryForTls())', source)
            self.assertNotIn('HttpDownloader::MIN_TLS_', source)
        opds = (ROOT / 'src/activities/browser/OpdsBookBrowserActivity.cpp').read_text()
        self.assertIn('downloadFile(downloadUrl, filename, server.username, server.password)', opds)
        self.assertNotIn('HttpDownloader::MIN_TLS_', opds)


if __name__ == '__main__':
    unittest.main()
