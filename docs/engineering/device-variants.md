# Supported device variants

This fork supports two ESP32-S3 devices. Each needs its own firmware image; the
board tag in the image prevents installing a tagged image for the other board.
The canonical release targets are in [`scripts/nightly_targets.py`](../../scripts/nightly_targets.py).

| Device | Development build | Nightly build | Stable build | Board tag |
|---|---|---|---|---|
| Xteink X4 Pro | `pio run -e x4pro` | `pio run -e x4pro_nightly` | `pio run -e x4pro-gh_release` | `x4pro` |
| Waveshare ePaper 3.97 | `pio run -e waveshare_epaper_397` | `pio run -e waveshare_epaper_397_nightly` | — | `waveshare_epaper_397` |

The desktop `simulator` environment is a development aid. It does not count as
a hardware target or validate display waveforms, button wiring, or sleep power.
See [Waveshare hardware notes](waveshare-epaper-397.md) and
[firmware release](firmware-release.md) for installation and release details.
Other device profiles are no longer built or published by this fork.

## X4 Pro panel identity

A user-owned X4 Pro boot probe on 2026-09-17 reported an 800 × 480 UC8279
controller, variant `0x68`, VER `00 0F 68 00 00`, and FLG `0x13`. This is
specific to that unit. X4 Pro hardware can also use SSD1677 or UC8179, so
runtime controller detection remains necessary. Build success is not a
hardware acceptance test.
