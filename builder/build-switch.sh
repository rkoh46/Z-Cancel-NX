#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Nintendo Switch programs:
#   build/switch/znx-bench.nro     the headless speed test; copy to sdmc:/switch/z-cancel-nx/
#                                  (or send with nxlink -s)
#   build/switch/z-cancel-nx.nro   the game with a picture (GLideN64 on the console's OpenGL)
#   build/switch/<name>.elf        the same program with symbols, for aarch64-none-elf-addr2line
#   build/switch/<name>.map        linker map
# Usage: builder/build-switch.sh            znx-bench (REBUILD=1 recompiles everything)
#        builder/build-switch.sh game       z-cancel-nx
#        builder/build-switch.sh all        both
#            The core's assert()s stay in, unlike in the Mac build: on a console a failed one
#            ends the run with a line in the log, where a silent wrong branch would be a crash
#            with no explanation. They cost nothing measurable (self-test: same fps either way);
#            ZNX_NDEBUG=1 builds without them.
#        builder/build-switch.sh --selftest ROM [FRAMES]
#            The same recipe built for the container's own Linux (ARM64) and run there:
#            build/switch-selftest/znx-selftest. It executes what can be executed without a
#            console: the statically linked plugins and their name tables, the core's __SWITCH__
#            code paths (code cache in .text with a second writable view, cache flush on both
#            views, x18 in use), the ELF assembly. libnx is replaced by switch/selftest/linux_main.c.
#            The ROM is piped into the container's /tmp and never touches the repository.
#            ZNX_COMPACT=1 makes it refuse the core's 512 MB block, as an applet on the console does.
#
# Runs on the Mac: prepares build/src (patches/ applied), then compiles inside the devkitA64
# container (the image Melee-NX-NetPlay builds, devkitA64 GCC + libnx). Only this
# repository is mounted, as /project.
#
# One program, no dlopen: the core, the RSP HLE plugin and the video plugin (the null one
# for znx-bench, GLideN64 for the game) are each linked into one relocatable object (ld -r). Their internal symbols (everything compiled
# with hidden visibility) are made local with objcopy --localize-hidden, and what a plugin
# exports is renamed PluginStartup -> znxp_<plugin>_PluginStartup with objcopy
# --redefine-syms, because every plugin exports the same names. The same symbol lists become
# gen/static_libs.c, the name tables switch/dynlib_static.c searches in place of dlsym.
# No source of the core or the plugins is touched for this.
#
# GLideN64 is C++. Its objects are linked the same way, with --force-group-allocation so
# that its inline functions and templates (COMDAT groups) are resolved inside the plugin
# before its hidden symbols are made local; what is left visible is the plugin API and the
# weak template instantiations of the C++ library, which the final link merges with
# libstdc++'s. Its global constructors stay in .init_array, which libnx's start-up code runs
# before main() (checked below). It is built for desktop OpenGL through EGL (-DEGL
# -DOS_LINUX, as mupen64plus-libretro-nx builds it for the Switch), without the Qt settings
# dialog (mupenplus/Config_mupenplus.cpp: settings in the core's configuration), without the
# texture-pack library (TxFilterStub.cpp) and without on-screen text (TextDrawerStub.cpp).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
IMAGE="${ZNX_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"

if [ "${1:-}" != "--in-container" ]; then
    cd "$ROOT"
    builder/prepare-src.sh
    command -v docker >/dev/null || { echo "docker not found (start Colima)" >&2; exit 69; }
    docker image inspect "$IMAGE" >/dev/null 2>&1 || {
        echo "Docker image $IMAGE not found: build it with Melee-NX-NetPlay's builder/docker.sh image" >&2; exit 69; }
    run=(docker run --rm -i -v "$ROOT:/project" -w /project -e REBUILD="${REBUILD:-}" -e ZNX_NDEBUG="${ZNX_NDEBUG:-}" ${ZNX_COMPACT:+-e ZNX_COMPACT=1} "$IMAGE")
    if [ "${1:-}" = "--selftest" ]; then
        [ -f "${2:-}" ] || { echo "usage: $0 --selftest ROM [FRAMES]" >&2; exit 64; }
        "${run[@]}" bash builder/build-switch.sh --in-container selftest </dev/null
        exec "${run[@]}" sh -c 'cat > /tmp/rom.z64 && build/switch-selftest/znx-selftest /tmp/rom.z64 "$@"' sh "${@:3}" < "$2"
    fi
    case "${1:-}" in
        "") what=switch ;;
        game|all) what="$1" ;;
        *) echo "usage: $0 [game|all] | --selftest ROM [FRAMES]" >&2; exit 64 ;;
    esac
    exec "${run[@]}" bash builder/build-switch.sh --in-container "$what" </dev/null
fi

# ---- inside the container; every path is relative to /project ----
TARGET="${2:-switch}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TOOLS="$DEVKITPRO/tools/bin"
SRC="build/src"
if [ "$TARGET" = selftest ]; then
    P=""                # the container's own GCC and binutils
    OUT="build/switch-selftest"
else
    P="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-"
    OUT="build/switch"
fi
OBJ="$OUT/obj"
GEN="$OUT/gen"
CORE="$SRC/mupen64plus-core"
RSP="$SRC/mupen64plus-rsp-hle"
GLN="$SRC/mupen64plus-video-GLideN64/src"
case "$TARGET" in
    switch)   PROGRAMS="znx-bench" ;;
    game)     PROGRAMS="z-cancel-nx" ;;
    all)      PROGRAMS="znx-bench z-cancel-nx" ;;
    selftest) PROGRAMS="" ;;
    *) echo "unknown target $TARGET" >&2; exit 64 ;;
esac
WITH_BENCH=""; WITH_GAME=""
case " $PROGRAMS " in *" znx-bench "*) WITH_BENCH=1 ;; esac
case " $PROGRAMS " in *" z-cancel-nx "*) WITH_GAME=1 ;; esac
[ "$TARGET" = selftest ] && WITH_BENCH=1   # the self-test is znx-bench's recipe
JOBS="$(nproc)"
VERSION="$(cat VERSION)"
[ -d "$CORE/src" ] || { echo "build/src is missing: run builder/build-switch.sh on the host" >&2; exit 1; }
mkdir -p "$OBJ" "$GEN" "$OUT/include" "$OUT/romfs/data"

# Only SDL3's headers are used (bench/shim/sdl_shim.c stands in for the library); RMG-K
# carries a copy inside its SDL_net tree.
SDL_HEADERS="ref/RMG-K/Source/3rdParty/SDL_net/Xcode/macOS/SDL3.framework/Versions/A/Headers"
[ -f "$SDL_HEADERS/SDL.h" ] || { echo "SDL3 headers not found in $SDL_HEADERS" >&2; exit 1; }
if [ ! -f "$OUT/include/SDL3/SDL.h" ]; then
    rm -rf "$OUT/include/SDL3"
    cp -R "$SDL_HEADERS" "$OUT/include/SDL3"
fi

# devkitPro's flags for a Switch program (position-independent: an NRO is loaded at a random
# address), plus the ones the Mac build uses. -ffp-contract=off: GCC would otherwise fuse
# multiply-add on ARM64 and round differently from x86-64.
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
ZLIB_INC="-isystem $DEVKITPRO/portlibs/switch/include"
FRONT_INC="-isystem $DEVKITPRO/libnx/include"
PORT_INC="-isystem $DEVKITPRO/portlibs/switch/include"
# The console around a run, shared by both programs; then what each adds.
FRONT_COMMON="switch/front_switch.c switch/jit_switch.c switch/dynlib_static.c switch/clocks_switch.c switch/libc_switch.c bench/bench_core.c"
FRONT_BENCH="switch/main_switch.c"
FRONT_GAME="switch/game_switch.c switch/vidext_switch.c switch/input_switch.c game/game_core.c"
if [ "$TARGET" = selftest ]; then
    ARCH="-fPIE"
    # zlib: Debian's runtime library with devkitPro's copy of the two headers.
    cp "$DEVKITPRO/portlibs/switch/include/zlib.h" "$DEVKITPRO/portlibs/switch/include/zconf.h" "$OUT/include/"
    ZLIB_INC=""
    FRONT_INC="-D_GNU_SOURCE"
    PORT_INC=""
    FRONT_COMMON="switch/selftest/linux_main.c switch/dynlib_static.c bench/bench_core.c"
    FRONT_BENCH=""
fi
ASSERTS=""
[ -n "${ZNX_NDEBUG:-}" ] && ASSERTS="-DNDEBUG"
COMMON="$ARCH -O3 -g -ffp-contract=off -fno-strict-aliasing -ffunction-sections -fvisibility=hidden -D__SWITCH__ $ASSERTS -w"
# zlib is devkitPro's static portlib (the Mac build links the system's); minizip is the core's own copy.
CORE_FLAGS="$COMMON -I$CORE/src -I$GEN -I$CORE/subprojects/md5 -I$CORE/subprojects/xxhash -I$CORE/subprojects/minizip -I$OUT/include $ZLIB_INC -DM64P_PARALLEL -DUSE_SDL3 -DNOCRYPT -DNOUNCRYPT -DDYNAREC -DNEW_DYNAREC=4 -DNO_KEYBINDINGS -DACCURATE_FPU_BEHAVIOR -DIOAPI_NO_64"
RSP_FLAGS="$COMMON -I$RSP/src -I$CORE/src/api"
FRONT_FLAGS="$ARCH -O2 -g -Wall -ffunction-sections -D__SWITCH__ -I$CORE/src/api -I$GEN -Ibench -Igame -Iswitch $FRONT_INC $PORT_INC"
# GLideN64: RMG-K's CMake options that matter (MUPENPLUSAPI, TXFILTER_LIB) plus EGL on a
# "Linux" as far as its #ifdefs go. gnu++17 as its CMakeLists asks; the ARMv8 CRC32
# instructions for its texture checksums (-march has +crc).
GLN_FLAGS="$COMMON -fvisibility-inlines-hidden -DMUPENPLUSAPI -DTXFILTER_LIB -DOS_LINUX -DEGL -I$GLN -I$GLN/inc -I$GLN/osal -I$GEN/gliden64 $PORT_INC -isystem $DEVKITPRO/libnx/include"

# The Mac build's list without the two files that are the platform: osal/dynamiclib_unix.c
# (dlsym; switch/dynlib_static.c instead) and osal/files_macos.c (files_unix.c instead).
CORE_SOURCES="
src/api/callbacks.c src/api/common.c src/api/config.c src/api/debugger.c src/api/frontend.c src/api/vidext.c
src/backends/api/video_capture_backend.c src/backends/plugins_compat/audio_plugin_compat.c
src/backends/plugins_compat/input_plugin_compat.c src/backends/clock_ctime_plus_delta.c
src/backends/dummy_video_capture.c src/backends/file_storage.c
src/device/cart/cart.c src/device/cart/af_rtc.c src/device/cart/cart_rom.c src/device/cart/eeprom.c
src/device/cart/flashram.c src/device/cart/is_viewer.c src/device/cart/sram.c
src/device/controllers/game_controller.c src/device/controllers/vru_controller.c
src/device/controllers/paks/biopak.c src/device/controllers/paks/mempak.c
src/device/controllers/paks/rumblepak.c src/device/controllers/paks/transferpak.c
src/device/dd/dd_controller.c src/device/dd/disk.c src/device/device.c
src/device/gb/gb_cart.c src/device/gb/mbc3_rtc.c src/device/gb/m64282fp.c
src/device/memory/memory.c src/device/pif/bootrom_hle.c src/device/pif/cic.c
src/device/pif/n64_cic_nus_6105.c src/device/pif/pif.c
src/device/r4300/cached_interp.c src/device/r4300/cp0.c src/device/r4300/cp1.c src/device/r4300/cp2.c
src/device/r4300/idec.c src/device/r4300/interrupt.c src/device/r4300/pure_interp.c
src/device/r4300/r4300_core.c src/device/r4300/tlb.c src/device/r4300/new_dynarec/new_dynarec.c
src/device/rcp/ai/ai_controller.c src/device/rcp/mi/mi_controller.c src/device/rcp/pi/pi_controller.c
src/device/rcp/rdp/fb.c src/device/rcp/rdp/rdp_core.c src/device/rcp/ri/ri_controller.c
src/device/rcp/rsp/rsp_core.c src/device/rcp/si/si_controller.c src/device/rcp/vi/vi_controller.c
src/device/rdram/rdram.c
src/main/main.c src/main/util.c src/main/cheat.c src/main/eventloop.c src/main/rom.c
src/main/savestates.c src/main/sdl_key_converter.c src/main/workqueue.c src/main/pif_sync_callback.c
src/plugin/plugin.c src/plugin/dummy_video.c src/plugin/dummy_audio.c src/plugin/dummy_input.c
src/plugin/dummy_rsp.c
src/osal/files_unix.c
subprojects/md5/md5.c subprojects/minizip/ioapi.c subprojects/minizip/zip.c subprojects/minizip/unzip.c
"
RSP_SOURCES="alist.c alist_audio.c alist_naudio.c alist_nead.c audio.c cicx105.c hle.c hvqm.c jpeg.c
memory.c mp3.c musyx.c re2.c plugin.c"
# GLN_SOURCES: the GLideN64 files, the same list as the Mac check build's.
. builder/gliden64-sources.sh
CMDS="$OUT/compile.cmds"
: > "$CMDS"
CORE_OBJS=""
RSP_OBJS=""
GLN_OBJS=""
# queue <source> <object> <flags> [compiler]: compiled only when the source, this script or
# the flags' inputs are newer
queue() {
    mkdir -p "$(dirname "$2")"
    if [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ "$0" -nt "$2" ] || [ "$SRC/.stamp" -nt "$2" ] || [ -n "${REBUILD:-}" ]; then
        printf '%s\n' "echo '  CC $1' && ${P}${4:-gcc} $3 -c $1 -o $2" >> "$CMDS"
    fi
}
# front_objects <sources>: queues them and prints their objects
front_objects() {
    for f in $1; do
        o="$OBJ/front/$(basename "${f%.c}").o"
        queue "$f" "$o" "$FRONT_FLAGS"
        printf '%s ' "$o"
    done
}
# A switch between builds with and without assert()s recompiles everything.
MODE_STAMP="$OUT/.mode"
[ "$(cat "$MODE_STAMP" 2>/dev/null)" = "asserts:$ASSERTS" ] || { REBUILD=1; echo "asserts:$ASSERTS" > "$MODE_STAMP"; }

echo "#define ZNX_VERSION \"$VERSION\"" > "$GEN/znx_version.h.new"
cmp -s "$GEN/znx_version.h.new" "$GEN/znx_version.h" 2>/dev/null || cp "$GEN/znx_version.h.new" "$GEN/znx_version.h"
rm -f "$GEN/znx_version.h.new"

# Structure offsets for the dynarec's assembly, read back out of a compiled object.
mkdir -p "$OBJ/core/src/asm_defines"
${P}gcc $CORE_FLAGS -fno-lto -c "$CORE/src/asm_defines/asm_defines.c" -o "$OBJ/core/src/asm_defines/asm_defines.o"
rm -f "$GEN"/asm_defines_*.h.new
LC_ALL=C grep -a "@ASM_DEFINE" "$OBJ/core/src/asm_defines/asm_defines.o" | LC_ALL=C sort | while read -r _ name value _; do
    echo "%define $name ($value)" >> "$GEN/asm_defines_nasm.h.new"
    echo "#define $name ($value)" >> "$GEN/asm_defines_gas.h.new"
done
[ -s "$GEN/asm_defines_gas.h.new" ] || { echo "no structure offsets found in asm_defines.o" >&2; exit 1; }
for h in asm_defines_nasm.h asm_defines_gas.h; do
    cmp -s "$GEN/$h.new" "$GEN/$h" 2>/dev/null || cp "$GEN/$h.new" "$GEN/$h"
    rm -f "$GEN/$h.new"
done
CORE_OBJS="$OBJ/core/src/asm_defines/asm_defines.o"

for f in $CORE_SOURCES; do
    o="$OBJ/core/${f%.c}.o"
    queue "$CORE/$f" "$o" "$CORE_FLAGS"
    CORE_OBJS="$CORE_OBJS $o"
done
# Stand-ins for SDL3 and for the part of the core that needs libpng.
for f in bench/shim/*.c; do
    o="$OBJ/shim/$(basename "${f%.c}").o"
    queue "$f" "$o" "$CORE_FLAGS"
    CORE_OBJS="$CORE_OBJS $o"
done
LINKAGE="$CORE/src/device/r4300/new_dynarec/arm64/linkage_arm64.S"
queue "$LINKAGE" "$OBJ/core/linkage.o" "$CORE_FLAGS"
CORE_OBJS="$CORE_OBJS $OBJ/core/linkage.o"
for f in $RSP_SOURCES; do
    o="$OBJ/rsp/${f%.c}.o"
    queue "$RSP/src/$f" "$o" "$RSP_FLAGS"
    RSP_OBJS="$RSP_OBJS $o"
done
LIBS="core rsp_hle"
if [ -n "$WITH_BENCH" ]; then
    queue bench/video-null.c "$OBJ/video-null.o" "$COMMON -I$CORE/src/api"
    LIBS="$LIBS video_null"
fi
if [ -n "$WITH_GAME" ]; then
    # RMG-K's tree has no .git of its own inside GLideN64, so its build gets an empty Revision.h.
    mkdir -p "$GEN/gliden64"
    [ -f "$GEN/gliden64/Revision.h" ] || : > "$GEN/gliden64/Revision.h"
    for f in $GLN_SOURCES; do
        o="$OBJ/gliden64/${f%.*}.o"
        case "$f" in
            *.c) queue "$GLN/$f" "$o" "$GLN_FLAGS" ;;
            *)   queue "$GLN/$f" "$o" "$GLN_FLAGS -std=gnu++17" g++ ;;
        esac
        GLN_OBJS="$GLN_OBJS $o"
    done
    LIBS="$LIBS gliden64"
    # The audio plugin is this project's own (switch/audio_switch.c), linked like the others.
    queue switch/audio_switch.c "$OBJ/audio-switch.o" "$COMMON -I$CORE/src/api -Iswitch -isystem $DEVKITPRO/libnx/include"
    LIBS="$LIBS audio"
fi
FRONT_COMMON_OBJS="$(front_objects "$FRONT_COMMON")"
FRONT_BENCH_OBJS=""; FRONT_GAME_OBJS=""
[ -n "$WITH_BENCH" ] && FRONT_BENCH_OBJS="$(front_objects "$FRONT_BENCH")"
[ -n "$WITH_GAME" ] && FRONT_GAME_OBJS="$(front_objects "$FRONT_GAME")"
queue switch/jit_memory.S "$OBJ/front/jit_memory.o" "$ARCH"

tr '\n' '\0' < "$CMDS" | xargs -0 -r -n 1 -P "$JOBS" sh -c

# ---- one relocatable object per library, internals made local ----
echo "  LD $LIBS (relocatable)"
${P}ld -r -o "$OBJ/lib_core.o" $CORE_OBJS
${P}ld -r -o "$OBJ/lib_rsp_hle.o" $RSP_OBJS
[ -n "$WITH_BENCH" ] && ${P}ld -r -o "$OBJ/lib_video_null.o" "$OBJ/video-null.o"
[ -n "$WITH_GAME" ] && ${P}ld -r --force-group-allocation -o "$OBJ/lib_gliden64.o" $GLN_OBJS
[ -n "$WITH_GAME" ] && ${P}ld -r -o "$OBJ/lib_audio.o" "$OBJ/audio-switch.o"
for lib in $LIBS; do
    ${P}objcopy --localize-hidden "$OBJ/lib_$lib.o"
done

# exports <object>: "<type> <name>" of every symbol the library still shows the world
exports() {
    ${P}readelf -sW "$1" | awk '$5 == "GLOBAL" && $6 == "DEFAULT" && $7 != "UND" && ($4 == "FUNC" || $4 == "OBJECT") { print $4, $8 }' | LC_ALL=C sort -k2
}
for lib in $LIBS; do
    exports "$OBJ/lib_$lib.o" > "$GEN/exports_$lib.txt"
    if [ "$lib" != core ]; then
        awk -v p="znxp_${lib}_" '{ print $2, p $2 }' "$GEN/exports_$lib.txt" > "$GEN/rename_$lib.txt"
        ${P}objcopy --redefine-syms="$GEN/rename_$lib.txt" "$OBJ/lib_$lib.o"
    fi
done
# tables <name> <libraries>: gen/static_libs_<name>.c and its object, the name tables of one program
tables() {
    local tables="$GEN/static_libs_$1.c" list="" lib prefix
    {
        echo "/* Generated by builder/build-switch.sh: the functions each linked library exports. */"
        echo "#include <stddef.h>"
        echo "struct znx_static_sym { const char* name; void (*function)(void); };"
        echo "struct znx_static_lib { const char* name; const struct znx_static_sym* syms; };"
    } > "$tables.new"
    for lib in $2; do
        prefix=""
        [ "$lib" != core ] && prefix="znxp_${lib}_"
        # The table holds functions only. The core's SDL stand-ins are visible because SDL's
        # headers say so; they are not part of the mupen64plus API.
        awk -v p="$prefix" '$1 == "FUNC" && $2 !~ /^SDL_/ { print "void " p $2 "(void);" }' "$GEN/exports_$lib.txt" >> "$tables.new"
        echo "static const struct znx_static_sym syms_$lib[] = {" >> "$tables.new"
        awk -v p="$prefix" '$1 == "FUNC" && $2 !~ /^SDL_/ { print "    { \"" $2 "\", " p $2 " }," }' "$GEN/exports_$lib.txt" >> "$tables.new"
        echo "    { NULL, NULL } };" >> "$tables.new"
        echo "static const struct znx_static_lib lib_$lib = { \"$lib\", syms_$lib };" >> "$tables.new"
        list="$list &lib_$lib,"
    done
    echo "const struct znx_static_lib* const znx_static_libs[] = {$list NULL };" >> "$tables.new"
    cmp -s "$tables.new" "$tables" 2>/dev/null || cp "$tables.new" "$tables"
    rm -f "$tables.new"
    ${P}gcc $FRONT_FLAGS -Wno-builtin-declaration-mismatch -c "$tables" -o "$OBJ/front/static_libs_$1.o"
}
# needs <library> <function>...: the build stops if the library does not export one of them
needs() {
    local lib="$1" f
    shift
    for f in "$@"; do
        grep -q "^FUNC $f\$" "$GEN/exports_$lib.txt" || { echo "$f is not exported by $lib" >&2; exit 1; }
    done
}
needs core CoreStartup CoreAttachPlugin ConfigOpenSection CoreOverrideVidExt VidExt_GL_SwapBuffers
needs rsp_hle PluginStartup DoRspCycles
if [ -n "$WITH_BENCH" ]; then
    needs video_null PluginStartup PluginGetVersion ProcessDList
    tables bench "core rsp_hle video_null"
fi
if [ -n "$WITH_GAME" ]; then
    needs gliden64 PluginStartup PluginGetVersion InitiateGFX ProcessDList UpdateScreen RomOpen RomClosed
    needs audio PluginStartup PluginGetVersion InitiateAudio AiDacrateChanged AiLenChanged RomOpen RomClosed
    tables game "core rsp_hle gliden64 audio"
    # Nothing of the plugin may stay visible under a name the core, another plugin or the
    # front could also define: only its API (renamed above) and C++ library templates (weak).
    stray="$(${P}nm -g --defined-only "$OBJ/lib_gliden64.o" | awk '$2 != "W" && $2 != "V" && $2 != "u" && $3 !~ /^znxp_gliden64_/ { print $3 }')"
    [ -z "$stray" ] || { echo "GLideN64 leaves global symbols:" >&2; echo "$stray" | head -20 >&2; exit 1; }
fi

# ---- the programs ----
if [ "$TARGET" = selftest ]; then
    echo "  LD znx-selftest"
    gcc -pie -g -Wl,--gc-sections -Wl,-z,text \
        $FRONT_COMMON_OBJS "$OBJ/front/static_libs_bench.o" "$OBJ/lib_core.o" "$OBJ/lib_rsp_hle.o" "$OBJ/lib_video_null.o" \
        "$OBJ/front/jit_memory.o" \
        /usr/lib/aarch64-linux-gnu/libz.so.1 -lpthread -lm -o "$OUT/znx-selftest"
    mkdir -p "$OUT/data" "$OUT/profile"
    cp "$CORE"/data/mupen64plus.ini "$CORE"/data/mupencheat.txt "$OUT/data/"
    echo "built $OUT/znx-selftest"
    exit 0
fi

# The core's ROM database and cheat list (settings per game) travel inside the NRO as romfs:/data.
cp "$CORE"/data/mupen64plus.ini "$CORE"/data/mupencheat.txt "$OUT/romfs/data/"

# finish <name> <title> <author>: checks on the linked program build/switch/<name>.elf, then the NRO
finish() {
    local ELF="$OUT/$1.elf" undefined jit jit_addr jit_size jit_section section_line code_segment code_size image_end section_name
    undefined="$(${P}nm -u "$ELF")"
    [ -z "$undefined" ] || { echo "unresolved symbols:" >&2; echo "$undefined" >&2; exit 1; }
    if ${P}nm "$ELF" | grep -Eqw 'dlopen|dlsym|dlclose'; then echo "dlopen is still linked in" >&2; exit 1; fi
    # The code cache must lie in the executable segment, page-aligned and 32 MB long.
    jit="$(${P}readelf -sW "$ELF" | awk '$8 == "znx_jit_memory" { print $2, $3, $7 }')"
    set -- "$1" "$2" "$3" $jit
    jit_addr=$((16#$4)); jit_size=$(($5)); jit_section=$6   # readelf prints a large size as 0x...
    section_line="$(${P}readelf -SW "$ELF" | sed 's/\[ *\([0-9]*\)\]/\1/' | awk -v n="$jit_section" '$1 == n')"
    case "$section_line" in *" AX "*) ;; *) echo "znx_jit_memory is not in an executable section: $section_line" >&2; exit 1 ;; esac
    [ $((jit_addr % 4096)) -eq 0 ] && [ "$jit_size" -eq 33554432 ] || { echo "znx_jit_memory: bad address or size ($jit)" >&2; exit 1; }
    # The first LOAD segment is the code segment (libnx's switch.ld): flags R E, starting at 0.
    code_segment="$(${P}readelf -lW "$ELF" | awk '$1 == "LOAD" { print $6, $7 $8; exit }')"
    code_size=$((16#$(echo "$code_segment" | awk '{ sub(/^0x/, "", $1); print $1 }')))
    [ "$(echo "$code_segment" | awk '{ print $2 }')" = "RE" ] && [ $((jit_addr + jit_size)) -le "$code_size" ] \
        || { echo "znx_jit_memory is not inside the read+execute segment ($code_segment)" >&2; exit 1; }
    # Generated code calls C helpers with direct branches (+/-128 MB) and addresses data with
    # adrp (+/-4 GB): true wherever the image is loaded as long as the whole image is smaller.
    image_end=$((16#$(${P}nm "$ELF" | awk '$3 == "__end__" { print $1 }')))
    [ "$image_end" -gt 0 ] && [ "$image_end" -lt $((128 * 1024 * 1024)) ] \
        || { echo "the image is $image_end bytes: generated code could not reach its helpers" >&2; exit 1; }

    "$TOOLS/nacptool" --create "$2" "$3" "$VERSION" "$OUT/$1.nacp"
    "$TOOLS/elf2nro" "$ELF" "$OUT/$1.nro" --icon=switch/icon.jpg --nacp="$OUT/$1.nacp" --romfsdir="$OUT/romfs"
    section_name="$(echo "$section_line" | awk '{ print $2 }')"
    printf 'znx_jit_memory: 0x%x, %d MB, section %s (AX), code segment R E; image ends at %d MB\n' \
        "$jit_addr" "$((jit_size >> 20))" "$section_name" "$((image_end >> 20))"
    ls -l "$OUT/$1.nro" "$ELF" | awk '{ print $5, $NF }'
    echo "built $OUT/$1.nro"
}

# switch.specs: libnx's linker script, -pie, --gc-sections, -z text (no relocation may land
# in the code segment, which holds the code cache), --require-defined=main.
# --wrap: abort() and failed asserts go through switch/front_switch.c so they reach the log.
if [ -n "$WITH_BENCH" ]; then
    echo "  LD znx-bench.elf"
    ${P}gcc -specs="$DEVKITPRO/libnx/switch.specs" $ARCH -g \
        -Wl,-Map,"$OUT/znx-bench.map" -Wl,--wrap=abort -Wl,--wrap=__assert_func \
        $FRONT_COMMON_OBJS $FRONT_BENCH_OBJS "$OBJ/front/static_libs_bench.o" \
        "$OBJ/lib_core.o" "$OBJ/lib_rsp_hle.o" "$OBJ/lib_video_null.o" "$OBJ/front/jit_memory.o" \
        -L"$DEVKITPRO/portlibs/switch/lib" -L"$DEVKITPRO/libnx/lib" -lz -lnx -lm \
        -o "$OUT/znx-bench.elf"
    finish znx-bench "Z-Cancel NX speed test" "Z-Cancel NX"
fi
if [ -n "$WITH_GAME" ]; then
    echo "  LD z-cancel-nx.elf"
    # g++ for the C++ library. The core and the code cache come first so that generated code
    # stays close to the helpers it calls; GLideN64 and Mesa (libEGL with the nouveau driver,
    # libglapi, libdrm_nouveau: the Switch homebrew OpenGL stack) follow.
    ${P}g++ -specs="$DEVKITPRO/libnx/switch.specs" $ARCH -g \
        -Wl,-Map,"$OUT/z-cancel-nx.map" -Wl,--wrap=abort -Wl,--wrap=__assert_func \
        $FRONT_COMMON_OBJS $FRONT_GAME_OBJS "$OBJ/front/static_libs_game.o" \
        "$OBJ/lib_core.o" "$OBJ/lib_rsp_hle.o" "$OBJ/front/jit_memory.o" "$OBJ/lib_gliden64.o" "$OBJ/lib_audio.o" \
        -L"$DEVKITPRO/portlibs/switch/lib" -L"$DEVKITPRO/libnx/lib" -lEGL -lglapi -ldrm_nouveau -lz -lnx -lm \
        -o "$OUT/z-cancel-nx.elf"
    # GLideN64's global constructors must be among those libnx runs before main().
    grep -q "^ \.init_array.*lib_gliden64\.o" "$OUT/z-cancel-nx.map" \
        || { echo "GLideN64's constructors are not in .init_array" >&2; exit 1; }
    finish z-cancel-nx "Z-Cancel NX" "rkoh"
fi
