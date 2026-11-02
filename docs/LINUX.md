# Let's Go Recomp on Linux (native build)

The Linux branch of the Makefile is LAS Recomp's. With the four small fixes in this change it builds and runs: both games were built
from this tree and played on a **Steam Deck** (Mesa RADV, SteamOS, desktop and Game Mode): about 28-30 fps in the world and 14-25 fps
during the intro movie, which is software-decoded. Not a release: no AppImage, nothing but a source build, and the FidelityFX features
(FSR 3.1, frame generation by the DLLs) are Windows only.

## Build
Packages (Ubuntu / Debian names; SDL3 3.2 or newer is needed, `DEPS=<prefix>` points the Makefile at a prefix with its own SDL3 / FFmpeg):

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev libavutil-dev \
        libswscale-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev glslc

Then, from the repository root, with your own game files extracted to `extracted/` (`exefs/` and `romfs.bin`):

    python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
    make gen           # recompiles the game code: about two minutes
    make -j8           # about 40 minutes on 7 cores: ~1000 generated files

- **No spaces or apostrophes in the build path.** The Makefile does not quote paths (`cd tools && <path to python> gen.py`); a checkout under
  `Let's Go/` fails in `make gen`. Use a symlink with a plain name.
- clang 18 stalls or crashes on a few of the generated giants at `-O1`. A wrapper that compiles `*generated/*` files under `timeout 300` and
  again with `-O0` when that fails (put it first in `PATH`) lets the build finish.
- `SHADERC=-lshaderc_shared` for distributions that name shaderc's library that way.

## Run
`build/lasrecomp extracted --play` from the repository root (the data folder is the working directory). Start it from a terminal to see
the log; without one the log goes to `lasrecomp.log` in the data folder.

## What was needed
`runtime/mods.c` and `runtime/hle/movie.c` use `rmdir` / `stat` without including `<unistd.h>` / `<sys/stat.h>` (Windows' headers pulled them in);
`runtime/hle/catch_button.c` has a static `clone()` that clashes with glibc's under `_GNU_SOURCE`; the Makefile needs `-fdeclspec` (FidelityFX's
headers), `-D_GNU_SOURCE` and `-DRT_APP_NAME`.
