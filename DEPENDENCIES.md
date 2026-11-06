# Dependency baseline and cache ownership

The authority is `platformio.ini`, `requirements.txt`, the FreeInk submodule and
the project-owned patch scripts. This organization pass does not upgrade an SDK,
library, compiler or firmware. See [the build guide](docs/engineering/build-system.md).

| Build | Existing baseline |
|---|---|
| Waveshare reader | pioarduino platform 55.03.311; Arduino-ESP32 3.3.11; IDF libs `5.5.5+sha.b774170ff46` |
| StickS3 remote | platform 55.03.37 / Arduino 3.3.7; M5Unified 0.2.17 and M5GFX 0.2.32; independent config, libraries, partitions and build |
| CLI and host tools | root requirements pins pioarduino 6.2.0; remote requirements separately pins pyserial 3.5 and esptool 5.3.0 |
| Reader libraries | preserve ArduinoJson 7.4.2, SdFat 2.3.1, PNGdec 1.1.6, WebSockets 2.7.3, Arduino-wolfSSL 5.7.2, NimBLE-Arduino 2.3.8 and the existing Git pins |

The reader's installed package metadata observed on 2026-11-06 reports Xtensa
GCC `14.2.0+20260121` and esptool 5.3.0. This describes that installation, not
the compiler identity of every old ELF or the remote's older framework.
The existing FreeInk dirty state is preserved; the organization pass does not
update or commit the submodule.

## Shared downloads, isolated mutable installations

The reader and remote now select `cache_dir=~/.cache/pioarduino-6.2.0` for the
same CLI family. PlatformIO expands `~` for the executing user; no host path,
global environment setting or symlink is needed. `PLATFORMIO_CACHE_DIR`, when
explicitly set by a caller, still takes precedence and must be checked.

Only registry/download cache data is shared. Each project keeps its existing
`core_dir=.platformio`, installed platforms/frameworks/tools, `.pio/libdeps`,
build directory and derived-object cache. In particular the root reader retains
`build_cache_dir=.cache`; none of these existing directories is moved or pruned.
Future fetches can reuse the shared cache; old caches are retained, so this is
not a claim of disk space reclaimed.

`scripts/patch_pioarduino_cache.py`, Windows middleware, ROM libc, wolfSSL,
SdFat and BLE patches are project-owned build inputs. Two matching version
labels do not prove matching patched SDKs. Never use the download cache as an
installed SDK, copy another checkout's custom framework, or share an application's
build directory. Cache-switch tests retain their existing explicit overrides.

Validation for this change compares effective CLI configuration and directory
resolution: only the download-cache option may change; board, SDK, libraries,
flags, partitions and build/install directories must remain the same. No package
installation, compiler invocation, firmware publication or device operation is
part of this pass.
