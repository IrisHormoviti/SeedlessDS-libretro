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

### RG DS / RG DS Plus performance

New AArch64 builds default to `-mcpu=cortex-a55`, `-O2`, and link-time
optimization when CMake detects compiler-aware C and C++ archivers and confirms
IPO support. This matches BananaDS's CPU targeting and LTO setup for these
handhelds. Pass `-DSEEDLESS_ARM_CPU=` or `-DSEEDLESS_ENABLE_IPO=OFF` to disable
the corresponding target optimization; pass `-DSEEDLESS_ENABLE_IPO=ON` to
require LTO. If a compiler accepts only tuning, SeedlessDS falls back to
`-mtune=cortex-a55`; incomplete cross-toolchain wrappers stay on the regular
build path when LTO is selected automatically. Toolchains need an LTO-capable
linker and archive tools, such as `gcc-ar` and `gcc-ranlib` with GCC.

The libretro frontend converts each DS screen directly into its half of the
stacked output frame. This removes a 384 KiB copy per delivered frame, following
BananaDS's approach of writing directly to the presentation buffer.
[BananaDS KMS design](https://github.com/JorreFog/SuperDrastic/blob/e54aaa8e2693352ad9f33a0d3d75ea4b887f1957/bananads/docs/BANANALINUX.md)

The AArch64 JIT emitter uses host `CLZ` and `RBIT` instructions when it encodes
ARM logical immediates. This replaces the portable bit-scanning and bit-reversal
sequences and reduces work during block compilation. The guest-code output is
unchanged.

JIT block analysis reads instructions through the page table directly when the
code page has a direct mapping, retaining the bus helper for handled or unmapped
pages. This avoids a helper call for each decoded instruction on the common
ROM/RAM path without changing guest memory mapping behavior.

The ARM64 analyzer keeps records for common blocks in a small stack buffer,
spilling only larger blocks to the heap. It also avoids shrinking this
temporary buffer after every block compilation.

Slow ARM LDMs batch reads from directly mapped memory after refreshing the
page-table descriptors. ARM9 STM bursts into the GX FIFO reuse the existing
batched command decoder when the command ring is safely below its flush
boundary, avoiding a slow I/O dispatch for each word.

With the Linux GLES build, use the ROM replay tool to collect startup and
scheduler throughput from the same fresh process. Use the same BIOS, saves and
benchmark interval for both builds.

```sh
cmake --build build-device --target seedless_rom_replay
build-device/libretro/compute/seedless_rom_replay \
  --bench "$rom" "$system_dir" software 120 1200
```

Configure the build with -DSEEDLESS_JIT_PROFILE=ON to also report cold-start
block analysis count and time, plus blocks analyzed during warmup. The option
adds timing only around JIT compilation misses and is off in normal builds.
It also reports primary target-cache misses and secondary-way hits during the
measured interval, showing what share of misses the second way rescues.
For the compute renderer, configure with -DSEEDLESS_COMPUTE_PROFILE=ON to report
average CPU time per draw for uploads, dispatch submission, buffer-map wait, and
copy/unmap after the warmup interval. When the driver exposes
GL_EXT_disjoint_timer_query, it also reports elapsed GPU time for the raster and
edge compute passes; disjoint timer samples are excluded. The map wait includes
GPU completion and driver readback; dispatch submission time is CPU-side and is
not a GPU timer.

The first video callback includes the replay tool's scheduler wait. Throughput
is measured after the 120-frame warmup and excludes frontend pacing and KMS.
Use `compute` in place of `software` to measure the GLES path with the same ROM.
The persistent JIT warm-start manifest keeps up to 256 validated guest PCs.
Once per emulated frame it samples the ARM9 and ARM7 PCs, then prioritizes
repeatedly sampled blocks for precompilation on the next normal JIT-enabled
boot. During savestate restore, manifest entries are revalidated and warmed
after the saved memory has been restored; direct-ROM boot seeds are skipped.
On a direct header boot, Seedless also seeds any ARM9 and ARM7 executable
entry points that the manifest did not already warm. The JIT's existing
direct-branch linking discovers reachable blocks; modified code and overlays
still use normal runtime compilation and invalidation.

In addition to the warm-start manifest, the AArch64 JIT now writes a persistent
native snapshot beside it as a `.native` file. It contains generated code and
lookup/invalidation state from blocks observed in prior play sessions. On a
normal ROM boot, Seedless restores it only when the compiler/emitter ABI, bus
mapping, ROM identity, and full guest-instruction signatures all match; any
failure leaves the standard JIT path active. It also validates guest data
ranges folded into native code, includes directly compiled branch targets, and
rebuilds page write watches on restore. Savestate loads continue to use
their own memory-aware warm path. The core does not scan or compile the whole
ROM before first play, so cold launch remains lazy. With `SEEDLESS_JIT_PROFILE`
enabled, ROM replay reports how many native blocks were restored.

BananaLinux builds BananaDS at `e54aaa8e2693352ad9f33a0d3d75ea4b887f1957`
with a patched melonDS core. Build that source as a separate baseline so its JIT,
Banana 2D renderer, Gengis, and KMS frontend can run together on the handheld.
Keep the stock BananaDS build flags when reproducing upstream numbers. Compare
identical ROMs, saves, input, resolution, and clock settings. Measure emulator
throughput and presentation latency separately because RetroArch and BananaDS's
KMS frontend have different presentation paths.
[BananaLinux build recipe](https://github.com/JorreFog/BananaLinux/blob/main/buildroot/package/bananads/bananads.mk)
[BananaDS performance results](https://github.com/JorreFog/BananaLinux/blob/main/docs/PERFORMANCE-0.5.0.md)

Seedless already links JIT blocks and rejects primitives with a common clip-plane
outcode. Two candidates to evaluate against that baseline are BananaDS's cheaper
indirect JIT exits (`0342`–`0345`) and its reuse of unchanged 2D output. Use a
profile to pick between them. Changes to JIT exits need tests for CPU timing,
interrupts, and code invalidation. Changes to scanline reuse need tests for
mid-frame writes, display capture, and savestate restore.
[BananaDS patch series](https://github.com/JorreFog/SuperDrastic/tree/e54aaa8e2693352ad9f33a0d3d75ea4b887f1957/bananads/patches/melonds)

For Mali, measure the software and GLES paths at matching settings and separate
GPU dispatch time from readback stalls. If the readback dominates, evaluate
melonDS's GPU-resident 2D composition and capture in a standalone GPL build
before choosing a Seedless integration. For faster startup, first measure ROM
loading and JIT compilation separately. Use a recorded startup block set to
evaluate prewarming while retaining JIT fallback for overlays and modified code.
[melonDS GPU compositor](https://github.com/melonDS-emu/melonDS/blob/906e9ebb27da8c6a715cd7abab4abfe8a8d29427/src/GPU_OpenGL.cpp)

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
- **3D Raster Thread Count** = `8` (the software renderer clamps this to the
  worker pool size; four participants fit the RG DS Plus's four A55 cores)
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
