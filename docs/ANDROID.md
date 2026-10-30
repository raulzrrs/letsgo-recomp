# Android

Let's Go Recomp runs on Android phones and tablets (arm64-v8a, Android 10 / API 29 or later, Vulkan 1.3). The runtime
is the same C code as on PC, cross-compiled with the Android NDK; the game code is generated from your own game files,
as on PC. Tested on a Xiaomi 15 (Snapdragon 8 Elite, Adreno 830, Android 16) with an 8BitDo controller.

Status: the game boots from the start screen to the world, with sound, text and a controller, in fullscreen landscape
(either way round). The world runs at the game's 30 fps with room to spare (~70 fps unlimited at 720p). There are no
touch controls yet: a controller (Bluetooth or USB) is needed.

## What you need

On the Windows PC that builds it (the scripts are bash: Git Bash or MSYS2, with MSYS2's `make` on the PATH):

- Android NDK r29, Android SDK build-tools 36.1.0 and platform android-36, a JDK 21. The scripts look for them in
  `%USERPROFILE%\Android` (`ndk\29`, `build-tools\36.1.0`, `platforms\android-36`, `jdk\*`, `platform-tools\adb.exe`);
  `ANDROID_NDK` / `ANDROID_SDK` / `JAVA_HOME` point elsewhere. No Android Studio, no Gradle.
- MSYS2 (`C:\msys64`) for `make` and the usual tools, as for the Windows build (docs/COMPILAR-WINDOWS.md).
- Your game files extracted as for the PC build (`extracted/exefs`, `extracted/romfs.bin`).

## Building

```bash
export PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH
export ANDROID_NDK=/c/Users/<you>/Android/ndk/29 ANDROID_SDK=/c/Users/<you>/Android

# 1. the libraries the runtime links against (once): FFmpeg, SDL3 (and its Java side), shaderc, VMA -> build/deps-android
make deps-android     # (tools/build_deps_android.sh)
# (optional) FSR 3.1: AMD's FidelityFX SDK built from its sources -> build/deps-android/lib/libffx_fsr3.a, linked in
# when present (without it FSR 3.1 falls back to FSR 1)
make ffx-android      # (tools/build_ffx_android.sh)

# 2. the game code for Android (once per game / generator change): generated-android/, then build/android/liblasgame.so
make TARGET=android gen
make -j16 TARGET=android build/android/liblasgame.so

# 3. the runtime: build/android/libmain.so
make -j16 TARGET=android build/android/libmain.so

# 4. the APK: build/android/LetsGoRecomp.apk (signed with a debug key made on first use, build/android/debug.keystore)
bash tools/build_apk.sh
```

Notes:

- The dependencies script needs `TMPDIR` as a POSIX path (FFmpeg's configure; `make deps-android` sets it). Run
  `tools/build_apk.sh` without
  `MSYS_NO_PATHCONV` set (its `cmd //c` calls need Git Bash's argument conversion).
- The game code is generated for Android's guest address window (`GUEST_LO = 0x10_0000_0000`, see below): a PC
  `lasgame.dll` / `generated/` cannot be reused. `GEN=` and `B=` choose other folders, e.g. one per game
  (`GEN=generated-android-pika B=build/android-pika`, then copy its `liblasgame.so` to `build/android/`).
- The libraries in the APK are stripped; `build/android/*.so` keep their debug info, for crash backtraces
  (`llvm-addr2line -f -i -C -e build/android/libmain.so <offset>`).

## Installing and the game files

```bash
adb install -r build/android/LetsGoRecomp.apk
```

On Xiaomi / HyperOS phones, "Install via USB" must be on in the developer options (and the confirmation accepted on
the phone).

The app's data root is its external files folder, `/sdcard/Android/data/dev.letsgorecomp/files`:

| Path | What |
|---|---|
| `extracted/exefs`, `extracted/romfs.bin` | the game files (as on PC) |
| `save/save_0/` | Eevee's save (`savedata.bin`) |
| `save-pikachu/save_0/` | Pikachu's save |
| `larecomp.ini` | the settings (start screen and F12 menu) |
| `cache/` | the SPIR-V and pipeline caches |
| `lasrecomp.log`, `lasrecomp-prev.log` | this run's log and the previous one's |

Copy them with `adb push`, a file manager or a USB cable. Folders made by `adb push` belong to the shell user and the
app can't enter them; open them up afterwards:

```bash
adb push extracted /sdcard/Android/data/dev.letsgorecomp/files/
adb shell chmod -R a+rwX /sdcard/Android/data/dev.letsgorecomp/files/extracted
```

Files copied in with adb can be read by the app but not always written: the runtime replaces its settings file
(written to `larecomp.ini.new`, then renamed) instead of writing over it, so settings stick either way.

## Running and testing from the PC

The launcher icon opens the start screen. From a PC, `LetsGoActivity` takes the runtime's command line and its
environment as intent extras (`args`, and `env` as `NAME=value` pairs separated by `;`). Quote the extras inside the
device's shell, or `;` splits the command:

```bash
adb shell "am start -n dev.letsgorecomp/.LetsGoActivity --es args --play --es env 'LARECOMP_FPS=0;LARECOMP_VK_PROF=1'"
adb shell cat /sdcard/Android/data/dev.letsgorecomp/files/lasrecomp.log
adb exec-out screencap -p > shot.png
```

- `--play` goes straight into the game (no start screen).
- Some phones (Xiaomi) block `adb shell input`: scripted runs use `LARECOMP_INPUT` (docs/DEPURACAO.md), which also
  ignores the real controller. The battery's world route reaches the world from a save in Pewter / Viridian.
- The phone must be unlocked with its screen on when the app starts (a dozing phone gives no window surface).
- `simpleperf` (from the NDK, pushed to `/data/local/tmp`) profiles the app: `simpleperf record --app dev.letsgorecomp`.
  Per-thread CPU time: `/proc/<pid>/task/*/stat` (fields 14 and 15).
- GPU: `LARECOMP_GPUPROF_AT=<frame>` (time per pass), `LARECOMP_PIPE_STATS=all` (the Adreno compiler's registers,
  occupancy and instruction counts per shader), `LARECOMP_VK_PROF=1` (render-thread time and GPU waits by site).

## What is different on Android

### Address space

Android's app processes inherit zygote's native heap where the PC keeps the guest window (`0x70_0000_0000`), so the
window moves to `0x10_0000_0000` (`GUEST_LO`, runtime/rt.h; the game code is generated for it, `make TARGET=android`).
Phones have a 39-bit user address space: the window (12.5 GB) fits below it. The manifest turns off native heap pointer
tagging (`allowNativeHeapPointerTagging="false"`): host memory handed to the game must be plain addresses.

### The game's memory pools in GPU memory

On PC the guest heap is imported into Vulkan (`VK_EXT_external_memory_host`), so shaders that read guest memory directly
(global memory, `LDG` / `STG` - the in-game text, for one) and in-place vertex reads see it. Adreno has no such import.
Instead, every NVN memory pool the game makes in its heap - what the Switch's GPU can see, nothing else - becomes GPU
memory itself (runtime/nvn/vulkan/exec.c, `backend_pool_created`): an exportable allocation (`VK_KHR_external_memory_fd`)
gets the pool's contents and is mapped over the pool's pages from its fd, so the game reads and writes it as before;
when the game finalizes the pool its pages go back to plain memory. Shaders reach global memory through a table of
device addresses, one per 4 KB page of the heap (`bufferDeviceAddress`, `GL_EXT_buffer_reference`;
`maxwell_gmem_chunk_shift` in the shader translator; pages outside pools read zero).

Why not the whole heap: the driver commits an allocation's memory at once (the 3.2 GB heap would cost 2.2 GB more RAM),
and Adreno's exportable memory is uncached for the CPU. For the pools that costs nothing measurable (the game expects
most pools to be CPU-uncached on the Switch too): +0.5 GB of RAM over not sharing, no CPU time lost.

Vertex and index data is copied to a GPU ring (`vertex_read = copy`, the default on Android): read in place from the
pools' memory, the GPU's vertex fetch was slow and every buffer copy of the game had to wait for the GPU.

### Vulkan

- Features are requested only when the device has them (Adreno: no 8-bit uniform buffers; custom border colours and
  device-fault reports checked as features, not only as extensions).
- The swapchain is not pre-rotated: the window is landscape and the compositor turns the images
  (`preTransform = IDENTITY`). The driver answers every present with `VK_SUBOPTIMAL_KHR` for that; it is ignored on
  Android (rebuilding the swapchain each frame serialised the game, the render thread and the GPU: 19 fps instead of 70).
- Going to the background (screen locked, another app) takes the window's surface away: nothing is presented
  meanwhile, and on return the surface and swapchain are made again (device.c `lifecycle_watch` / `surface_again`).
- Shuffles (`SHFL`) run in 32-lane slices, the Switch's warp size (Adreno's subgroups are wider, and only in fragment
  and compute shaders).
- `GL_EXT_texture_shadow_lod` is enabled, not required (the NDK's shaderc is older).

### Settings and look

- Fullscreen always, 720p rendering by default (`render_height = 720`): the phone's GPU cost goes with the pixels
  (main scene pass ~4.4 ms + ~7.9 ms per megapixel on the Adreno 830).
- The start screen is drawn at 2x; there are no window buttons.
- The game's own glyphs (the PC's higher-resolution font cache, runtime/hle/font_hires.c, is off).
- The SDL main thread polls events with a short sleep (SDL's wait polls without sleeping on Android: a core at 100%).

## Performance (Xiaomi 15, world, 720p)

| | before | now |
|---|---|---|
| world, unlimited | ~19 fps | ~70 fps |
| GPU per frame | ~21 ms | ~10 ms (main pass 5.5, shadow map 1.5) |
| normal play (30 fps) | | steady 30, frames up to ~40 ms |

Measured and ruled out on the tiler: loading attachments at each pass, storing depth, the barrier between passes (~0
each). Shaders with branches (all forward in this game) are straight GLSL instead of a `switch (pc)` state machine
(translate.c `straight_ok`; Android only by default, `LARECOMP_VK_STRAIGHT=1` on PC): the world's materials went from ~27 to ~17 full-precision registers (Adreno occupancy 9% ->
15%), the main pass ~10% faster with the GPU saturated (2160p A/B; at 720p the GPU is not the limit and its clock
follows the load, so timestamps there can't show it).

Our own post-processing was ~23% of a 1080p frame: debanding is off on Android (~12% of the frame; `LARECOMP_DEBAND=1`
turns it on), and the colour grade is applied in the present's own draw when it is the last step (one full-size pass
less). With SMAA the post is now ~0.38 of a main pass (was 0.85); without it, nothing. (Snapdragon GSR 1, one pass in
the present's draw at ~0.3 ms a frame at the phone's 2133x1200, was the Graphics tab's phone upscaler until it was
removed with GSR 2: DittoSR took their place.) Next levers: half precision in the materials (their ~44 texture reads keep occupancy at 15%);
the game's 60 fps mode; clear-only passes as the next pass's load (~5%).

## Temporal upscalers on a phone

Both take the jittered scene at a render size from their mode (Native AA 1.0x to Ultra Performance 3.0x), its depth,
the motion vectors and the interface mask (exec.c post_process_ex). Measured on the Adreno 830, the display's 16:9 box
2133x1200, Performance (1067x600), the frame's GPU time from first to last timestamp:

| | frame | the upscaler |
|---|---|---|
| SMAA, 1067x600, bilinear | 18.1 ms | - |
| SMAA, 1280x720, bilinear | 20.9 ms | - |
| FSR 3.1 Performance | 22.3 ms | ~4 - 5 ms |
| Snapdragon GSR 2 Performance (since replaced by DittoSR) | 21.6 ms | 3.4 ms |

- **FSR 3.1**: the FidelityFX SDK 1.1.4 (MIT) built for arm64 by `make ffx-android` (tools/build_ffx_android.sh: a
  sparse clone of the pinned release, tools/ffx_android/patch_sdk.py for what it takes from MSVC and Windows, the passes'
  SPIR-V made with the SDK's own FidelityFX_SC and glslang). The backend picks its 16-bit variants (the device enables
  `shaderFloat16`); the Adreno reports 64-wide waves only, so no wave64 variants. On a tiler its compute passes'
  timestamps don't split the time between them (the last pass's row takes ~9 ms of work done before it), so the cost
  above is the frame against SMAA at the same render size.
- **Snapdragon GSR 2** (Qualcomm, BSD 3-Clause), as it was measured here: its 2-pass fragment variant, the upscale in
  half precision as Qualcomm's - in full precision it took 2.5 ms instead of 1.45 - with a 10-bit history, then our RCAS
  and the interface composite in one pass (two passes: 1 ms each). It was removed from the Upscaler setting; **DittoSR**
  (`upscaler = dittosr`, older files' `gsr2` load as it) keeps its convert pass (shaders.c gsr2_convert.frag) and the
  10-bit history (exec.c gsr2_run) with an upscale and a line pass of our own (x2_upscale.frag, x2_final.frag).
- With frame generation (re-rendering, 2x) both hold 60 images a second, the game at 30: the in-between images are
  resolved against frame N's upscaled output (side_resolve.frag), the upscaler's history goes on with the real frames.

## Frame generation on a phone

- The display: Android keeps an app at 60 Hz unless it asks, so 3x / 4x at 30 fps (90 / 120 images a second) were drawn
  and dropped. With frame generation on, the runtime asks for the images' rate (`ANativeWindow_setFrameRate`, device.c
  `android_frame_rate`: the world's fps x the images per frame; Auto: the display's highest), and Auto knows the
  display's refresh (`vk_display_hz`). The system still has the last word: warm, Xiaomi's thermal policy caps the
  display at 60 Hz (`dumpsys display`: `PRIORITY_THERMAL_LIMIT_REFRESH_RATE`), whatever an app asks.
- GPU use on the overlay barely moves between 2x, 3x and 4x: it is the GPU's busy time, and the GPU clock follows the
  load (more work, a higher clock).
- Re-rendering is bound by the render thread on a phone: at 4x it uses one core whole (the frame's draws again for
  each image: ~47% of its time in the replays, the driver's `vkCmdBeginRendering` allocating for every pass) and the
  game can fall below 30. Optical flow does no CPU replay (4x: 120 images a second, the game at 30).
- Optical flow's block matching reads each block's frame-N samples once for all its candidates (flow_match.frag /
  flow_refine.frag `prep`): 3.7 + 1.7 ms -> 0.25 + 0.14 ms a frame on the Adreno 830 (the same result).
- `LARECOMP_VK_PROF` puts timers around every draw: with frame generation on a phone it costs enough to make
  re-rendering look slower than it is (4x: 22 fps with it, 27 - 30 without).

## Known gaps

- No touch controls; a controller is needed.
- The APK is signed with a debug key and is debuggable.
