#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Memory pokes that start a VS match on any stage with any fighters, in vanilla Super Smash Bros. (USA),
Smash Remix 2.0.1 and Smash Remix 2.0.1 +EXTRA 0.6.0 (the addresses are the same in all three).

The input script only has to reach the mode select screen (bench/inputs/boot-to-mode-select.txt: START at
frames 200 and 420). On the frame the scene byte 0x800A4AD0 first reads 0x07 (mode select requested, old
scene still unwinding) the pokes below are applied and the scene manager then loads the VS battle (0x16)
instead of the menu. See docs/STAGE_MATRIX.md for the proof and the limits.

Output, one poke per line:   when 800A4AD0 07 0 <address> <hex bytes>
("when the byte at 800A4AD0 first equals 07, 0 frames later, write these bytes there"; big-endian N64
addresses, bytes in N64 order). --c prints a C table instead.

usage: tools/vs_poke_plan.py --stage 0x04 --fighters 0x01,0x03[,0x05,0x09] [--level 9] [--human 1]
                             [--time MIN|inf] [--stocks N] [--items 0..5] [--costumes a,b,c,d] [--c]
  --stage     gkind / Remix stage id (docs/STAGE_MATRIX.md)
  --fighters  2 to 4 fighter kinds, port 1 first; "-" leaves a port empty (e.g. 0x01,-,0x03)
  --level     CPU level 1..9 for every CPU (default 9)
  --human N   port N (1..4) is a human instead of a CPU (it then follows the input script)
  --time      time match of MIN minutes; inf (default) never ends: the benchmark's choice
  --stocks N  stock match with N stocks instead (ends when one fighter is left)
  --items R   item appearance rate 0 none .. 5 very high (default: leave the game's setting)
"""
import sys

SCENE = 0x800A4AD0            # gSCManagerSceneData.scene_curr
SCENE_GKIND = 0x800A4ADF      # gSCManagerSceneData.gkind: copied into the battle state when the battle scene starts
TRANSFER = 0x800A4D08         # gSCManagerTransferBattleState: what the VS battle scene runs on
PLAYER0 = TRANSFER + 0x20
PLAYER_SIZE = 0x74
TRIGGER_SCENE = 0x07          # nSCKindModeSelect
BATTLE_SCENE = 0x16           # nSCKindVSBattle
TIME_INFINITE = 100           # SCBATTLE_TIMELIMIT_INFINITE


def plan(stage, fighters, level=9, human=None, time="inf", stocks=None, items=None, costumes=None):
    """[(address, bytes)] in the order they should be written (the scene byte last)."""
    pokes = []
    seen = {}
    humans = cpus = 0
    rules = 2 if stocks else 1
    for port in range(4):
        fkind = fighters[port] if port < len(fighters) else None
        base = PLAYER0 + port * PLAYER_SIZE
        if fkind is None:
            pokes.append((base + 2, bytes([2, 0x1C])))        # pkind none, fkind NONE
            continue
        is_human = human == port + 1
        humans += is_human
        cpus += not is_human
        shade = seen.get(fkind, 0)                             # same fighter twice: a different shade
        seen[fkind] = shade + 1
        costume = costumes[port] if costumes and port < len(costumes) else 0
        pokes.append((base, bytes([
            level,                      # +0x00 level (CPU level 1..9)
            9,                          # +0x01 handicap
            0 if is_human else 1,       # +0x02 pkind: 0 human, 1 CPU, 2 none
            fkind,                      # +0x03 fkind
            port,                       # +0x04 team
            port,                       # +0x05 player (port)
            costume,                    # +0x06 costume
            shade,                      # +0x07 shade
            port if is_human else 4,    # +0x08 color (4: the grey CPU colour)
            1 if rules == 1 else 0,     # +0x09 is_single_stockicon (time matches)
            port if is_human else 4,    # +0x0A tag (1P..4P, or CP)
        ])))
    time_limit = TIME_INFINITE if time == "inf" else int(time)
    pokes.append((TRANSFER + 1, bytes([
        stage,                          # +0x01 gkind
        0,                              # +0x02 is_team_battle
        rules,                          # +0x03 game_rules: 1 time, 2 stock, 3 both
        humans,                         # +0x04 pl_count
        cpus,                           # +0x05 cp_count
        time_limit,                     # +0x06 time_limit, minutes; 100 = infinite
        (stocks - 1) if stocks else 0,  # +0x07 stocks, minus one
    ])))
    if items is not None:
        pokes.append((TRANSFER + 0x1C, bytes([items])))   # item_appearance_rate
    pokes.append((SCENE_GKIND, bytes([stage])))
    pokes.append((SCENE, bytes([BATTLE_SCENE])))
    return pokes


def main():
    args = sys.argv[1:]
    opt = {"--level": "9", "--time": "inf"}
    flags = set()
    i = 0
    while i < len(args):
        if args[i] == "--c":
            flags.add("c")
            i += 1
        elif args[i].startswith("--") and i + 1 < len(args):
            opt[args[i]] = args[i + 1]
            i += 2
        else:
            sys.exit(__doc__)
    if "--stage" not in opt or "--fighters" not in opt:
        sys.exit(__doc__)
    fighters = [None if f.strip() == "-" else int(f, 0) for f in opt["--fighters"].split(",")]
    if not 2 <= sum(f is not None for f in fighters) <= 4 or len(fighters) > 4:
        sys.exit("2 to 4 fighters")
    pokes = plan(int(opt["--stage"], 0), fighters, int(opt["--level"]),
                 int(opt["--human"]) if "--human" in opt else None, opt["--time"],
                 int(opt["--stocks"]) if "--stocks" in opt else None,
                 int(opt["--items"]) if "--items" in opt else None,
                 [int(c, 0) for c in opt["--costumes"].split(",")] if "--costumes" in opt else None)
    if "c" in flags:
        print("/* when RDRAM byte 0x%08X first reads 0x%02X: */" % (SCENE, TRIGGER_SCENE))
        for addr, data in pokes:
            print("{ 0x%08X, %d, { %s } }," % (addr, len(data), ", ".join("0x%02X" % b for b in data)))
    else:
        for addr, data in pokes:
            print("when %08X %02X 0 %08X %s" % (SCENE, TRIGGER_SCENE, addr, data.hex().upper()))


if __name__ == "__main__":
    main()
