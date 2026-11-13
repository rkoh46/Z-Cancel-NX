#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a bench/inputs script that goes from power-on into a VS match on a chosen stage with chosen
fighters, by controller input only (no memory pokes), for Super Smash Bros. (USA), Smash Remix 2.0.1 and
Smash Remix 2.0.1 +EXTRA 0.6.0. The poke start (tools/vs_poke_plan.py) is cheaper and reaches every
stage and fighter; this is the fallback and the cross-check. Method and limits: docs/STAGE_MATRIX.md.

What the script does (frames are the harness's frames, one controller poll each):
  200, 420   START: skip the intro, leave the title           (scene 01 -> 07 at 421)
  540, 600   d-pad down, A: VS Mode                           (scene 09 at 601)
  720        A on VS Start                                    (scene 10 at 721: character select)
  801...     every port at once, with its own controller: carry the puck to the portrait, A; (Remix: d-pad
             for a variant while the puck is held;) cursor to the port's HMN/CPU button, A (the placed
             fighter stays, the port becomes a CPU); cursor to the port's level arrow, A x (level - 3)
  then       port 1: cursor to the time arrow, A x n (vanilla: 3 min -> infinite), START   (scene 15)
  +120...    stage select: (Remix: R per page;) d-pad right/down to the icon, (Remix: C-down per variant,) A
             (scene 16: battle)
  --mash     afterwards the human ports (if any) mash seeded pseudo-random sticks and buttons, never START

usage: tools/make_vs_script.py --rom vanilla|remix|extra --stage NAME --fighters A,B[,C,D]
           [--level 9] [--humans] [--time-presses N] [--mash SEED] [--frames N] [-o FILE] [--verify]
           [--list]
  --stage     a stage name or id from --list (vanilla: the 8 starter stages; Mushroom Kingdom is locked on
              the blank save the harness runs with. Remix: any stage on the stage select pages or a variant)
  --fighters  2 to 4 names from --list, port 1 first (Remix: NAME or a variant such as JFOX, NFOX)
  --humans    leave the ports human (no CPU button, no level)
  --verify    run build/mac-arm64/znx-bench on the script and print the battle state read from RDRAM
  --no-tune   do not run the harness while generating (open loop: exact in vanilla, not in Remix, where
              loading a portrait's model drops game ticks; by default each cursor move is checked against
              sMNPlayersVSSlots in RDRAM and corrected, and the generator stops if a step did not take)
"""
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ROMS = {
    # your own Super Smash Bros. (USA) ROM: set ZNX_VANILLA_ROM, or put it at build/roms/vanilla.z64
    "vanilla": os.environ.get("ZNX_VANILLA_ROM", os.path.join(REPO, "build/roms/vanilla.z64")),
    "remix": os.path.join(REPO, "build/remix/remix-2.0.1.z64"),
    "extra": os.path.join(REPO, "build/remix/remix-extra-0.6.0.z64"),
}

A, B, Z, START = 0x80000000, 0x40000000, 0x20000000, 0x10000000
D_UP, D_DOWN, D_LEFT, D_RIGHT = 0x08000000, 0x04000000, 0x02000000, 0x01000000
R, C_DOWN = 0x00100000, 0x00040000

# ---- vanilla tables (ssb-decomp-re: mnplayersvs.c, mnmaps.c) ----
VANILLA_FIGHTERS = {"MARIO": 0, "FOX": 1, "DONKEY": 2, "DK": 2, "SAMUS": 3, "LUIGI": 4, "LINK": 5, "YOSHI": 6,
                    "CAPTAIN": 7, "FALCON": 7, "KIRBY": 8, "PIKACHU": 9, "JIGGLYPUFF": 10, "PURIN": 10, "NESS": 11}
VANILLA_PORTRAIT = [1, 9, 2, 4, 0, 3, 7, 5, 8, 10, 11, 6]      # fkind -> portrait (6 columns, 2 rows)
VANILLA_LOCKED = {4, 7, 10, 11}                                # locked on a blank save
VANILLA_STAGES = {"PEACHS_CASTLE": 0, "SECTOR_Z": 1, "CONGO_JUNGLE": 2, "PLANET_ZEBES": 3, "HYRULE_CASTLE": 4,
                  "YOSHIS_ISLAND": 5, "DREAM_LAND": 6, "SAFFRON_CITY": 7, "MUSHROOM_KINGDOM": 8, "RANDOM": 0xDE}
VANILLA_SLOTS = [0, 2, 4, 3, 8, 5, 6, 1, 7, 0xDE]              # stage select slot -> gkind (5 columns, 2 rows)

# ---- Remix tables (smashremix 2.0.1: CharacterSelect.asm layout, Stages.asm stage_table_normal) ----
REMIX_SLOTS = ("MARINA DRM LUIGI MARIO DONKEY LINK SAMUS CAPTAIN GND SONIC "
               "DEDEDE YLINK NESS YOSHI KIRBY FOX PIKACHU JIGGLYPUFF FALCO SHEIK "
               "GOEMON CRASH WARIO PEACH BOWSER WOLF CONKER MTWO MARTH BANJO").split()
REMIX_PAGES = [
    "PEACHS_CASTLE CONGO_JUNGLE HYRULE_CASTLE PLANET_ZEBES MUSHROOM_KINGDOM META_CRYSTAL YOSHIS_ISLAND DREAM_LAND "
    "SECTOR_Z SAFFRON_CITY DUEL_ZONE FINAL_DESTINATION DRAGONKING FIRST_DESTINATION DREAM_LAND_BETA_1 HOW_TO_PLAY "
    "BATTLEFIELD RANDOM",
    "ZLANDING GANONS_TOWER SPIRALM COOLCOOL DR_MARIO BOWSERB N64 DEKU_TREE MADMM GB_LAND MUTE KITCHEN FROSTY "
    "FRAYS_STAGE WARIOWARE GYM_LEADER_CASTLE POKEMON_STADIUM RANDOM",
    "TALTAL SMASHVILLE2 REAPERS RAIDBLUE GREAT_BAY FOD TOH SMASHKETBALL NORFAIR DELFINO PEACH2 BLUE ONETT GLACIAL "
    "HTEMPLE NPC FALLS RANDOM",
    "FLAT_ZONE OSOHE YOSHI_STORY_2 GERUDO GOOMBA_ROAD BOWSERS_KEEP RITH_ESSA VENOM WINDY DATA CLANCER JAPES CSIEGE "
    "YOSHIS_ISLAND_II GHZ SUBCON PIRATE RANDOM",
    "CASINO MMADNESS RAINBOWROAD TOADSTURNPIKE DRACULAS_CASTLE MT_DEDEDE EDO TWILIGHT_CITY MELRODE SCUTTLE_TOWN "
    "BIG_BOOS_HAUNT YOSHIS_ISLAND_MELEE SPAWNED_FEAR POKEFLOATS BIG_SNOWMAN DISCOVERY_FALLS TIME_TWISTER RANDOM",
]
EXTRA_PAGE = ("STAGE_BATTLEHARBOR STAGE_EMERALDCOAST STAGE_EMERALDSHRINE STAGE_HALBERD STAGE_LAUNCHBASE "
              "STAGE_PUMPKINHILL STAGE_RADICALHIGHWAY STAGE_SKYSANCTUARY STAGE_SUZAKUCASTLE STAGE_THEPIT "
              "STAGE_TRAININGSTAGE STAGE_TREETOPTOWN STAGE_WAFFLEHOUSE RANDOM RANDOM RANDOM RANDOM RANDOM")
VARIANT_ORDER = ["DL", "OMEGA", "REMIX", "REMIX2", "REMIX3", "REMIX4", "REMIX5", "REMIX6"]   # C-down cycles these
CHAR_VARIANT_DPAD = {"SPECIAL": D_UP, "POLYGON": D_DOWN, "J": D_LEFT, "E": D_RIGHT}


def remix_ids(extra):
    """Stage and character rows from tools/remix_ids.py (needs ref/smashremix, ref/smashremix-plus-extra)."""
    sys.path.insert(0, HERE)
    import remix_ids as ids
    stages = ids.remix_stages()
    chars = ids.remix_characters()
    if extra:
        stages += ids.extra_stages(stages[-1][0] + 1)
        chars += ids.extra_characters(chars[-1][0] + 1)
    return stages, chars


class Script:
    def __init__(self):
        self.events = []          # (frame, player, word, comment)

    def hold(self, player, frame, frames, word, comment=""):
        self.events.append((frame, player, word, comment))
        self.events.append((frame + frames, player, 0, ""))
        return frame + frames

    def tap(self, player, frame, word, comment="", down=4, gap=8):
        self.hold(player, frame, down, word, comment)
        return frame + down + gap

    def move(self, player, frame, pos, target, speed=4):
        """Walk the character select cursor from pos to target: the game adds stick/20 pixels a frame, so
        +-80 is 4 px and +-20 is 1 px. Returns the frame after the move."""
        dx, dy = target[0] - pos[0], target[1] - pos[1]
        last = None
        start = frame
        while dx or dy:
            sx = max(-speed, min(speed, dx))
            sy = max(-speed, min(speed, dy))
            dx -= sx
            dy -= sy
            word = (((sx * 20) & 0xFF) << 8) | ((-sy * 20) & 0xFF)     # stick y is up-positive, screen y is down
            if word != last:
                self.events.append((frame, player, word, "cursor to (%d, %d)" % target if frame == start else ""))
                last = word
            frame += 1
        self.events.append((frame, player, 0, ""))
        return frame + 4

    def write(self, out, header):
        merged = {}
        order = 0
        for frame, player, word, comment in self.events:     # a later event on the same frame wins
            merged[(frame, player)] = (order, word, comment or merged.get((frame, player), (0, 0, ""))[2])
            order += 1
        for line in header:
            out.write("# " + line + "\n")
        for (frame, player), (_, word, comment) in sorted(merged.items()):
            out.write("%d %d %08X%s\n" % (frame, player, word, "   # " + comment if comment else ""))


SLOTS, SLOT_SIZE = 0x8013BA88, 0xBC      # sMNPlayersVSSlots (the same address in the Remix ROMs)


def probe(script, rom, frame):
    """RDRAM after `frame` frames of the script so far (build/mac-arm64/znx-bench --dump-ram)."""
    sys.path.insert(0, HERE)
    import read_battle_state as rbs
    with tempfile.TemporaryDirectory() as tmp:
        path, dump = os.path.join(tmp, "in.txt"), os.path.join(tmp, "ram.bin")
        with open(path, "w") as out:
            script.write(out, [])
        subprocess.run([os.path.join(REPO, "build/mac-arm64/znx-bench"), "--rom", ROMS[rom], "--frames", str(frame),
                        "--inputs", path, "--hash-every", "0", "--dump-ram", str(frame), dump],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        return rbs.Ram(dump)


def pos_after(port, picks, remix):
    """Where the port's cursor is after placing its puck (the same expression as the portrait target)."""
    col, row = picks[port][0], picks[port][1]
    nudge = port * 4 - 6
    return (37 + 24 * col + nudge // 2, 67 + 24 * row) if remix else (23 + 45 * col + nudge, 59 + 43 * row)


def build(rom, stage_name, fighter_names, level, humans, time_presses, mash_seed, frames, tune):
    s = Script()
    remix = rom != "vanilla"
    note = []

    # ---- resolve the fighters to (portrait column, row, d-pad variant, kind id) ----
    picks = []
    if remix:
        stages, chars = remix_ids(rom == "extra")
        by_name = {c[1]: c for c in chars}
        for n in VANILLA_FIGHTERS:                    # vanilla names for the vanilla kinds, and their polygons
            by_name.setdefault(n, (VANILLA_FIGHTERS[n], n, "", ""))
            by_name.setdefault("N" + n, (VANILLA_FIGHTERS[n] + 0x0E, "N" + n, n, "POLYGON"))
        for name in fighter_names:
            c = by_name.get(name.upper())
            if c is None:
                sys.exit("unknown fighter %s (see --list)" % name)
            base, dpad = c[1], 0
            if base not in REMIX_SLOTS and c[3] in CHAR_VARIANT_DPAD:
                # a variant sits on its parent's portrait, behind a d-pad direction. Which portrait: the
                # constant with the J/E/N prefix removed when that is a slot, else the parent's slot.
                for cand in (base[1:], c[2]):
                    if cand in REMIX_SLOTS:
                        base, dpad = cand, CHAR_VARIANT_DPAD[c[3]]
                        break
            aliases = {"DK": "DONKEY", "FALCON": "CAPTAIN", "PURIN": "JIGGLYPUFF", "PUFF": "JIGGLYPUFF"}
            base = aliases.get(base, base)
            if base not in REMIX_SLOTS:
                sys.exit("%s is not on Remix 2.0.1's character select grid and not a d-pad variant of a "
                         "fighter that is; start it with tools/vs_poke_plan.py instead" % name)
            slot = REMIX_SLOTS.index(base)
            picks.append((slot % 10, slot // 10, dpad, c[0]))
    else:
        for name in fighter_names:
            if name.upper() not in VANILLA_FIGHTERS:
                sys.exit("unknown fighter %s (see --list)" % name)
            fkind = VANILLA_FIGHTERS[name.upper()]
            if fkind in VANILLA_LOCKED:
                sys.exit("%s is locked on a blank save; start it with tools/vs_poke_plan.py instead" % name)
            portrait = VANILLA_PORTRAIT[fkind]
            picks.append((portrait % 6, portrait // 6, 0, fkind))

    # ---- menus to the character select (the same frames work in all three ROMs) ----
    s.tap(1, 200, START, "START: skip the intro")
    s.tap(1, 420, START, "START at the title -> mode select (scene 07 at 421)")
    s.tap(1, 540, D_DOWN, "d-pad down -> VS Mode")
    s.tap(1, 600, A, "A -> VS menu (scene 09 at 601)")
    s.tap(1, 720, A, "A on VS Start -> character select (scene 10 at 721)")

    # ---- character select: every port works its own panel, all at once ----
    # Open loop the cursor is exact in vanilla. In Remix a puck crossing a portrait loads a model and the
    # game drops ticks, for every port's cursor, so with tune=True the generator runs the harness after
    # each move, reads where each cursor really is (sMNPlayersVSSlots[p].cursor -> SObj position) and
    # appends a correction until all are on target. The script it writes is still a plain input script.
    ports = range(len(picks))

    def settle(frame, targets, pos):
        """Corrections until every port's cursor is at targets[port]; returns the frame after."""
        for _ in range(8):
            if not tune:
                return frame
            ram = probe(s, rom, frame + 6)
            end, moved = frame + 8, False
            for port in ports:
                sobj = ram.u32(ram.u32(SLOTS + port * SLOT_SIZE) + 0x74)
                actual = tuple(int(round(v)) for v in struct.unpack(">ff", ram.bytes(sobj + 0x58, 8)))
                if actual != targets[port]:
                    end = max(end, s.move(port + 1, frame + 8, actual, targets[port]))
                    moved = True
            if not moved:
                return frame
            frame = end
        sys.exit("the cursors did not settle; see docs/STAGE_MATRIX.md")

    def check(frame, what, offset, wanted):
        if tune:
            ram = probe(s, rom, frame)
            for port in ports:
                got = ram.u32(SLOTS + port * SLOT_SIZE + offset)
                if got != wanted[port]:
                    sys.exit("port %d: %s is 0x%X, wanted 0x%X. (A Remix variant that is not behind the d-pad direction of its "
                             "kind on its parent's portrait cannot be picked by this script: use tools/vs_poke_plan.py.)"
                             % (port + 1, what, got, wanted[port]))

    pos = {port: (40 + 68 * port, 170) for port in ports}             # mnPlayersVSMakeCursor
    targets = {}
    for port, (col, row, dpad, kind) in enumerate(picks):
        nudge = port * 4 - 6                              # keep pucks on one portrait from sitting on each other
        if remix:
            # measured by sweeping the cursor (docs/STAGE_MATRIX.md): column c is cursor x 25+24c..48+24c,
            # row r is cursor y 55+24r..78+24r
            targets[port] = (37 + 24 * col + nudge // 2, 67 + 24 * row)
        else:
            targets[port] = (23 + 45 * col + nudge, 59 + 43 * row)   # 45 px portraits from x 25, rows 36..78 and 79..121
    f = 801
    for port in ports:                                    # along the bottom first, then straight up
        mid = (targets[port][0], pos[port][1])
        f = max(f, s.move(port + 1, s.move(port + 1, 801, pos[port], mid), mid, targets[port], speed=2))
    f = settle(f, targets, pos) + 8
    for port, (col, row, dpad, kind) in enumerate(picks):
        g = f
        if dpad:
            g = s.tap(port + 1, g, dpad, "d-pad: the variant (while the puck is held)")
        s.tap(port + 1, g + 4, A, "A: place the puck (fighter kind 0x%02X)" % kind)
    f += 40
    check(f, "fighter kind", 0x48, [p[3] for p in picks])
    end = f
    if not humans:
        targets = {port: (69 * port + 54, 133) for port in ports}     # mnPlayersVSCheckPlayerKindSelectInRange
        g = f
        for port in ports:
            g = max(g, s.move(port + 1, f, pos_after(port, picks, remix), targets[port]))
        f = settle(g, targets, pos) + 4
        for port in ports:
            s.tap(port + 1, f, A, "A on the HMN/CPU button -> CPU")
        f += 20
        check(f, "player kind", 0x84, [1] * len(picks))
        arrows = {port: (69 * port + 59, 200) for port in ports}      # mnPlayersVSCheckHandicapArrowRInRange
        g = f
        for port in ports:
            g = max(g, s.move(port + 1, f, targets[port], arrows[port]))
        f = settle(g, arrows, pos) + 4
        for i in range(max(0, level - 3)):                # CPUs start at level 3
            for port in ports:
                s.tap(port + 1, f, A, "A on the level arrow x %d -> level %d" % (level - 3, level) if i == 0 else "")
            f += 12
        check(f + 4, "CPU level", 0x38, [level] * len(picks))
        targets = arrows
    else:
        targets = {port: pos_after(port, picks, remix) for port in ports}
    if time_presses:
        arrow = (200, 20)                                 # mnPlayersVSCheckTimeArrowRInRange
        g = s.move(1, f + 6, targets[0], arrow)
        f = settle(g, {**targets, 0: arrow}, pos) + 4
        for i in range(time_presses):
            f = s.tap(1, f, A, "A on the time/stock arrow x %d" % time_presses if i == 0 else "")
    end = f
    start = end + 30
    s.tap(1, start, START, "START -> stage select (scene 15 about 31 frames later)")

    # ---- stage select ----
    f = start + 150
    if remix:
        by_const = {r[1]: r for r in stages}
        by_title = {re.sub(r"[^a-z0-9]", "", r[2].lower()): r for r in stages}
        row_ = by_const.get(stage_name.upper()) or by_title.get(re.sub(r"[^a-z0-9]", "", stage_name.lower()))
        if row_ is None and re.match(r"0x[0-9a-fA-F]+$", stage_name):
            row_ = next((r for r in stages if r[0] == int(stage_name, 16)), None)
        if row_ is None:
            sys.exit("unknown stage %s (see --list)" % stage_name)
        pages = [p.split() for p in REMIX_PAGES] + ([EXTRA_PAGE.split()] if rom == "extra" else [])
        base, presses = row_[1], 0
        if row_[4]:                                       # a variant: go to its base stage, then C-down
            base = row_[4] if rom != "extra" or not row_[1].startswith("STAGE_") else "STAGE_" + row_[4]
            present = sorted({VARIANT_ORDER.index(r[5]) for r in stages
                              if r[4] == row_[4] and r[5] in VARIANT_ORDER and r[1].startswith("STAGE_") == row_[1].startswith("STAGE_")})
            presses = present.index(VARIANT_ORDER.index(row_[5])) + 1
        where = [(p, i) for p, page in enumerate(pages) for i, n in enumerate(page) if n == base]
        if not where:
            sys.exit("%s is not on the stage select pages (nor a variant of a stage that is); start it with "
                     "tools/vs_poke_plan.py instead" % row_[1])
        page, index = where[0]
        for i in range(page):
            f = s.tap(1, f, R, "R: next page x %d" % page if i == 0 else "", down=3, gap=17)
        for i in range(index % 6):
            f = s.tap(1, f, D_RIGHT, "d-pad right x %d" % (index % 6) if i == 0 else "", down=2, gap=14)
        for i in range(index // 6):
            f = s.tap(1, f, D_DOWN, "d-pad down x %d" % (index // 6) if i == 0 else "", down=2, gap=14)
        for i in range(presses):
            f = s.tap(1, f, C_DOWN, "C-down: variant x %d" % presses if i == 0 else "", down=3, gap=17)
        stage_id = row_[0]
        note.append("stage 0x%02X %s (%s): page %d, icon %d, %d variant presses" % (stage_id, row_[1], row_[2], page + 1, index, presses))
    else:
        key = re.sub(r"[^A-Z0-9_]", "", stage_name.upper().replace(" ", "_").replace("'", ""))
        if re.match(r"0x[0-9a-fA-F]+$", stage_name):
            stage_id = int(stage_name, 16)
        elif key in VANILLA_STAGES:
            stage_id = VANILLA_STAGES[key]
        else:
            sys.exit("unknown stage %s (see --list)" % stage_name)
        if stage_id not in VANILLA_SLOTS:
            sys.exit("gkind 0x%02X is not on vanilla's stage select; start it with tools/vs_poke_plan.py" % stage_id)
        if stage_id == 8:
            sys.exit("Mushroom Kingdom is locked on a blank save; start it with tools/vs_poke_plan.py instead")
        slot = VANILLA_SLOTS.index(stage_id)
        for i in range(slot % 5):
            f = s.tap(1, f, D_RIGHT, "d-pad right x %d" % (slot % 5) if i == 0 else "", down=2, gap=14)
        for i in range(slot // 5):
            f = s.tap(1, f, D_DOWN, "d-pad down", down=2, gap=14)
        note.append("stage gkind 0x%02X: stage select slot %d" % (stage_id, slot))
    battle = f + 20
    s.tap(1, battle, A, "A -> VS battle (scene 16 on the next frame)")

    # ---- optional mash for human ports ----
    if mash_seed is not None and humans:
        rng = random.Random(mash_seed)
        buttons = [0x80000000, 0x40000000, 0x20000000, 0x00100000, 0x00010000, 0x00020000, 0x00040000, 0x00080000]
        for player in range(1, len(picks) + 1):
            f = battle + 200
            while f < frames:
                word = rng.choice(buttons) if rng.random() < 0.6 else 0
                if rng.random() < 0.7:
                    word |= (rng.choice([0x50, 0xB0, 0x7F, 0x81, 0]) << 8) | rng.choice([0x50, 0xB0, 0x7F, 0x81, 0])
                f = s.hold(player, f, rng.randint(2, 20), word) + rng.randint(0, 6)
    return s, battle, stage_id, [p[3] for p in picks], note


def verify(rom, path, battle, stage_id, kinds, humans, level):
    bench = os.path.join(REPO, "build/mac-arm64/znx-bench")
    sys.path.insert(0, HERE)
    import read_battle_state as rbs
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, "ram.bin")
        frames = battle + 600
        subprocess.run([bench, "--rom", ROMS[rom], "--frames", str(frames), "--inputs", path, "--hash-every", "0",
                        "--dump-ram", str(frames), dump], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        ram = rbs.Ram(dump)
    scene, gkind = ram.u8(0x800A4AD0), ram.u8(0x800A4D09)
    ok = scene == 0x16 and gkind == stage_id
    print("verify: scene %02X  battle gkind 0x%02X (wanted 0x%02X)  collision lines %d" % (scene, gkind, stage_id, ram.u32(0x80131384)))
    for i, kind in enumerate(kinds):
        p = 0x800A4D28 + i * 0x74
        lv, pk, fk = ram.u8(p), ram.u8(p + 2), ram.u8(p + 3)
        good = fk == kind and pk == (0 if humans else 1) and (humans or lv == level) and ram.u32(p + 0x58) >> 24 == 0x80
        ok = ok and good
        print("verify: P%d %s fkind 0x%02X (wanted 0x%02X) level %d fighter gobj %08X  %s" % (
            i + 1, {0: "HMN", 1: "CPU", 2: "none"}.get(pk, pk), fk, kind, lv, ram.u32(p + 0x58), "ok" if good else "WRONG"))
    print("verify: %s" % ("PASS" if ok else "FAIL"))
    return ok


def main():
    args = sys.argv[1:]
    opt = {"--level": "9", "--frames": "24000", "--rom": "vanilla"}
    flags = set()
    i = 0
    while i < len(args):
        if args[i] in ("--humans", "--verify", "--list", "--no-tune"):
            flags.add(args[i])
            i += 1
        elif args[i] in ("--rom", "--stage", "--fighters", "--level", "--time-presses", "--mash", "--frames", "-o") and i + 1 < len(args):
            opt[args[i]] = args[i + 1]
            i += 2
        else:
            sys.exit(__doc__)
    rom = opt["--rom"]
    if rom not in ROMS:
        sys.exit(__doc__)
    if "--list" in flags:
        if rom == "vanilla":
            print("stages:", " ".join(k for k in VANILLA_STAGES if k != "MUSHROOM_KINGDOM"))
            print("fighters:", " ".join(k for k, v in VANILLA_FIGHTERS.items() if v not in VANILLA_LOCKED))
        else:
            stages, chars = remix_ids(rom == "extra")
            print("stage select pages:")
            for p in REMIX_PAGES + ([EXTRA_PAGE] if rom == "extra" else []):
                print("  " + p)
            print("variants (C-down):", " ".join("%s<-%s/%s" % (r[1], r[4], r[5]) for r in stages if r[4]))
            print("fighters on the grid:", " ".join(REMIX_SLOTS))
            print("d-pad variants:", " ".join("%s(%s)" % (c[1], c[3]) for c in chars if c[3] in CHAR_VARIANT_DPAD))
        return
    if "--stage" not in opt or "--fighters" not in opt:
        sys.exit(__doc__)
    fighters = opt["--fighters"].split(",")
    if not 2 <= len(fighters) <= 4:
        sys.exit("2 to 4 fighters")
    humans = "--humans" in flags
    level = int(opt["--level"])
    time_presses = int(opt["--time-presses"]) if "--time-presses" in opt else (6 if rom == "vanilla" else 0)
    s, battle, stage_id, kinds, note = build(rom, opt["--stage"], fighters, level, humans, time_presses,
                                             int(opt["--mash"]) if "--mash" in opt else None, int(opt["--frames"]),
                                             "--no-tune" not in flags and os.path.exists(os.path.join(REPO, "build/mac-arm64/znx-bench")))
    header = ["Power-on to a VS match by input only: %s, stage %s, %s, %s." % (
                  {"vanilla": "Super Smash Bros. (USA)", "remix": "Smash Remix 2.0.1", "extra": "Smash Remix 2.0.1 +EXTRA 0.6.0"}[rom],
                  opt["--stage"], ", ".join(fighters), "human ports" if humans else "CPU level %d" % level),
              "Written by tools/make_vs_script.py; " + "; ".join(note) + ".",
              "A on the stage select at frame %d: the battle (scene byte 800A4AD0 = 16) starts on the next frame." % battle,
              "Run with --input-players %d or more controllers plugged (the harness plugs four by default)." % len(fighters)]
    path = opt.get("-o")
    if path:
        with open(path, "w") as out:
            s.write(out, header)
        print("%s: battle from frame %d" % (path, battle + 1))
    else:
        s.write(sys.stdout, header)
    if "--verify" in flags:
        if not path:
            sys.exit("--verify needs -o FILE")
        sys.exit(0 if verify(rom, path, battle, stage_id, kinds, humans, level) else 1)


if __name__ == "__main__":
    main()
