# SeedlessDS-libretro

A libretro Nintendo DS core built on [SeedlessDS-Core](https://github.com/SapphireRhodonite/SeedlessDS-Core).

SeedlessDS-Core is a DraStic reconstruction. The CPU backend is an **AArch64 JIT**, so this core only runs on aarch64 hosts or cross-compiled aarch64 targets (e.g. Android arm64, Linux aarch64).

This specific fork is adapted for running on [ClamOS](https://github.com/IrisHormoviti/ClamOS)

---

## Features

- Dual screens stacked vertically (256×384, XRGB8888)
- 44100 Hz stereo (OpenSL stub → ring buffer → libretro)
- Standard NDS buttons + touch (Pointer / Mouse on the lower screen)
- Virtual stylus: right stick (left stick fallback) moves the cursor, L3 presses the lower screen; crosshair overlay
- BIOS / Firmware under the system directory (same filenames as SeedlessDS)
- Core options map DraStic / Seedless `config_bits` (frameskip, hires 3D, audio, saves, Slot-2, etc.)

---

## Build

```bash
git submodule update --init --recursive

# Native aarch64 host
make

# Debian/Ubuntu cross packages
make CROSS=aarch64-linux-gnu-

# Custom toolchain (match the actual binary prefix)
make CROSS_PREFIX=/path/to/toolchain/bin/aarch64-buildroot-linux-gnu-
```

Output：`build/seedlessds_libretro.so`

Requires CMake ≥ 3.18 and pthread. If the target lacks zlib, zlib 1.3.1 is fetched and linked statically. The first configure also downloads UnRAR / LZMA SDK.

Android NDK example：

```bash
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

An x86_64 build is possible with `SEEDLESS_ALLOW_NON_AARCH64` (JIT stubs), but it **cannot actually run games**.

---

## Usage

1. Place `seedlessds_libretro.so` and `seedlessds_libretro.info` in RetroArch’s cores / info directories.

2. Put BIOS / Firmware in the RetroArch **System Directory** (the core copies them to `$system/User/system/`):
   - `nds_bios_arm9.bin` / `nds_bios_arm7.bin`（`*_replacement.bin`）
   - optional：`nds_firmware.bin` / `nds_firmware_modified.bin`

3. Load a `.nds` / `.dsi` / `.ids` / `.srl` ROM.

The core uses `SEEDLESS_ROOT` (defaults to the System Directory) to find BIOS and data under `User/system/`.

---

## Core options
Configure under RetroArch Quick Menu → Core Options. Options map to Seedless / DraStic `config_bits` by category; some require a **ROM reload** (marked Restart).

| Category | Main options |
|---------------|------------------------|
| Video | Frameskip type/value、Safe frameskip、Hires 3D、Threaded 3D、Edge marking、Keep main on top、16-bit colour、Show FPS |
| Audio | Sound、Latency、Volume、Host mic / audio path |
| System | Cheats、Savestate extra、Raw `.sav`、RTC、Ignore gamecard limit、Auto-trim、Preload ROMs、Slot-2 |
| Advanced | Compose thread count、Button profile、Lua |

Defaults：

- **Frameskip Type** = `None`
- **Compose Thread Count** = `8`
- **Sound** = enabled；**Audio Latency** = low

If the menu still shows old defaults, use “Reset to defaults” in core options.

---

## Architecture

| Component | Description |
|----------------|------------------|
| `core/` | SeedlessDS-Core vendored clone |
| `libretro/libretro.c` | libretro API; emu on a worker thread, `retro_run` syncs video/audio |
| `libretro/libretro_core_options.h` | Core option definitions (v2) |
| `libretro/opensl_stub.c` | OpenSL ES stub → ring buffer |
| `libretro/gl_stubs.c` | GLES stubs for software path |
| `libretro/cpu_stub.c` | JIT/LZMA stubs for non-aarch64 builds |
| `core/.../files_posix.c` | Non-JNI file I/O (`SEEDLESS_ROOT`) |
| `core/.../mirror.c` | Memory maps; prefer memfd, ashmem fallback |

---

## Attribution
Full attribution to [lualiliu](https://github.com/lualiliu) for adapting the seedlessDS core into libretro.

DraStic is owned by its original authors (including Exophase). This project is a libretro frontend for the reconstructed library, not official DraStic. Game ROMs, original BIOS / Firmware, and DraStic binaries are not included. See `core/third_party/LICENSES.md` for dependency licenses.
