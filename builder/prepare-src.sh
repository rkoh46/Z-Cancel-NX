#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copies the RMG-K core and RSP HLE plugin from ref/ into build/src and applies patches/*.patch.
# ref/ is never modified. Re-run after changing a patch (the copy is recreated from scratch).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="$ROOT/ref/RMG-K/Source/3rdParty"
SRC="$ROOT/build/src"
STAMP="$SRC/.stamp"

shopt -s nullglob
patches=("$ROOT"/patches/*.patch)
want="$(git -C "$ROOT/ref/RMG-K" rev-parse HEAD; cat /dev/null ${patches[@]+"${patches[@]}"} | shasum)"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    exit 0
fi

[ -d "$REF/mupen64plus-core" ] || { echo "ref/RMG-K is missing (clone Jay-Day/RMG-K into ref/)" >&2; exit 1; }
rm -rf "$SRC"
mkdir -p "$SRC"
for d in mupen64plus-core mupen64plus-rsp-hle; do
    rsync -a --exclude .git --exclude '_obj*' "$REF/$d" "$SRC/"
done
# Generated per architecture into the build directory instead.
rm -f "$SRC"/mupen64plus-core/src/asm_defines/asm_defines_*.h
for p in ${patches[@]+"${patches[@]}"}; do
    echo "applying $(basename "$p")"
    patch -d "$SRC" -p1 --no-backup-if-mismatch < "$p"
done
echo "$want" > "$STAMP"
