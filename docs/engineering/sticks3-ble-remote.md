# StickS3 BLE remote and USB closed-loop validation

CrossMux is a BLE HID **host**; the independent StickS3 firmware is a keyboard
peripheral. The reader's maintained build/release matrix remains Waveshare 3.97
only. The peripheral project is now [remotes/sticks3](../../remotes/sticks3/README.md) in
this repository, with independent environment `sticks3_remote`. From the repo root
run `pio run -d remotes/sticks3 -e sticks3_remote -j 4`. Its README is the source
of truth for build, safe flashing, USB commands and tests. Its project-local
toolchain is isolated from the reader; generic upload/erase targets are rejected.
The dedicated StickS3 CI builds an application artifact without changing the
reader Nightly/OTA matrix. The old sibling directory and remote-specific caches were subsequently moved
to the Recycle Bin at the owner's request. The previously installed 0.2.3 app,
original private 8 MiB Flash image and compact verification manifest now live
in `remotes/sticks3/firmware/` (gitignored). `tools/flash_saved.py --check` checks
these files without device access; omitting `--check` invokes the existing
identity/backup-gated app-only flasher, without rebuilding. Raw peripheral logs
and older images are no longer retained in the project; historical results
below are summaries, not currently available raw evidence.

## GUI-flashable full image

The owner-requested local full image is
`remotes/sticks3/firmware/StickS3-CrossMux-Remote-0.2.3-full-private.bin`
(8,388,608 bytes). Use ESP32-S3, offset `0x0`, DIO, 80 MHz and 8 MB in the
flashing tool. No Python script or rebuild is required for downloading it.
The image preserves every byte outside the reader application partition from
the verified original Flash snapshot; the reader slot contains the previously
installed 0.2.3 application with its unused tail filled with `0xff`.
Bootloader, partition table, NVS, OTA selection, rescue, SPIFFS and coredump
remain equal to the original snapshot. It therefore restores original settings
and data and may require Bluetooth re-pairing. This is a private, device-specific
snapshot kept out of Git and public artifacts. Source digests, app placement,
full length, preserved regions and boot image integrity were checked; this
packaging request did not flash hardware or verify a full-image reboot.

## Pairing and mapping

Open Settings → Controls → Bluetooth, enable Bluetooth, scan and pair
`StickS3 CrossMux`. Use Map Remote Buttons to learn these four functions:

| Remote gesture | HID key | Reader action |
|---|---|---|
| A short | PageDown, 0x4e | Next page |
| B short | PageUp, 0x4b | Previous page |
| A hold ≥650 ms | Escape, 0x29 | Back |
| B hold ≥650 ms | Enter, 0x28 | Confirm / reader menu |
| Joystick2 directions (0.2.0+) | Arrow Up/Down/Left/Right | Global UI navigation |
| Joystick2 click / hold ≥650 ms | Enter / Escape | Confirm / Back |

Since peripheral version 0.1.4, A/B short presses are Next/Previous respectively.
Existing 0.1.3 receiver HID mappings and bonds remain usable without relearning.

One gesture emits a press and an all-zero release. Long holds do not emit a
second short action on release. The joystick requires center after boot,
reconnect and each deflection; it does not repeat while held. Standard arrows
have default direction actions; learned bindings take priority. The original
four-key profile still requires learned actions:
`BleButtonMapActivity` captures and persists them, and `MappedInputManager`
looks up the saved map before dispatching input.

## Lifecycle and boundaries

`main.cpp::updateBluetoothLifecycle()` starts the reader host on demand and
stops it during Wi-Fi or exclusive storage. Waveshare keeps the host alive on
awake Home, library, settings and reading pages, and automatically starts it
using the existing conservative reader memory gate. Other targets retain
their previous lifecycle policy. Explicit Bluetooth settings pages can still
start their own host. Reader preparation can defer a
fresh start (`EpubReaderActivity::deferBluetoothStart()`); the first uncached
31-page TXT fixture did not connect until its pagination completed. Cached
reading then reconnected automatically. Global awake operation consumes radio
and accessory power; battery life has not been measured. Do not describe peripheral advertising
or a saved bond as a currently usable reader connection.

A deeply sleeping reader cannot be woken by the remote. Wake the reader using
its own power button. USB `KEEP 1` is a RAM-only test-firmware setting and does
not establish production power behavior or a battery-life measurement.

## Closed-loop proof

The dedicated Waveshare debug image and `scripts/device_debug.py` provide
receiver status and checksummed framebuffer captures. StickS3's USB `REM1`
interface injects A/B holds and joystick inputs through the same gesture decoders as physical controls,
then sends real BLE input. No receiver-local pulse substitutes for BLE during
the accepted remote action tests.

The peripheral `ready` flag means encrypted connection plus notification
subscription. After reconnect, the receiver must also finish HID setup; the
test waits for CrossMux's own `[BLE] connected:` transition before sending keys.
It verifies expected body-content changes/restoration and menu transitions,
rather than treating any frame increment as proof of the command's effect.

Arduino-ESP32 3.3.7 can issue both submission and transmission callbacks for one
notification. Report submission count, callback count, and receiver execution
are separate evidence. Arduino HWCDC intermittently truncated ~550-byte USB
replies, including after increasing its default 256-byte queue. The peripheral
therefore disables Arduino CDC ownership and uses the pinned ESP-IDF USB
Serial/JTAG driver, bounded static formatting, a checked one-time 2 KiB TX /
256 B RX allocation, and zero-wait I/O. Its read-only regression alternates
100 complete status records and 20 checksummed reader frames with idle gaps.

Use a separate local synthetic book, with WeRead sync unavailable/inactive.
Keep mutation timeouts as unknown outcomes; inspect status/screenshots without
automatically resending. Preserve previous evidence directories. Framebuffer
verification does not prove optical panel quality or manual finger presses.

The retained installed 0.2.3 image and original Flash digest are indexed by
`remotes/sticks3/firmware/manifest.json`. New source builds are not proof of
installation or additional physical acceptance. The local cleanup did not
flash either device or re-run physical acceptance. Private Flash backups and raw device logs stay
local. Global joystick operation requires the corresponding reader lifecycle
and standard-arrow changes; the original four-button remote works with the
previous receiver. No Nightly publication is needed for local USB acceptance.

## Accessories (peripheral 0.2.0+)

User-confirmed wiring is Unit Joystick2 at SDA9/SCL10, I2C address 0x63,
and the vibration HAT control rewired to G1. G0 is a boot strap and must not be
used. Register reads follow the official
[M5Unit-Joystick2 driver](https://github.com/m5stack/M5Unit-Joystick2/blob/main/src/m5_unit_joystick2.hpp);
calibration, address and device firmware are never rewritten. Shared external
5V remains on for the joystick. Motor PWM channel 4 does not share the display
backlight's channel 7. Output starts at zero; boot lockout is 2500 ms.

Accepted HID actions trigger a 35 ms local vibration at duty 128/255. This
signals submission, not receiver acceptance. USB `JOY UP|DOWN|LEFT|RIGHT|CLICK
<30..2000 ms>` and `HAPTIC <10..250 ms>` retain identity/session checks. Joystick
online/axes/read/error/event counts and motor active/pulse counts are exposed
in STATUS. I2C failure resets the gesture instead of generating a click release.
The physical joystick must be centered before USB injection.

Peripheral 0.2.3 uses the latest physical calibration: Up `(127,3)`, Down
`(-126,2)`, Left `(0,-127)`, Right `(0,127)`. Raw-to-logical axes are
`(rawY, -rawX)`; STATUS reports `joy_axis_mode=swap_xy_invert_y`. The 0.2.2
fixed-rotation assumption was insufficient: physical Up/Down/Left/Right were
observed as Right/Left/Down/Up. Replay of those raw samples reproduced the bug
before the transform was fixed. Version 0.2.1 used `swap_xy` (`rawY, rawX`);
0.2.2 used `(rawX, -rawY)`. Raw STATUS X/Y remain unchanged. USB JOY directions
are already logical and bypass this transform. Recheck physical directions if
the module mounting changes. `calibrate_joystick.py --directions UP --verify`
can check a single direction; it never writes module calibration or firmware.

## Physical calibration, 2026-10-12 (0.2.3)

Version 0.2.3 was installed and read back with rescue/data regions verified
preserved. The regression test fails on 0.2.2's transform for the captured Up
sample, then passes on all four actual samples and signed-axis limits after
correction. Fourteen digital BLE navigation steps passed, including Up/Down in
Settings. Final physical direction verification is tracked separately in the
peripheral manifest. The reader returned to Home with the original novel
selected and KEEP 1 retained; its application was not reflashed.

## Direction rotation, 2026-10-12 (0.2.2 history)

Version 0.2.2 was built, installed and read back, with rescue/data regions
verified preserved. Host tests cover all four rotated directions, signed-axis
limits and no-repeat behavior. Runtime STATUS confirms the new version and
axis mode; encrypted BLE reconnect and six logical navigation actions passed.
The receiver application was not reflashed. Physical Up-to-reader verification
is separately tracked in the peripheral manifest; software tests and logical
USB gestures alone do not establish the user's current physical orientation.

## Local acceptance, 2026-10-12 (0.2.1 history)

StickS3 0.2.1 and the Waveshare debug application were installed and read back;
the reader runtime string remains `1.6.5-84d6e35-ws397-dbg`, so use the manifest
digest to identify this build. Original rescue/inactive applications and data
regions were verified preserved. This was local USB delivery, not a Nightly
release.

Digital BLE tests covered Home/library/apps/settings navigation, reader arrows,
A-next/B-previous, menu direction/click/hold, and disconnect/reconnect followed
by page changes. The local fixture returned to page 30/31 with matching body
hashes. The user confirmed a physical motor pulse. Four physical gestures
produced four completed HID actions, four receiver frames and four motor pulses,
then restored the body. The first physical LEFT was observed as LEFT after axis
correction; the strict RIGHT-first observer stopped at that first action, so
intermediate physical frames were not all captured. Do not describe the entire
user-reported LEFT/RIGHT/RIGHT/LEFT sequence as independently captured.

The reader was returned to Home with its original novel selected, retaining
USB KEEP 1. Before cleanup, the peripheral manifest indexed both successful and failed captures
and the exact firmware hashes; prior release indexes remain available.
