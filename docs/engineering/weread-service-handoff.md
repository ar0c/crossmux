# WeRead service handoff (X4 Pro and Waveshare 3.97)

The firmware can delegate measured offline time to the owner's Go service at
`https://wesync.ar0c.com`. It persists ownership before sending, then returns
to offline reading once the server has durably accepted the record. Cloud
credit remains asynchronous and is displayed separately from local acceptance.

## Device configuration

Place the private `service.conf` file in `/WeReadSync/service.conf` on the SD
card. Its three newline-terminated ASCII lines are account ID, device ID, and
device bearer token. The URL is fixed to the HTTPS service; there is no insecure
or arbitrary-host fallback. The compiled firmware contains no account secrets.
The provisioning copy is kept in the service project's private `local-secrets`
directory; never commit it, share it, or put it in a public firmware archive.

On explicit reading-time sync, the device first checks `/api/v1/device` and
requires both account and device ID to match the local source/configuration.
The verified HTTP transport requires a caller-owned receive buffer. The service
client supplies a 512-byte buffer on the existing worker stack and streams the
response into its bounded parser. A credential-free phase/result record is saved
to `/WeReadSync/last-service-diagnostic.json` before device verification and
each job request, then on transport failure or successful device verification.
It can locate a reboot within the handoff without USB serial access.
Missing configuration retains the existing direct mode only for undelegated
histories. Invalid configuration stops the run. Any existing service ledger
blocks direct fallback when configuration disappears. An account switch or
device-token replacement requires reconciliation of old handoffs first.

## Ownership and recovery

The existing WRTL/external/WRP2 audit remains required. The direct sender's
consumed prefix, including uncertain time, is excluded before selecting a
service range. No historical upload or uncertain direct batch is replayed.

`/.crosspoint/weread/time/<account>/<source>/<day>.wrs1` stores append-only
256-byte WRS1 frames: identity, fixed direct prefix, bound service device,
range, and Reserved/Accepted/Confirmed state. Version 2 also records per-task
confirmed seconds and observation time. Every frame has a checksum and
must follow a legal monotonic transition. A torn tail or changed binding blocks
the run. Original reading records and legacy journals remain untouched.

The device posts only Reserved records, with a deterministic task ID and the
same immutable body on every retry. Accepted records use GET only. A missing
server receipt is an error, not permission to submit again. Only exact matching
identity/range and monotonic confirmed seconds advance cached cloud credit.
A source day allows one unacknowledged Reserved tail and up to 24 outstanding
tasks. Once the tail is Accepted, a new measured range can be reserved without
waiting for cloud completion. A 50-minute `[0,3000)` handoff and a later 10-minute
`[3000,3600)` handoff remain distinct jobs. Their IDs never change with progress.
Partial/out-of-order receipts are summed per task, never inferred as a prefix.
Reserved time stays in the local-unhanded UI count, not the server-pending count.

Each explicit run recovers the Reserved tail first, reads at most four oldest
accepted receipts per day, and submits new measured ranges. A valid uncertain
receipt does not prevent new handoff; the server still freezes cloud execution.
A missing/malformed/regressed receipt stops the run. A full queue retains records
locally. Cached credit shows its oldest outstanding observation time; the device
does not continuously poll offline. The run uses a frozen source snapshot:
reading done after it began requires another explicit handoff.

Migration is append-only: validate every existing v1 frame and retain it exactly,
then append v2 frames with the same account/device/source/date/range IDs. No file
rename, prefix copy, deletion or history reset is needed. A torn v2 append blocks
all sending. A v1 service-journal reader rejects version 2; older firmware without
any service ownership support is still unsafe to downgrade to.

The active-task array is fixed (24 entries, no per-task heap allocation), keeping
the accounting scratch below 6 KiB and the journal below 2 KiB. The service
worker allocates that journal fallibly in PSRAM, with a fallible internal-memory
fallback, rather than leaving it live on its 8 KiB stack during HTTPS calls.
Slots are reused only after full confirmation. The existing 8 KiB worker stack,
fallible job allocation and internal-memory reserves remain. The journal is capped at
4 MiB; a full journal stops new reservations rather than deleting ownership.

Do not downgrade to firmware that does not understand WRS1 after delegating
time. Such firmware cannot see service ownership and might resend it. Preserve
the SD journals and reconcile all server receipts before any rollback. Do not
delete WRS1 to clear an error; do not restore an older service database and
blindly replay device records.

## Verification and limitations

Run `scripts/test_weread_service.ps1`: pure journal crash/torn-tail checks,
production client parsing and authenticated HTTPS transport boundary tests
(including the required receive-buffer preflight),
and production worker tests with internal RAM and PSRAM paths. The tests make
no real network requests or reading-time increments. Build with `pio run -e
x4pro` or `pio run -e waveshare_epaper_397` for the matching board; verify the
version-first exported image and its SHA-256 before upload.

The local device status is the last explicit readback. To see newer cloud
credit, open the service management page or initiate another device sync. The
server applies its own pacing and freezes ambiguous cloud writes.
The sync screen identifies service mode from the first published worker status.
While a handoff is running, it says the complete server receipt is pending.
Only a complete, audited handoff says the server durably accepted the record
and that the reader may leave the page; an empty run says no time was handed
off. Cloud credit remains a separate count.
Cloudflare may still challenge clients outside the permitted network region;
the device stops with its reservation intact, without changing User-Agent to
impersonate a browser or weakening TLS. The client identifies itself as
`weread-sync-device/0.1`.

Build and native tests do not prove physical-device Wi-Fi/TLS/SD operation.
After installing the package, verify one genuinely new, unsent reading range,
the server's durable receipt, device disconnect, and later exact cloud credit.
