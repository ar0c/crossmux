# Firmware Release Architecture

> Current scope (2026-10-08): only Waveshare ePaper 3.97 development/Nightly firmware is maintained, built, checked and packaged. X4 Pro instructions below describe retired history, not an active build/release target. Previous releases/backups are retained. Embedded hyphenation patterns are English-only; Chinese uses existing CJK breaking without a dictionary. UI translations and font glyph coverage are unchanged. No Stable firmware target is configured.


## Fork firmware name

The firmware's startup/About name and new packages use `crossmux-ar0c`.
Local `waveshare_epaper_397` builds use `<base>-<revision7>-ws397-dev` and export
`crossmux-ar0c-<base>-waveshare-epaper-397-<revision>-<image-digest8>.bin`.
PlatformIO retains its internal `firmware.bin` for packaging. Nightly versions
use `<base>-<revision7>-ws397-rc`; local staged-tree Nightly versions end in
`-local`. The exact embedded version must match a published manifest.
X4 Pro identity helpers remain solely for validating historical filenames;
no current build environment or export action produces X4 Pro images.

The previously built full time-sync image (SHA-256 beginning `d9bc65ce`) used
the historical name `260915-181930-d9bc65ce-ar0c-x4pro.bin`, based on its recorded build output time.
That historical filename-only change left its embedded runtime version unchanged.
No rebuild/flash is required merely to rename an SD
firmware file. Old device packages are backed up on Windows before removal;
accounting `.bin` receipts, book data and the running application are not firmware
package cleanup targets.

New releases use the Waveshare-only Nightly pipeline. Existing Stable/X4 Pro releases are historical and remain available; no new Stable target is configured.

## Canonical targets

[`scripts/nightly_targets.py`](../../scripts/nightly_targets.py) is the release
source of truth despite its compatibility filename. Each target defines its
runtime models, artifact slug, embedded board tag, per-channel PlatformIO
environments, chip, install capability, and supported channels. The workflow,
packager, index builder, and tests import this table rather than copy it.

Waveshare ePaper 3.97 is the sole canonical firmware target. Its development
and Nightly environments use the same ESP32-S3 board tag. Compatibility
`global` and `zh-CN` manifests alias one unified image. X4 Pro and Read Pico
are not current build, package or release targets. Shared HAL code stays available.

### Firmware-write progress rendering

OTA and SD firmware updates draw an initial progress frame, refresh after at
least another 10 percentage points (jumping directly to the latest value), and
finish the 100% frame before continuing. The progress callback updates byte
snapshots under `RenderLock`, releases the lock, then calls
`requestUpdateAndWait()`. Flash erase/write and the subsequent boot-partition
switch must wait for that frame: overlapping Flash operations can suspend
Read Pico's display feeder tasks during a panel refresh. Render methods always
submit a complete frame; they do not throttle after clearing the framebuffer.

OTA rendering reads only the locked snapshots, not the updater's live counters.
Progress bars clamp completed bytes to the reported total; an unknown/zero
total displays 0%. Download progress reaching 100% does not mean installation
succeeded: the completion page is shown only after firmware verification and
boot-partition selection succeed. No additional heap buffer is allocated.

Run `python3 scripts/tests/test_firmware_update_refresh.py` for the production
callback, Flash-operation ordering, failure-path and theme-render regressions,
alongside `test_font_preload_refresh.py` and `test_download_memory_lifecycle.py`.
Device acceptance must record the installed firmware hash and serial log,
repeat Read Pico OTA through the completion page and expected reboot, and check
interrupted-download recovery plus the SD update path. Builds and host tests do
not establish physical display or reboot acceptance.

## Publishing

### Owner-authorized direct releases

On 2026-10-02 the owner authorized direct releases of this personal fork and
its `ar0c/weread-sync` companion without repeated per-release confirmation.
Use the established GitHub Releases/OTA path for firmware and the service-owned
Helm chart on the personal K3s cluster for the backend. The generic GitLab
`ship` master/tag path does not apply to these two GitHub projects. This
authorization does not extend to other projects or release destinations.

Before publishing, run relevant local tests against the exact source snapshot,
verify the target board or backend image identity, and keep an immutable build
plus a rollback reference. Publish the firmware's immutable assets before its
rolling index; check the GitHub index and public mirror afterward. For the
backend, change only the verified image, preserve existing Helm values and
`workerHold`, use an atomic upgrade, and read back the running version and
business result. A healthy Pod or OTA index alone does not prove device
behavior. Do not release uncertain reading-time batches, fabricate credit, or
perform irreversible hardware/security writes under this authorization.

Each target job builds once and packages one binary set plus two compatibility
manifests. Packaging checks the ESP image chip ID, required board tag, partition
layout, app-slot size, and SHA-256 before emitting the manifests.

The fork publishes to its GitHub Releases only, in this order:

1. immutable binaries and checksum files;
2. immutable target manifests;
3. rolling channel index.

Every target selected for a channel must build successfully before publication.
CI resolves every manifest and verifies each distinct asset's size and SHA-256.
Previous Nightly indexes are saved as evidence. The current workflow does
not delete retired firmware assets or historical builds. The rolling Nightly
index advertises only Waveshare; immutable older releases stay accessible.
Firmware plus both compatibility manifests live in an immutable
`nightly-build-<sha>-<run>-<attempt>` GitHub Release. No workflow in this scope
writes to COS or `crossmux.cn`.

## Index contract and failure behavior

The schema-v1 index contains `channel`, `updatedAt`, `buildId`, and a `targets`
map, plus optional localized `releaseNotes`. Each target repeats its identity and
channel capabilities and contains `global` and `zh-CN` pointers with version,
CrossMux SHA, SDK SHA, publish time, and immutable manifest URL. Stable requires
both release-note locales.

Every target advances together only when both compatibility manifests are valid
and have the same CrossMux revision, SDK revision, version, and assets. A
missing or malformed manifest prevents the whole channel from publishing.
Build objects are never overwritten; cleanup runs only after the new rolling
index and its assets pass verification.

## Public K3s mirror

The fork's GitHub Releases remain the build and publication source. The
`deploy/k3s/ota` workload on `mst01` checks the public `nightly` release every
five minutes and mirrors changed builds into `/home/ar0c/crossmux-ota-dist`. After
`verify_publish`, the Release workflow also sends a signed notification to
`POST /hooks/release` to synchronize immediately. Stable notifications run only
after the versioned Stable release succeeds. The handler rejects stale or invalid
HMAC-SHA256 signatures and checks the requested build ID; both entry points use
one file lock. The scheduled job remains the fallback. Nightly accepts the
maintained Waveshare-only index and historical two-board indexes. Stable accepts
historical X4 Pro content; its scheduled job remains suspended. Each mirror checks every manifest against
the rolling index and checks every
binary's length and SHA-256. It writes an immutable build directory before
atomically replacing the rolling index. The serving Pod mounts that directory
read-only and exposes only the release download paths. The Ingress for
`ooo.ar0c.com` requires Cloudflare authenticated origin pulls. The first Stable
release has not been published, so the Stable mirror CronJob is suspended until
that release exists.

Historical partial Nightly indexes can retain X4 Pro pointers to an older
immutable build when that target entry exactly matches the previous published
index. The mirror stores exact verified source indexes in a private
`.verified-indexes` directory, seeding the currently mirrored index on upgrade.
This permits rollback from a Waveshare-only index to a previously verified
two-board index, including mixed-build pointers. Rollback still verifies every
manifest, binary hash and existing immutable file. Changed preserved targets,
unknown target sets or missing immutable assets keep the old public index in place.

The main integration retains the committed 16-item WeRead batch handoff,
the fixed request workspace, portable worker notifications and query-only
recovery of uncertain batches. The WRS1/v2 service-journal byte format and
immutable per-range job IDs remain compatible. Existing backend single-job
endpoints remain supported. Source and host checks do not establish physical
handoff, sleep or cloud-credit acceptance.

The public OTA contract is
`https://ooo.ar0c.com/releases/download/<channel>/release-index.json` with
immutable manifests and binaries under
`/releases/download/<channel>-build-<sha>-<run>-<attempt>/`. GitHub remains the
source of the bytes, but the public index points only to this host. The device
uses the ESP certificate bundle and does not follow redirects for OTA fetches;
other network downloads retain their existing transport. Cloudflare must allow
non-interactive GET/HEAD requests to these exact public paths. The separate
signed POST hook also needs a narrowly scoped Cloudflare exception; it never
serves files. A browser challenge breaks e-paper clients, so an HTTP 403 with `cf-mitigated: challenge`
is a release blocker. An external GET and complete asset verification pass in
CI; confirm the certificate-verified OTA fetch on physical devices before
claiming device-level validation.

Apply the K3s manifest with `k3s kubectl apply -k deploy/k3s/ota` from a
checkout on `mst01`. The 32-byte random notification key must be installed as
GitHub Actions secret `CROSSMUX_OTA_HOOK_KEY` and as key `key` in K3s Secret
`default/crossmux-ota-hook-key` using the same 64-character hex value; never
store the value in the repository. Then trigger one initial mirror Job from
`cronjob/crossmux-ota-mirror-nightly`. Verify the cluster Service and the
external hostname separately. Mirror failures leave the previous index in
place; inspect the failed Job's logs rather than replacing the index by hand.

## Consumers and safety

The Web flasher remains an upstream service. Device Check Updates reads only
the fork's K3s mirror of its GitHub Releases: `stable` for X4 Pro, `nightly` for
X4 Pro and Waveshare 3.97. The index selects the exact board and content
variant, then the immutable manifest supplies the firmware asset, size, and
SHA-256. The device rejects mismatched board, channel, revision, size, digest,
or a URL outside this fork's immutable release namespace. Until a channel's
first release and `release-index.json` are published, checks report a fetch
error; they never fall back to upstream.

Official packages must contain the board tag. The OTA stream aborts a tagged
image for another board before selecting the new partition. Untagged historical
or third-party images remain compatible, so chip and board checks in the
official packaging path are mandatory.

CI, indexes, and checksum checks do not replace real-device acceptance. Before
using a Nightly image on hardware, test boot, reading, input, storage, display,
and sleep/wake on that exact S3 board. In-device OTA also needs a real update,
reboot, and wrong-board rejection check on each board before claiming hardware
acceptance. OTA uses the certificate-verified ESP HTTP path; the general
wolfSSL downloader still uses `setInsecure()` for unrelated downloads. This
does not provide signed metadata or a cryptographic rollback counter.

### Managed-session development branch

Every branch uses the same Waveshare-only Nightly matrix. Dispatch accepts
`auto` or `waveshare_epaper_397`; X4 Pro selection is rejected. A first release
does not require a previous index. Local prepared releases retain previous
index bytes for rollback without rewriting retired firmware artifacts.

### Local Waveshare Nightly test release

`scripts/manual_nightly_release.py` can prepare, publish, and restore a
Waveshare-only Nightly index from a local Windows checkout. It does not publish
`service.conf` or any device credentials. The source identity for this mode is
the staged Git **tree** object, recorded as `git-tree-local` in the two
manifests; the source archive stays in the local prepared directory. This
distinguishes a local build from a pushed Git commit. Keep the prepared
directory until the release has been accepted or rolled back.

Stage exactly the source snapshot to build, with no unstaged tracked changes
or untracked source files. Put validated English and Chinese OTA note arrays in
a local UTF-8 JSON file using the `en` and `zh` keys. Then run:

```text
python scripts/manual_nightly_release.py prepare --build --notes-json <notes.json> --output <new-prepared-directory>
python scripts/manual_nightly_release.py publish --prepared <prepared-directory>
python scripts/manual_nightly_release.py rollback --prepared <prepared-directory>
```

Preparation builds `waveshare_epaper_397_nightly` with an embedded
`1.6.0-<tree7>-ws397-local`-style version, checks that
version in the ESP32-S3 image, packages the four install segments and two
manifests, and verifies only the Waveshare candidate index and assets. The
saved previous index may include retired X4 Pro pointers; its full bytes are
retained for rollback, which verifies exact index readback and Waveshare assets.
The actual base version comes from `platformio.ini`. Publication
refuses a changed rolling index, uploads an immutable build first, verifies
its assets, moves the GitHub Nightly index last, then waits for and verifies
the public K3s mirror. This is a shared Nightly channel: other Waveshare
Nightly devices can see the test build.

Rollback restores only the saved previous rolling index if the channel still
points at this prepared release. The K3s mirror validates that old index and
reuses immutable assets. An already installed higher base version cannot
automatically downgrade through the device's update check; repair it with a
newer build or a deliberate SD-card install. Never delete a referenced
immutable release while preparing or rolling back.
