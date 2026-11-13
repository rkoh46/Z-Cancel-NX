#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds a release of Z-Cancel NX: the NRO in the folder layout of an SD card, zipped.
#   tools/release.sh            build dist/Z-Cancel-NX-v<VERSION>.zip from a clean checkout
#   tools/release.sh --publish  also tag v<VERSION>, push the tag and create the GitHub release
# The version is the VERSION file (shown in the app's top right corner); CHANGELOG.md must have a
# "## <VERSION>" section, which becomes the release notes. The zip holds no game: players bring
# their own ROM. Source for everything in the NRO is this repository at the tag plus the pinned
# sources builder/fetch-deps.sh fetches.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="$(tr -d ' \n' < VERSION)"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "VERSION must be X.Y.Z, found '$VERSION'" >&2; exit 64; }
grep -q "^## $VERSION\b" CHANGELOG.md || { echo "CHANGELOG.md has no '## $VERSION' section" >&2; exit 64; }
[ -z "$(git status --porcelain)" ] || { echo "the working tree has uncommitted changes" >&2; exit 65; }

builder/fetch-deps.sh
REBUILD=1 bash builder/build-switch.sh game
NRO="build/switch/z-cancel-nx.nro"
[ -f "$NRO" ] || { echo "no $NRO" >&2; exit 66; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/switch/z-cancel-nx" dist
cp "$NRO" "$STAGE/switch/z-cancel-nx/z-cancel-nx.nro"
cp LICENSE THIRD_PARTY_NOTICES.md "$STAGE/switch/z-cancel-nx/"
cat > "$STAGE/READ-ME.txt" <<TXT
Z-Cancel NX $VERSION
Super Smash Bros. (N64) and Smash Remix with rollback netplay for a Nintendo Switch running Atmosphere.

1. Copy the "switch" folder onto the root of your SD card.
2. Copy your own Super Smash Bros. (USA) ROM into switch/z-cancel-nx/ (.z64, .n64 or .v64; any name).
3. Open the Homebrew Menu by holding R while starting a game (the Album shortcut does not give
   homebrew enough memory), and start Z-Cancel NX.

No game is included. Guide, controls and source: https://github.com/rkoh46/Z-Cancel-NX
Licence: GNU GPL v3 (LICENSE). Third-party notices: THIRD_PARTY_NOTICES.md.
TXT
ZIP="dist/Z-Cancel-NX-v$VERSION.zip"
rm -f "$ZIP"
(cd "$STAGE" && zip -qr -X "$ROOT/$ZIP" READ-ME.txt switch)
# Nothing from a game may be in a release.
if unzip -l "$ZIP" | grep -qiE '\.(z64|n64|v64|rom|xdelta)$'; then echo "a game file is in $ZIP" >&2; exit 70; fi
echo "built $ZIP ($(du -h "$ZIP" | cut -f1))"
shasum -a 256 "$ZIP"

if [ "${1:-}" = "--publish" ]; then
    NOTES="$(awk -v v="$VERSION" '$0 ~ "^## " v "([^0-9.]|$)" { on = 1; next } on && /^## / { exit } on' CHANGELOG.md)"
    git tag -a "v$VERSION" -m "Z-Cancel NX $VERSION"
    git push origin "v$VERSION"
    gh release create "v$VERSION" "$ZIP" --title "Z-Cancel NX $VERSION" --notes "$NOTES"
fi
