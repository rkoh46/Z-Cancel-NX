# Stage matrix: starting any VS match without a human

How to put a VS match on any stage, with any fighters, as two to four level-9 CPUs, in Super Smash Bros.
(USA), Smash Remix 2.0.1 and Smash Remix 2.0.1 +EXTRA 0.6.0, from the harness, and which cases the
performance matrix should run. Researched 2026-11-12 on the Mac harness. Nothing here is copied from a ROM:
ids and names come from `ref/ssb-decomp-re`, `ref/smashremix` (tag 2.0.1) and `ref/smashremix-plus-extra`
(tag 0.6.0); everything marked "measured" was read out of RDRAM while the ROM ran.

## The short answer

- **One mechanism for all three ROMs: at most 53 bytes of pokes on one frame.** The game's VS setup is the
  structure `gSCManagerTransferBattleState` at `0x800A4D08`, the stage is copied into it from
  `gSCManagerSceneData.gkind` at `0x800A4ADF` when the battle scene starts, and the scene manager picks the
  next scene from the scene byte `0x800A4AD0`. Remix patches the vanilla ROM and keeps all three where they
  are. Press START twice (frames 200 and 420, `bench/inputs/boot-to-mode-select.txt`); on the frame the scene
  byte first reads `0x07` write the fighters, the rules, the stage and the scene byte `0x16`. The battle is
  running about 30 frames later (frame ~450 instead of ~1,450 through the menus, or 5,200 with the old mash
  script). It reaches every stage and fighter, including the ones no menu offers.
- **Proved by doing it**, with a scratch copy of the harness that can poke
  (`build/stage-research/bin/znx-bench-sr`, not part of the repo): 14 of 17 vanilla stage ids, 145 of 146
  Remix battle stages and 161 of 162 in +EXTRA were started this way with four level-9 CPUs and ran 6,450
  game tics each; 114 fighter kinds of +EXTRA were run four at a time.
- **Fallback without pokes:** `tools/make_vs_script.py` writes a plain input script through the menus:
  stage, two to four fighters, every port switched to CPU and raised to level 9. Checked with the stock
  harness (`--watch 0x800A4D09`, `--dump-ram`) on all 8 vanilla starter stages and on Remix and +EXTRA
  stages including variants and the +EXTRA page. It cannot reach locked or hidden things (see the limits).

## 1. What the game reads

Scene manager (`ref/ssb-decomp-re/src/sc/scmanager.c`, `sctypes.h`; symbols in `symbols/symbols_us.txt`):

| Symbol | Address | Size | What |
|---|---|---|---|
| `gSCManagerSceneData` (`SCCommonData`) | `0x800A4AD0` | 0x48 | `scene_curr` at +0x00, `scene_prev` +0x01, **`gkind` +0x0F (`0x800A4ADF`)**: the stage the next battle loads, `maps_vsmode_gkind` +0x41 (`0x800A4B11`): where the stage select cursor starts |
| `gSCManager1PGameBattleState` | `0x800A4B18` | 0x1F0 | 1P Game's copy |
| **`gSCManagerTransferBattleState`** (`SCBattleState`) | **`0x800A4D08`** | 0x1F0 | what the character select fills in and the VS battle scene runs on |
| `gSCManagerVSBattleState` | `0x800A4EF8` | 0x1F0 | sudden death's copy, built from the above after a tie |
| `gSCManagerBattleState` | `0x800A50E8` | 4 | pointer to the state the running battle uses (reads `0x800A4D08` in a VS battle) |
| `gSCManagerBackupData` | `0x800A44E0` | | the save: unlock mask, fighter mask |
| `sMNPlayersVSSlots[4]` | `0x8013BA88` | 4 x 0xBC | the VS character select's per-port state (only while scene 0x10 is loaded) |
| `sMNMapsCursorSlot` | `0x80134BD8` | 4 | vanilla stage select cursor, 0..9 (only while scene 0x15 is loaded) |

Scene byte values used here: `0x01` title, `0x07` mode select, `0x09` VS menu, `0x10` VS character select,
`0x15` stage select, `0x16` VS battle, `0x18` VS results, `0x1B` boot, `0x1C`.. intro, `0x3C` auto demo;
Remix shows its own splash as `0x37` between boot and title.

`SCBattleState` header (offsets from `0x800A4D08`):

| Offset | Address | Field | Values |
|---|---|---|---|
| +0x00 | `0x800A4D08` | `game_type` | set to 1 (royal) by the battle scene itself |
| +0x01 | `0x800A4D09` | **`gkind`** | stage id; overwritten from `0x800A4ADF` when the battle scene starts |
| +0x02 | `0x800A4D0A` | `is_team_battle` | 0 |
| +0x03 | `0x800A4D0B` | `game_rules` | bit 0 time, bit 1 stock (vanilla default 1, Remix default 3) |
| +0x04 | `0x800A4D0C` | `pl_count` | number of humans |
| +0x05 | `0x800A4D0D` | `cp_count` | number of CPUs |
| +0x06 | `0x800A4D0E` | `time_limit` | minutes; 100 is infinite |
| +0x07 | `0x800A4D0F` | `stocks` | stocks minus one |
| +0x08 | `0x800A4D10` | `handicap` | 0 off, 1 on, 2 auto |
| +0x09 | `0x800A4D11` | `is_team_attack` | |
| +0x0A | `0x800A4D12` | `is_stage_select` | 1: character select goes to the stage select; 0: it picks a random stage itself |
| +0x0B | `0x800A4D13` | `damage_ratio` | percent, 100 |
| +0x0C | `0x800A4D14` | `item_toggles` (u32) | item switch mask |
| +0x10 | `0x800A4D18` | `is_reset_players` | |
| +0x11 | `0x800A4D19` | `game_status` | |
| +0x14 | `0x800A4D1C` | `time_remain` (u32) | tics |
| +0x18 | `0x800A4D20` | `time_passed` (u32) | tics since GO: the "is the battle running" counter |
| +0x1C | `0x800A4D24` | `item_appearance_rate` | 0 none .. 5 very high |
| +0x20 | `0x800A4D28` | `players[4]` | 0x74 bytes each: `0x800A4D28`, `0x800A4D9C`, `0x800A4E10`, `0x800A4E84` |

`SCPlayerData` (offsets from the port's base):

| Offset | Field | Values |
|---|---|---|
| +0x00 | `level` | CPU level 1..9 |
| +0x01 | `handicap` | 9 |
| +0x02 | **`pkind`** | 0 human, 1 CPU, 2 not playing |
| +0x03 | **`fkind`** | fighter id (0x1C none) |
| +0x04 | `team` | port number outside team battles |
| +0x05 | `player` | port number |
| +0x06 | `costume` | 0.. |
| +0x07 | `shade` | 0; 1, 2.. when the same fighter and costume is used again |
| +0x08 | `color` | port number for a human, 4 for a CPU |
| +0x09 | `is_single_stockicon` | 1 in a time match |
| +0x0A | `tag` | port number for a human, 4 ("CP") for a CPU |
| +0x0B | `stock_count` | set by the battle |
| +0x10 | `falls` (s32) | |
| +0x34 | `total_damage_given` (s32) | rises while the CPUs fight |
| +0x58 | `fighter_gobj` | `0x80......` once the fighter exists |

Where they are written and read:

- `mnPlayersVSSetSceneData` (`mnplayersvs.c`) copies `sMNPlayersVSSlots` (fkind, pkind, costume, shade,
  level), the time, stocks and rule into `gSCManagerTransferBattleState` when the character select is left.
- The stage select (`mnmaps.c`) stores the stage under the cursor in `gSCManagerSceneData.gkind` on A/START
  and sets `scene_curr = 0x16`.
- `scVSBattleStartScene` (`scvsbattle.c`) does `gSCManagerBattleState = &gSCManagerTransferBattleState;
  ->gkind = gSCManagerSceneData.gkind;` and starts the battle from that structure.
- The scene manager's loop (`scManagerRunLoop`) switches on `gSCManagerSceneData.scene_curr` each time a
  scene returns.

Read back across a scripted match (stock harness, vanilla, `bench/inputs/ssb-usa-vs-mash.txt`, which picks
Donkey Kong and Kirby on the default stage):

    build/mac-arm64/znx-bench --rom ROM --frames 6000 --inputs bench/inputs/ssb-usa-vs-mash.txt --hash-every 0 --watch 0x800A4D2B
    watch 22 0a4d2b 1c      (none)        watch 5031 0a4d2b 02   (P1 fkind = Donkey Kong, on leaving the character select)
    --watch 0x800A4D9F: watch 5031 0a4d9f 08 (P2 Kirby)    --watch 0x800A4D0C: watch 5031 0a4d0c 02 (two humans)
    --watch 0x800A4D2A: watch 5031 0a4d2a 00 (human)       --watch 0x800A4AD0: 10 at 4821, 15 at 5031, 16 at 5201

and `tools/read_battle_state.py DUMP` prints the whole structure from a `--dump-ram` file.

## 2. The poke start

On the frame on which RDRAM byte `0x800A4AD0` first reads `0x07` (the title has asked for the mode select
and is unwinding), write, in any order:

| Address | Bytes | Meaning |
|---|---|---|
| `0x800A4D28` + 0x74 x port | `LL 09 01 FF PP PP CC SS 04 01 04` (11 bytes) | per CPU port: level `LL` (09), handicap, pkind 1, fkind `FF`, team and port `PP`, costume `CC` (00), shade `SS` (00, or 01.. for a repeated fighter), color 4, single stock icon 1, tag 4. A human port is `LL 09 00 FF PP PP CC SS PP 01 PP`. |
| `0x800A4D2A` + 0x74 x port | `02 1C` | per unused port: not playing, no fighter |
| `0x800A4D09` | `GG 00 01 HH NN 64 00` (7 bytes) | stage `GG`, no teams, time rule, `HH` humans, `NN` CPUs, time limit 100 = infinite, stocks. For a stock match: rule `02`, time anything, stocks minus one, and byte +0x09 of each port `00`. |
| `0x800A4ADF` | `GG` | the stage again: this is the byte the battle scene copies from |
| `0x800A4AD0` | `16` | VS battle instead of mode select |

Four CPUs are 4 x 11 + 7 + 1 + 1 = 53 bytes; `tools/vs_poke_plan.py --stage 0x04 --fighters 1,3,5,9` prints
them (and `--c` prints a C table). With an infinite time match the battle never ends, which is what a
benchmark wants: 29,159 tics were still counting at frame 30,000.

Proof (scratch harness, see "Reproducing"): the same pokes in each ROM, then the state read back from RDRAM
3,600 frames later with `tools/read_battle_state.py`:

| ROM | Poked | Read back at frame 4,000 |
|---|---|---|
| vanilla | stage 0x04, fighters 01 03 05 09 | scene 16 from frame 421; `gSCManagerBattleState -> 800A4D08`; gkind 04; collision lines 15 (the same as Hyrule Castle picked in the menu); 4 CPUs level 9 with fighter objects; damage given 212/70/106/76; time passed 3,155 tics |
| Remix 2.0.1 | stage 0x2D Pokemon Stadium, fighters 1D 1E 34 3B (Falco, Ganondorf, Bowser, Sonic) | scene 16 from 421; gkind 2D; 4 CPUs level 9; damage 160/185/107/160; 3,157 tics |
| +EXTRA 0.6.0 | stage 0xDD Halberd, fighters 64 6F 70 6D (Cloud, Ryu, Snake, Meta Knight) | scene 16 from 421; gkind DD; 4 CPUs level 9; damage 208/109/220/48; 3,159 tics |

Timing, measured in vanilla by delaying the pokes:

- At the title -> mode select change (01 -> 07): works 0, 1 and 2 frames after the byte changes, not 4 or
  8 frames after (the mode select has loaded and the battle never starts). Write on the frame of the
  change.
- At the stage select -> battle change (15 -> 16), stage only (`0x800A4ADF` and `0x800A4D09`): works 0 to 5
  frames after, not 8 or 12 (the stage is already loading; the byte reads the poked value but the old stage
  runs). So a harness that prefers to go through the menus for the fighters can still force the stage
  there; fighter fields poked when the scene byte first reads 0x15 are kept too (the stage select does not
  touch them).
- Not usable: the boot -> intro change (1B -> 1C at frame 95), the intro's own scene changes and the
  intro -> title change: the scene byte reads the poked 0x16 and nothing loads. Only changes made by a
  menu scene's exit were found to work.
- The harness must write after the frame completes (the scratch harness pokes where `--dump-ram` and
  `--watch` read, in `frame_done`), into RDRAM in the core's word order (byte address `^ 3`). The pokes are
  data, not code, so the recompiler's caches are not involved.

What the poke start does not do: it skips the character and stage select, so anything those screens set
up besides the structure keeps its boot value. In Remix that is its per-match extras (hazard mode, music
choice, the stage select's variant bookkeeping, toggles); the stages and fighters themselves loaded and
fought in every case tried. It also starts with the save as it is: nothing needs unlocking, since the
battle scene does not check.

## 3. Vanilla Super Smash Bros. (USA)

Stages (`GRKind`, `ref/ssb-decomp-re/src/gr/grdef.h`). "Poke" is the result of starting it with four
level-9 CPUs (Fox, Samus, Link, Pikachu) by poke; "ms" is the Mac harness's emulation time per frame over
frames 5,000 to 7,200 of that run (no video, so it is the game's own cost, not the picture's).

| gkind | Stage | VS stage select | Poke | ms |
|---|---|---|---|---|
| 0x00 | Peach's Castle (nGRKindCastle) | slot 0 | runs | 0.133 |
| 0x01 | Sector Z (Sector) | slot 7 | runs | 0.127 |
| 0x02 | Kongo Jungle (Jungle) | slot 1 | runs | 0.125 |
| 0x03 | Planet Zebes (Zebes) | slot 3 | runs | 0.135 |
| 0x04 | Hyrule Castle (Hyrule) | slot 2 | runs | 0.125 |
| 0x05 | Yoshi's Island (Yoster) | slot 5 | runs | 0.128 |
| 0x06 | Dream Land (Pupupu) | slot 6 | runs | 0.123 |
| 0x07 | Saffron City (Yamabuki) | slot 8 | runs | 0.133 |
| 0x08 | Mushroom Kingdom (Inishie) | slot 4 (locked on a blank save) | runs | 0.126 |
| 0x09 | Beta Dream Land (PupupuSmall) | not offered | stops after 2141 tics |  |
| 0x0A | Test stage (PupupuNew) | not offered | stops after 5290 tics |  |
| 0x0B | How to Play (Explain) | not offered | runs | 0.130 |
| 0x0C | Small Yoshi's Island (YosterSmall) | not offered | runs | 0.122 |
| 0x0D | Meta Crystal (Metal) | not offered | runs | 0.133 |
| 0x0E | Duel Zone (Zako) | not offered | runs | 0.124 |
| 0x0F | Race to the Finish (Bonus3) | not offered | runs | 0.142 |
| 0x10 | Final Destination (Last) | not offered | does not start |  |
| 0x11..0x1C | Break the Targets: Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Falcon, Kirby, Pikachu, Jigglypuff, Ness | not offered | not tried | |
| 0x1D..0x28 | Board the Platforms, same order | not offered | not tried | |
| 0xDE | "random" marker of the stage select, not a stage | slot 9 | | |

The nine VS stages are 0x00 to 0x08 (`nGRKindBattleStart`..`nGRKindBattleEnd`). The stage select is two rows
of five: Peach's Castle, Kongo Jungle, Hyrule Castle, Planet Zebes, Mushroom Kingdom / Yoshi's Island, Dream
Land, Sector Z, Saffron City, Random (`mnMapsGetGroundKind`). By poke the 1P-only stages run in VS as well;
Final Destination does not start in vanilla (it does in Remix), and the two beta Dream Lands stop.

Fighters (`FTKind`): 0x00 Mario, 01 Fox, 02 Donkey Kong, 03 Samus, 04 Luigi, 05 Link, 06 Yoshi, 07 Captain
Falcon, 08 Kirby, 09 Pikachu, 0A Jigglypuff, 0B Ness, 0C Master Hand, 0D Metal Mario, 0E..19 the twelve
Fighting Polygons in the same order, 1A Giant Donkey Kong, 1C none. Luigi, Falcon, Jigglypuff and Ness are
locked on the blank save the harness runs with; by poke they play (0x04, 0x07, 0x0A, 0x0B on Mushroom
Kingdom ran), and so did 0x0C, 0x0D, 0x0E and 0x1A as CPUs.

### Input-only fallback (vanilla)

`tools/make_vs_script.py --rom vanilla --stage HYRULE_CASTLE --fighters FOX,SAMUS,LINK,PIKACHU -o FILE --verify`

What the script it writes does, and where each number comes from:

- Frames 200 and 420 START, 540 d-pad down, 600 A, 720 A: title at 201, mode select 421, VS menu 601,
  character select at 721. The same frames work in the Remix ROMs.
- Character select (`mnplayersvs.c`): each port's cursor starts at (40 + 68p, 170) holding its puck and
  moves `stick / 20` pixels a frame (stick 80: 4 px). A puck is over portrait column c, row r when the
  cursor is at (23 + 45c, 59 + 43r) (`mnPlayersVSGetPuckFighterKind`); the portraits are Luigi, Mario, DK,
  Link, Samus, Falcon / Ness, Yoshi, Kirby, Fox, Pikachu, Jigglypuff. A places the puck.
- CPU: the port's own cursor then presses A on its HMN/CPU button, cursor (69p + 54, 133)
  (`mnPlayersVSCheckPlayerKindSelectInRange`). A placed puck keeps its fighter, the port becomes a CPU of
  level 3. Level 9: A six times on the right level arrow, cursor (69p + 59, 200)
  (`mnPlayersVSCheckHandicapArrowRInRange`). Every port does this at once with its own controller, so
  the harness must have that many controllers plugged (it plugs four by default).
- Time: port 1 presses A six times on the time arrow, cursor (200, 20): 3 -> 5, 10, 15, 30, 60, infinite.
- START, then on the stage select d-pad right (slot mod 5) times and down once for the second row (taps
  16 frames apart: the game waits 12 after a d-pad move), A.
- The generator is closed-loop: after each cursor move it runs the stock harness, reads the cursor's real
  position (`sMNPlayersVSSlots[p].cursor` -> GObj +0x74 -> SObj position at +0x58) and each slot's `fkind`
  (+0x48), `pkind` (+0x84) and `cpu_level` (+0x38), appends a correction if a cursor is off, and stops if
  a step did not take. Vanilla needs no corrections; Remix does (below). `--no-tune` skips this.

Verified with the stock harness: all eight starter stages, two to four CPUs (table: the stage byte
`0x800A4D09` under `--watch`, then `--verify`'s read of the dump 600 frames into the battle).

| Script | `--watch 0x800A4D09` | Read back |
|---|---|---|
| `bench/inputs/vs-vanilla-hyrule-4cpu.txt` | `watch 1451 0a4d09 04` | 4 CPUs level 9: Fox, Samus, Link, Pikachu; 15 collision lines |
| `bench/inputs/vs-vanilla-sectorz-4cpu.txt` | `watch 1481 0a4d09 01` | DK, Yoshi, Mario, Kirby; 9 lines |
| `bench/inputs/vs-vanilla-saffron-4cpu.txt` | `watch 1488 0a4d09 07` | Pikachu, Mario, Samus, Fox; 16 lines |
| `bench/inputs/vs-vanilla-dreamland-2cpu.txt` | `watch 1411 0a4d09 06` | Kirby, Fox; 7 lines |
| `bench/inputs/vs-vanilla-castle-2cpu.txt` | (stays 00) | Mario, Link; 16 lines |
| generated, not kept: Planet Zebes, Kongo Jungle, Yoshi's Island | 03, 02, 05 | PASS |

Limits: Mushroom Kingdom and the four hidden fighters are locked on a blank save, and the 1P stages are
never offered: poke only.

## 4. Smash Remix 2.0.1 and +EXTRA 0.6.0

**Same structure, same addresses.** Remix is a patch of the vanilla ROM and leaves the scene manager's
data where it was. Measured in both ROMs: the scene byte at `0x800A4AD0` walks 1B, 37, 01, 07, 09, 10, 15,
16 with the same inputs; `sMNPlayersVSSlots` at `0x8013BA88` holds cursor pointers, level 3, fighter 0x1C on
entering the character select; `gSCManagerTransferBattleState` at `0x800A4D08` holds Remix's defaults at the
title (rules 3 = time and stock, 8 minutes, 4 stocks) and the picked fighters and stage in a battle; the
poke start works unchanged (section 2). Remix's own variables (stage select page and variant, toggles) are
in its added code segment and were not located.

### Stages

Ids are the vanilla `gkind` extended: `add_stage` calls in `ref/smashremix/src/Stages.asm` number from 0x29
in file order, to `MAX_STAGE_ID` 0xD7. +EXTRA appends its `extra_stages` folders, and their `remix`/`dl`
variant folders, in sorted order from 0xD8, and steps over 0xDE, which stays the "random" marker
(`character_appender.py`); the stage names sit in that order in the +EXTRA ROM. `tools/remix_ids.py` prints
these tables from the sources. "Variant of" names the stage whose icon it hides behind on the stage select
(DL = Dream Land layout, OMEGA = flat, REMIX.. = alternate). "2.0.1" and "+EXTRA" are the poke result in
each ROM with four level-9 CPUs (Fox, Samus, Link, Pikachu) and the Mac emulation ms per frame.

| id | Constant | Name | Variant of | Hazards | 2.0.1 | ms | +EXTRA | ms |
|---|---|---|---|---|---|---|---|---|
| 0x00 | PEACHS_CASTLE | Peach's Castle |  |  | runs | 0.153 | runs | 0.152 |
| 0x01 | SECTOR_Z | Sector Z |  |  | runs | 0.152 | runs | 0.158 |
| 0x02 | CONGO_JUNGLE | Congo Jungle |  |  | runs | 0.146 | runs | 0.172 |
| 0x03 | PLANET_ZEBES | Planet Zebes |  |  | runs | 0.148 | runs | 0.160 |
| 0x04 | HYRULE_CASTLE | Hyrule Castle |  |  | runs | 0.151 | runs | 0.163 |
| 0x05 | YOSHIS_ISLAND | Yoshi's Island |  |  | runs | 0.154 | runs | 0.167 |
| 0x06 | DREAM_LAND | Dream Land |  |  | runs | 0.153 | runs | 0.159 |
| 0x07 | SAFFRON_CITY | Saffron City |  |  | runs | 0.152 | runs | 0.150 |
| 0x08 | MUSHROOM_KINGDOM | Mushroom Kingdom |  |  | runs | 0.150 | runs | 0.149 |
| 0x09 | DREAM_LAND_BETA_1 | Dream Land Beta 1 |  |  | runs | 0.146 | runs | 0.145 |
| 0x0A | DREAM_LAND_BETA_2 | Dream Land Beta 2 |  |  | runs | 0.152 | runs | 0.151 |
| 0x0B | HOW_TO_PLAY | How to Play |  |  | runs | 0.142 | runs | 0.142 |
| 0x0C | MINI_YOSHIS_ISLAND | Mini Yoshi's Island |  |  | runs | 0.144 | runs | 0.145 |
| 0x0D | META_CRYSTAL | Meta Crystal |  |  | runs | 0.162 | runs | 0.159 |
| 0x0E | DUEL_ZONE | Duel Zone |  |  | runs | 0.140 | runs | 0.140 |
| 0x0F | RACE_TO_THE_FINISH | Race to the Finish |  |  | runs | 0.178 | runs | 0.173 |
| 0x10 | FINAL_DESTINATION | Final Destination |  |  | runs | 0.146 | runs | 0.148 |
| 0x29 | DEKU_TREE | Deku Tree |  | NONE | runs | 0.142 | runs | 0.144 |
| 0x2A | FIRST_DESTINATION | First Destination |  | NONE | runs | 0.142 | runs | 0.144 |
| 0x2B | GANONS_TOWER | Ganon's Tower |  | NONE | runs | 0.147 | runs | 0.146 |
| 0x2C | GYM_LEADER_CASTLE | Gym Leader Castle |  | NONE | runs | 0.158 | runs | 0.159 |
| 0x2D | POKEMON_STADIUM | Pokemon Stadium |  | NONE | runs | 0.161 | runs | 0.161 |
| 0x2E | TALTAL | Tal Tal Heights |  | HAZARDS | runs | 0.147 | runs | 0.147 |
| 0x2F | GLACIAL | Glacial River |  | NONE | runs | 0.146 | runs | 0.149 |
| 0x30 | WARIOWARE | WarioWare, Inc. |  | NONE | runs | 0.144 | runs | 0.149 |
| 0x31 | BATTLEFIELD | Battlefield |  | NONE | runs | 0.151 | runs | 0.150 |
| 0x32 | FLAT_ZONE | Flat Zone |  | HAZARDS | runs | 0.134 | runs | 0.135 |
| 0x33 | DR_MARIO | Dr. Mario |  | NONE | runs | 0.151 | runs | 0.150 |
| 0x34 | COOLCOOL | Cool Cool Mountain |  | NONE | runs | 0.155 | runs | 0.155 |
| 0x35 | DRAGONKING | Dragon King |  | NONE | runs | 0.145 | runs | 0.146 |
| 0x36 | GREAT_BAY | Great Bay |  | MOVEMENT | runs | 0.138 | runs | 0.139 |
| 0x37 | FRAYS_STAGE | Fray's Stage |  | NONE | runs | 0.143 | runs | 0.144 |
| 0x38 | TOH | Tower of Heaven |  | NONE | runs | 0.157 | runs | 0.157 |
| 0x39 | FOD | Fountain of Dreams |  | MOVEMENT | runs | 0.150 | runs | 0.153 |
| 0x3A | MUDA | Muda Kingdom | GB_LAND / REMIX | NONE | runs | 0.133 | runs | 0.134 |
| 0x3B | MEMENTOS | Mementos | REAPERS / REMIX | MOVEMENT | runs | 0.153 | runs | 0.153 |
| 0x3C | SHOWDOWN | Showdown | FIRST_DESTINATION / REMIX2 | NONE | runs | 0.154 | runs | 0.154 |
| 0x3D | SPIRALM | Spiral Mountain |  | MOVEMENT | runs | 0.150 | runs | 0.151 |
| 0x3E | N64 | N64 |  | NONE | runs | 0.148 | runs | 0.146 |
| 0x3F | MUTE_DL | Mute City DL | MUTE / DL | NONE | runs | 0.151 | runs | 0.153 |
| 0x40 | MADMM | Mad Monster Mansion |  | NONE | runs | 0.151 | runs | 0.151 |
| 0x41 | SMBBF | Mushroom Kingdom DL | MUSHROOM_KINGDOM / DL | HAZARDS | runs | 0.135 | runs | 0.135 |
| 0x42 | SMBO | Mushroom Kingdom ~ | MUSHROOM_KINGDOM / OMEGA | HAZARDS | runs | 0.130 | runs | 0.155 |
| 0x43 | BOWSERB | Bowser's Stadium |  | HAZARDS | runs | 0.160 | runs | 0.179 |
| 0x44 | PEACH2 | Peach's Castle II |  | MOVEMENT | runs | 0.149 | runs | 0.169 |
| 0x45 | DELFINO | Delfino Plaza |  | MOVEMENT | runs | 0.141 | runs | 0.157 |
| 0x46 | CORNERIA2 | Corneria | SECTOR_Z / REMIX2 | HAZARDS | runs | 0.151 | runs | 0.157 |
| 0x47 | KITCHEN | Kitchen Island |  | MOVEMENT | runs | 0.141 | runs | 0.142 |
| 0x48 | BLUE | Big Blue |  | BOTH | runs | 0.166 | runs | 0.169 |
| 0x49 | ONETT | Onett |  | HAZARDS | runs | 0.154 | runs | 0.153 |
| 0x4A | ZLANDING | Crateria |  | MOVEMENT | runs | 0.149 | runs | 0.149 |
| 0x4B | FROSTY | Frosty Village |  | NONE | runs | 0.150 | runs | 0.150 |
| 0x4C | SMASHVILLE2 | Smashville |  | MOVEMENT | runs | 0.150 | runs | 0.152 |
| 0x50 | BATTLEFIELD_DL | Battlefield DL | BATTLEFIELD / DL | NONE | runs | 0.148 | runs | 0.148 |
| 0x55 | HTEMPLE | Hyrule Temple |  | NONE | runs | 0.156 | runs | 0.156 |
| 0x58 | NPC | New Pork City |  | NONE | runs | 0.146 | runs | 0.148 |
| 0x5A | SMASHKETBALL | Smashketball |  | HAZARDS | runs | 0.151 | runs | 0.151 |
| 0x5C | NORFAIR | Norfair |  | HAZARDS | runs | 0.150 | runs | 0.151 |
| 0x5D | RAIDBLUE | Raid Blue |  | NONE | runs | 0.149 | runs | 0.149 |
| 0x5E | FALLS | Congo Falls |  | HAZARDS | runs | 0.151 | runs | 0.153 |
| 0x5F | OSOHE | Pigmask Fortress |  | NONE | runs | 0.144 | runs | 0.147 |
| 0x60 | YOSHI_STORY_2 | Yoshi's Story |  | MOVEMENT | runs | 0.158 | runs | 0.157 |
| 0x61 | WORLD1 | World 1-1 | MUSHROOM_KINGDOM / REMIX2 | MOVEMENT | runs | 0.151 | runs | 0.154 |
| 0x62 | FLAT_ZONE_2 | Flat Zone II | FLAT_ZONE / REMIX | BOTH | runs | 0.132 | runs | 0.133 |
| 0x63 | GERUDO | Gerudo Valley |  | NONE | runs | 0.151 | runs | 0.149 |
| 0x67 | HCASTLE_DL | Hyrule Castle DL | HYRULE_CASTLE / DL | HAZARDS | runs | 0.150 | runs | 0.149 |
| 0x68 | HCASTLE_O | Hyrule Castle ~ | HYRULE_CASTLE / OMEGA | HAZARDS | runs | 0.148 | runs | 0.145 |
| 0x69 | CONGOJ_DL | Congo Jungle DL | CONGO_JUNGLE / DL | HAZARDS | runs | 0.145 | runs | 0.144 |
| 0x6A | CONGOJ_O | Congo Jungle ~ | CONGO_JUNGLE / OMEGA | HAZARDS | runs | 0.145 | runs | 0.147 |
| 0x6B | PCASTLE_DL | Peach's Castle DL | PEACHS_CASTLE / DL | HAZARDS | runs | 0.145 | runs | 0.150 |
| 0x6C | PCASTLE_O | Peach's Castle ~ | PEACHS_CASTLE / OMEGA | HAZARDS | runs | 0.142 | runs | 0.143 |
| 0x6E | FRAYS_STAGE_NIGHT | Fray's Stage - Night | FRAYS_STAGE / DL | NONE | runs | 0.148 | runs | 0.148 |
| 0x6F | GOOMBA_ROAD | Goomba Road |  | NONE | runs | 0.151 | runs | 0.154 |
| 0x71 | SECTOR_Z_DL | Sector Z DL | SECTOR_Z / DL | HAZARDS | runs | 0.147 | runs | 0.149 |
| 0x72 | SAFFRON_DL | Saffron City DL | SAFFRON_CITY / DL | MOVEMENT | runs | 0.147 | runs | 0.146 |
| 0x73 | YOSHI_ISLAND_DL | Yoshi's Island DL | YOSHIS_ISLAND / DL | HAZARDS | runs | 0.152 | runs | 0.150 |
| 0x74 | ZEBES_DL | Planet Zebes DL | PLANET_ZEBES / DL | HAZARDS | runs | 0.148 | runs | 0.155 |
| 0x75 | SECTOR_Z_O | Sector Z ~ | SECTOR_Z / OMEGA | HAZARDS | runs | 0.146 | runs | 0.147 |
| 0x76 | SAFFRON_O | Saffron City ~ | SAFFRON_CITY / OMEGA | NONE | runs | 0.144 | runs | 0.145 |
| 0x77 | YOSHI_ISLAND_O | Yoshi's Island ~ | YOSHIS_ISLAND / OMEGA | HAZARDS | runs | 0.150 | runs | 0.150 |
| 0x78 | DREAM_LAND_O | Dream Land ~ | DREAM_LAND / OMEGA | HAZARDS | runs | 0.149 | runs | 0.149 |
| 0x79 | ZEBES_O | Planet Zebes ~ | PLANET_ZEBES / OMEGA | HAZARDS | runs | 0.144 | runs | 0.145 |
| 0x7C | BOWSERS_KEEP | Bowser's Keep |  | MOVEMENT | runs | 0.149 | runs | 0.151 |
| 0x7D | RITH_ESSA | Rith Essa |  | NONE | runs | 0.142 | runs | 0.141 |
| 0x7E | VENOM | Venom |  | HAZARDS | runs | 0.150 | runs | 0.150 |
| 0x83 | WINDY | Windy |  | MOVEMENT | runs | 0.150 | runs | 0.151 |
| 0x84 | DATA | dataDyne Central |  | MOVEMENT | runs | 0.150 | runs | 0.151 |
| 0x85 | CLANCER | Planet Clancer |  | MOVEMENT | runs | 0.158 | runs | 0.159 |
| 0x86 | JAPES | Jungle Japes |  | HAZARDS | runs | 0.154 | runs | 0.155 |
| 0x88 | GB_LAND | Game Boy Land |  | BOTH | runs | 0.154 | runs | 0.154 |
| 0x8B | REST | Allstar Rest Area |  | NONE | does not start |  | does not start |  |
| 0x8D | CSIEGE | Castle Siege |  | NONE | runs | 0.150 | runs | 0.149 |
| 0x8E | YOSHIS_ISLAND_II | Yoshi's Island II |  | MOVEMENT | runs | 0.146 | runs | 0.148 |
| 0x8F | FINAL_DESTINATION_DL | Final Destination DL | FINAL_DESTINATION / DL | NONE | runs | 0.145 | runs | 0.144 |
| 0x90 | FINAL_DESTINATION_TENT | Final Tentination | FINAL_DESTINATION / REMIX | NONE | runs | 0.148 | runs | 0.149 |
| 0x91 | COOLCOOL_REMIX | Cool Cool Mountain SR | COOLCOOL / REMIX | NONE | runs | 0.152 | runs | 0.150 |
| 0x92 | DUEL_ZONE_DL | Duel Zone DL | DUEL_ZONE / DL | NONE | runs | 0.146 | runs | 0.149 |
| 0x93 | COOLCOOL_DL | Cool Cool Mountain DL | COOLCOOL / DL | NONE | runs | 0.149 | runs | 0.157 |
| 0x94 | META_CRYSTAL_DL | Meta Crystal DL | META_CRYSTAL / DL | NONE | runs | 0.155 | runs | 0.157 |
| 0x95 | DREAM_LAND_SR | Dream Greens | DREAM_LAND / REMIX | HAZARDS | runs | 0.152 | runs | 0.152 |
| 0x96 | PCASTLE_BETA | Peach's Castle Beta | PEACHS_CASTLE / REMIX | BOTH | runs | 0.145 | runs | 0.145 |
| 0x97 | HCASTLE_REMIX | Hyrule Castle SR | HYRULE_CASTLE / REMIX | HAZARDS | runs | 0.148 | runs | 0.149 |
| 0x98 | SECTOR_Z_REMIX | Sector Z SR | SECTOR_Z / REMIX | HAZARDS | runs | 0.148 | runs | 0.149 |
| 0x99 | MUTE | Mute City |  | BOTH | runs | 0.151 | runs | 0.151 |
| 0x9A | HRC | Home Run Contest |  | NONE | not tried (RTTF) |  | not tried (RTTF) |  |
| 0x9B | MK_REMIX | Mushroom Kingdom SR | MUSHROOM_KINGDOM / REMIX | HAZARDS | runs | 0.146 | runs | 0.146 |
| 0x9C | GHZ | Green Hill Zone |  | MOVEMENT | runs | 0.151 | runs | 0.150 |
| 0x9D | SUBCON | Subcon |  | MOVEMENT | runs | 0.143 | runs | 0.143 |
| 0x9E | PIRATE | Pirate Land |  | HAZARDS | runs | 0.157 | runs | 0.156 |
| 0x9F | CASINO | Casino Night Zone |  | HAZARDS | runs | 0.152 | runs | 0.152 |
| 0xA2 | MMADNESS | Metallic Madness |  | BOTH | runs | 0.157 | runs | 0.157 |
| 0xA3 | RAINBOWROAD | Rainbow Road |  | BOTH | runs | 0.150 | runs | 0.153 |
| 0xA4 | POKEMON_STADIUM_2 | Pokemon Stadium 2 | POKEMON_STADIUM / REMIX | NONE | runs | 0.155 | runs | 0.156 |
| 0xA5 | NORFAIR_REMIX | Norfair Remix | NORFAIR / REMIX | HAZARDS | runs | 0.151 | runs | 0.156 |
| 0xA6 | TOADSTURNPIKE | Toad's Turnpike |  | BOTH | runs | 0.163 | runs | 0.166 |
| 0xA7 | TALTAL_REMIX | Tal Tal Heights Remix | TALTAL / REMIX | HAZARDS | runs | 0.145 | runs | 0.145 |
| 0xA9 | WINTER_DL | Winter Dream Land | DREAM_LAND / DL | HAZARDS | runs | 0.150 | runs | 0.150 |
| 0xAB | GLACIAL_REMIX | Glacial River Remix | GLACIAL / REMIX | NONE | runs | 0.146 | runs | 0.145 |
| 0xAD | DRAGONKING_REMIX | Dragon King Remix | DRAGONKING / REMIX | NONE | runs | 0.144 | runs | 0.146 |
| 0xB0 | DRACULAS_CASTLE | Dracula's Castle |  | MOVEMENT | runs | 0.154 | runs | 0.155 |
| 0xB1 | INVERTED_CASTLE | Reverse Castle | DRACULAS_CASTLE / REMIX | MOVEMENT | runs | 0.155 | runs | 0.153 |
| 0xB3 | MT_DEDEDE | Mt. Dedede |  | MOVEMENT | runs | 0.162 | runs | 0.159 |
| 0xB4 | EDO | Edo Town |  | NONE | runs | 0.157 | runs | 0.156 |
| 0xB5 | DEKU_TREE_DL | Deku Tree DL | DEKU_TREE / DL | NONE | runs | 0.138 | runs | 0.139 |
| 0xB6 | ZLANDING_DL | Crateria DL | ZLANDING / DL | NONE | runs | 0.142 | runs | 0.142 |
| 0xB8 | FIRST_REMIX | First Destination Remix | FIRST_DESTINATION / REMIX | NONE | runs | 0.147 | runs | 0.150 |
| 0xBA | TWILIGHT_CITY | Twilight City |  | MOVEMENT | runs | 0.160 | runs | 0.159 |
| 0xBB | MELRODE | Melrode |  | NONE | runs | 0.168 | runs | 0.162 |
| 0xBC | META_REMIX | Meta Crystent | META_CRYSTAL / REMIX | NONE | runs | 0.157 | runs | 0.161 |
| 0xBD | REMIX_RTTF | Remix 1p Race to the Finish |  | NONE | not tried (RTTF) |  | not tried (RTTF) |  |
| 0xBE | REAPERS | Grim Reaper's Cavern |  | NONE | runs | 0.149 | runs | 0.151 |
| 0xBF | SCUTTLE_TOWN | Scuttle Town |  | MOVEMENT | runs | 0.163 | runs | 0.168 |
| 0xC0 | BIG_BOOS_HAUNT | Big Boo's Haunt |  | MOVEMENT | runs | 0.154 | runs | 0.159 |
| 0xC1 | YOSHIS_ISLAND_MELEE | Dinosaur Land |  | MOVEMENT | runs | 0.161 | runs | 0.166 |
| 0xC3 | SPAWNED_FEAR | Spawned Fear |  | BOTH | runs | 0.166 | runs | 0.169 |
| 0xC4 | SMASHVILLE_REMIX | Smashville Remix | SMASHVILLE2 / REMIX | MOVEMENT | runs | 0.147 | runs | 0.147 |
| 0xC6 | POKEFLOATS | Poke Floats |  | MOVEMENT | runs | 0.170 | runs | 0.174 |
| 0xC7 | BIG_SNOWMAN | Big Snowman |  | MOVEMENT | runs | 0.145 | runs | 0.150 |
| 0xC8 | DL_BETA_DL | Dream Land Beta DL | DREAM_LAND_BETA_1 / DL | NONE | runs | 0.150 | runs | 0.149 |
| 0xC9 | LMAO_CASTLE | LMAO Castle | PEACHS_CASTLE / REMIX2 | BOTH | runs | 0.142 | runs | 0.144 |
| 0xCA | DISCOVERY_FALLS | Discovery Falls |  | NONE | runs | 0.152 | runs | 0.152 |
| 0xCC | DISCOVERY_FALLS_REMIX | Discovery Falls Remix | DISCOVERY_FALLS / REMIX | NONE | runs | 0.157 | runs | 0.152 |
| 0xCD | N64_REMIX | N64 Remix | N64 / REMIX | NONE | runs | 0.153 | runs | 0.149 |
| 0xD1 | SOCCER | Soccer |  | NONE | not tried (RTTF) |  | not tried (RTTF) |  |
| 0xD2 | TIME_TWISTER | Time Twister |  | MOVEMENT | runs | 0.158 | runs | 0.152 |
| 0xD3 | TIME_TWISTER_SSS | Time Twister |  | MOVEMENT | not tried (SSS_PREVIEW) |  | not tried (SSS_PREVIEW) |  |
| 0xD4 | NSANITY_BEACH | N. Sanity Beach | TIME_TWISTER / REMIX | NONE | runs | 0.149 | runs | 0.150 |
| 0xD5 | SNOW_GO | Snow Go | TIME_TWISTER / REMIX2 | NONE | runs | 0.150 | runs | 0.150 |
| 0xD6 | FUTURE_FRENZY | Future Frenzy | TIME_TWISTER / REMIX3 | MOVEMENT | runs | 0.143 | runs | 0.144 |
| 0xD7 | HTP_FALL | Autumn How to Play | HOW_TO_PLAY / REMIX | NONE | runs | 0.146 | runs | 0.148 |
| 0xD8 | STAGE_BATTLEHARBOR | Battle Harbor |  | HAZARDS | - |  | runs | 0.156 |
| 0xD9 | STAGE_EMERALDCOAST | Emerald Coast |  | HAZARDS | - |  | runs | 0.149 |
| 0xDA | STAGE_EMERALDCOAST_REMIX | Emerald Coast Remix | EMERALDCOAST / REMIX | HAZARDS | - |  | runs | 0.154 |
| 0xDB | STAGE_EMERALDSHRINE | Emerald Shrine |  | NONE | - |  | runs | 0.148 |
| 0xDC | STAGE_EMERALDSHRINE_REMIX | Emerald Shrine - Night | EMERALDSHRINE / REMIX | NONE | - |  | runs | 0.151 |
| 0xDD | STAGE_HALBERD | Halberd |  | NONE | - |  | runs | 0.148 |
| 0xDE | RANDOM | (dummy entry: 0xDE means random) |  |  | - |  | not tried |  |
| 0xDF | STAGE_LAUNCHBASE | Launch Base Zone |  | NONE | - |  | runs | 0.148 |
| 0xE0 | STAGE_PUMPKINHILL | Pumpkin Hill |  | NONE | - |  | runs | 0.153 |
| 0xE1 | STAGE_RADICALHIGHWAY | Radical Highway |  | NONE | - |  | runs | 0.143 |
| 0xE2 | STAGE_SKYSANCTUARY | Sky Sanctuary Zone |  | NONE | - |  | runs | 0.154 |
| 0xE3 | STAGE_SUZAKUCASTLE | Suzaku Castle |  | NONE | - |  | runs | 0.154 |
| 0xE4 | STAGE_THEPIT | The Pit |  | NONE | - |  | runs | 0.148 |
| 0xE5 | STAGE_TRAININGSTAGE | Training Stage |  | NONE | - |  | runs | 0.154 |
| 0xE6 | STAGE_TRAININGSTAGE_DL | Training Stage DL | TRAININGSTAGE / DL | NONE | - |  | runs | 0.152 |
| 0xE7 | STAGE_TREETOPTOWN | Tree Top Town |  | HAZARDS | - |  | runs | 0.147 |
| 0xE8 | STAGE_WAFFLEHOUSE | Waffle House |  | NONE | - |  | runs | 0.154 |

Not in the table: the Break the Targets and Board the Platforms stages, vanilla 0x11..0x28 and Remix's 42 (0x4D BTT_DRM, 0x4E BTT_GND, 0x4F BTT_YL, 0x51 BTT_DS, 0x52 BTT_STG1, 0x53 BTT_FALCO, 0x54 BTT_WARIO, 0x56 BTT_LUCAS, 0x57 BTP_GND, 0x59 BTP_DS, 0x5B BTP_DRM, 0x64 BTP_YL, 0x65 BTP_FALCO, 0x66 BTP_POLY, 0x6D BTP_WARIO, 0x70 BTP_LUCAS2, 0x7A BTT_BOWSER, 0x7B BTP_BOWSER, 0x7F BTT_WOLF, 0x80 BTP_WOLF, 0x81 BTT_CONKER, 0x82 BTP_CONKER, 0x87 BTT_MARTH, 0x89 BTT_MTWO, 0x8A BTP_MARTH, 0x8C BTP_MTWO, 0xA0 BTT_SONIC, 0xA1 BTP_SONIC, 0xA8 BTP_SHEIK, 0xAA BTT_SHEIK, 0xAC BTT_MARINA, 0xAE BTP_MARINA, 0xAF BTT_DEDEDE, 0xB2 BTP_DEDEDE, 0xB7 BTT_GOEMON, 0xB9 BTP_GOEMON, 0xC2 BTT_BANJO, 0xC5 BTP_BANJO, 0xCB BTT_CRASH, 0xCE BTP_CRASH, 0xCF BTT_PEACH, 0xD0 BTP_PEACH). They are bonus-game stages, not tried in VS.

Results: every battle-class stage runs from the poke except 0x8B (Allstar Rest Area, which does not
start), in both ROMs. Final Destination and the two beta Dream Lands, which fail in vanilla, run in Remix.

**Stage select by input** (`Stages.asm`): a 6 x 3 grid of icons per page, the last icon of every page is
Random. D-pad moves the cursor (Remix moves vanilla's L/R/C bindings onto the d-pad), **R** is next page,
**Z** previous page, **C-down** steps through the variants of the stage under the cursor (in the order DL,
OMEGA, REMIX, REMIX2.., skipping the ones that stage does not have, then back to the default), L toggles
the hazard mode, A or START starts. The cursor starts on Peach's Castle, page 1. Pages
(`stage_table_normal`):

1. Peach's Castle (0x00), Congo Jungle (0x02), Hyrule Castle (0x04), Planet Zebes (0x03), Mushroom Kingdom (0x08), Meta Crystal (0x0D) / Yoshi's Island (0x05), Dream Land (0x06), Sector Z (0x01), Saffron City (0x07), Duel Zone (0x0E), Final Destination (0x10) / Dragon King (0x35), First Destination (0x2A), Dream Land Beta 1 (0x09), How to Play (0x0B), Battlefield (0x31), Random
2. Crateria (0x4A), Ganon's Tower (0x2B), Spiral Mountain (0x3D), Cool Cool Mountain (0x34), Dr. Mario (0x33), Bowser's Stadium (0x43) / N64 (0x3E), Deku Tree (0x29), Mad Monster Mansion (0x40), Game Boy Land (0x88), Mute City (0x99), Kitchen Island (0x47) / Frosty Village (0x4B), Fray's Stage (0x37), WarioWare, Inc. (0x30), Gym Leader Castle (0x2C), Pokemon Stadium (0x2D), Random
3. Tal Tal Heights (0x2E), Smashville (0x4C), Grim Reaper's Cavern (0xBE), Raid Blue (0x5D), Great Bay (0x36), Fountain of Dreams (0x39) / Tower of Heaven (0x38), Smashketball (0x5A), Norfair (0x5C), Delfino Plaza (0x45), Peach's Castle II (0x44), Big Blue (0x48) / Onett (0x49), Glacial River (0x2F), Hyrule Temple (0x55), New Pork City (0x58), Congo Falls (0x5E), Random
4. Flat Zone (0x32), Pigmask Fortress (0x5F), Yoshi's Story (0x60), Gerudo Valley (0x63), Goomba Road (0x6F), Bowser's Keep (0x7C) / Rith Essa (0x7D), Venom (0x7E), Windy (0x83), dataDyne Central (0x84), Planet Clancer (0x85), Jungle Japes (0x86) / Castle Siege (0x8D), Yoshi's Island II (0x8E), Green Hill Zone (0x9C), Subcon (0x9D), Pirate Land (0x9E), Random
5. Casino Night Zone (0x9F), Metallic Madness (0xA2), Rainbow Road (0xA3), Toad's Turnpike (0xA6), Dracula's Castle (0xB0), Mt. Dedede (0xB3) / Edo Town (0xB4), Twilight City (0xBA), Melrode (0xBB), Scuttle Town (0xBF), Big Boo's Haunt (0xC0), Dinosaur Land (0xC1) / Spawned Fear (0xC3), Poke Floats (0xC6), Big Snowman (0xC7), Discovery Falls (0xCA), Time Twister (0xD2), Random
6. (+EXTRA only) Battle Harbor (0xD8), Emerald Coast (0xD9), Emerald Shrine (0xDB), Halberd (0xDD), Launch Base Zone (0xDF), Pumpkin Hill (0xE0) / Radical Highway (0xE1), Sky Sanctuary Zone (0xE2), Suzaku Castle (0xE3), The Pit (0xE4), Training Stage (0xE5), Tree Top Town (0xE7) / Waffle House (0xE8), Random, Random, Random, Random, Random

Every battle stage is on a page or is a variant of a stage that is, except 0x8B (which does not start
anyway) and vanilla's 0x0A and 0x0C, which Remix registers as variants of 0x09 and 0x05 with
`set_remix_variant` (the generator does not know those two: poke). There is also a tournament layout
(toggle `sss_layout`, 3 pages) which changes the pages; the scripts assume the default layout.

Verified by input with the stock harness (`tools/make_vs_script.py --rom remix|extra ... --verify`; the
stage byte read back is the one asked for in every row):

| ROM | Asked | Path | Stage byte |
|---|---|---|---|
| 2.0.1 | POKEMON_STADIUM | R, to icon 16 | 2D |
| 2.0.1 | POKEMON_STADIUM_2 (variant) | R, icon 16, C-down | A4 (`bench/inputs/vs-remix-pokemonstadium2-4cpu.txt`: `watch 1449 0a4d09 a4`) |
| 2.0.1 | HCASTLE_DL (variant) | icon 2, C-down | 67 |
| 2.0.1 | TIME_TWISTER | R x 4, icon 16 | D2 |
| 2.0.1 | SNOW_GO (second variant) | R x 4, icon 16, C-down x 2 | D5 |
| 2.0.1 | HYRULE_CASTLE | icon 2 | 04 (`bench/inputs/vs-remix-hyrule-4cpu.txt`) |
| +EXTRA | STAGE_HALBERD | R x 5, icon 3 | DD (`bench/inputs/vs-extra-halberd-4cpu.txt`: `watch 1439 0a4d09 dd`) |
| +EXTRA | STAGE_EMERALDCOAST_REMIX (variant) | R x 5, icon 1, C-down | DA |
| +EXTRA | STAGE_WAFFLEHOUSE | R x 5, icon 12 | E8 |

### Fighters

Ids continue vanilla's: the full `define_character` calls in `ref/smashremix/src/Character.asm` take the next
id in file order from 0x1D (0x1B is Remix's "random" placeholder, 0x1C none); +EXTRA appends its
`extra_characters` folders in sorted order from 0x61. "Kind": NA a full fighter, J/E the Japanese/European
version, POLYGON, SPECIAL (bosses, joke and hidden fighters). "ms": Mac emulation ms per frame of four of
that fighter as level-9 CPUs on Dream Land in the +EXTRA ROM (poke start).

| id | Constant | Parent | Kind | ms |
|---|---|---|---|---|
| 0x00 | Mario |  |  | 0.149 |
| 0x01 | Fox |  |  | 0.149 |
| 0x02 | Donkey Kong |  |  | 0.145 |
| 0x03 | Samus |  |  | 0.156 |
| 0x04 | Luigi |  |  | 0.135 |
| 0x05 | Link |  |  | 0.145 |
| 0x06 | Yoshi |  |  | 0.149 |
| 0x07 | Captain Falcon |  |  | 0.143 |
| 0x08 | Kirby |  |  | 0.142 |
| 0x09 | Pikachu |  |  | 0.144 |
| 0x0A | Jigglypuff |  |  | 0.137 |
| 0x0B | Ness |  |  | 0.154 |
| 0x0C | Master Hand |  |  | 0.118 |
| 0x0D | Metal Mario |  |  | 0.145 |
| 0x0E | Polygon Mario |  |  | 0.132 |
| 0x0F | Polygon Fox |  |  | 0.136 |
| 0x10 | Polygon DK |  |  | 0.137 |
| 0x11 | Polygon Samus |  |  | 0.143 |
| 0x12 | Polygon Luigi |  |  | 0.134 |
| 0x13 | Polygon Link |  |  | 0.131 |
| 0x14 | Polygon Yoshi |  |  | 0.130 |
| 0x15 | Polygon Falcon |  |  | 0.134 |
| 0x16 | Polygon Kirby |  |  | 0.136 |
| 0x17 | Polygon Pikachu |  |  | 0.137 |
| 0x18 | Polygon Jigglypuff |  |  | 0.132 |
| 0x19 | Polygon Ness |  |  | 0.146 |
| 0x1A | Giant DK |  |  | 0.152 |
| 0x1B | RANDOM | CAPTAIN | SPECIAL | not a fighter |
| 0x1C | NONE |  | (empty slot marker) | not a fighter |
| 0x1D | FALCO | FOX | NA | 0.153 |
| 0x1E | GND | CAPTAIN | NA | 0.145 |
| 0x1F | YLINK | LINK | NA | 0.159 |
| 0x20 | DRM | MARIO | NA | 0.150 |
| 0x21 | WARIO | MARIO | NA | 0.149 |
| 0x22 | DSAMUS | SAMUS | SPECIAL | 0.152 |
| 0x23 | ELINK | LINK | E | 0.148 |
| 0x24 | JSAMUS | SAMUS | J | 0.153 |
| 0x25 | JNESS | NESS | J | 0.152 |
| 0x26 | LUCAS | NESS | SPECIAL | 0.151 |
| 0x27 | JLINK | LINK | J | 0.145 |
| 0x28 | JFALCON | CAPTAIN | J | 0.147 |
| 0x29 | JFOX | FOX | J | 0.152 |
| 0x2A | JMARIO | MARIO | J | 0.149 |
| 0x2B | JLUIGI | LUIGI | J | 0.136 |
| 0x2C | JDK | DONKEY | J | 0.145 |
| 0x2D | EPIKA | PIKACHU | E | 0.147 |
| 0x2E | JPUFF | JIGGLYPUFF | J | 0.133 |
| 0x2F | EPUFF | JIGGLYPUFF | E | 0.143 |
| 0x30 | JKIRBY | KIRBY | J | 0.140 |
| 0x31 | JYOSHI | YOSHI | J | 0.147 |
| 0x32 | JPIKA | PIKACHU | J | 0.147 |
| 0x33 | ESAMUS | SAMUS | E | 0.153 |
| 0x34 | BOWSER | YOSHI | NA | 0.149 |
| 0x35 | GBOWSER | YOSHI | SPECIAL | 0.151 |
| 0x36 | PIANO | MARIO | SPECIAL | 0.132 |
| 0x37 | WOLF | FOX | NA | 0.142 |
| 0x38 | CONKER | FOX | NA | 0.156 |
| 0x39 | MTWO | YOSHI | NA | 0.159 |
| 0x3A | MARTH | CAPTAIN | NA | 0.139 |
| 0x3B | SONIC | FOX | NA | 0.153 |
| 0x3C | SANDBAG | CAPTAIN | SPECIAL | 0.108 |
| 0x3D | SSONIC | FOX | SPECIAL | 0.141 |
| 0x3E | SHEIK | CAPTAIN | NA | 0.151 |
| 0x3F | MARINA | CAPTAIN | NA | 0.147 |
| 0x40 | DEDEDE | CAPTAIN | NA | 0.151 |
| 0x41 | GOEMON | MARIO | NA | 0.140 |
| 0x42 | PEPPY | FOX | SPECIAL | 0.147 |
| 0x43 | SLIPPY | FOX | SPECIAL | 0.148 |
| 0x44 | BANJO | CAPTAIN | NA | 0.154 |
| 0x45 | MLUIGI | LUIGI | SPECIAL | 0.135 |
| 0x46 | EBI | MARIO | SPECIAL | 0.138 |
| 0x47 | DRAGONKING | CAPTAIN | SPECIAL | 0.134 |
| 0x48 | CRASH | MARIO | NA | 0.146 |
| 0x49 | PEACH | FOX | NA | 0.150 |
| 0x4A | ROY | CAPTAIN | SPECIAL | 0.144 |
| 0x4B | DRL | LUIGI | SPECIAL | 0.134 |
| 0x4C | LANKY | MARIO | SPECIAL | 0.145 |
| 0x4D | NWARIO | MARIO | POLYGON | 0.143 |
| 0x4E | NLUCAS | NESS | POLYGON | 0.153 |
| 0x4F | NBOWSER | YOSHI | POLYGON | 0.154 |
| 0x50 | NWOLF | FOX | POLYGON | 0.147 |
| 0x51 | NDRM | MARIO | POLYGON | 0.148 |
| 0x52 | NSONIC | FOX | POLYGON | 0.139 |
| 0x53 | NSHEIK | CAPTAIN | POLYGON | 0.142 |
| 0x54 | NMARINA | CAPTAIN | POLYGON | 0.151 |
| 0x55 | NFALCO | FOX | POLYGON | 0.154 |
| 0x56 | NGND | CAPTAIN | POLYGON | 0.135 |
| 0x57 | NDSAMUS | SAMUS | POLYGON | 0.142 |
| 0x58 | NMARTH | CAPTAIN | POLYGON | 0.137 |
| 0x59 | NMTWO | YOSHI | POLYGON | 0.140 |
| 0x5A | NDEDEDE | CAPTAIN | POLYGON | 0.145 |
| 0x5B | NYLINK | LINK | POLYGON | stops after 1428 tics |
| 0x5C | NGOEMON | MARIO | POLYGON | 0.158 |
| 0x5D | NCONKER | FOX | POLYGON | 0.148 |
| 0x5E | NBANJO | CAPTAIN | POLYGON | 0.148 |
| 0x5F | NPEACH | FOX | POLYGON | 0.159 |
| 0x60 | NCRASH | MARIO | POLYGON | 0.137 |
| 0x61 | BIRDO | YOSHI | +EXTRA | 0.138 |
| 0x62 | CBKNUCKLES | FOX | +EXTRA | 0.141 |
| 0x63 | CBMKNUCKLES | FOX | +EXTRA | 0.132 |
| 0x64 | CLOUD | LINK | +EXTRA | 0.137 |
| 0x65 | DKULT | DONKEY | +EXTRA | 0.142 |
| 0x66 | KAZUYA | CAPTAIN | +EXTRA | 0.138 |
| 0x67 | KEN | CAPTAIN | +EXTRA | 0.133 |
| 0x68 | KNUCKLES | FOX | +EXTRA | 0.131 |
| 0x69 | MKNUCKLES | FOX | +EXTRA | 0.131 |
| 0x6A | MRGAW | MARIO | +EXTRA | 0.132 |
| 0x6B | MRGAWPLUS | MARIO | +EXTRA | 0.131 |
| 0x6C | MRGAWTHREED | MARIO | +EXTRA | 0.147 |
| 0x6D | METAKNIGHT | JIGGLYPUFF | +EXTRA | 0.138 |
| 0x6E | REBECCA | FOX | +EXTRA | 0.142 |
| 0x6F | RYU | CAPTAIN | +EXTRA | 0.131 |
| 0x70 | SNAKE | CAPTAIN | +EXTRA | 0.135 |
| 0x71 | SPIDERMAN | CAPTAIN | +EXTRA | 0.135 |
| 0x72 | TERRY | CAPTAIN | +EXTRA | 0.134 |
| 0x73 | YZELDA | FOX | +EXTRA | 0.142 |

113 of the 114 kinds tried fought for the whole run; four Polygon Young Links (0x5B) stopped after 1,428
tics. Sandbag (0x3C) runs but never attacks.

**Character select by input.** Remix keeps vanilla's screen logic (cursor speed, HMN/CPU button, level
arrows at the same cursor positions: measured) and replaces the portraits with a 10 x 3 grid of 24 px
cells. Measured by sweeping the cursor: the puck is over column c when the cursor x is 25 + 24c .. 48 +
24c and over row r when the cursor y is 55 + 24r .. 78 + 24r (centre (37 + 24c, 67 + 24r)); left of the
grid is the bonus bookend, right of it Random (0x1B). The grid is the same in 2.0.1 and on +EXTRA's
first page:

    MARINA  DRM    LUIGI MARIO DONKEY LINK SAMUS   CAPTAIN    GND   SONIC
    DEDEDE  YLINK  NESS  YOSHI KIRBY  FOX  PIKACHU JIGGLYPUFF FALCO SHEIK
    GOEMON  CRASH  WARIO PEACH BOWSER WOLF CONKER  MTWO       MARTH BANJO

While a puck is held over a portrait the d-pad picks that fighter's variant: up special, down polygon,
left J, right E (`Character.variants`: for example Mario: Metal, Polygon, J; Samus: -, Polygon, J, E).
Checked: JFOX and NFOX from Fox, JSAMUS, EPIKA, NWOLF. Not every special fighter is behind its parent's
"up" (Dark Samus is not on Samus), and +EXTRA's 19 added fighters are not on the first page; the generator
stops with a message when the slot's fighter is not the one asked for. Those are poke only.

One thing differs from vanilla and matters for any scripted cursor: **each portrait a puck crosses loads
that fighter's model and the game drops ticks while it does, for every port's cursor.** An open-loop
script that is exact in vanilla lands 12 px short in Remix (measured: cursor at (194, 85) instead of
(206, 73)). `make_vs_script.py` therefore moves along the bottom, then up at 2 px a frame, and corrects
from the cursor positions read out of RDRAM. Remix's time rule is left alone (8 minutes and 4 stocks), so
an input-started Remix match ends; the poke start uses infinite time.

**Toggles that would help** (`Toggles.asm`; none was exercised, they live in Remix's Settings menu and in
SRAM, and the harness starts from blank saves): `default_cpu_level` (CPUs would appear at the chosen level without
the six arrow presses), `skip_results_screen`, `skip_start_cheer`, `sss_layout` (normal or tournament),
`hazard_mode`, `variant_random`, the per-stage `random_stage_*` toggles (with A on the Random icon the
stage comes from the enabled set; C-down on Random is also bound), `improved_ai`, `fps`.
Random with one stage enabled would be a stage picker, but it costs more inputs than the page navigation
above. There is no debug stage select in the VS path; vanilla's debug scenes (0x02..0x06) were not tried.

## 5. Recommendation for the matrix

**Add a poke to the harness and use the same case description for all three ROMs.** What it must do:

1. Run `bench/inputs/boot-to-mode-select.txt` (START at 200 and 420).
2. After each frame, if RDRAM byte `0x800A4AD0` (index `0x0A4AD0 ^ 3` in the core's RDRAM) is `0x07` for
   the first time, write the bytes of section 2 on that frame: for each port `0x800A4D28 + 0x74 * port`
   (11 bytes, or `02 1C` at +2 for an unused port), `0x800A4D09` (7 bytes), `0x800A4ADF` (stage),
   `0x800A4AD0` = `0x16`. It triggered on frame 421 in all three ROMs.
3. Treat the case as started when `0x800A4AD0` reads `0x16` and the u32 at `0x800A4D20` (`time_passed`)
   counts, and as broken if it has not passed 600 by frame 1,500. Measure from frame 1,000 (the battle clock starts
   around frame 845).

A generic form is enough and stays ROM-agnostic: `--poke-when ADDR VALUE ADDR HEXBYTES` (repeatable),
which is what `tools/vs_poke_plan.py` prints. On the Switch the same works through the core's RDRAM
pointer, or as an `inputs=`-style list in the ini. Keep `tools/make_vs_script.py` scripts for the cases
where the menus themselves should be part of the run, and as a cross-check that the poke start and the
menu start load the same stage (collision line count and default BGM at `0x80131384` / `0x801313A0` agree
for the stages tried both ways).

**What is likely heaviest.** The Mac numbers above are the emulated CPU only (null video): four CPUs cost
0.12 to 0.14 ms a frame in vanilla and 0.13 to 0.18 in Remix, so game logic varies by about a third across
stages and by about a fifth across fighters. On the console the picture (GLideN64) will dominate and
scale with geometry, transparency and the camera's zoom-out, which this run cannot see. Candidates, from
the measured logic cost (stages that are in the top of both Remix runs) plus size:

- vanilla: Hyrule Castle (largest, tornados), Sector Z (largest model, Arwings), Saffron City (Pokemon),
  Peach's Castle and Planet Zebes (moving parts, acid); Race to the Finish 0x0F is the costliest logic.
- Remix: Poke Floats 0xC6, Big Blue 0x48, Bowser's Stadium 0x43, Pokemon Stadium 0x2D and Stadium 2 0xA4
  (transformations), Toad's Turnpike 0xA6, Rainbow Road 0xA3, Melrode 0xBB, Spawned Fear 0xC3, Scuttle
  Town 0xBF, Mt. Dedede 0xB3, Dinosaur Land 0xC1, Twilight City 0xBA, Gym Leader Castle 0x2C, Hyrule
  Temple 0x55 (largest), Delfino Plaza 0x45 and Peach's Castle II 0x44 (travelling stages).
- +EXTRA: Battle Harbor 0xD8 (its costliest logic, 0.156), Emerald Coast 0xD9 and Tree Top Town 0xE7 (the
  three with hazard code), Radical Highway 0xE1 (most collision lines, 32) and Halberd 0xDD.
- fighters: projectile and effect users. Measured top: Young Link 0x1F, Mewtwo 0x39, Conker 0x38, Samus
  0x03, Ness 0x0B, Banjo 0x44, Falco 0x1D, Sonic 0x3B; lightest Sandbag, Master Hand. For the picture add
  the large models: Bowser 0x34, Giga Bowser 0x35, Giant DK 0x1A, Dedede 0x40, Ganondorf 0x1E.

**Proposed cases** (each: poke start, time rule, infinite time, items as the ROM defaults, level 9;
measure frames 1,000 to 8,200 = two minutes):

| # | ROM | Stage | Fighters (port 1..4) | Why |
|---|---|---|---|---|
| 1 | vanilla | 0x06 Dream Land | 00 08 | baseline, two fighters, the netplay stage |
| 2 | vanilla | 0x06 Dream Land | 01 03 05 09 | four projectile users, small stage |
| 3 | vanilla | 0x04 Hyrule Castle | 01 03 05 09 | largest vanilla stage |
| 4 | vanilla | 0x01 Sector Z | 01 03 05 09 | largest model, Arwing |
| 5 | vanilla | 0x07 Saffron City | 01 03 05 09 | Pokemon spawns |
| 6 | vanilla | 0x00 Peach's Castle | 02 02 02 02 | four of the largest fighter |
| 7 | vanilla | 0x03 Planet Zebes | 0B 0B 03 03 | acid, PK effects |
| 8 | vanilla | 0x08 Mushroom Kingdom | 04 07 0A 0B | the locked stage and fighters |
| 9..16 | Remix 2.0.1 | cases 1..8 again | same ids | what Remix itself costs on vanilla content |
| 17 | Remix 2.0.1 | 0xC6 Poke Floats | 1D 1E 34 3B | top logic cost, moving geometry |
| 18 | Remix 2.0.1 | 0x48 Big Blue | 1F 39 38 44 | top logic cost, scrolling; costliest fighters |
| 19 | Remix 2.0.1 | 0x43 Bowser's Stadium | 34 35 40 1E | hazards, largest fighters |
| 20 | Remix 2.0.1 | 0x2D Pokemon Stadium | 1F 39 38 03 | transformations |
| 21 | Remix 2.0.1 | 0x55 Hyrule Temple | 1D 1E 34 3B | largest Remix stage |
| 22 | Remix 2.0.1 | 0xA6 Toad's Turnpike | 3B 3B 3B 3B | traffic hazard, same fighter four times |
| 23 | Remix 2.0.1 | 0xA3 Rainbow Road | 01 03 05 09 | |
| 24 | Remix 2.0.1 | 0x45 Delfino Plaza | 01 03 05 09 | travelling stage: loads while playing |
| 25 | Remix 2.0.1 | 0x31 Battlefield | 3A 4A | what a netplay match looks like: two fighters, legal stage |
| 26..34 | +EXTRA 0.6.0 | cases 17..25 again | same ids | +EXTRA against 2.0.1 on the same content |
| 35 | +EXTRA 0.6.0 | 0xDD Halberd | 64 6F 70 6D | +EXTRA stage and fighters (Cloud, Ryu, Snake, Meta Knight) |
| 36 | +EXTRA 0.6.0 | 0xD9 Emerald Coast | 68 66 72 73 | Knuckles, Kazuya, Terry, Young Zelda |
| 37 | +EXTRA 0.6.0 | 0xE1 Radical Highway | 61 65 6A 71 | Birdo, DK (Ultimate), Mr. Game & Watch, Spider-Man |
| 38 | +EXTRA 0.6.0 | 0xD8 Battle Harbor | 64 64 64 64 | |

A full sweep (every stage once with fighters 01 03 05 09, 2 minutes each: 17 + 146 + 162 runs, about 11
hours on a console at real time) is the other option once the poke exists; on the Mac the same sweep took
9 minutes.

## 6. Not proven

- Nothing here ran on a Switch. The pokes depend only on RDRAM contents and frame boundaries, which the
  Mac and Switch builds share, but the frame on which the scene byte changes was only observed on the Mac
  (ARM64 recompiler).
- The poke facility itself: it exists only in the scratch harness below. The stock harness cannot write
  memory; with it the addresses were proved by reading only.
- Remix's per-match extras under a poke start (hazard mode, music, variant bookkeeping) are whatever boot
  left; stages and fighters loaded and fought, but a stage whose behaviour depends on the stage select
  having run would not show it here. 0x8B does not start, 0x5B x 4 stops; why was not looked into.
- Each sweep run was 6,450 tics with one fighter set; "runs" means the battle clock kept counting and
  four fighter objects existed, not that the stage was looked at. No picture was rendered.
- The Break the Targets / Board the Platforms / RTTF ids as VS stages, team battles, items off, and
  Remix's toggles (addresses unknown) were not tried. Vanilla's debug scenes were not tried.
- Input fallback: +EXTRA's added fighters, special variants and stages that are on no page cannot be
  picked; Remix's time setting is not changed. The generated scripts are tied to the exact ROM (a lost tick
  moves a cursor), which is why the generator checks itself against the harness.
- The ms figures were taken while other harness runs were going on the same Mac, and exclude the picture:
  use them for ordering, not as numbers.

## 7. Reproducing

Files:

- `tools/vs_poke_plan.py`: the pokes for a case (`--stage`, `--fighters`, `--level`, `--human`, `--time`,
  `--stocks`, `--items`, `--c`).
- `tools/make_vs_script.py`: the input-only script generator (`--rom vanilla|remix|extra`, `--list`,
  `--verify`).
- `tools/read_battle_state.py DUMP [ADDR[:LEN]..]`: decodes a `--dump-ram` file.
- `tools/remix_ids.py [--extra] [--stages|--characters] [--tsv]`: the id tables from the sources in `ref/`.
- `bench/inputs/boot-to-mode-select.txt` and the `bench/inputs/vs-*.txt` examples listed above.
- `ref/smashremix` (tag 2.0.1) and `ref/smashremix-plus-extra` (tag 0.6.0), cloned for this.

Stock harness, reading only:

    tools/make_vs_script.py --rom vanilla --stage SECTOR_Z --fighters DK,YOSHI,MARIO,KIRBY -o /tmp/s.txt --verify
    build/mac-arm64/znx-bench --rom ROM --frames 2500 --inputs bench/inputs/vs-vanilla-hyrule-4cpu.txt --hash-every 0 --watch 0x800A4D09
    build/mac-arm64/znx-bench --rom ROM --frames 2500 --inputs bench/inputs/vs-vanilla-hyrule-4cpu.txt --hash-every 0 --dump-ram 2500 /tmp/ram.bin
    tools/read_battle_state.py /tmp/ram.bin

Scratch harness with pokes (under `build/`, which git ignores; it is not a deliverable and can be deleted):
`build/stage-research/src/` is a copy of `bench/bench_core.c`, `bench_core.h` and `znx-bench.c` taken on
2026-11-12 with about 50 added lines in `frame_done`: `ZNX_WATCH=addr,addr,..` watches several bytes, and
`ZNX_POKE=file` applies lines `at FRAME ADDR HEX`, `when CADDR CVAL DELAY ADDR HEX` (once, DELAY frames
after the byte at CADDR first equals CVAL) or `while CADDR CVAL ADDR HEX`. Built with
`clang -O2 -Ibuild/src/mupen64plus-core/src/api build/stage-research/src/znx-bench.c
build/stage-research/src/bench_core.c -o build/stage-research/bin/znx-bench-sr`, with the libraries of
`build/mac-arm64` linked into `build/stage-research/bin/`.

    tools/vs_poke_plan.py --stage 0xDD --fighters 0x64,0x6F,0x70,0x6D > /tmp/p.txt
    ZNX_POKE=/tmp/p.txt ZNX_WATCH=800A4AD0 build/stage-research/bin/znx-bench-sr --rom build/remix/remix-extra-0.6.0.z64 \
        --frames 4000 --inputs bench/inputs/boot-to-mode-select.txt --hash-every 0 --dump-ram 4000 /tmp/ram.bin
    tools/read_battle_state.py /tmp/ram.bin

`build/stage-research/sweep.py` ran the stage and fighter sweeps (`sweep-stages.tsv`, `sweep-chars.tsv`),
`scan.py` the cursor sweep of the Remix character select.

## Results on a Switch, 2026-11-13

Erista console at 1785 MHz, `matrix=<file>` in the ini (switch/game_switch.c), four level 9 CPUs,
fighters 0 to 3, 1,800 to 3,000 timed frames a stage from frame 660. Times are the emulation thread's
CPU time per frame, picture included.

| Run | Stages | Slowest stage: average / 99 % of frames under | Frames over 16.7 ms |
|---|---|---|---|
| Vanilla, before compiling ahead | 14 | 5.6 / 10.4 ms | 5 to 12 a match |
| +EXTRA 0.6.0, before | 161 | 6.9 / 11.8 ms | 1,797 in all (649 over 33 ms) |
| Vanilla, compiling ahead, vsync on | 14 | 5.8 / 6.9 ms | 0 (worst frame 9.9 ms) |
| +EXTRA 0.6.0, compiling ahead, vsync on | 161 | 6.9 / 8.5 ms | 0 (worst frame 14.2 ms) |
| Remix 2.0.1, compiling ahead, vsync on | 144 of 145 | 6.8 / 8.7 ms | 2, both a shader compiled for the first time |
| Every fighter, four at a time on Dream Land | 56 matches | 99 % under 8.3 ms | 21, all first-time shaders |

What the "before" frames were: the recompiler meeting a block for the first time (about 0.5 ms each,
about 1,200 a match). What is left after: a shader GLideN64 has not built on this console before (60 to
210 ms, once per console per shader). The app now ships the 62 combiner keys these runs needed
(assets/shaders); on a console with its own lists deleted, three matches that had hitched ran with no
frame over 16.7 ms and none shown late.

Not closed:
- With vsync on, in about 40 % of matches one frame (rarely two) near the start of the fight is handed
  to the display 9 to 12 ms later than its turn (gap 25 to 30 ms between two swaps instead of 16.7; never
  33). The emulation thread is not busy during it (under 12 ms of CPU): the time is spent blocked in a GL
  call before the swap. Not yet traced.
- Remix 2.0.1 stage 0xD7 (Autumn How to Play), the last case of that run, has no result: the app closed
  without a line in the log.
- Online (console hosting over Wi-Fi, a Mac joining with 30 ms of added latency each way): all 15
  checkpoints of 9,000 frames identical on both sides. Frames the session did not hold for the other
  player's input: no frame over 11.3 ms while there were no rollbacks; with rollbacks, 13 frames of
  23 to 36 ms, because frames re-run after a rollback still drew their display lists. Patch 0013 leaves
  the drawing out of those frames; it passes the rollback stress test on the Mac and has not run on a
  console yet (rollback_stress=K,D in the ini does it with the picture on). The console's Wi-Fi link
  itself spiked to 119 ms with the app idle, and the session held 12 to 27 s of a 150 s match for it.

### Rollback on the console after patches 0014-0015, 2026-11-13

`rollback_stress=10,7` now does what RMG-K's session does (a save after every frame and every re-run
frame, into preallocated slots). Console, picture on, one stage per game, 365 rollbacks of 7 frames each,
every re-run frame identical:

| | Before (whole copies) | After (written pages only) |
|---|---|---|
| Save | 2.0 ms | 0.14-0.19 ms |
| Load | 2.4 ms | 0.15-0.23 ms |
| Re-run, per frame | 1.4-1.5 ms | 1.3-1.7 ms |
| 7-frame rollback with its saves, average | about 27 ms (computed) | 10.3-13.0 ms (measured) |
| Worst | | 21 ms |

About 110 of 2,048 RDRAM pages are copied per save. What is left is re-simulation itself (on the Mac:
about 65 % recompiled game code, 20 % RSP HLE audio, 8 % the recompiler's linker): a frame carrying a full
7-frame rollback still lands at 15-18 ms; rollbacks of up to 5 frames fit.

### Late frames and the closed app, 2026-11-13

- Late frames (vsync on, a frame shown 9-13 ms after its turn): the same frame numbers are late in every
  run of a stage (for example 839-842 in many stages, 1011/1107/2279 on Mini Yoshi's Island), with the
  compile-ahead at 12 ms or at 8 ms with oversized blocks off, so they come from the game, not from the
  recompiler. The emulation thread's CPU time on those frames stays under 13 ms while 16-22 ms pass before
  the swap and the swap then waits 6-13 ms: the thread is blocked in the GL driver (a texture or buffer
  stall is the likely cause). Never two frames. Next step: time GLideN64's GL calls on those frames.
- The app closing during Remix stage 0xD7: that stage passes on its own (twice). It was the 145th emulator
  start in one session; 40 starts in one session keep the heap at 17.3-17.5 MB (about 5 KB per start), so
  it is not a memory leak in malloc. Not reproduced; a longer run (more than 145 starts) would be needed.
