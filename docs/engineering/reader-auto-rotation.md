# Reader automatic orientation

## Scope and capability

Automatic orientation is a reading policy, not a global display setting. The
first port is Waveshare ePaper 3.97 with a successfully probed QMI8658. The
`HalDeviceCapabilities::supportsReaderAutoRotation()` opt-in and the HAL runtime
probe must both pass. An IMU on another board does not implicitly enable this
feature. A missing sensor keeps all four manual directions available.

The pinned BSP enables QMI8658 on SDA41/SCL42 at 400 kHz, preferring 0x6B and
using the existing driver's 0x6A fallback and WHO_AM_I check. No power rails,
bootloader, partition layout or permanent hardware state are changed.
The maintained `WAVESHARE_EPAPER_397` profile swaps IMU X/Y because physical
upright gravity was measured on raw +X; the normalized portrait frame uses +Y.
The distinct historical `WS_EPAPER_397` profile is unchanged.

## Layering and future ports

* BSP: sensor type, bus and address, plus `imuSwapXY` / `imuFlipX` / `imuFlipY`
  mount correction. Verify axes against physical portrait before enabling a port.
* HAL: `HalTiltSensor` owns the one sensor instance, shared sample, wake/sleep,
  errors and pending orientation. `OrientationTracker` is hardware independent;
  its input frame has X right and Y up in physical portrait. Clockwise physical
  rotation therefore produces negative X specific force; counterclockwise
  rotation produces positive X.
* Activity: only the current activity's `allowsReaderAutoRotation()` can opt in.
  EPUB reuses its visible-text anchor and existing reflow path. XTC bitmap books
  remain portrait because their renderer does not support the EPUB reflow path.
* Settings: `ReaderOrientationSettings` supplies the shared four manual labels
  and a fifth Automatic choice on supported, present hardware.

To add hardware, configure/probe its sensor and validate its mount correction,
then extend the central HAL opt-in. No board-name branches belong in reader,
menu, settings or classifier code. A new reading format explicitly opts in only
after implementing orientation-aware rendering and position preservation.

## Interaction and state

`orientation` remains the saved manual fallback (0..3). The separate
`readerAutoRotate` policy defaults off, is validated as a boolean, and is saved
only for supported hardware. Old settings retain their original direction.
Choosing a manual direction disables Automatic. Sensor updates change only the
current EPUB layout; they never save settings or change the saved fallback.
The reading cache key uses the effective layout direction, not the fallback.
Consecutive rotations retain the original visible-text anchor. Replacing it
with each reflowed page's start would round the position backward on every
rotation. Explicit navigation and other layout changes reset this anchor;
offset resolution alone does not. The bounded anchor adds 12 bytes to the
existing activity on ESP32-S3, with no separate heap allocation.

Candidate direction must remain stable for 900 ms. Diagonal, near-flat,
non-finite, free-fall and excessive-acceleration readings reset the candidate;
sample gaps over 350 ms also restart the dwell. The gravity direction must stay
within 10 degrees of the candidate's initial vector for that whole interval;
ongoing movement restarts it. Posture does not use the gyro as a prerequisite:
on the tested Waveshare, live acceleration was accompanied by three gyro
outputs saturated at -512 dps, which previously blocked every rotation. This
policy separates posture from gyro health; it does not claim to repair that
gyro fault. Manual tilt-flick retains its existing gyro behavior. Unsigned
elapsed times handle the millis rollover.

Held/edge input, rendering/reflow, font prompts, toolbar/popups, child activities
and active background time synchronization freeze automatic rotation and clear
candidates. A new stable interval is required after resuming. The render lock
freezes active work: a readable page with a paused
incremental chapter builder may still rotate. `Section::isBuilding()` indicates
retained parser state, not concurrent work; requiring it to be false stranded
automatic orientation after a landscape reflow. Resetting the section suspends
and preserves the partial cache through its existing destructor.
Reading menus inherit the frozen direction. Returning from a child restores the runtime
reading direction; leaving the reader restores portrait. Global pages do not
subscribe to automatic rotation. Automatic mode suppresses tilt-flick page
turning, so rotating the device cannot also turn a page. Manual orientation
retains the existing tilt behavior.

## Resource and power budget

The classifier and pending event use bounded scalar member state, with no new
task, heap allocation, framebuffer or sample queue. One existing IMU sample is
shared per poll. The gravity dwell anchor adds three floats (12 bytes) to the
existing HAL singleton in internal RAM, with no heap or PSRAM allocation.
The current QMI8658 driver runs accelerometer and gyroscope at
117 Hz; HAL polling is 20 Hz. This is not an accelerometer-only low-power mode.
Enabled-mode battery cost requires measurement; no power-saving figure is
claimed. Automatic disabled, global pages, modal freezes and sleep leave the
sensor in the existing hardware standby path. Posture samples do not reset the
inactivity timer. This ordinary firmware feature has no dependency on USB debug;
debug control remains excluded from Nightly/OTA builds.

## Verification

Run `python -m unittest discover -s scripts/tests -p test_reader_auto_rotation.py -v`
to compile the actual production HAL and classifier against a deterministic IMU.
Cases cover four directions, dwell, jitter/flat/invalid inputs, stale samples,
millis wrap, single-sample polling, modal/read-failure reset, one-shot delivery,
sleep lifecycle, unchanged manual flick behavior and unsupported-board rejection.
Physical clockwise/counterclockwise roundtrips, sustained gravity movement and
live gravity with saturated gyro outputs have dedicated production-code cases.
Build Waveshare debug and Nightly environments and run the repository checks.

Physical acceptance is separate: verify sensor identity and each gravity axis,
all four visible directions, readable one-line physical button hints, single
button actions, preserved text position, menu freeze/return, home portrait,
manual/automatic persistence across reboot, missing-sensor fallback, sleep and
enabled-mode power. Test real body movement and flat placement; software sample
tests and framebuffer screenshots do not prove physical axes or optical output.
Keep the user's USB debug keep-awake preference enabled after acceptance.
