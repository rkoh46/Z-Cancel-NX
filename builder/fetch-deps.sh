#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetches the sources Z-Cancel NX is built from that are not in this repository, at the exact
# commits it is tested with, into ref/ (never modified afterwards; our changes are patches/):
#   RMG-K (github.com/Jay-Day/RMG-K, GPL-3.0): the mupen64plus core with rollback, RSP HLE, GLideN64,
#     GekkoNet, the n02 P2P client, the session driver, libjuice. Release v0.9.16.
#   xdelta3 (github.com/jmacd/xdelta, Apache-2.0): builder/fetch-menu-deps.sh.
# The game is never fetched: you bring your own ROM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RMGK="$ROOT/ref/RMG-K"
RMGK_COMMIT=2f5454afe2db40cc56ecf7fd3284f703969d085c   # tag v0.9.16
mkdir -p "$ROOT/ref"
if [ ! -d "$RMGK/.git" ]; then
    echo "fetching RMG-K v0.9.16 into ref/RMG-K"
    git clone -q https://github.com/Jay-Day/RMG-K.git "$RMGK"
fi
if [ "$(git -C "$RMGK" rev-parse HEAD)" != "$RMGK_COMMIT" ]; then
    git -C "$RMGK" fetch -q origin "$RMGK_COMMIT" 2>/dev/null || git -C "$RMGK" fetch -q --tags origin
    git -C "$RMGK" checkout -q "$RMGK_COMMIT"
fi
"$ROOT/builder/fetch-menu-deps.sh"
echo "ref/ is ready: RMG-K $(git -C "$RMGK" rev-parse --short HEAD), xdelta $(git -C "$ROOT/ref/xdelta" rev-parse --short HEAD)"
