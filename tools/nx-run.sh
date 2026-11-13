#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Launch a Z-Cancel NX build on a Switch over the network and stream its output back.
#   NX_IP=<console address> tools/nx-run.sh [nro [arguments...]]   (default build/switch/znx-bench.nro;
#   arguments reach the program as argv[1..], e.g. a ROM path on the SD card)
# Needs the console tooling of the sister project Melee-NX-NetPlay (nx-bot.py for sys-botbase, nxlink):
# MELEE_NX_TOOLS, default ~/Melee-NX-NetPlay/tools. The console opens the Homebrew Menu with a game's
# memory (hold R on a game), its netloader takes the NRO, stdout comes back and is kept in build/nxlink-logs.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"; repo="$(cd "$here/.." && pwd -P)"
ip="${NX_IP:?set NX_IP to the console address}"
mt="${MELEE_NX_TOOLS:-$HOME/Melee-NX-NetPlay/tools}"
nro="${1:-$repo/build/switch/znx-bench.nro}"; [[ $# -gt 0 ]] && shift
[[ -f "$nro" ]] || { echo "no $nro (bash builder/build-switch.sh)" >&2; exit 66; }
py="$mt/../build/venv/bin/python"; [[ -x "$py" ]] || py=python3
mkdir -p "$repo/build/nxlink-logs" "$repo/build/launch"
log="$repo/build/nxlink-logs/$(date +%Y%m%d-%H%M%S)-$(basename "$nro" .nro).log"
echo "== opening the Homebrew Menu's netloader"
"$py" "$mt/nx-bot.py" "$ip" hbmenu-netloader "$repo/build/launch" || { echo "could not reach the Homebrew Menu"; exit 1; }
echo "== sending $(du -h "$nro" | cut -f1); output also in $log"
deadline=$(( $(date +%s) + ${NX_WAIT:-120} ))
while :; do
  set +e
  "$mt/bin/nxlink" -a "$ip" -r 20 -s -p "z-cancel-nx/$(basename "$nro")" "$nro" "$@" 2>&1 </dev/null | tee -a "$log"
  set -e
  if ! tail -c 200 "$log" | grep -q "Connection to .* failed"; then break; fi
  (( $(date +%s) >= deadline )) && { echo "gave up: the netloader never answered"; exit 1; }
  : > "$log"; sleep 1
done
