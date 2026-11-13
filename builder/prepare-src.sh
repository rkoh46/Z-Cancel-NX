#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copies the RMG-K core, RSP HLE plugin and GLideN64 video plugin from ref/ into build/src and
# applies patches/*.patch. Also the netplay sources znx-net is built from (net/): GekkoNet
# (GekkoLib, without asio), RMG-K's n02 P2P client (Source/n02) and RMG-Core (only its session
# driver rmgk_gekko.cpp and the thin core wrappers around it are compiled).
# ref/ is never modified. Re-run after changing a patch (the copy is recreated from scratch).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="$ROOT/ref/RMG-K/Source/3rdParty"
SRC="$ROOT/build/src"
STAMP="$SRC/.stamp"

shopt -s nullglob
patches=("$ROOT"/patches/*.patch)
want="$(git -C "$ROOT/ref/RMG-K" rev-parse HEAD; cat "$0" /dev/null ${patches[@]+"${patches[@]}"} | shasum)"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    exit 0
fi

[ -d "$REF/mupen64plus-core" ] || { echo "ref/RMG-K is missing (clone Jay-Day/RMG-K into ref/)" >&2; exit 1; }
rm -rf "$SRC"
mkdir -p "$SRC"
for d in mupen64plus-core mupen64plus-rsp-hle; do
    rsync -a --exclude .git --exclude '_obj*' "$REF/$d" "$SRC/"
done
# GLideN64 as RMG-K ships it: the sources and its per-game settings; not the Qt and Windows parts.
[ -d "$REF/mupen64plus-video-GLideN64/src" ] || { echo "ref/RMG-K has no GLideN64" >&2; exit 1; }
rsync -a --exclude .git --exclude translations --exclude projects --exclude GLideNUI-wtl \
    "$REF/mupen64plus-video-GLideN64" "$SRC/"
# Netplay: GekkoNet's library (its asio copy is not needed: RMG-K's P2P mode carries GekkoNet's
# packets over the n02 socket), the n02 client and RMG-Core.
rsync -a --exclude .git --exclude 'thirdparty/asio' "$ROOT/ref/RMG-K/GekkoNet-main/GekkoLib" "$SRC/"
rsync -a --exclude .git "$ROOT/ref/RMG-K/Source/n02" "$ROOT/ref/RMG-K/Source/RMG-Core" "$SRC/"
# Generated per architecture into the build directory instead.
rm -f "$SRC"/mupen64plus-core/src/asm_defines/asm_defines_*.h
for p in ${patches[@]+"${patches[@]}"}; do
    echo "applying $(basename "$p")"
    patch -d "$SRC" -p1 --no-backup-if-mismatch < "$p"
done
echo "$want" > "$STAMP"
