#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the headless test harness for macOS:
#   build/mac-<arch>/libmupen64plus.dylib        RMG-K's core (patches/ applied), new_dynarec
#   build/mac-<arch>/mupen64plus-rsp-hle.dylib   RSP HLE plugin
#   build/mac-<arch>/znx-video-null.dylib        video plugin that only completes display lists (bench/)
#   build/mac-<arch>/znx-bench                   the harness (bench/znx-bench.c + bench/bench_core.c)
#   build/mac-<arch>/znx-net                     headless RMG-K P2P rollback session (net/): RMG-K's session
#                                                driver, n02 P2P client and GekkoNet, compiled from ref/ unchanged
#   build/mac-<arch>/znx-input-net.dylib         its input plugin (net/input-net.c)
# Usage: builder/build-mac.sh [arm64|x86_64]     (x86_64 runs under Rosetta and needs nasm)
#        builder/build-mac.sh arm64 game        also the check build of the picture program:
#   build/mac-arm64/mupen64plus-video-GLideN64.dylib   RMG-K's GLideN64 (patches/ applied), built as for the Switch
#                                                      except for the OpenGL it talks to (macOS's instead of EGL)
#   build/mac-arm64/znx-game                           game/mac/: the same frame driver and settings as the Switch
#                                                      program, drawing into an offscreen OpenGL context (no window);
#                                                      also the start screen to PNG (znx-game --menu-shot, menu/)
set -euo pipefail
ARCH="${1:-arm64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"   # every path below is relative to the repo, so a space in its location cannot break a command line
SRC="build/src"
OUT="build/mac-$ARCH"
OBJ="$OUT/obj"
GEN="$OUT/gen"
CORE="$SRC/mupen64plus-core"
RSP="$SRC/mupen64plus-rsp-hle"
JOBS="$(sysctl -n hw.ncpu)"

case "$ARCH" in
    arm64)  DYNAREC=4; LINKAGE="device/r4300/new_dynarec/arm64/linkage_arm64.S" ;;
    x86_64) DYNAREC=2; LINKAGE="device/r4300/new_dynarec/x64/linkage_x64.asm"
            command -v nasm >/dev/null || { echo "nasm is required for x86_64 (brew install nasm)" >&2; exit 1; } ;;
    *) echo "usage: $0 [arm64|x86_64]" >&2; exit 1 ;;
esac

builder/prepare-src.sh
mkdir -p "$OBJ" "$GEN"

# Only SDL3's headers are used: bench/shim/sdl_shim.c stands in for the library.
SDL_INC="$(pkg-config --variable=includedir sdl3 2>/dev/null || true)"
[ -n "$SDL_INC" ] && [ -f "$SDL_INC/SDL3/SDL.h" ] || { echo "SDL3 headers not found (brew install sdl3)" >&2; exit 1; }

# -ffp-contract=off: no fused multiply-add, so both architectures round the same way.
# The other defines are what RMG-K's own build passes (OSD=0 NEW_DYNAREC=1 KEYBINDINGS=0
# ACCURATE_FPU=1 VULKAN=0), without its SDL_net netplay.
COMMON="-arch $ARCH -mmacosx-version-min=11.0 -O3 -ffp-contract=off -fno-strict-aliasing -fPIC -fvisibility=hidden -DNDEBUG -w"
CORE_FLAGS="$COMMON -I$CORE/src -I$GEN -I$CORE/subprojects/md5 -I$CORE/subprojects/xxhash -I$CORE/subprojects/minizip -I$SDL_INC -DM64P_PARALLEL -DUSE_SDL3 -DNOCRYPT -DNOUNCRYPT -DDYNAREC -DNEW_DYNAREC=$DYNAREC -DNO_KEYBINDINGS -DACCURATE_FPU_BEHAVIOR"
RSP_FLAGS="$COMMON -I$RSP/src -I$CORE/src/api"

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
src/osal/dynamiclib_unix.c src/osal/files_macos.c
subprojects/md5/md5.c subprojects/minizip/ioapi.c subprojects/minizip/zip.c subprojects/minizip/unzip.c
"
RSP_SOURCES="alist.c alist_audio.c alist_naudio.c alist_nead.c audio.c cicx105.c hle.c hvqm.c jpeg.c
memory.c mp3.c musyx.c re2.c plugin.c osal_dynamiclib_unix.c"

CMDS="$OUT/compile.cmds"
: > "$CMDS"
CORE_OBJS=""
RSP_OBJS=""
# queue <source> <object> <flags>: compiled only when the source or this script is newer
queue() {
    mkdir -p "$(dirname "$2")"
    if [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ "$0" -nt "$2" ] || [ -n "${REBUILD:-}" ]; then
        echo "echo '  CC $1' && clang $3 -c $1 -o $2" >> "$CMDS"
    fi
}

# Structure offsets for the dynarec's assembly, read back out of a compiled object.
mkdir -p "$OBJ/core/src/asm_defines"
clang $CORE_FLAGS -fno-lto -c "$CORE/src/asm_defines/asm_defines.c" -o "$OBJ/core/src/asm_defines/asm_defines.o"
rm -f "$GEN"/asm_defines_*.h.new
LC_ALL=C grep -a "@ASM_DEFINE" "$OBJ/core/src/asm_defines/asm_defines.o" | sort | while read -r _ name value _; do
    echo "%define $name ($value)" >> "$GEN/asm_defines_nasm.h.new"
    echo "#define $name ($value)" >> "$GEN/asm_defines_gas.h.new"
done
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
for f in $RSP_SOURCES; do
    o="$OBJ/rsp/${f%.c}.o"
    queue "$RSP/src/$f" "$o" "$RSP_FLAGS"
    RSP_OBJS="$RSP_OBJS $o"
done

LINKAGE_OBJ="$OBJ/core/linkage.o"
if [ "$ARCH" = arm64 ]; then
    echo "echo '  AS $LINKAGE' && clang $CORE_FLAGS -c $CORE/src/$LINKAGE -o $LINKAGE_OBJ" >> "$CMDS"
else
    echo "echo '  AS $LINKAGE' && nasm -f macho64 -d LEADING_UNDERSCORE -d PIC -I$CORE/src/ -I$GEN/ -o $LINKAGE_OBJ $CORE/src/$LINKAGE" >> "$CMDS"
fi
CORE_OBJS="$CORE_OBJS $LINKAGE_OBJ"

tr '\n' '\0' < "$CMDS" | xargs -0 -n 1 -P "$JOBS" sh -c

echo "  LD libmupen64plus.dylib"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -dynamiclib -o "$OUT/libmupen64plus.dylib" $CORE_OBJS \
    -install_name @rpath/libmupen64plus.dylib -framework CoreFoundation -lz
echo "  LD mupen64plus-rsp-hle.dylib"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -dynamiclib -o "$OUT/mupen64plus-rsp-hle.dylib" $RSP_OBJS
echo "  LD znx-video-null.dylib"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -Wall -fvisibility=hidden -dynamiclib -I"$CORE/src/api" \
    bench/video-null.c -o "$OUT/znx-video-null.dylib"
echo "  CC znx-bench"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -Wall -I"$CORE/src/api" bench/znx-bench.c bench/bench_core.c -o "$OUT/znx-bench"
# znx-net: RMG-K's P2P rollback netplay, from RMG-K's own sources (build/src copies of ref/RMG-K,
# no changes) plus the glue in net/. See docs/PLAN.md, "RMG-K P2P rollback, in detail".
GEK="$SRC/GekkoLib"
N02="$SRC/n02"
RMGC="$SRC/RMG-Core"
NET_CXX="-arch $ARCH -mmacosx-version-min=11.0 -O2 -fno-strict-aliasing -DNDEBUG -w"
GEK_FLAGS="$NET_CXX -std=gnu++17 -DGEKKONET_STATIC -DGEKKONET_NO_ASIO -I$GEK/include -I$GEK/thirdparty"
N02_FLAGS="$NET_CXX -std=gnu++20 -D__cdecl= -I$N02"
# k_socket.cpp: the POSIX branch (its WSAStartup guard tests "linux"), and sendto/bind through
# net/netsim.cpp (simulated latency, jitter and loss; the socket bound to loopback only).
KSOCK_FLAGS="$N02_FLAGS -Dlinux=1 -Dsendto=znx_netsim_sendto -Dbind=znx_netsim_bind"
RMGC_FLAGS="$NET_CXX -std=gnu++20 -D__cdecl= -DRMGK_HAVE_GEKKONET -DRMGK_HAVE_P2P_TRANSPORT -I$RMGC -I$GEK/include -I$N02"
# RollbackNetcode.cpp: three wrappers renamed so net/rmgk_glue.cpp can watch saves, loads and re-runs.
RBN_FLAGS="$RMGC_FLAGS -DCoreRollbackSaveGameStateInto=rmgk_CoreRollbackSaveGameStateInto -DCoreRollbackLoadGameState=rmgk_CoreRollbackLoadGameState -DCoreRollbackRunFrame=rmgk_CoreRollbackRunFrame"
GLUE_FLAGS="$RMGC_FLAGS -Wall -Wno-unused-function -Ibench -I$CORE/src/api -I$CORE/subprojects/md5"
: > "$CMDS"
NET_OBJS=""
# queue_cxx <source> <object> <flags>
queue_cxx() {
    mkdir -p "$(dirname "$2")"
    if [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ "$0" -nt "$2" ] || [ -n "${REBUILD:-}" ] || \
       { [ -n "${4:-}" ] && [ "$4" -nt "$2" ]; }; then
        echo "echo '  CXX $1' && clang++ $3 -c $1 -o $2" >> "$CMDS"
    fi
    NET_OBJS="$NET_OBJS $2"
}
for f in "$GEK"/src/*.cpp; do queue_cxx "$f" "$OBJ/net/gekko/$(basename "${f%.cpp}").o" "$GEK_FLAGS"; done
for f in core/p2p_core.cpp core/p2p_instruction.cpp stats.cpp errr.cpp; do
    queue_cxx "$N02/$f" "$OBJ/net/n02/$(basename "${f%.cpp}").o" "$N02_FLAGS"
done
queue_cxx "$N02/common/k_socket.cpp" "$OBJ/net/n02/k_socket.o" "$KSOCK_FLAGS"
queue_cxx "$RMGC/rmgk_gekko.cpp" "$OBJ/net/rmgc/rmgk_gekko.o" "$RMGC_FLAGS"
queue_cxx "$RMGC/RollbackNetcode.cpp" "$OBJ/net/rmgc/RollbackNetcode.o" "$RBN_FLAGS"
for f in Error.cpp Library.cpp m64p/Api.cpp m64p/CoreApi.cpp m64p/ConfigApi.cpp; do
    queue_cxx "$RMGC/$f" "$OBJ/net/rmgc/$(basename "${f%.cpp}").o" "$RMGC_FLAGS"
done
for f in net/*.cpp; do queue_cxx "$f" "$OBJ/net/glue/$(basename "${f%.cpp}").o" "$GLUE_FLAGS" net/znx_net.h; done
tr '\n' '\0' < "$CMDS" | xargs -0 -n 1 -P "$JOBS" sh -c
echo "  LD znx-net"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -Wall -c bench/bench_core.c -I"$CORE/src/api" -o "$OBJ/net/bench_core.o"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -w -c "$CORE/subprojects/md5/md5.c" -o "$OBJ/net/md5.o"
clang++ -arch "$ARCH" -mmacosx-version-min=11.0 -o "$OUT/znx-net" $NET_OBJS "$OBJ/net/bench_core.o" "$OBJ/net/md5.o"
echo "  LD znx-input-net.dylib"
clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -Wall -fvisibility=hidden -dynamiclib -I"$CORE/src/api" \
    net/input-net.c -o "$OUT/znx-input-net.dylib"
# The core's ROM database and cheat list (settings per game), read from <program dir>/data.
mkdir -p "$OUT/data"
cp "$CORE"/data/mupen64plus.ini "$CORE"/data/mupencheat.txt "$OUT/data/"
echo "built $OUT/znx-bench $OUT/znx-net"

if [ "${2:-}" = game ]; then
    [ "$ARCH" = arm64 ] || { echo "the picture check build is arm64 only" >&2; exit 1; }
    GLN="$SRC/mupen64plus-video-GLideN64/src"
    . builder/gliden64-sources.sh
    mkdir -p "$GEN/gliden64"
    [ -f "$GEN/gliden64/Revision.h" ] || : > "$GEN/gliden64/Revision.h"
    GLN_FLAGS="$COMMON -fvisibility-inlines-hidden -DMUPENPLUSAPI -DTXFILTER_LIB -DOS_MAC_OS_X -DGL_SILENCE_DEPRECATION -I$GLN -I$GLN/inc -I$GLN/osal -I$GEN/gliden64"
    : > "$CMDS"
    GLN_OBJS=""
    for f in $GLN_SOURCES; do
        o="$OBJ/gliden64/${f%.*}.o"
        mkdir -p "$(dirname "$o")"
        if [ ! -f "$o" ] || [ "$GLN/$f" -nt "$o" ] || [ "$0" -nt "$o" ] || [ "$SRC/.stamp" -nt "$o" ] || [ -n "${REBUILD:-}" ]; then
            case "$f" in
                *.c) echo "echo '  CC $f' && clang $GLN_FLAGS -c $GLN/$f -o $o" >> "$CMDS" ;;
                *)   echo "echo '  CC $f' && clang++ -std=gnu++17 $GLN_FLAGS -c $GLN/$f -o $o" >> "$CMDS" ;;
            esac
        fi
        GLN_OBJS="$GLN_OBJS $o"
    done
    tr '\n' '\0' < "$CMDS" | xargs -0 -n 1 -P "$JOBS" sh -c
    echo "  LD mupen64plus-video-GLideN64.dylib"
    clang++ -arch "$ARCH" -mmacosx-version-min=11.0 -dynamiclib -o "$OUT/mupen64plus-video-GLideN64.dylib" $GLN_OBJS \
        -framework OpenGL
    # The start screen (menu/) for znx-game --menu-shot: FreeType from Homebrew, the system's libcurl,
    # the core's md5.c and minizip, xdelta3's decoder from ref/xdelta (builder/fetch-menu-deps.sh).
    builder/fetch-menu-deps.sh
    FT_FLAGS="$(pkg-config --cflags freetype2 2>/dev/null)" && FT_LIBS="$(pkg-config --libs freetype2)" \
        || { echo "FreeType not found (brew install freetype)" >&2; exit 1; }
    MENU_OBJ="$OBJ/menu"
    mkdir -p "$MENU_OBJ"
    MENU_DEPS="-I$CORE/subprojects/md5 -I$CORE/subprojects/minizip -Iref/xdelta/xdelta3"
    for f in "$CORE/subprojects/md5/md5.c" "$CORE/subprojects/minizip/unzip.c" "$CORE/subprojects/minizip/ioapi.c"; do
        clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -w -DNOCRYPT -DNOUNCRYPT -I"$CORE/subprojects/minizip" \
            -c "$f" -o "$MENU_OBJ/$(basename "${f%.c}").o"
    done
    clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -w $MENU_DEPS -Imenu -c menu/xdelta_apply.c -o "$MENU_OBJ/xdelta_apply.o"
    echo "  CC znx-game"
    clang -arch "$ARCH" -mmacosx-version-min=11.0 -O2 -Wall -I"$CORE/src/api" -Ibench -Igame -Imenu $MENU_DEPS $FT_FLAGS \
        -DZNX_VERSION="\"$(cat VERSION)\"" \
        game/mac/znx-game-mac.c game/mac/vidext_cgl.c game/mac/menu_mac.c game/game_core.c bench/bench_core.c \
        menu/menu.c menu/draw.c menu/catalog.c menu/patcher.c menu/settings.c menu/online.c \
        "$MENU_OBJ"/md5.o "$MENU_OBJ"/unzip.o "$MENU_OBJ"/ioapi.o "$MENU_OBJ"/xdelta_apply.o \
        $FT_LIBS -lcurl -framework OpenGL -lz -o "$OUT/znx-game"
    mkdir -p "$OUT/romfs/fonts"
    cp assets/fonts/BarlowSemiCondensed-Medium.ttf "$OUT/romfs/fonts/"
    echo "built $OUT/znx-game"
fi
