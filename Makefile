# Let's Go Recomp build (from LAS Recomp's).
# Windows (the supported platform): MSYS2 UCRT64 shell or Git Bash with MSYS2 on PATH, clang + Vulkan renderer
# (docs/COMPILAR-WINDOWS.md). The Linux branch is LAS Recomp's; it builds and runs (docs/LINUX.md).
#   make gen           translate your own game files (extracted/exefs) into generated/
#   make -j20          build/lasrecomp.exe (runtime) and build/lasgame.dll (the game code)
#   make jogo          a copy to play while build/ is rebuilt (build/jogo/)
#   make dist          the release zip (build/dist/LetsGoRecomp-<version>-windows.zip): needs make deps-windows,
#                      make gen-helper and tools/fetch_zig.sh first
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. DEPS: a prefix with SDL3 / FFmpeg to use instead of the system's (MSYS2's).
B         ?= build
DEPS      ?=
# EXE_NAME: the exe's name (the release's: LetsGoRecomp); the game code library imports from the exe by it
EXE_NAME  ?= lasrecomp
CC       := clang $(CROSS)
CXX      := clang++ $(CROSS)
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c) $(wildcard $(GEN)/subsdk0/*.c) $(wildcard $(GEN)/subsdk1/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),linux)
EXE      := $(B)/lasrecomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows (see there)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format -fdeclspec -DRT_APP_NAME=\"LetsGoRecomp\" -D_GNU_SOURCE  # (%llx for uint64_t, which is unsigned long here: the same size; -fdeclspec: FidelityFX's headers)
SHADERC  ?= -lshaderc  # (Debian / Ubuntu's libshaderc.so; a shaderc built from source: -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++
GAMELIB  := $(B)/lasgame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/$(EXE_NAME).exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
# GENARCH: the CPU level `make` builds the game code for, x86-64-v3 (AVX2, FMA, BMI2) or x86-64-v2 (SSE4.2, slower).
# The start screen picks it per machine instead (runtime/gamelib.c gamelib_cpu_level); a library is loaded only on a
# machine at its level (LARECOMP_GAME_CPU=v2 for a v2 one on a v3 CPU).
GENARCH  ?= -march=x86-64-v3
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -ldwmapi -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> lasrecomp.log)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++
GAMELIB  := $(B)/lasgame.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/lasrecomp.dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/lasrecomp.dll.a
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
endif

# Research build (docs/DEPURACAO.md, LARECOMP_TRACE_STORES): make TRACE=1 B=build/trace GEN=generated-trace gen / -j20 all.
# Every store of the game reports its instruction to runtime/storetrace.c.
ifdef TRACE
GENFLAGS += -DSTORE_TRACE
GENTRACE := --trace-stores
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags but the CPU level). A library's own id adds "-<level>" (GAMELIB_LEVEL here; runtime/gamelib.c gamelib_id). A
# library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_CFLAGS := $(strip $(filter-out -Iruntime $(GENARCH),$(GENFLAGS)))
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GAMELIB_CFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
GAMELIB_LEVEL := $(or $(patsubst -march=x86-64-%,%,$(filter -march=x86-64-v%,$(GENARCH))),base)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn -Ithird_party/fidelityfx/include $(DEPINC)

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)-$(GAMELIB_LEVEL)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(GAMELIB_CFLAGS)"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file. The temp file keeps
# the real name (in a folder of its own): on Windows the import library records the name the game code library loads
# it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID)-$(GAMELIB_LEVEL) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Python: .venv (Windows: Scripts/python.exe; Linux: bin/python)
PY := $(firstword $(wildcard .venv/Scripts/python.exe) $(wildcard .venv/bin/python) $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

gen:
	cd tools && $(abspath $(PY)) gen.py sdk ../extracted/exefs/sdk 0x7004000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt $(GENSIMD) $(GENTRACE)
	cd tools && $(abspath $(PY)) gen.py main ../extracted/exefs/main 0x7000000000 ../$(GEN)/main $(GENSIMD) $(GENTRACE)
	cd tools && $(abspath $(PY)) gen.py subsdk0 ../extracted/exefs/subsdk0 0x7006000000 ../$(GEN)/subsdk0 $(GENSIMD) $(GENTRACE)
	cd tools && $(abspath $(PY)) gen.py subsdk1 ../extracted/exefs/subsdk1 0x7008000000 ../$(GEN)/subsdk1 $(GENSIMD) $(GENTRACE)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico runtime/platform/windows.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -I runtime/platform -O coff $< -o $@

# Windows release: build/dist/LetsGoRecomp-<version>-windows.zip, top folder "LetsGoRecomp/": LetsGoRecomp.exe, the
# DLLs it loads (runs without MSYS2) and what its start screen builds the game code with on the player's machine
# (runtime/gamebuild.c): the frozen generator (make gen-helper), zig (tools/fetch_zig.sh slimmed by tools/zig_slim.sh:
# its runtime built ahead of time), gamelib-src/ and the exe's import library. No game code, no game files. The exe is
# a release build in $(WIN_B): linked against the LGPL FFmpeg from `make deps-windows` (MSYS2's is a GPLv3 build) and
# without a console window (RELEASE=1: the log goes to lasrecomp.log next to the exe). vulkan-1.dll is left out: every
# Vulkan driver installs the loader in System32. Saves, settings, game files and mods go next to the exe at run time.
VERSION  := $(shell sed -n 's/^\#define RT_APP_VERSION "\(.*\)".*/\1/p' runtime/paths.h)
DIST     := build/dist/LetsGoRecomp
WIN_B    := build/win-release
deps-windows:
	tools/build_deps_windows.sh
dist:
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -d build/helper/lasgen ] || { echo "make gen-helper first (the game code generator)"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 EXE_NAME=LetsGoRecomp $(WIN_B)/LetsGoRecomp.exe
	! nm $(WIN_B)/LetsGoRecomp.exe | grep ' f_70' >/dev/null  # (no game code inside)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/LetsGoRecomp.exe $(WIN_B)/lasrecomp.dll.a "$(DIST)/"
	tools/zig_slim.sh $(GAMELIB_CFLAGS)  # (zig with its runtime built ahead of time)
	cp -R build/helper/lasgen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	cp docs/icon/icon_1024.png "$(DIST)/icon.png"  # (window icon)
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$(DIST)/"  # (in-game menu font, OFL)
	cp third_party/fonts/Baloo2/OFL.txt "$(DIST)/Baloo2-OFL.txt"
	cp third_party/fidelityfx/bin/amd_fidelityfx_vk.dll "$(DIST)/"  # (FSR 3.1, loaded at run time: without it, FSR 1)
	cp third_party/fidelityfx/LICENSE.txt "$(DIST)/FidelityFX-LICENSE.txt"
	cp LICENSE "$(DIST)/LICENSE.txt"
	cp LICENSE-LASRecomp "$(DIST)/LICENSE-LASRecomp.txt"
	cp docs/release-README.txt "$(DIST)/README.txt"
	PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/LetsGoRecomp.exe | awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}' | sort -u | \
	    xargs -I{} cp -u {} "$(DIST)/"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (no GPL codec libraries in the release)
	cd build/dist && zip -qr "LetsGoRecomp-$(VERSION)-windows.zip" LetsGoRecomp
	@echo "build/dist/LetsGoRecomp-$(VERSION)-windows.zip"

# The game code generator as a standalone helper, so players need no Python (tools/lasgen.spec): build/helper/lasgen/,
# shipped as lasgen/ next to the exe. The start screen's setup runs it on the player's own game files.
gen-helper:
	USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)" \
	$(PY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/lasgen.spec
	$(PY) tools/pe_no_cfg.py build/helper/lasgen/lasgen.exe
	@du -sh build/helper/lasgen

# A copy to play while build/ is rebuilt (the exe in use is locked on Windows): build/jogo/ (tools/publicar_jogo.sh)
jogo:
	tools/publicar_jogo.sh

.PHONY: all gen gen-objs runtime-objs dist deps-windows gen-helper jogo
