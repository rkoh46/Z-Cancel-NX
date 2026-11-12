# Z-Cancel NX: plan and findings

Goal: Super Smash Bros. (N64) and Smash Remix on a Nintendo Switch with rollback netplay against the
PC players who use [RMG-K](https://github.com/Jay-Day/RMG-K), at a locked 60 fps, from the player's own
ROM. Vanilla, Remix and Remix +Extra are picked from a menu; the app builds the patched ROMs itself.

## Why a port of RMG-K and not a recompilation

Rollback sends inputs only, so both sides must emulate bit for bit the same machine from power-on.
RMG-K syncs the whole emulated console (timers, interrupts, RSP tasks). A static recompilation
(smash64r) replaces the N64 OS layer with host code, so its timing differs from an emulator even with
identical game code: it could only play against itself. It also covers vanilla only and has no netcode.
It stays plan B for speed if the emulator cannot roll back fast enough on the Switch.

## What RMG-K does (read from its source, 2f5454a, v0.8.818)

Paths are relative to the RMG-K tree.

- CPU: netplay and rollback force `R4300Emulator = 2`, the new_dynarec recompiler
  (`Source/RMG-Core/Emulation.cpp:900`, again at 1154/1188/1239). On x86-64 that is new_dynarec's x64
  backend; the same recompiler has an arm64 backend (`projects/unix/Makefile:137-141`,
  `-DNEW_DYNAREC=4`, `linkage_arm64.S`) that no RMG-K build uses yet. COP1 maths goes through the C
  helpers in `fpu.h` on both (`ACCURATE_FPU=1`).
- Settings a session forces (`Emulation.cpp:880-916`): HLE RSP, CountPerOp 0 and DenomPot 0 (ROM
  database values), SiDmaDuration -1, 8 MB always, no save-file loading (saves live in memory:
  `M64CMD_ROLLBACK_SET_SAVE_DATA`). `M64CMD_ROLLBACK_SET_DETERMINISTIC` only turns off
  `randomize_interrupt` (`api/frontend.c:468`). The RTC still reads the wall clock.
- Rollback in the core: `savestates_save_rollback_buffer` (`savestates.c:2996`) writes a normal state
  without the 8 MB TLB lookup table, about 8.4 MB, into RAM; load is deferred to a safe interrupt point;
  `M64CMD_ROLLBACK_RUN_FRAME` -> `main_rollback_run_frame` (`main.c:1405`). Re-simulated frames skip
  `gfx.updateScreen` and audio output; the RSP and the video plugin's display-list processing still run.
  Inputs: one 4-byte BUTTONS per player injected into the PIF controller read (`pif.c:327-410`).
- Desync detection is not there: the state checksum is a stub that is always 0
  (`savestates.c:2657`, `rmgk_gekko.hpp:237`). We need our own state hash as a local diagnostic.
- Netcode: GekkoNet (bundled, `GekkoNet-main`) carries rollback. Three ways in:
  - Lobby: GekkoNet over libjuice ICE; lobby server over WebSocket (`LobbyConnectDialog.cpp:32`), STUN
    `stun.l.google.com:19302`, TURN from the server's HELLO_OK; HELLO carries `clientVersion` and ROM
    MD5s, a prematch manifest syncs cheats and saves (`LobbyClient.cpp:328-412, 824-846`).
  - P2P with connect codes: `nat.smash64.net:6364` UDP text protocol `N02TRAV1|...`
    (`KailleraTraversalConfig.hpp`); public waiting games `p2plist.smash64.net:27887`; mode agreed in
    chat (`RMGK:MODE:STD|RB`, `RMGK:RBDELAY/RBPRED/RBPACE`); rollback tunnelled over the n02 socket
    (`n02/core/p2p_core.cpp:104-139`).
  - Kaillera servers: delay-based (n02 `kcore`), list from kaillerareborn.2manygames.fr.
- Peers compare only the app string `"RMG-K " + version` (a mismatch is a chat warning) and the ROM's
  GoodName; the video plugin is neither forced nor checked.
- Qt: none in `Source/RMG-Core`, `Source/n02`, `GekkoNet-main`. The lobby client, traversal and server
  browser are in `Source/RMG` (Qt) and need rewriting. Libraries: minizip, lzma, libjuice, asio (in
  GekkoNet), zlib, libpng, SDL3, SDL3_net; core and plugins are loaded with dlopen.

## Risks, in the order they get tested

1. Speed: a rollback re-runs several frames inside one 16.7 ms frame, with RSP HLE and display-list
   processing still running. Unknown on a 2 GHz Cortex-A57 until measured.
2. Bit-exact emulation across x86-64 and ARM64: the two new_dynarec backends, float-to-int out of range
   (`(int32_t)truncf(x)`, `fpu.h:347`: 0x80000000 on x86, saturating on ARM), NaN payloads, FMA
   contraction (`-ffp-contract=off` needed).
3. Recompiled code on Switch homebrew: new_dynarec wants memory that is writable and executable;
   Horizon gives separate writable and executable views (libnx `Jit`). mupen64plus-libretro-nx already
   runs new_dynarec on the Switch and is the reference.
4. Video plugin side effects: GLideN64 can write framebuffers back into RDRAM, which is game-visible
   state. The PC side does not force its settings.
5. Plumbing: static linking instead of dlopen, lobby/traversal protocols without Qt, asio and libjuice on
   libnx sockets.

## Steps

1. Speed test (go/no-go): RMG-K's core for the Switch, no picture, RSP HLE, new_dynarec arm64; frames per
   second in a fight and the cost of a rollback save and load.
2. Determinism rig on the Mac: the same core built for arm64 and for x86-64 (Rosetta), fed the same
   inputs, RDRAM hashed every frame. Finds every cross-architecture difference without a second player.
3. Picture, sound, controllers (GLideN64 on the Switch's OpenGL; GameCube adapter and menus from
   Melee-NX-NetPlay).
4. Netplay: GekkoNet, connect codes, public list, lobby; a controller-driven lobby screen.
5. Cross-play against RMG-K on a PC.
6. ROM picker and on-device patching (vanilla ROM in; Remix and Remix +Extra built from their xdelta
   patches and cached). Nothing from a game is ever committed or uploaded.

## Results so far

### Mac harness (`builder/build-mac.sh arm64|x86_64`, `bench/znx-bench.c`), 2026-11-12

RMG-K's core and RSP HLE, a null video plugin that only raises the DP interrupt, RMG-K's forced netplay
settings, frames driven with `M64CMD_ROLLBACK_EXECUTE`. Vanilla US ROM, no input (logo, intro, title at
frame 4313, CPU demo fight from 4983). Apple M5, so only the ratios carry over to the Switch.

- new_dynarec arm64 runs natively on macOS with two patches (`patches/`): Mach-O assembly syntax; x18 is
  not preserved on Darwin, so it is no longer allocated; the code cache is an anonymous RW mapping with an
  RX view from `mach_vm_remap` (RWX is refused and the shm double mapping dies with SIGBUS).
- Speed without a picture: about 0.16 ms of emulation per frame in the intro; a rollback state is
  8,438,120 bytes, saved in about 0.15 ms, loaded in about 6.5 ms; re-simulating 60 frames inside the demo
  fight costs about 0.27 ms per frame. The load cost is the number to watch on the Switch.
- Rollback is exact on each architecture: save at A, run to B, load, re-run gives identical RDRAM hashes
  for every frame (intro, demo fight, scripted menu input).
- ARM64 against x86-64 (Rosetta): RDRAM is not bit-identical. The first differing frame is 266; about 14%
  of the first 10,897 frames differ by 1 to 6 words and the state reconverges each time; the scene
  sequence is identical. Every difference seen sits in two small areas (0x80040D04-DC4, 0x80040560) laid
  out like a saved libultra thread context, in caller-saved register slots (at, v0, a0, a1, t6, t8, t9)
  with the saved PC equal. Likely cause, not proven: the two backends leave different stale values in
  registers the recompiler treats as dead when an interrupt saves the thread. If so it never reaches game
  logic, and RMG-K does not compare checksums (its checksum is always 0), so it would not break a session;
  a raw RDRAM hash cannot be the desync check, though. Next: prove or disprove it, and compare a real VS
  match with inputs, not only the demo.
- The interpreters differ from the recompiler from frame 1 at the RDRAM level, so the Switch must run
  new_dynarec, as the PCs do.

### Switch harness (`builder/build-switch.sh`, `switch/`), 2026-11-12: builds, not yet run on a console

The same measurement (`bench/bench_core.c`, shared with the Mac harness) as `build/switch/znx-bench.nro`.
Put the ROM at `sdmc:/switch/z-cancel-nx/rom.z64`; output goes to nxlink or the screen and to
`sdmc:/switch/z-cancel-nx/bench.log`; optional `bench.ini` (`frames=`, `hash_every=`, `rollback_test=A,B`,
`inputs=`, `cpu_mhz=`, `verbose=`). Defaults: 7200 frames, hash every 600, rollback test 5400..5460.

- One program: core, RSP HLE and the null video plugin are linked in (`ld -r`, `objcopy --localize-hidden`,
  plugin exports renamed `znxp_<plugin>_*`) and found through generated name tables (`switch/dynlib_static.c`).
- Code cache: 32 MB of the program's `.text` (`switch/jit_memory.S`), written through a second view from
  `svcMapProcessMemory` (`switch/jit_switch.c`, `patches/0003`), as mupen64plus-libretro-nx does. x18 stays in
  use (Horizon keeps it; Darwin does not).
- `builder/build-switch.sh --selftest ROM` builds the same recipe for the container's Linux (ARM64) and runs it,
  with libnx replaced by `switch/selftest/linux_main.c`. It gives the Mac build's RDRAM hashes at every 600th
  frame up to 7200 (`hash 3600 1499ebb25c8606c4`, `hash 7200 873c9f0585a22f37`) in both memory layouts, and the
  rollback test passes: the console should print the same hashes.
- Known cost to watch: loading a state invalidates the whole 32 MB code cache (`invalidate_all_pages`), a data
  and instruction cache flush of all of it on every rollback.
