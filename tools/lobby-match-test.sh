#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A lobby match between two copies of znx-net on this Mac, through the mock lobby server
# (net/lobby/test/mock_lobby.py) and the same lobby code the Switch's lobby screens drive
# (net/lobby_play.cpp): connect, open/join a room, ICE over 127.0.0.1 (no STUN, no TURN), pre-match
# sync, then a real emulator match through RMG-K's start_lobby_session. Compares the two sides' check
# lines and prints the reference from znx-bench (2 synchronized ports, like a 2-seat lobby match).
#
#   tools/lobby-match-test.sh ROM [FRAMES] [extra znx-net options for both sides, e.g. --sim-latency 60 ...]
# Output in build/lobby-test/. Nothing leaves 127.0.0.1.
set -euo pipefail
ROM="${1:?usage: $0 ROM [FRAMES] [znx-net options]}"
FRAMES="${2:-17000}"
shift $(( $# >= 2 ? 2 : 1 ))
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT=build/lobby-test
BIN=build/mac-arm64
PORT="${ZNX_LOBBY_TEST_PORT:-18199}"
mkdir -p "$OUT"
COMMON=(--rom "$ROM" --lobby "ws://127.0.0.1:$PORT/ws" --inputs bench/inputs/ssb-usa-vs-mash.txt --frames "$FRAMES"
        --check-every 60 --check-skip 0x80040500 0x80040E00 --lobby-timeout 90 "$@")

python3 -I net/lobby/test/mock_lobby.py --port "$PORT" > "$OUT/mock.out" 2> "$OUT/mock.err" &
MOCK=$!
trap 'kill $MOCK 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q READY "$OUT/mock.out" 2>/dev/null && break; sleep 0.1; done

"$BIN/znx-net" "${COMMON[@]}" --create-room "Mac lobby test" --name mac-host --player 1 > "$OUT/host.out" 2> "$OUT/host.err" &
HOST=$!
sleep 1
"$BIN/znx-net" "${COMMON[@]}" --join-room --name mac-join --player 2 > "$OUT/join.out" 2> "$OUT/join.err" &
JOIN=$!
status=0
wait $HOST || status=1
wait $JOIN || status=1

grep '^check' "$OUT/host.out" > "$OUT/host.check" || true
grep '^check' "$OUT/join.out" > "$OUT/join.check" || true
"$BIN/znx-bench" --rom "$ROM" --frames "$FRAMES" --inputs bench/inputs/ssb-usa-vs-mash.txt --input-players 2 \
    --check-every 20 --check-skip 0x80040500 0x80040E00 2>/dev/null | grep '^check' | awk -v n="$FRAMES" '$2 % 60 == 0 || $2 == n' > "$OUT/bench.check" || true
echo "host:  $(wc -l < "$OUT/host.check") check lines, $(tail -1 "$OUT/host.check")"
echo "join:  $(wc -l < "$OUT/join.check") check lines, $(tail -1 "$OUT/join.check")"
echo "bench: $(wc -l < "$OUT/bench.check") check lines, $(tail -1 "$OUT/bench.check")"
if cmp -s "$OUT/host.check" "$OUT/join.check"; then echo "host = join"; else echo "host != join"; status=1; fi
if cmp -s "$OUT/host.check" "$OUT/bench.check"; then echo "host = bench (same machine as a local run)"; else echo "host != bench"; fi
grep -h "^frames\|^wall\|^rollbacks\|^netsim" "$OUT/host.out" "$OUT/join.out" || true
exit $status
