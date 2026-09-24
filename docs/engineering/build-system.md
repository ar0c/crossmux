# Build System & Build Flags

> Deep reference for [AGENTS.md](../../AGENTS.md). Covers PlatformIO usage, the
> build environments, the critical build flags that change firmware behavior, and
> personal local overrides.

## Build System: PlatformIO

**PlatformIO is BOTH a VS Code extension AND a CLI tool**:

1. **VS Code Extension** (Recommended):
   * Extension ID: `platformio.platformio-ide` (see `.vscode/extensions.json`)
   * Provides: Toolbar buttons, IntelliSense, integrated build/upload/monitor
   * Configuration: `.vscode/c_cpp_properties.json`, `.vscode/tasks.json`
   * Usage: Click Build (✓), Upload (→), or Monitor (🔌) buttons

2. **CLI Tool** (`pio` command):
   * **Installation**: Python package (typically `pip install platformio`)
   * **Windows Location**: `C:\Users\<user>\AppData\Local\Programs\Python\Python3xx\Scripts\pio.exe`
   * **Verify**: `which pio` (Git Bash) or `where.exe pio` (cmd)
   * **Usage**: `pio run`, `pio run -t upload`, etc.

**Configuration Files**:
* `platformio.ini`: Main build configuration (committed to git)
* `platformio.local.ini`: Local overrides (gitignored, create if needed)
* `partitions.csv`: ESP32 flash partition layout

The default PlatformIO core directory is project-local (`.platformio`, ignored).
This isolates both pinned framework packages and platform build-script patches:
another project's upgrade must not replace Arduino headers during this build.
Install packages through PlatformIO; do not copy a mutable/customized framework
from another checkout. An explicit `PLATFORMIO_CORE_DIR` still overrides this
default for the isolated cache-switch tests below.

## Build Environment

### ar0c fork identity

The `x4pro` development target uses `YYMMDD-HHMMSS-ar0c-<base>-x4pro`
(China-time build stamp; base version read from `platformio.ini`). Startup, About, and `/api/status`
share `CROSSPOINT_VERSION`. The startup/About firmware name is `crossmux-ar0c`,
with English fallback for other UI languages. Hardware model, chip, MAC,
statistics, partition layout, and USB descriptors are unchanged.

On a successful development image build, `scripts/git_branch.py` exports an
identical `crossmux-ar0c-*.bin` copy beside PlatformIO's internal `firmware.bin`.
X4 Pro uses `crossmux-ar0c-YYMMDD-HHMMSS-<image-sha256-prefix8>-x4pro.bin`;
other development targets include the base version, device, Git revision, and
image digest. The digest distinguishes local uncommitted builds sharing the
same Git revision; these are local development artifacts, not a tagged release.
Commit the intended source when explicitly requested for a revision-based release.
Build targets retain their existing runtime version generation;
upstream OTA endpoints are not redirected by this branding change.

* **Standard**: C++20 (`-std=c++2a`). No Exceptions, No RTTI.
* **Logging**: ALWAYS use `LOG_INF`, `LOG_DBG`, or `LOG_ERR` from `Logging.h`. Raw Serial output is deprecated.
* **Environments** (in `platformio.ini`):
  * `x4pro`: Default X4 Pro development build (LOG_LEVEL=2, serial enabled)
  * `x4pro-gh_release`: X4 Pro stable release build
  * `x4pro-gh_release_rc`, `x4pro_nightly`: X4 Pro release candidate and Nightly
  * `waveshare_epaper_397`: Waveshare ePaper 3.97 development build
  * `waveshare_epaper_397_nightly`: Waveshare Nightly build
  * `simulator`: Native desktop simulator for UI development

The two S3 environments are separate hardware binaries, each with one unified
language firmware. Routine CI and Hardware CI build only these boards.

Bluetooth Page Turner Beta is compiled into both hardware targets,
including development, Nightly, release-candidate, and stable builds.
The runtime Bluetooth switch defaults to off; native simulators use SDK stubs.
Both S3 hardware profiles inherit the PSRAM and IPC configuration and retain
their prebuilt `dio_opi` core so the TinyUSB MSC component graph remains intact.

The SDK's obsolete passkey callback is removed only from a generated source copy
under `$BUILD_DIR/ble-compat`; the SDK and NimBLE dependency sources are never
rewritten. The source is a build dependency and unexpected callback signatures
fail the build. The same translation unit includes the `_btLibraryInUse` weak
shim for both the custom-core bootstrap (which omits application sources) and
the final firmware, without suppressing NimBLE's Arduino BT usage header.

The pinned prebuilt S3 core still creates 1 KiB IPC stacks. BLE controller
interrupt allocation can overflow `ipc0`; upstream Arduino lib-builder #386
raises the budget to 2 KiB. S3 BLE builds use a narrow link adapter at task
creation to apply that minimum only to `ipc0`/`ipc1` on their matching cores.
It adds at most 2 KiB of internal stack RAM across both tasks and preserves
larger configured stacks, allocation failures, other tasks, and the prebuilt
TinyUSB core. The adapter travels with the same bootstrap-compatible source;
remove it when the pinned core supplies the upstream budget. BLE diagnostics
include both IPC stack high-water marks; check them after repeated starts.

When switching from a custom core to a prebuilt target, retain the framework's
`sdkconfig.orig` marker until PlatformIO restores the original core package.
Restoring only `sdkconfig` leaves custom IDF archives behind; mixing these with
an untouched `dio_opi` header can omit PSRAM initialization entirely.
`scripts/tests/test_pioarduino_cache.py` covers this transition.

For isolated cache-switch validation, run builds sequentially with all four
overrides below (the directories are gitignored). Do not copy compiled core
packages from an existing PlatformIO installation into this environment.

```bash
export PLATFORMIO_CORE_DIR="$PWD/.platformio/ble-psram"
export PLATFORMIO_BUILD_DIR="$PWD/.pio/ble-psram-build"
export PLATFORMIO_BUILD_CACHE_DIR="$PWD/.cache/ble-psram"
export IDF_COMPONENT_CACHE_PATH="$PWD/.cache/ble-psram-idf-components"
pio run -e x4pro_nightly
pio run -e waveshare_epaper_397_nightly
```

Use the same overrides when uploading. Check the resulting ELF for the actual
PSRAM initialization and heap-registration call paths, not just the
`BOARD_HAS_PSRAM` macro or `psramInit` symbol. Runtime BLE diagnostics must report
nonzero PSRAM capacity and a successful allocator probe before connection tests.

## Windows middleware compatibility

The pinned pioarduino 55.03.37 Windows dispatcher ignores middleware file
patterns and temporarily replaces `Object()` with a function returning `None`.
This breaks BLE generated-source compilation and per-library configuration.
`scripts/patch_windows_middleware.py` applies a hash-guarded, idempotent repair
to that build-tool dispatcher, following the existing platform cache patch
mechanism. It preserves the original callback patterns, replacement sources,
and real object nodes; unmatched sources retain platform include shortening.
Custom callbacks retain their own compilation flags. SDK sources are untouched.
Review this patch when upgrading the pinned platform; unknown source fails closed.

The i18n summary uses ASCII escapes under SCons because its parent process's
console encoding cannot be inferred from captured stdout. Standalone diagnostic
output respects stdout and the outer console encoding; translations remain UTF-8.
Run the Windows regression tests with
`python -X utf8 -m unittest discover -s scripts/tests -p test_windows_build.py`.
The UTF-8 interpreter option is also needed by older tests that read UTF-8 files
without specifying an encoding.

## Desktop Simulator

Install SDL2 and `curl` (plus OpenSSL development headers on Linux), place EPUB
files under `fs_/books/`, and run:

```bash
pio run -e simulator -t run_simulator
```

The simulator implementation and launcher come from the pinned
[`0x1abin/crosspoint-simulator`](https://github.com/0x1abin/crosspoint-simulator)
fork; the exact revision is recorded in `platformio.ini`.
The firmware repository does not carry a second host implementation. Arrow
keys are Up/Down, `P` is Power, mouse input provides touch, and `S` sleeps.

This simulator covers UI, input, RTC state,
and sleep/wake flows. It does not emulate EPD waveforms or ghosting, bus timing,
SDMMC contention, PSRAM, or power consumption.

## Critical Build Flags
These flags in `platformio.ini` fundamentally affect firmware behavior:

```cpp
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1  // Single framebuffer (saves 48KB RAM!)
-DARDUINO_USB_MODE=1                 // Enable USB CDC
-DARDUINO_USB_CDC_ON_BOOT=1          // Serial available immediately at boot
-DXML_CONTEXT_BYTES=1024             // XML parser memory limit (EPUB parsing)
-DUSE_UTF8_LONG_NAMES=1              // SD card long filename support
-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1   // Avoid zlib name conflicts
-DXML_GE=0                           // Disable XML general entities (security)
-DDESTRUCTOR_CLOSES_FILE=1           // FsFile destructor auto-closes (SdFat)
```

**DESTRUCTOR_CLOSES_FILE implications**:
- SdFat's `FsBaseFile` destructor calls `close()` automatically when the object goes out of scope
- **Do NOT add explicit `file.close()` calls** for local `FsFile` variables — the destructor handles it
- Explicit `close()` is still required in these cases:
  1. **Close before delete**: Must close before `Storage.remove()` on the same path
  2. **Close before reopen**: Must close before reopening the same `FsFile` variable (e.g., write then reopen for read, or rewrite the same path)
  3. **Member variables**: `FsFile` members persist beyond any single function scope, so close at the intended release point (e.g., in `onExit()`)

**SINGLE_BUFFER_MODE implications**:
- Only ONE framebuffer exists (not double-buffered)
- Grayscale rendering requires temporary buffer allocation (`renderer.storeBwBuffer()`)
- Must call `renderer.restoreBwBuffer()` to free temporary buffers
- See [lib/GfxRenderer/GfxRenderer.cpp:439-440](../../lib/GfxRenderer/GfxRenderer.cpp) for malloc usage

**Supported display implications**:
- The application uses the SDK board profiles for display initialization and
  waveform selection.
- X4 Pro probes its controller before display initialization; a unit may have
  SSD1677, UC8179, or UC8279 hardware.
- Waveshare 3.97 uses its own SSD1677 board profile and timing.

---

## Local Development Configuration

### platformio.local.ini (Personal Overrides)

**Purpose**: Personal development settings that should NEVER be committed.

**Use Cases**:
- Serial port configuration (varies by machine)
- Debug flags for specific testing
- Local build optimizations
- Developer-specific paths

**Example** `platformio.local.ini`:
```ini
# platformio.local.ini (gitignored)
[env:x4pro]
upload_port = COM7              # Windows: COMx, Linux: /dev/ttyUSBx
monitor_port = COM7

build_flags =
  ${x4pro_hardware.build_flags}
  -DMY_DEBUG_FLAG=1             # Personal debug flags
  -DTEST_FEATURE_ENABLED=1
```

**Configuration Hierarchy**:
1. `platformio.ini` - **Committed**, shared project settings
2. `platformio.local.ini` - **Gitignored**, personal overrides
3. Local file extends/overrides base config

**Rules**:
- **NEVER commit** `platformio.local.ini`
- **NEVER put** personal info (serial ports, credentials) in main `platformio.ini`
- Use `${base.build_flags}` to extend (not replace) base flags

See also: [getting-started](../contributing/getting-started.md) for first-time toolchain setup, [testing-and-debugging.md](testing-and-debugging.md) for build/monitor commands.
