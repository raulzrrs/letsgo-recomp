# Building and running on Linux (native)

The Linux build runs the game as a native ELF binary (no Wine, no Proton). It was built and played on Fedora 45,
x86-64, AMD RADV (RDNA2 iGPU), Vulkan 1.4. The Windows build is unaffected by anything here.

## What you need

| | |
|---|---|
| GPU | Vulkan 1.3 |
| CPU | x86-64 with SSE4.2; AVX2 recommended (the game code is built for x86-64-v3) |
| Disk | about 5 GB for the game library, plus about 5 GB for the RomFS image built on the first setup |
| Runtime libraries | SDL3, FFmpeg 8 (`libavformat`, `libavcodec`, `libavutil`, `libswscale`), `libvulkan`, shaderc (`libshaderc` or `libshaderc_shared`) |

Fedora 45:

```bash
sudo dnf install clang lld make git python3 SDL3-devel ffmpeg-free-devel \
    vulkan-loader-devel vulkan-headers libshaderc-devel dejavu-sans-fonts

# Fedora has no VulkanMemoryAllocator package: put its header where the build finds it
sudo mkdir -p /usr/include/vma
sudo curl -L -o /usr/include/vma/vk_mem_alloc.h \
    https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.5.0/include/vk_mem_alloc.h
```

Debian / Ubuntu: the same packages (`libsdl3-dev`, `libavformat-dev`, ..., `libvulkan-memory-allocator-dev`,
`fonts-dejavu-core`). `vulkan-memory-allocator-dev` provides `<vk_mem_alloc.h>` directly.

The compiler and linker flags come from pkg-config (`sdl3`, `libavformat`, `libavcodec`, `libavutil`, `libswscale`,
`vulkan`, `shaderc`), so a distribution that keeps FFmpeg's headers in a subdirectory (Fedora:
`/usr/include/ffmpeg`) needs no extra flags.

## Build

```bash
python3 -m venv .venv
.venv/bin/pip install unicorn capstone lz4    # the generator (tools/gen.py)
make gen                                      # extracted/exefs  ->  generated/
make -j$(nproc)                               # build/lasrecomp and build/lasgame.so
```

`make gen` translates your own game files (`extracted/exefs`) into C. It takes about a minute per module. The Windows
release ships the same generator frozen as `lasgen`; on Linux it runs from `.venv`.

The game code is built for this machine's CPU level: x86-64-v3 (AVX2) where `/proc/cpuinfo` has it, x86-64-v2
otherwise (`make GENARCH=-march=x86-64-v3` forces one). `make CC=... CXX=...` and `LARECOMP_CC` pick another compiler.

`make -j` then builds two things: `build/lasrecomp` (the runtime: window, input, Vulkan renderer, system services)
and `build/lasgame.so` (the game code, ~900 C files, about 35 minutes on 16 threads). Only the first build is that
slow; changing runtime files rebuilds just those.

## Run

```bash
./build/lasrecomp                      # the start screen: pick the game folder there
./build/lasrecomp extracted --play     # straight into the game
```

The game folder is the one holding `exefs/` and `romfs/`. The first setup copies (hard-links, where the file system
allows) the game files into the data root and writes `romfs.bin` from `romfs/`: about 4.5 GB, a few minutes. Saves,
settings (`larecomp.ini`), shader caches and mods stay in the data root.

To run it without the source tree, copy these three files into one folder and start the binary there:

```bash
cp build/lasrecomp build/lasgame.so build/lasgame.so.id ~/letsgo/
~/letsgo/lasrecomp ~/lets_go_eevee --play
```

`lasgame.so.id` is what tells the runtime that the library was built from the same generator and flags; without it
(or with another id) the start screen wants to recompile the game code. That recompile works from a source checkout:
it runs `.venv/bin/python tools/gen.py` and `/usr/bin/clang` (or `LARECOMP_CC`, or the release's Zig on Windows).

## AppImage

```bash
make appimage        # tools/make_appimage.sh: build/LetsGoRecomp-<version>-x86_64.AppImage
```

It carries the runtime, `lasgame.so`, the start screen's font and the SDL3 / FFmpeg / shaderc libraries it links. The
Vulkan loader and driver stay the system's (as with the Windows build's `vulkan-1.dll`), and so does the shared font.
The AppImage's own folder is read-only, so saves, settings, mods and the game files go to
`$XDG_DATA_HOME/lasrecomp`. Portability is the libc of the machine that builds it: one made on Fedora 45 needs
Fedora 45's glibc or newer.

## Steam

`make steam` writes `~/.local/share/applications/letsgo-recomp.desktop` with `--play` in it, and prints the two clicks
left in Steam (*Games → Add a Non-Steam Game… → Browse* to the binary). The start screen is then only for setup, and
Steam's Play button starts the game. `tools/steam_shortcut.sh --remove` removes the entry file.

`--play` needs the game folder the start screen set up in the data root (`extracted/`), which is why the entry carries
no path: it cannot go stale. An AppImage can be added to Steam the same way; give it the launch option `--play`.

## Linux notes

- **FSR 3.1 upscaling is Windows only** (AMD ships that Vulkan backend as a DLL, `amd_fidelityfx_vk.dll`). Where the
  DLL is not there, the anti-aliasing choice reads **FSR 1 (upscale)** instead of **FSR 3.1** and does what it says; the
  FSR 3.1 presets and sharpening are not offered then. SMAA, TAA, SSAO, GI, FSR 1 and the in-house 2x / 3x / 4x frame
  generation all work.
- **Mods** installed from a downloaded archive (the start screen's Mods page) are unpacked with libarchive's `tar`.
  Windows has it as `tar.exe`; on Linux install `bsdtar` (Fedora: `bsdtar`, Debian / Ubuntu: `libarchive-tools`). A
  plain `tar` is used when `bsdtar` is missing, which reads `.tar` / `.tgz` but not `.zip` / `.rar` / `.7z`.
  `LARECOMP_TAR=<program>` picks another.
- **Frame generation** paces without the display's vertical-blank grid (that timing is Windows' compositor's): each
  image is shown when it is due, which on a 240 Hz panel can space two of them one refresh instead of two apart.
- **Fonts:** the game's font engine needs a TrueType font as a stand-in for the console's system fonts. In order:
  `LARECOMP_FONT=<file>`, a dump in `fonts/` in the data root, then common system paths (DejaVu Sans, Noto Sans,
  Liberation Sans, Hiragino, Yu Gothic). Without any of those the game stops at boot with
  `FATAL: no usable system font substitute`. Do not point it at Noto Sans CJK: those are CFF outlines and the game's
  font engine crashes on them.
- **Audio, controllers, keyboard and mouse** go through SDL3, so PipeWire / PulseAudio and evdev / udev work as for
  any other SDL3 game. `LARECOMP_NO_AUDIO=1` silences it.
- **Software Vulkan (llvmpipe) cannot present to an Xvfb display**: the run stops with `FATAL: no graphics+present
  queue`. Use a real display, or a real GPU with Xvfb.
- **Screenshots for bug reports**: `LARECOMP_LAUNCHER_SHOT=<frame>` writes `build/launcher.png` and exits;
  `LARECOMP_WINDOW_SHOT=<frame> LARECOMP_EXIT_AFTER_SHOT=1` writes `build/window.png`. Scripted input:
  `LARECOMP_INPUT="<frame>:<BUTTON>:<frames>,..."` with `LARECOMP_NO_USER_INPUT=1`.
- Crash logs go to `lasrecomp.log` in the data root, the same as on Windows (`docs/DEPURACAO.md`).
