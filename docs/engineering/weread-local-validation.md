# Local WeRead device/backend validation

Run on Windows with PowerShell, `g++`, Python and Go installed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_weread_local.ps1
```

The default backend location is the sibling application workspace's
`svc/weread-sync`. Override with `-BackendRepo <path>` when needed. Use
`-Suites contract,backend` for the narrower cross-language/backend checkpoint,
or `-Help` for options. Each suite has a bounded timeout (300 seconds by default).
Failures stop the run. Logs and `result.json` are saved under
`artifacts/wesync-validation/runs/<timestamp>/`.

The five suites are:

- `service`: production device service client, journal and background worker,
  including internal-RAM and PSRAM paths.
- `offline`: existing ledger/cloud/POST-context/network-boundary tests.
- `contract`: production C++ `ServiceClient`, streaming parser and journal
  exchange requests with the actual Go HTTP handler and bbolt store. The bridge
  substitutes only OS/TLS transport with bounded pipe frames.
- `backend`: full Go test delivery checkpoint, including session boundary tests.
- `session-ui`: starts a loopback-only backend with synthetic credentials and
  fixture upstream transport, then exercises the real management JavaScript
  through a minimal DOM. Verifies stage/probe/single renewal/activation, cleared
  credential input, paused sender, unchanged queue and device-scoped status.
  This is functional acceptance, not browser rendering.

The matching backend must contain `internal/server/device_contract_test.go`
and `internal/syncer/session_boundary_test.go`. The cross-language test explicitly
skips in a plain Go test run without `WESYNC_DEVICE_CONTRACT_EXE`; the `contract`
suite compiles the bridge and supplies that variable for its own child process.

## Evidence boundary

All identities, credentials, reading ranges and upstream responses are fixtures.
The runner does not read private provisioning files, contact the public service,
create a WeRead login, report real reading time, install firmware or deploy.
The tested application logic is real; network, SD and upstream behavior remain
host substitutes. Physical TLS/SD reliability, multi-device login coexistence
and real cloud credit require separate acceptance.

The contract verifies durable receipt loss + backend restart, identical POST
deduplication, GET-only accepted-record recovery, account login pause, additive
status fields, wrong identity, missing receipt and device-token revocation.
No ACK is converted into cloud credit.

Session tests now cover the backend session foundation: non-auth business and
missing-reader errors do not request login, unknown renewals freeze across
restart, staged activation remains paused, stale credentials are rejected and
uncertain uploads remain frozen. The raw protocol Prepare call never renews;
renewal ownership belongs to the Store session manager before reservation.

The unified-session implementation plan is saved in Garden as
`CrossMux 微信读书统一会话实现方案.md`. Proposed status APIs, staged credential
activation are implemented locally in the backend development branch, but not
deployed. Managed reading transport is not implemented or advertised yet.

## Desktop simulator acceptance

The default `simulator` uses the pinned SDK's X4 Pro profile.
`simulator_waveshare_ui` uses its non-touch 480 x 800 profile with the project's
Waveshare button-hint layout. This second environment tests UI geometry only;
it does not emulate Waveshare GPIO, PMIC, PSRAM, SDMMC or display waveforms.
Neither environment is a firmware release target.

Build and run in a Linux environment with SDL2 and the pinned dependencies:

```sh
pio run -e simulator -e simulator_waveshare_ui -j 4
python3 scripts/test_weread_simulator.py --root "$PWD" --out /mnt/c/Project/app/embedded/crossmux-trial/artifacts/wesync-validation/simulator-acceptance-new
```

The output directory must be new. Under WSL it must be on a mounted Windows
drive. The script creates an isolated SD fixture without credentials, opens the
real simulator, captures Home and Settings, requests sleep, wakes with Power,
and captures the resumed Home screen. It verifies exit status, sleep logging,
480 x 800 nonblank BMPs and navigation changes, with a 25-second process timeout
per profile. It preserves the tested Linux executable, logs, screenshots and
SHA-256 values. This is baseline lifecycle/UI acceptance; it does not exercise
the proposed managed-reading client or prove physical wake timing.

On 2026-09-27 both builds and both scripted runs passed. Evidence is in
`artifacts/wesync-validation/simulator-acceptance-20260927/result.json`.
The backend/contract/management-JavaScript checkpoint is separately recorded in
`artifacts/wesync-validation/runs/20260927-165639-276/result.json`.
The management JavaScript test uses a minimal DOM and actual loopback HTTP
requests; browser visual acceptance remains unverified because no browser
runtime was available.
