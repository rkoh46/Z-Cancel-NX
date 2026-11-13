#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The one outside source the start screen needs that is not in the repository or a package:
# xdelta3 (github.com/jmacd/xdelta, Apache-2.0), whose VCDIFF decoder builds Smash Remix from the
# vanilla ROM on the console (menu/xdelta_apply.c). Cloned into ref/xdelta at release v3.2.2 if it
# is not there yet; ref/ is never modified afterwards. See THIRD_PARTY_NOTICES.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XDELTA="$ROOT/ref/xdelta"
XDELTA_COMMIT=5f8f5a553181c139d91b0289c009e5c276c5318c   # tag v3.2.2
if [ ! -f "$XDELTA/xdelta3/xdelta3.c" ]; then
    echo "fetching xdelta3 v3.2.2 into ref/xdelta"
    git clone -q https://github.com/jmacd/xdelta.git "$XDELTA"
fi
if [ "$(git -C "$XDELTA" rev-parse HEAD)" != "$XDELTA_COMMIT" ]; then
    git -C "$XDELTA" fetch -q origin tag v3.2.2 2>/dev/null || true
    git -C "$XDELTA" checkout -q "$XDELTA_COMMIT"
fi
