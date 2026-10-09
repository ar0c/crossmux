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

The device submits only Reserved records, with a deterministic task ID and the
same immutable identity/range on every retry. Accepted records are queried only;
the legacy client uses GET, while the batch client uses explicit `query` items.
A missing server receipt is an error, not permission to submit again. Only exact matching
identity/range and monotonic confirmed seconds advance cached cloud credit.
A source day allows one unacknowledged Reserved tail and up to 24 outstanding
tasks. Once the tail is Accepted, a new measured range can be reserved without
waiting for cloud completion. A 50-minute `[0,3000)` handoff and a later 10-minute
`[3000,3600)` handoff remain distinct jobs. Their IDs never change with progress.
Partial/out-of-order receipts are summed per task, never inferred as a prefix.
Reserved time stays in the local-unhanded UI count, not the server-pending count.

Each explicit run reads at most four oldest accepted receipts per day and
recovers any Reserved tail before reserving a successor range. A full local
task array flushes its readbacks before reserving new time, allowing confirmed
tasks to free slots. The backend must
advertise `time_batch_limit: 16` in `/api/v1/device` before any reservation is
made. A batch POST to `/api/v1/jobs/batch` combines up to 16 immutable `submit`
or `query` items in one durable transaction. A conflict, missing query or full
queue rolls back all new jobs in that batch. Query never recreates a job. The
schema-1 response repeats account/device and ordered range/state/credit fields;
the client validates the whole response before appending any local receipts.
Lost responses recover the same reserved IDs, including after backend restart.
Recovering an older reserved tail may require a second batch for new time.
A valid uncertain receipt does not prevent new handoff; the server still freezes cloud execution.
A missing/malformed/regressed receipt stops the run. A full queue retains records
locally. Cached credit shows its oldest outstanding observation time; the device
does not continuously poll offline. The run uses a frozen source snapshot:
reading done after it began requires another explicit handoff.

Returning to reading leaves this asynchronous handoff running. A manual sleep
request now drains the entire frozen service handoff before Wi-Fi/storage are
released, rather than pausing the remaining dates. It waits for durable backend
receipts and local journal flushes, not cloud credit. Direct cloud mode and
explicit pause retain cooperative cancellation. A 60-second budget checked
between operations bounds a run; an already-started HTTP request retains its
15-second timeout and final SD accounting must also finish. On timeout or
failure, preserved reservations recover on the next explicit sync. No latency
guarantee is implied for physical TLS, SD or network operation.

Migration is append-only: validate every existing v1 frame and retain it exactly,
then append v2 frames with the same account/device/source/date/range IDs. No file
rename, prefix copy, deletion or history reset is needed. A torn v2 append blocks
all sending. A v1 service-journal reader rejects version 2; older firmware without
any service ownership support is still unsafe to downgrade to.

The active-task array is fixed (24 entries, no per-task heap allocation), keeping
the accounting scratch below 6 KiB and the journal below 2 KiB. The service
worker allocates one fixed journal plus 16-item/8192-byte request workspace
(under 12 KiB total) fallibly in PSRAM, with a fallible internal-memory
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

The progress Activity releases rebuildable resident font caches under the render
lock immediately before starting a time worker. Rendering cannot refill them
between that release and startup's checked allocation. The existing 96 KiB
internal free-memory and 32 KiB contiguous-block reserves remain unchanged; low
memory still rejects the run before any network request or new reservation.
Startup failures keep their concrete diagnostic visible in service mode instead
of replacing it with a generic configuration/network retry message. Serial logs
record only free/largest-block bytes before and after cache reclamation.

Run `scripts/test_weread_service.ps1`: pure journal crash/torn-tail checks,
production client parsing and authenticated HTTPS transport boundary tests
(including the required receive-buffer preflight),
and production worker tests with internal RAM and PSRAM paths, including immediate
manual sleep, 20 dates crossing the 16-item boundary, full local slots,
between-operation timeout recovery and immutable-tail recovery.
`TestProductionDeviceBatchContract` in the companion service runs the actual
C++ parser against the Go handler and durable database through the pipe bridge:
five dates, lost response plus database restart, identical retry, query-only
recovery, malformed last receipt and revocation. The tests make
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

## Unified managed reading (development branch)

`codex/weread-managed-session` adds backend-managed shelf, detail, catalog,
notes/reviews, content context/shards and registered images. The device requires
`managed_reading_v1` from `/api/v2/device/status` with exact account/device
identity. Enable it separately with `WESYNC_MANAGED_READING=true` on the backend;
the default is disabled. No reading request can supply a URL or web credential.
The existing three-line service configuration also supplies reading identity.
After successful negotiation an identity-only `managed-account` marker blocks
QR/direct fallback if configuration disappears. It does not replace or erase
old session files, cached books or time ledgers. Reconcile old ownership before
an account/device change; never clear a service ledger to resume direct uploads.

`POST /api/v2/reading` uses closed logical actions (`shelf`, `detail`, `catalog`,
`progress`, `browse`, `context`, `shard`, `asset`). Shelf pages stream at most 100
books to one atomic IndexWriter. Each chapter obtains its own expiring context;
book/generation/device mismatch fails closed. Existing SD shard validation,
codec, chapter cache and EPUB packager are reused. There is no Range concatenation:
failed transient `.part` files restart cleanly, and complete validated chapters
may be reused. CDN resource IDs are SHA-256 of the normalized registered URL;
the backend restricts hosts, checks image magic and forwards no cookie or bearer.
Encrypted image registrations survive backend restart for seven days.

Position writes use independent durable `/api/v2/progress-jobs`. Before POST the
device flushes one identical logical position to `managed-progress.part`.
Queued/uncertain/running states retain that outbox. Only exact bound verified
receipts clear it. A durable conflict proves no write was admitted; clear that
refused outbox and require a new explicit comparison/direction. The backend
checks expected cloud update time, reserves before sending, omits `rt`, and
read-backs position. Unknown writes freeze successors, activation and renewals;
repeat submission performs only read-only recovery when uncertain. Position
verification never means credited reading time; WRS1/guard contracts are unchanged.

The client reuses the owning Operation's 4 KiB buffer and inactive legacy scratch;
no new persistent heap buffer is allocated. Host sizeof(Operation)=8176 remains
below its 8192-byte static budget. Transport and parser evidence (numbers only,
no IDs, URL, body or credentials) is in `last-managed-diagnostic.json` and
`last-managed-parser-diagnostic.json`; request_start survives a mid-request crash.

### Local acceptance

- `scripts/test_weread_managed_simulator.py --help`: production Operation,
  parsers/codec/SD writers/EPUB packaging against synthetic loopback backend.
- The explicit `simulator_managed_acceptance` profile injects transport only
  under **both** SIMULATOR and CROSSPOINT_MANAGED_ACCEPTANCE; hardware images
  cannot compile this path. Fixtures never use real credentials or cloud writes.
- `scripts/test_weread_simulator.py`: two UI profiles' home/settings/sleep/wake
  screenshots; Waveshare profile checks geometry, not hardware controller timing.
- Backend `scripts/validate-managed-session.ps1 --help`: retained real login,
  paused account/held sender, bounded readonly shelf/catalog/content/images and
  notes/reviews. Private responses stay beside the private device file; public
  report contains only counts, status and timing. No new QR or time/position POST.

Local acceptance does not prove physical Wi-Fi/TLS/SD, external-device session
coexistence, or real credited time. Keep production feature activation and
physical acceptance as separate gates. The retained encrypted database/key,
not an expired QR, is the reusable credential source.
