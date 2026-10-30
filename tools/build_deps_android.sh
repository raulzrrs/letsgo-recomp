#!/bin/bash
# make deps-android (MSYS2 UCRT64 shell or Git Bash, Android NDK in ANDROID_NDK): what the Android build (arm64-v8a)
# links against, built from pinned sources into build/deps-android:
#   FFmpeg  - the same LGPL-only, movies-and-Opus build as tools/build_deps_windows.sh (static libraries)
#   SDL3    - libSDL3.so with ndk-build, and its Java side (android-project's org.libsdl.app) for the APK
#   shaderc - libshaderc_combined.a from the NDK's own sources (sources/third_party/shaderc)
#   VMA     - vk_mem_alloc.h
# Skipped where the output exists (delete build/deps-android/<part> to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
NDK=${ANDROID_NDK:-$(cygpath -u "${USERPROFILE:-$HOME}")/Android/ndk/29}
[ -x "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe" ] || { echo "deps-android: no Android NDK at $NDK (ANDROID_NDK)"; exit 1; }
API=29
P=$PWD/build/deps-android
S=$P/src
mkdir -p "$S" "$P/lib" "$P/include"
JOBS=$(nproc)
TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
# ndk-build's make runs its recipes with cmd.exe: no sh.exe on its PATH (it would take that), no SHELL
ndkmake() { env -u SHELL -u MAKEFLAGS -u MFLAGS -u MAKELEVEL PATH="/c/Windows/System32:$NDK/prebuilt/windows-x86_64/bin" "$NDK/prebuilt/windows-x86_64/bin/make.exe" "$@"; }
w() { cygpath -m "$1"; }

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

# ---- FFmpeg
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
if [ ! -f "$P/lib/libavcodec.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --enable-cross-compile --target-os=android --arch=aarch64 \
        --cc="$TC/clang.exe" --cxx="$TC/clang++.exe" --ar="$TC/llvm-ar.exe" --nm="$TC/llvm-nm.exe" \
        --ranlib="$TC/llvm-ranlib.exe" --strip="$TC/llvm-strip.exe" --host-cc="${HOSTCC:-clang}" \
        --extra-cflags="--target=aarch64-linux-android$API -fPIC" --extra-ldflags="--target=aarch64-linux-android$API" \
        --enable-static --disable-shared --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "deps-android: FFmpeg is not LGPL"; exit 1; }
for c in opus vorbis vp9 vp8 h264 aac; do
    C=$(echo "$c" | tr a-z A-Z)
    grep -q "^#define CONFIG_${C}_DECODER 1" "$S/ffmpeg/config_components.h" || { echo "deps-android: FFmpeg built without the $c decoder"; exit 1; }
done

# ---- SDL3 (the version MSYS2 builds the PC runtime against)
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz
if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.18.tar.gz SDL3
    ndkmake -j"$JOBS" -f "$(w "$NDK/build/core/build-local.mk")" NDK_PROJECT_PATH=null \
        APP_BUILD_SCRIPT="$(w "$S/SDL3/Android.mk")" NDK_OUT="$(w "$S/sdl-obj")" NDK_LIBS_OUT="$(w "$S/sdl-libs")" \
        APP_ABI=arm64-v8a APP_PLATFORM=android-$API APP_STL=c++_shared APP_SHORT_COMMANDS=true SDL3
    "$TC/llvm-strip.exe" --strip-unneeded -o "$P/lib/libSDL3.so" "$S/sdl-obj/local/arm64-v8a/libSDL3.so"
    rm -rf "$P/include/SDL3" && cp -r "$S/SDL3/include/SDL3" "$P/include/"
    rm -rf "$P/java" && mkdir -p "$P/java" && cp -r "$S/SDL3/android-project/app/src/main/java/org" "$P/java/"
fi

# ---- shaderc (the NDK's sources)
if [ ! -f "$P/lib/libshaderc_combined.a" ]; then
    SH=$NDK/sources/third_party/shaderc
    ndkmake -j"$JOBS" -f "$(w "$NDK/build/core/build-local.mk")" NDK_PROJECT_PATH=null \
        APP_BUILD_SCRIPT="$(w "$SH/Android.mk")" NDK_OUT="$(w "$S/shaderc-obj")" NDK_LIBS_OUT="$(w "$S/shaderc-libs")" \
        APP_ABI=arm64-v8a APP_PLATFORM=android-$API APP_STL=c++_shared APP_SHORT_COMMANDS=true libshaderc_combined || true
    # (its own last step, the libraries merged into one, writes an ar script with cmd's echo, quotes and all: done here)
    L=$S/shaderc-obj/local/arm64-v8a
    { echo "create $(w "$P/lib/libshaderc_combined.a")"
      for l in shaderc shaderc_util glslang OGLCompiler OSDependent SPIRV HLSL SPIRV-Tools-opt SPIRV-Tools; do
          [ -f "$L/lib$l.a" ] && echo "addlib $(w "$L/lib$l.a")"
      done
      echo save; echo end; } > "$S/shaderc_combine.mri"
    "$TC/llvm-ar.exe" -M < "$S/shaderc_combine.mri"
    rm -rf "$P/include/shaderc" && cp -r "$SH/libshaderc/include/shaderc" "$P/include/"
fi
# ---- Vulkan Memory Allocator (header only: the version MSYS2 gives the PC build)
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz       822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1 vma-3.4.0.tar.gz
[ -f "$P/include/vk_mem_alloc.h" ] || tar -xzf "$S/vma-3.4.0.tar.gz" -O VulkanMemoryAllocator-3.4.0/include/vk_mem_alloc.h > "$P/include/vk_mem_alloc.h"
cp "$TC/../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$P/lib/"  # (SDL3 and shaderc's C++ runtime)
echo "deps-android: $(ls "$P/lib" | tr '\n' ' ')"
