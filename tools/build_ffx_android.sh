#!/bin/bash
# make ffx-android (Git Bash or MSYS2, Android NDK in ANDROID_NDK): AMD FidelityFX Super Resolution 3.1's upscaler for
# the Android build - on Windows the game loads AMD's signed amd_fidelityfx_vk.dll, which Android can't. From the
# FidelityFX SDK 1.1.4's sources (MIT, github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, pinned below):
#   - a sparse checkout of what the upscaler needs: the FFX API and its FSR 3.1 provider, the fsr3upscaler component,
#     the Vulkan backend, the shared code, the GLSL passes, and the SDK's shader tools (FidelityFX_SC + glslang)
#   - tools/ffx_android/patch_sdk.py: what it takes from MSVC and Windows (DX12 driver providers, wchar_t sizes,
#     std::wstring_convert), plus tools/ffx_android/android_compat.h forced into every file
#   - the passes' SPIR-V permutations made with the SDK's own tools, as its CMake does (19 MB of headers: generated here,
#     not kept in the repository)
#   -> build/deps-android/lib/libffx_fsr3.a. The runtime links it when present (Makefile, runtime/nvn/vulkan/fsr3.cpp).
# Skipped where the output exists (delete build/deps-android/ffx to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
NDK=${ANDROID_NDK:-$(cygpath -u "${USERPROFILE:-$HOME}")/Android/ndk/29}
TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
[ -x "$TC/clang++.exe" ] || { echo "ffx-android: no Android NDK at $NDK (ANDROID_NDK)"; exit 1; }
P=$PWD/build/deps-android
R=$P/ffx/sdk
O=$P/ffx/obj
SHA=c6efa6bf7f2027b3ec94f28578bb5965eabb9e55  # v1.1.4
mkdir -p "$P/lib" "$P/ffx"
[ -f "$P/lib/libffx_fsr3.a" ] && { echo "ffx-android: $P/lib/libffx_fsr3.a already there"; exit 0; }
w() { cygpath -w "$1"; }

# ---- the sources (git: a sparse, blobless clone of the pinned commit; long paths on)
if [ ! -f "$R/.ffx_ok" ]; then
    rm -rf "$R"
    git -c core.longpaths=true clone -q --filter=blob:none --sparse --no-checkout \
        https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$R"
    git -C "$R" -c core.longpaths=true sparse-checkout set --no-cone sdk/include sdk/src/backends/vk sdk/src/backends/shared \
        sdk/src/components/fsr3upscaler sdk/src/shared sdk/tools/binary_store ffx-api/src ffx-api/include /LICENSE.txt
    git -C "$R" -c core.longpaths=true -c advice.detachedHead=false checkout -q "$SHA"
    python -I tools/ffx_android/patch_sdk.py "$(w "$R")"
    touch "$R/.ffx_ok"
fi

# ---- the passes' SPIR-V (CMakeCompileFSR3UpscalerShaders.txt's arguments; the four sets the blob accessors include:
# <pass>, <pass>_wave64 (FFX_PREFER_WAVE64: nothing in GLSL), <pass>_16bit (FFX_HALF=1), <pass>_wave64_16bit)
SC=$R/sdk/tools/binary_store/FidelityFX_SC.exe
GPU=$R/sdk/include/FidelityFX/gpu
SH=$P/ffx/shaders
mkdir -p "$SH"
for f in "$R"/sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl; do
    n=$(basename "$f" .glsl)
    for v in "" _wave64 _16bit _wave64_16bit; do
        [ -f "$SH/${n}${v}_permutations.h" ] && continue
        extra=()
        case "$v" in *wave64*) extra+=("-DFFX_PREFER_WAVE64") ;; esac
        case "$v" in *16bit*) extra+=("-DFFX_HALF=1") ;; esac
        "$SC" -reflection -deps=gcc -DFFX_GPU=1 "${extra[@]}" \
            -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 \
            -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 \
            -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 \
            -compiler=glslang "-glslangexe=$(w "$R/sdk/tools/binary_store/glslangValidator.exe")" -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1 \
            "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}" "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}" \
            "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}" "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}" \
            "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}" "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}" \
            "-I$(w "$GPU")" "-I$(w "$GPU/fsr3upscaler")" "-name=${n}${v}" "-output=$(w "$SH")" "$(w "$f")" > "$SH/${n}${v}.log" 2>&1 \
            || { echo "ffx-android: $n$v failed (see $SH/$n$v.log)"; exit 1; }
    done
done

# ---- the library
CXX="$TC/clang++ --target=aarch64-linux-android29 -fPIC -O2 -std=c++17 -fno-strict-aliasing -w -fdeclspec -fms-extensions"
CXX="$CXX -include $(w tools/ffx_android/android_compat.h) -DFFX_FSR3UPSCALER=1 -DFFX_BACKEND_VK=1 -DFFX_API_VK=1"
INC="-I$R/sdk/include -I$R/sdk/include/FidelityFX/host -I$R/sdk/src/shared -I$R/sdk/src/components -I$R/sdk/src/backends/shared"
INC="$INC -I$R/sdk/src/backends/shared/blob_accessors -I$SH -I$R/ffx-api/include -I$R/ffx-api/src"
mkdir -p "$O"
objs=()
for s in ffx-api/src/ffx_api.cpp ffx-api/src/backends.cpp ffx-api/src/ffx_provider.cpp ffx-api/src/ffx_provider_fsr3upscale.cpp ffx-api/src/validation.cpp \
         sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp sdk/src/backends/vk/ffx_vk.cpp sdk/src/backends/shared/ffx_shader_blobs.cpp \
         sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp sdk/src/shared/ffx_assert.cpp \
         sdk/src/shared/ffx_message.cpp sdk/src/shared/ffx_object_management.cpp sdk/src/shared/ffx_breadcrumbs_list.cpp; do
    o=$O/$(basename "$s" .cpp).o
    $CXX $INC -c "$R/$s" -o "$o" &
    objs+=("$o")
done
wait
for o in "${objs[@]}"; do [ -f "$o" ] || { echo "ffx-android: $o failed"; exit 1; }; done
rm -f "$P/lib/libffx_fsr3.a"
"$TC/llvm-ar" rcs "$P/lib/libffx_fsr3.a" "${objs[@]}"
mkdir -p "$P/licenses"; cp "$R/LICENSE.txt" "$P/licenses/FidelityFX-SDK.txt" 2>/dev/null || true
echo "ffx-android: $P/lib/libffx_fsr3.a ($(du -h "$P/lib/libffx_fsr3.a" | cut -f1))"
