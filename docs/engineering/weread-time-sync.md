# WeRead measured-time synchronization (ar0c fork)

## Delivery status

This is the **collection/journal slice**, not a functioning time uploader.
Opening WeRead progress sync checkpoints and imports the current local book's
dated reading statistics. Position sync is unchanged. The result screen shows
pending minutes separately and explicitly says time has not been uploaded.
No `rt` parameter is sent by this change. No device has been flashed or cloud
time modified as part of the implementation tests.

**User requirement:** historical time must be credited to its original reading
date. Crediting it on the upload date is explicitly rejected. Until an endpoint
with that capability and verifiable date accounting is established, historical
uploads stay blocked; this is not an option the implementation may relax.

## Evidence and scope

`ReadingStatsStore.h` stores per-book `readingDays` and cumulative milliseconds;
the progress activity previously copied only position fields. Import measured
cumulative counters, including existing history, rather than reconstructing
time from page turns or repeatedly importing the last session. Do not alter
original statistics, credentials, positions, or downloaded books.

The first backend uses one append-only journal per account/local-book/day in
`/.crosspoint/weread/time/<account>/<source>/<day>.wrtl`. Remote book identity is
inside the record, not the filename: rebinding the same source to another
remote book fails closed. Unknown dates and downward counter corrections need
review; they must not silently become today's time or start a new counter.

## State and verification contract

1. Persist cumulative measured milliseconds; an unchanged import writes nothing.
2. Obtain a fresh, explicitly scoped account/book/day cloud baseline.
3. Reserve 60 measured seconds and persist/read back the reservation **before**
   attempting a timed request. Historical dates require an explicitly supported
   reporting capability; never substitute request timestamp for event date.
4. A persisted reservation is `AwaitingVerification` even when HTTP succeeds,
   times out, or the device restarts. There is no automatic retransmit/reset API.
5. Read cloud data again. Require the same scope, newer observation, and exact
   60-second increases in both book/day and account/day counters. Missing fields,
   wrong dates, counter regressions, and excess increments remain unresolved.
6. Persist/read back verification before exposing the next batch as sendable.

This is conservative evidence of an observed scoped increment, **not** a
server transaction receipt or proof against simultaneous reading on another
device. There is no server idempotency key. Another client reading precisely
the same book/day for 60 seconds can still confound attribution. The eventual
UI must state this limitation; do not claim exactly-once cloud delivery.

## Journal format v1

Frames are 240 bytes, explicit little endian, no raw struct serialization:

| Offset | Contents |
| --- | --- |
| 0 | `WRTL` magic (4 bytes) |
| 4 | version 1 (1 byte) |
| 5 | state: 0 pending / 1 awaiting verification |
| 8 / 40 / 104 | account[32], remote book[64], source[64], NUL-terminated ASCII IDs |
| 168 | local day ordinal (uint64 wire, validated uint32) |
| 176 | cumulative measured milliseconds (uint64) |
| 184 | verified seconds (uint64) |
| 192 / 200 | baseline book/day and account/day seconds (uint64) |
| 208 | reservation epoch seconds (uint64) |
| 232 | CRC32 of bytes [0,232), stored in uint64; upper bits zero |

Other bytes are reserved zero. Partial/corrupt files block reporting rather than
fall back to an older pre-send frame. These files are durable accounting data,
not caches: do not delete, copy, roll back, or compact them to retry a batch.
Full SD rollback or external file replacement cannot be detected by this
local-only format. Only one firmware writer may own a journal.

Memory is fixed: one activity-scoped workspace, two 240-byte buffers, no
history-sized vector. The SD adapter uses HAL only, append+flush+reopen readback.
Filesystem/controller power-loss guarantees still require physical testing.

## Validation

The standalone C++ harness (also registered in CTest) covers historical import,
duplicate import, unknown dates, immutable identity, clock freshness, reboot
restoration, missing/mismatched cloud scope, corrupt frames, failed persistence,
and torn writes using a controllable storage boundary. It is not a live SD or
cloud acceptance test. Compile assertions enabled (no `NDEBUG`).

## Remaining before enabling uploads

- Integrate authenticated device-side cloud stats reads; the Web session and
  official Skills gateway have different credentials and response contracts.
- Establish a usable per-book/per-day metric. `/readdata/detail` totals include
  listening; `readLongest` is a top-ten list with a five-minute cutoff. An absent
  book is not zero. `recordReadingTime` is not normal text-reading time.
- Verify whether historical dates can be credited; never guess a backdated `ct`.
- Add signed timed POSTs only after durable reservation and baseline succeed.
- Add independent time controls/status, readback retries (never POST retries),
  account switching and concurrent-client warnings, and fork-owned OTA identity.
- Validate original measured history, request/readback logs, interruption cases,
  build identity, device flash/readback, and final WeRead accounting separately.
