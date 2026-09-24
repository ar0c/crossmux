# Firmware Release Architecture

## Fork firmware name

The firmware's startup/About name and all new exported firmware packages use
`crossmux-ar0c`. Local `x4pro` builds export
`crossmux-ar0c-YYMMDD-HHMMSS-<image-sha256-prefix8>-x4pro.bin`. The stamp uses China
time and is captured once at build setup. The digest is computed from the final
image, so distinct uncommitted builds cannot silently share a Git-only filename.
Other development targets export `crossmux-ar0c-<base-version>-<device>-<git-sha>-<image-sha>.bin`.
PlatformIO keeps its internal `firmware.bin` because the build and package scripts
consume it. Published install assets use `crossmux-ar0c-<device>-<segment>.bin`;
manifests record those exact names, while their URLs and board tags still
identify the matching device.
The runtime version on subsequent builds is
`YYMMDD-HHMMSS-ar0c-<base-version>-x4pro`, checked against the 32-byte limit.
The base upstream version is not artificially incremented. Identity/export tests are in
`scripts/tests/test_fork_identity.py`.

The previously built full time-sync image (SHA-256 beginning `d9bc65ce`) used
the historical name `260915-181930-d9bc65ce-ar0c-x4pro.bin`, based on its recorded build output time.
That historical filename-only change left its embedded runtime version unchanged.
No rebuild/flash is required merely to rename an SD
firmware file. Old device packages are backed up on Windows before removal;
accounting `.bin` receipts, book data and the running application are not firmware
package cleanup targets.

This fork has two release channels, `stable` and `nightly`, managed by one
channel-aware pipeline. Hardware identity is not a release channel. Both channels
contain ESP32-S3 images only: X4 Pro is the stable target, and X4 Pro plus
Waveshare ePaper 3.97 are in Nightly. Other device build profiles have been
removed from this fork.

## Canonical targets

[`scripts/nightly_targets.py`](../../scripts/nightly_targets.py) is the release
source of truth despite its compatibility filename. Each target defines its
runtime models, artifact slug, embedded board tag, per-channel PlatformIO
environments, chip, install capability, and supported channels. The workflow,
packager, index builder, and tests import this table rather than copy it.

X4 Pro stable uses `x4pro-gh_release`. X4 Pro and Waveshare ePaper 3.97 each
produce a separate ESP32-S3 Nightly image. Each image is aliased by the compatibility `global` and
`zh-CN` pointers.

## Publishing

Each target job builds once and packages one binary set plus two compatibility
manifests. Packaging checks the ESP image chip ID, required board tag, partition
layout, app-slot size, and SHA-256 before emitting the manifests.

The fork publishes to its GitHub Releases only, in this order:

1. immutable binaries and checksum files;
2. immutable target manifests;
3. rolling channel index.

Every target selected for a channel must build successfully before publication.
CI resolves every manifest and verifies each distinct asset's size and SHA-256.
If a previous Nightly index exists, cleanup protects its referenced builds.
The first Nightly run has no previous index and skips cleanup. Stable builds are
retained.

The index is the `release-index.json` asset of the rolling `stable` or
`nightly` GitHub Release in `ar0c/crossmux`. Binaries and both compatibility
manifests live in an immutable `<channel>-build-<sha>-<run>-<attempt>` GitHub
Release. Both variants reference the same binaries. After Stable verification, the version tag
also receives the board-specific `x4pro` full-install assets. The rolling
release removes obsolete generic C3 `firmware.bin` and `firmware-cn.bin` assets.

At steady state, GitHub retains the current build and any build referenced by
the previous index. Failed builds do not trigger cleanup. No fork workflow writes
to `crossmux.cn`, COS, or another release service.

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
one file lock. The scheduled job remains the fallback. Nightly accepts the two
S3 targets and Stable accepts X4 Pro. Each mirror checks every manifest against
the rolling index and checks every
binary's length and SHA-256. It writes an immutable build directory before
atomically replacing the rolling index. The serving Pod mounts that directory
read-only and exposes only the release download paths. The Ingress for
`ooo.ar0c.com` requires Cloudflare authenticated origin pulls. The first Stable
release has not been published, so the Stable mirror CronJob is suspended until
that release exists.

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
