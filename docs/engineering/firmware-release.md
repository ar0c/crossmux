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

## Consumers and safety

The Web flasher remains an upstream service. Device Check Updates reads only
`ar0c/crossmux` rolling GitHub Releases: `stable` for X4 Pro, `nightly` for
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
acceptance. The wolfSSL download path currently uses `setInsecure()`; manifest
hashes detect transfer corruption but cannot authenticate a forged manifest.
Treat this as a transport security limitation until certificate validation or
signed release metadata is implemented.
