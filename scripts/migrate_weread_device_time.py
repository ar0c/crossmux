"""Prepare one reviewed, device-owned WeRead reading-time claim offline.

This script never contacts the reader or WeRead. It reads a backed-up format-6
statistics file and the original WRTL journals, then writes a separate format-7
candidate. The operator must independently verify that no companion report or
handover receipt exists before installing the candidate on the original device.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path


ID = re.compile(r"[A-Za-z0-9_-]+\Z")
BOOK_ID = re.compile(r"[0-9a-f]{32}\Z")
DEVICE_ID = re.compile(r"[0-9a-f]{12}\Z")
FRAME_SIZE = 240


def checked_id(raw: bytes, limit: int) -> str:
    if len(raw) != limit or b"\0" not in raw:
        raise ValueError("Invalid journal identity")
    value = raw.split(b"\0", 1)[0].decode("ascii")
    if not value or not ID.fullmatch(value) or any(raw[len(value) + 1 :]):
        raise ValueError("Noncanonical journal identity")
    return value


def read_ledger(path: Path) -> tuple[tuple[str, str, str, int], int]:
    data = path.read_bytes()
    if not data or len(data) > 4 * 1024 * 1024 or len(data) % FRAME_SIZE:
        raise ValueError(f"Invalid WRTL length: {path.name}")
    identity = None
    previous = 0
    for offset in range(0, len(data), FRAME_SIZE):
        frame = data[offset : offset + FRAME_SIZE]
        if frame[:4] != b"WRTL" or frame[4] != 1 or frame[5] != 0 or any(frame[6:8]):
            raise ValueError(f"Invalid or reserved WRTL frame: {path.name}")
        if struct.unpack_from("<Q", frame, 232)[0] != zlib.crc32(frame[:232]):
            raise ValueError(f"WRTL CRC mismatch: {path.name}")
        current = (
            checked_id(frame[8:40], 32),
            checked_id(frame[40:104], 64),
            checked_id(frame[104:168], 64),
            struct.unpack_from("<Q", frame, 168)[0],
        )
        if not 0 < current[3] <= 0xFFFFFFFF or (identity and current != identity):
            raise ValueError(f"WRTL identity changed: {path.name}")
        measured = struct.unpack_from("<Q", frame, 176)[0]
        if measured < previous or any(frame[184:232]):
            raise ValueError(f"WRTL has credit or an unresolved reservation: {path.name}")
        identity, previous = current, measured
    assert identity is not None
    return identity, previous


def prepare(
    stats_path: Path,
    ledger_dir: Path,
    output_path: Path,
    expected_sha256: str,
    expected_total_ms: int,
    device_id: str,
    max_tail_ms: int,
) -> dict:
    if stats_path.resolve() == output_path.resolve() or output_path.exists():
        raise ValueError("Output must be a new file separate from the backup")
    original = stats_path.read_bytes()
    if hashlib.sha256(original).hexdigest() != expected_sha256.lower():
        raise ValueError("Reading statistics changed since review")
    data = json.loads(original)
    if data.get("formatVersion") != 6 or not DEVICE_ID.fullmatch(device_id):
        raise ValueError("Expected format 6 and a 12-digit lowercase device ID")
    files = sorted(ledger_dir.iterdir())
    if not files or any(not item.is_file() or item.suffix != ".wrtl" for item in files):
        raise ValueError("Ledger directory has missing or non-WRTL files")
    ledgers = {}
    common = None
    for path in files:
        identity, measured = read_ledger(path)
        account, remote_book, local_source, day = identity
        if path.name != f"{day}.wrtl" or not BOOK_ID.fullmatch(local_source):
            raise ValueError("WRTL path or local source is inconsistent")
        if common and common != (account, remote_book, local_source):
            raise ValueError("Ledger directory mixes sources")
        if day in ledgers:
            raise ValueError("Duplicate WRTL day")
        common = (account, remote_book, local_source)
        ledgers[day] = measured
    assert common is not None
    account, remote_book, local_source = common
    matches = [book for book in data.get("books", []) if book.get("bookId") == local_source]
    if len(matches) != 1:
        raise ValueError("Expected exactly one matching statistics book")
    book = matches[0]
    if book.get("wereadOwnedTime"):
        raise ValueError("Book already has device ownership; refuse a second claim")
    days = book.get("readingDays")
    if not isinstance(days, list) or len(days) != len(ledgers) or not days:
        raise ValueError("Statistics and WRTL dates do not match")
    total = 0
    previous_day = 0
    for item in days:
        day, measured = item.get("dayOrdinal"), item.get("readingMs")
        if (not isinstance(day, int) or not isinstance(measured, int) or day <= previous_day or
                day not in ledgers or measured < ledgers[day] or measured - ledgers[day] > max_tail_ms):
            raise ValueError("A day is unverified, reordered, or exceeds the reviewed tail")
        total += measured
        previous_day = day
    if total != expected_total_ms or total != book.get("totalReadingMs"):
        raise ValueError("Reviewed book total does not balance")
    source = f"d{device_id}_{local_source}"
    book["wereadOwnedTime"] = [{
        "account": account,
        "remoteBook": remote_book,
        "source": source,
        "totalMs": total,
        "days": days,
    }]
    data["formatVersion"] = 7
    encoded = (json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    output_path.write_bytes(encoded)
    return {
        "days": len(days),
        "measured_ms": total,
        "uncheckpointed_tail_ms": total - sum(ledgers.values()),
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "candidate_sha256": hashlib.sha256(encoded).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stats", type=Path, required=True)
    parser.add_argument("--ledger-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--expected-total-ms", type=int, required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--max-tail-ms", type=int, default=5000)
    args = parser.parse_args()
    if args.expected_total_ms <= 0 or not 0 <= args.max_tail_ms <= 60000:
        parser.error("Invalid duration bound")
    result = prepare(args.stats, args.ledger_dir, args.output, args.expected_sha256,
                     args.expected_total_ms, args.device_id, args.max_tail_ms)
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()
