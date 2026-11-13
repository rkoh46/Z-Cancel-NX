#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode the VS battle setup from an RDRAM dump written by `znx-bench --dump-ram FRAME FILE`.

Prints the scene byte, the stage the next battle will load, and the four player slots of
gSCManagerTransferBattleState (0x800A4D08) and gSCManagerVSBattleState (0x800A4EF8). The addresses
are the same in Super Smash Bros. (USA), Smash Remix 2.0.1 and Smash Remix 2.0.1 +EXTRA 0.6.0
(docs/STAGE_MATRIX.md). The dump is host-order 32-bit words: the byte at N64 address A is at
offset (A & 0x7FFFFF) ^ 3.

usage: tools/read_battle_state.py DUMP [ADDR[:LEN] ...]     extra addresses are printed as hex bytes
"""
import struct
import sys

SCENE = 0x800A4AD0          # gSCManagerSceneData (scene_curr, scene_prev at +1, gkind at +0x0F)
TRANSFER = 0x800A4D08       # gSCManagerTransferBattleState
VS = 0x800A4EF8             # gSCManagerVSBattleState
BATTLE_PTR = 0x800A50E8     # gSCManagerBattleState (pointer to the state the running battle uses)
PLAYERS = 0x20              # offset of players[4] in SCBattleState
PLAYER_SIZE = 0x74
PKIND = {0: "HMN", 1: "CPU", 2: "none"}


class Ram:
    def __init__(self, path):
        raw = open(path, "rb").read()
        words = struct.unpack("<%dI" % (len(raw) // 4), raw)
        self.data = struct.pack(">%dI" % len(words), *words)   # now plain big-endian N64 order

    def bytes(self, addr, n):
        addr &= 0x7FFFFF
        return self.data[addr:addr + n]

    def u8(self, addr):
        return self.bytes(addr, 1)[0]

    def u32(self, addr):
        return struct.unpack(">I", self.bytes(addr, 4))[0]

    def s32(self, addr):
        return struct.unpack(">i", self.bytes(addr, 4))[0]


def show_state(ram, name, base):
    h = ram.bytes(base, 0x20)
    print("%s @ %08X: game_type %d  gkind 0x%02X  teams %d  rules %d (1 time, 2 stock, 3 both)  "
          "humans %d  cpus %d  time %d min  stocks %d (+1)  stage_select %d  damage %d%%  items %08X" % (
              name, base, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[10], h[11], ram.u32(base + 0x0C)))
    for i in range(4):
        p = base + PLAYERS + i * PLAYER_SIZE
        b = ram.bytes(p, 14)
        print("  P%d @ %08X: %-4s fkind 0x%02X  level %d  handicap %d  team %d  port %d  costume %d  shade %d  "
              "stocks left %d  falls %d  damage given %d  fighter gobj %08X" % (
                  i + 1, p, PKIND.get(b[2], str(b[2])), b[3], b[0], b[1], b[4], b[5], b[6], b[7],
                  b[11] if b[11] < 128 else b[11] - 256, ram.s32(p + 0x10), ram.s32(p + 0x34), ram.u32(p + 0x58)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ram = Ram(sys.argv[1])
    print("scene %02X (previous %02X)  gSCManagerSceneData.gkind 0x%02X  gSCManagerBattleState -> %08X" % (
        ram.u8(SCENE), ram.u8(SCENE + 1), ram.u8(SCENE + 0x0F), ram.u32(BATTLE_PTR)))
    print("stage fingerprint: collision lines %d  default BGM 0x%X  time passed %d tics" % (
        ram.u32(0x80131384), ram.u32(0x801313A0), ram.u32(ram.u32(BATTLE_PTR) + 0x18) if ram.u32(BATTLE_PTR) >> 24 == 0x80 else -1))
    show_state(ram, "transfer", TRANSFER)
    show_state(ram, "vs      ", VS)
    for arg in sys.argv[2:]:
        addr, _, n = arg.partition(":")
        print("%s: %s" % (addr, ram.bytes(int(addr, 16), int(n or "16", 0)).hex(" ")))


if __name__ == "__main__":
    main()
