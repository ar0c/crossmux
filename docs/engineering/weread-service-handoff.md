# WeRead service handoff (X4 Pro)

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
range, and Reserved/Accepted/Confirmed state. Every frame has a checksum and
must follow a legal monotonic transition. A torn tail or changed binding blocks
the run. Original reading records and legacy journals remain untouched.

The device posts only Reserved records, with a deterministic task ID and the
same immutable body on every retry. Accepted records use GET only. A missing
server receipt is an error, not permission to submit again. Only exact matching
identity/range and a confirmed full duration advance local cloud credit. A day
with an unfinished handoff keeps later reading on the device until that handoff
has been confirmed. Other audited days can be accepted independently.

Do not downgrade to firmware that does not understand WRS1 after delegating
time. Such firmware cannot see service ownership and might resend it. Preserve
the SD journals and reconcile all server receipts before any rollback. Do not
delete WRS1 to clear an error; do not restore an older service database and
blindly replay device records.

## Verification and limitations

Run `scripts/test_weread_service.ps1`: pure journal crash/torn-tail checks,
production client parsing and authenticated HTTPS transport boundary tests,
and production worker tests with internal RAM and PSRAM paths. The tests make
no real network requests or reading-time increments. Build with `pio run -e
x4pro`; verify the version-first exported image and its SHA-256 before upload.

The local device status is the last explicit readback. To see newer cloud
credit, open the service management page or initiate another device sync. The
server applies its own pacing and freezes ambiguous cloud writes.
Cloudflare may still challenge clients outside the permitted network region;
the device stops with its reservation intact, without changing User-Agent to
impersonate a browser or weakening TLS. The client identifies itself as
`weread-sync-device/0.1`.

Build and native tests do not prove physical-device Wi-Fi/TLS/SD operation.
After installing the package, verify one genuinely new, unsent reading range,
the server's durable receipt, device disconnect, and later exact cloud credit.
