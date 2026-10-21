# WeRead measured-time synchronization (ar0c fork)

## Current implementation and date attribution

The current Waveshare firmware includes persistent measured-time collection,
paced direct upload, and service-backed handoff. Opening WeRead progress sync
checkpoints the current book's measured statistics; position sync and time
accounting have separate results. Leaving the page or rebooting does not erase
the journals. Source, host and simulator validation must be distinguished from
physical SD/TLS operation and verified cloud credit.

The owner accepted **upload-day attribution for verifiable offline time**.
Keep the original reading date in local source records and journals; the cloud
increment is observed on the upload date. Do not claim that historical cloud
daily statistics were backfilled. This supersedes the initial original-day-only
design restriction. Only real measured, still-unsent time is eligible; never
fabricate duration, replay consumed ranges, or automatically resend a request
whose cloud result is uncertain.

## Active paths and ownership

- `WeReadProgressSyncActivity.cpp` imports and audits the account/book/source/day
  counters before starting `WeReadTimeSync`. The original reading statistics
  remain the source of truth.
- Direct mode uses `WeReadPacedTime.h`, `WeReadTimeTransaction.h`, and
  `WeReadTimeQueue.h`. The default paced batch is 30 seconds. A fresh cloud
  baseline and a persisted/read-back reservation precede the write. An HTTP ACK
  alone does not count as credit: confirmation checks exact upload-day and
  monthly account increments. Missing/mismatched observations stay unresolved.
- Service mode uses `/WeReadSync/service.conf` and the WRS1 service journal.
  It reserves immutable ranges before handoff, validates durable server
  receipts, and tracks accepted time separately from confirmed cloud credit.
  Accepted jobs are queried rather than resubmitted as new work. See
  [service configuration, batching and recovery](weread-service-handoff.md).
- Existing service ownership blocks direct fallback when configuration
  disappears. Account/device changes require reconciliation. Corrupt, truncated
  or identity-mismatched journals fail closed; deleting receipts is not recovery.

Confirmation is statistical evidence under the single-active-client policy,
not a cloud transaction receipt or exactly-once delivery guarantee. Concurrent
reading on another client can confound attribution. Preserve uncertain ranges
and use read-only recovery instead of timed POST retries.

## Evidence and scope

`ReadingStatsStore.h` stores per-book `readingDays` and cumulative milliseconds;
the progress activity previously copied only position fields. Import measured
cumulative counters, including existing history, rather than reconstructing
time from page turns or repeatedly importing the last session. Do not alter
original statistics, credentials, positions, or downloaded books.

The original WRTL backend uses one append-only journal per account/local-book/day in
`/.crosspoint/weread/time/<account>/<source>/<day>.wrtl`. Remote book identity is
inside the record, not the filename: rebinding the same source to another
remote book fails closed. Unknown dates and downward counter corrections need
review; they must not silently become today's time or start a new counter.

## Historical WRTL v1 verification contract

The following contract describes the original book/day design and retained
legacy data. It is not the active paced uploader's upload-day account contract.

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

Legacy WRTL replay uses fixed frame buffers rather than a history-sized vector.
The current worker additionally owns bounded request/journal workspace and a
FreeRTOS stack, with internal-RAM/contiguous-block and TLS startup gates; see
[the current allocation budget](memory-and-allocation.md#background-weread-startup-gate).
The SD adapter uses HAL only, append+flush+reopen readback.
Filesystem/controller power-loss guarantees still require physical testing.

## Validation

The standalone C++ harness (also registered in CTest) covers historical import,
duplicate import, unknown dates, immutable identity, clock freshness, reboot
restoration, missing/mismatched cloud scope, corrupt frames, failed persistence,
and torn writes using a controllable storage boundary. It is not a live SD or
cloud acceptance test. Compile assertions enabled (no `NDEBUG`).

## Validation and acceptance

Use [local validation](weread-local-validation.md) for the existing offline,
production-client/backend contract, simulator and USB checks. Relevant native
tests live in `test/weread_webapi/`; script regressions live in `scripts/tests/`.
Do not run real timed writes merely to check documentation.

For an authorized hardware acceptance run, identify the installed firmware and
account/book/source dates, preserve original statistics, and select only a
genuinely new unsent range. Verify persisted reservation, service receipt when
applicable, and a later scoped cloud increment separately. Cover lost responses,
reboot, pause, sleep, account/configuration changes and storage failure without
resetting accounting state. Restore any task-owned device state after testing.
