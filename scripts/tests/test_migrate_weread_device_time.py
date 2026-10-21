import hashlib
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from migrate_weread_device_time import prepare


LOCAL = "0123456789abcdef0123456789abcdef"


def frame(day: int, measured: int) -> bytes:
    data = bytearray(240)
    data[:4] = b"WRTL"
    data[4] = 1
    for start, size, value in ((8, 32, "account"), (40, 64, "remote"), (104, 64, LOCAL)):
        data[start : start + len(value)] = value.encode()
    struct.pack_into("<Q", data, 168, day)
    struct.pack_into("<Q", data, 176, measured)
    struct.pack_into("<Q", data, 232, zlib.crc32(data[:232]))
    return bytes(data)


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ledgers = self.root / "ledgers"
        self.ledgers.mkdir()
        (self.ledgers / "20709.wrtl").write_bytes(frame(20709, 120000))
        (self.ledgers / "20710.wrtl").write_bytes(frame(20710, 180000))
        self.stats = self.root / "reading_stats.json"
        self.stats.write_text(json.dumps({
            "formatVersion": 6,
            "books": [{"bookId": LOCAL, "totalReadingMs": 303000,
                       "readingDays": [{"dayOrdinal": 20709, "readingMs": 120000},
                                       {"dayOrdinal": 20710, "readingMs": 183000}]}],
        }), encoding="utf-8")
        self.digest = hashlib.sha256(self.stats.read_bytes()).hexdigest()
        self.output = self.root / "out" / "reading_stats.json"
        self.output.parent.mkdir()

    def run_prepare(self):
        return prepare(self.stats, self.ledgers, self.output, self.digest,
                       303000, "123456789abc", 5000)

    def test_claim_is_scoped_and_original_is_preserved(self):
        result = self.run_prepare()
        self.assertEqual(result["uncheckpointed_tail_ms"], 3000)
        self.assertEqual(hashlib.sha256(self.stats.read_bytes()).hexdigest(), self.digest)
        owned = json.loads(self.output.read_text(encoding="utf-8"))["books"][0]["wereadOwnedTime"][0]
        self.assertEqual(owned["source"], "d123456789abc_" + LOCAL)
        self.assertEqual(owned["totalMs"], 303000)
        with self.assertRaises(ValueError):
            self.run_prepare()  # Never overwrite a reviewed candidate.

    def test_corrupt_or_reserved_journal_blocks_claim(self):
        path = self.ledgers / "20710.wrtl"
        bad = bytearray(path.read_bytes())
        bad[176] ^= 1
        path.write_bytes(bad)
        with self.assertRaises(ValueError):
            self.run_prepare()
        self.assertFalse(self.output.exists())
        bad = bytearray(frame(20710, 180000))
        struct.pack_into("<Q", bad, 184, 60)
        struct.pack_into("<Q", bad, 232, zlib.crc32(bad[:232]))
        path.write_bytes(bad)
        with self.assertRaises(ValueError):
            self.run_prepare()

    def test_changed_statistics_or_excess_tail_blocks_claim(self):
        with self.assertRaises(ValueError):
            prepare(self.stats, self.ledgers, self.output, "0" * 64, 303000, "123456789abc", 5000)
        with self.assertRaises(ValueError):
            prepare(self.stats, self.ledgers, self.output, self.digest, 303000, "123456789abc", 2000)


if __name__ == "__main__":
    unittest.main()
