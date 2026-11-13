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

## RMG-K P2P rollback, in detail (read from its source, 2f5454a = tag v0.9.16)

This is what a Switch build has to reproduce to play a "Peer to Peer" rollback game against RMG-K.
Paths are relative to `ref/RMG-K/Source`; GekkoNet is `ref/RMG-K/GekkoNet-main/GekkoLib`. Byte
layouts were checked by serializing with the same code on the Mac (all integers little-endian:
every structure is copied raw, so a big-endian host would break this). `znx-net` (step 4a below)
runs exactly this code.

### The parts

- n02 P2P client (`n02/core/p2p_core.cpp`, `p2p_message.h`, `p2p_instruction.*`, `common/k_socket.cpp`):
  one UDP socket for lobby, chat and game. No Qt.
- Qt dialog (`RMG/UserInterface/Dialog/Kaillera/KailleraP2PDialog.cpp`, `KailleraNetplayDialog.cpp`):
  host/join, settings negotiation in chat, ready button, NAT traversal, public list. Must be rewritten.
- Session driver (`RMG-Core/rmgk_gekko.cpp`): GekkoNet session, frame loop, time sync, pacer. No Qt;
  builds unchanged outside RMG-K with small stand-ins (`net/rmgk_glue.cpp`).
- GekkoNet (BSD-2): rollback, inputs, acks. In P2P mode its asio adapter is not used: GekkoNet's
  datagrams travel inside the n02 socket (`rmgk_gekko.cpp:1353-1427`, `start_p2p_session:3602-3604`),
  so GekkoNet builds with `GEKKONET_NO_ASIO` and the Switch needs only plain BSD sockets.

### 1. The lobby (n02), before any emulation

- Sockets: host `p2p_core_initialize(true, port, APP, gameName, username)` on the Kaillera port
  setting, default 27886 (`KailleraNetplayDialog.cpp:4315-4319`); joiner `p2p_core_initialize(false, 0, ...)`
  (ephemeral port, `:4483`), then `p2p_core_connect(ip, port)` (`KailleraP2PDialog.cpp:3793`). Bound
  to INADDR_ANY (`k_socket.cpp:64-106`), non-blocking, SO_RCVBUF >= 32 KB, polled with `select`.
- `APP` = `"RMG-K " + CORE_VERSION` (`RMG/UserInterface/MainWindow.cpp:4803`), CORE_VERSION being the
  git tag (`CMakeLists.txt:20-77`): `"RMG-K v0.9.16"` for this release. A connect-code host appends
  `" {CC:<code>}"` (`KailleraP2PDialog.cpp:2873`), which `p2p_appcode_split_inplace` strips before comparing.
- Datagram framing (`p2p_message.h`): byte 0 = number of instructions n (1..14), then n records
  `{serial u8, length u8, body[length]}`, newest first. Every send repeats the last `default_ipm` (3
  in the lobby) instructions from a 32-entry out cache (`:242-287`); the receiver keeps instructions
  whose serial is newer than the last one seen (`:97-152`). Byte 0 = 0 with more than 5 bytes is a
  super-server/traversal text packet (`:153-166`, sent by `send_ssrv`, `:197-223`). Byte 0 = 0xff
  followed by `"RMGKRB1"` is a GekkoNet datagram (section 3). Packets from any address other than the
  peer's are ignored once the peer is known (`:94-105`).
- Instruction body (`p2p_instruction.cpp:176-180`): one byte, type in the low nibble, flags in the high
  nibble (`p2p_instruction_st` bit-fields, `p2p_instruction.h:65-69`), then the payload. Types
  (`p2p_instruction.h:13-23`): LOGN 0, PING 1, PREADY 2, TSYNC 3, TTIME 4, LOAD 5, START 6, DATA 7,
  DROP 8, CHAT 9, EXIT 10. Strings are fixed-size raw copies: "sstring" 32 bytes, "string" 128 bytes
  (copied whole, bytes after the NUL are whatever followed in memory), "vstring" NUL-terminated (max 251).
- Handshake (`p2p_core.cpp:849-950`): joiner -> LOGN/REQ {username 32, app 128} (`:379-382`, sent once
  per `p2p_core_connect`); host -> LOGN/RPOS {username 32, game 128} and, if the base app strings
  differ, a chat "Emulator/version difference alert! Game may desync!" (`:852-883`); joiner -> LOGN/RPOS
  (empty) and PING/PING (128 bytes of junk), CONNECTED (`:921-947`); host on that RPOS -> CONNECTED,
  PING, and PREADY/READY if already ready (`:885-909`). PING/PING is echoed with flags ECHO.
  The host takes the joiner's source address from LOGN/REQ (`:855`): that is the endpoint used later.
  The dialog pings once a second; 10 s without an echo drops the peer (`:1025-1068`).
- Chat (`p2p_send_chat`, `:490-511`): CHAT {crframeno int32, vstring}. Messages starting with `RMGK:` are
  control messages, delivered at once and not shown (`:525-532`). In rollback mode the host sends, on
  peer join, on ready and on every change (`KailleraP2PDialog.cpp:3516-3518, 3829-3834, 2540-2546`):
  - `RMGK:MODE:RB` (or `STD` for the delay-based mode) (`:59-61, 2508-2518`);
  - `RMGK:RBDELAY:<mode>:<delay>` with mode 0 default, 1 lower, 2 higher, 3 custom and the host's
    effective delay 0..9 (`:62-70, 2096-2114`); the default is chosen from ping: <=50 ms 1, <=100 ms 2,
    ... (`:287-300`), with no ping 2 (`:71`);
  - `RMGK:RBPRED:<n>` prediction window, 0 = default 7 (`:63, 72, 2116-2136`);
  - `RMGK:RBPACE:<0|1>` pacing model, now ignored by the session (`rmgk_gekko.cpp:1272-1283`).
  The joiner adopts all of them (`KailleraP2PDialog.cpp:3207-3256`): delay and window are the host's.
- Start (`p2p_core.cpp:970-992`): PREADY/READY or NREADY on the ready button (`p2p_set_ready`). The side
  that sees both ready sends LOAD/LOAD and both call `p2p_game_callback(game, HOST ? 1 : 2, 2)` (a
  200 ms sleep first). Host = controller 1, joiner = controller 2, always.
- In rollback mode the n02 delay-based start (LOAD/LOADED, ping, TSYNC clock sync, START,
  `p2p_modify_play_values`, `:613-834, 1111-1243`) is never run: `onGameStarted` (`KailleraP2PDialog.cpp:3262-3293`)
  reads the peer endpoint (`p2p_core_get_peer_endpoint`) and emits `rollbackSessionReady(game, peerIp,
  localPort, peerPort, player, delay, prediction)`; MainWindow finds the ROM whose GoodName is the
  host's game name (`MainWindow.cpp:5101-5115`) and starts the emulation with the address
  `GEKKO|peerIp|peerPort|delay|prediction` (`RMG/Thread/EmulationThread.cpp:57-62`, parsed at
  `RMG-Core/Emulation.cpp:104-150`).
- NAT traversal (not in step 4a): text over the same socket via the ssrv path, no trailing NUL, to
  `nat.smash64.net:6364` (`KailleraTraversalConfig.hpp`): `N02TRAV1|CLAIM|AUTO` -> `CLAIMOK|code|token`,
  `HOSTOPEN|token` -> `HOSTOK|code|liveToken|...`, `HOSTKEEP|token`, `HOSTCLOSE|token`; joiner
  `JOIN|code|tick` -> `HOST|token|hostIp|hostPort` (and the host gets `PEER|token|peerIp|peerPort`);
  both send `PUNCH|token` x10 to the other's public endpoint, then the joiner `p2p_core_connect`s
  (`KailleraP2PDialog.cpp:2730-2800, 2910-3160`). Public list: `ENLIST`/`ENLISP game|app|user[|port]` to
  `p2plist.smash64.net:27887` with a trailing NUL (`:57-58, 2850-2895`).

### 2. Session start (`CoreStartEmulation`, `RMG-Core/Emulation.cpp:977-1330`)

- RSP plugin forced to HLE; after the overlays the deterministic settings (`:892-916`): RandomizeInterrupt
  off, R4300Emulator 2, CountPerOp 0, CountPerOpDenomPot 0, SiDmaDuration -1, DisableExtraMem off,
  DisableSaveFileLoading on.
- Cheats: `CoreApplyNetplayCheats` (`:1023`) applies the staged netplay cheat list, which only the
  lobby and the old netplay dialog fill: none in P2P. Saves: `CoreApplyNetplaySaveData(true)`
  (`:1104`) with nothing staged in P2P = blank saves kept in memory (`M64CMD_ROLLBACK_SET_SAVE_DATA`).
  Nothing about the ROM, settings or saves is exchanged: only the GoodName (as game name) and the
  app string are compared, and a mismatch is a chat warning.
- `rmgk_gekko::set_deterministic(true)`, then `start_p2p_session("rmgk-gekko", 2, 4, player, localPort,
  peerIp, peerPort, delay, prediction)` (`:1135-1170`) and `rmgk_gekko::execute()` (`:1256, 1325`).
- `start_p2p_session` (`rmgk_gekko.cpp:3553-3734`): GekkoConfig num_players 2, max_spectators 0,
  input_prediction_window clamp(pred, 1, 10), input_size 4, state_size 24 MiB (`:73`, per slot:
  GekkoNet allocates window + 2 slots plus one), limited_saving false, desync_detection true,
  check_distance 10; local delay clamp(delay, 0, 10); runahead 0; GekkoNet's disconnect timeout left
  at 5000 ms (lobby games raise it to 30 s, `:81`). Actors are added in controller order 1, 2, so the
  handles are 0 (host) and 1 (joiner) on both sides. The remote actor's address is the literal
  string `p2p-peer` (`:3664-3668`): GekkoNet never sees an IP. Input players 2 and the rollback
  input callback are installed (`install_core_input_callback`, `:4734-4738`). Controller ports 3 and 4
  then always read as unplugged in the core (`pif.c:276-298, 732-736`), whatever the PC has configured.

### 3. GekkoNet over the n02 socket

- Tunnel (`p2p_message.h:363-391`, `p2p_core.cpp:104-139`): each GekkoNet datagram is sent as
  `ff 52 4d 47 4b 52 42 31` ("\xffRMGKRB1") + the datagram, max 2000 bytes payload, to the peer
  endpoint, and only while n02 is CONNECTED. Received ones are queued only if they come from the
  peer's endpoint; GekkoNet's receive callback drains the socket (`p2p_poll_raw_socket_locked`) and
  hands each one over with the address `p2p-peer` (`rmgk_gekko.cpp:1363-1413`).
- Datagram (zpp serializer, GekkoNet `include/net.h`, `src/backend.cpp:31-41, 425-509`): header
  `{type u8, magic u16}`, then the body as a polymorphic object: u64 type id (first 8 bytes of SHA-1 of
  the C++ name, `zpp/serializer.h:3170-3276`), then the fields in order; `std::vector` = u32 count +
  elements, bool = 1 byte.

  | type | id (u64, LE on the wire) | body after the id |
  |---|---|---|
  | 1 Inputs | 7eef2102210c1892 (`Gekko::InputMsg`) | start_frame i32, input_count u16, total_size u16, compressed bool, inputs (u32 n + n bytes) |
  | 3 InputAck | d365c01983282856 (`Gekko::InputAckMsg`) | ack_frame i32, frame_advantage i8 |
  | 4 SyncRequest, 5 SyncResponse | 809f0fbf9bc9898e (`Gekko::SyncMsg`) | rng_data u16 (the sender's session magic) |
  | 6 SessionHealth | 5445b1fcc6fb86f3 (`Gekko::SessionHealthMsg`) | frame i32, checksum u32 |
  | 7 NetworkHealth | e65470dc0f327d02 (`Gekko::NetworkHealthMsg`) | send_time u64 (ms, sender's steady clock), received bool |

  Example, an InputAck for frame 0x101 with advantage -2 to a peer whose magic is 0xbeef:
  `03 ef be 56 28 28 83 19 c0 65 d3 01 01 00 00 fe` (16 bytes; on the socket, after the 8-byte prefix).
- Connection (`backend.cpp:302-337, 565-649`): each session has a random non-zero 16-bit magic. A
  remote starts Initiating; every 200 ms it sends SyncRequest (header magic 0, rng = own magic) or,
  once synced at least once, SyncResponse. A SyncRequest is answered with SyncResponse (header magic =
  the requester's magic, rng = own magic); the requester counts responses and is Connected after 4
  (NUM_TO_SYNC, `backend.h:189`). Any other packet whose header magic is not the receiver's own is
  dropped (`:531-538`). The game session starts (frame 0 can run) when every remote is Connected.
- Inputs (`backend.cpp:801-916`, `game_session.cpp:571-629`): every poll each side sends its local
  inputs from the peer's last acked frame + 1 to the newest, up to 128 frames per datagram (512 bytes),
  RLE-compressed (pairs count, value) only when that is smaller; an unchanged packet is resent at most
  every 200 ms. Frames are GekkoNet frames from 0; a local input added while the session is on frame F
  is filed under F + delay, and frames 0..delay-1 are zero inputs (`input.cpp:46-54`). Each input is the
  4-byte controller word as a little-endian u32: the word is the controller's reply with its first byte
  highest (A B Z Start Up Down Left Right | - - L R C-up C-down C-left C-right | stick X | stick Y,
  `api/frontend.c:63-70` in the core), so on the wire: stick Y, stick X, second button byte, first.
  For each new remote input frame F the receiver sends InputAck {F, current_frame - F + own delay}
  (`game_session.cpp:571-598`). A missing remote input is predicted by repeating the last one, at most
  `prediction window` frames ahead (`input.cpp`); frame 0 is never predicted.
- Health: NetworkHealth every 500 ms, echoed with received = true, gives the RTT (`backend.cpp:354-376,
  741-799`). SessionHealth sends the saved checksum of frame current - window - 1 (`game_session.cpp:322-359`);
  RMG-K's core always reports checksum 0 (`savestates.c:2658, 3016`), so desyncs are never detected.
- Disconnect: 5 s without any datagram from the peer (`backend.cpp:393-417`); its input then becomes
  zeros (`game_session.cpp:393-406`).

### 4. The frame loop and the core (`rmgk_gekko.cpp:2924-3549`)

The core calls `begin_frame` before and `end_frame` after every visible frame
(`M64CMD_ROLLBACK_EXECUTE`). `begin_frame`:
1. `gekko_network_poll` (`:2995`), then the time sync (section 5, `:3019`).
2. `submit_local_input` (`:1987-2070`): `M64CMD_ROLLBACK_SAMPLE_INPUT` reads controller 1 of the local
   input plugin (one player in P2P), `step_local_delay` (only moves when a live delay change was asked
   for), `gekko_add_local_input(localHandle)`.
3. Loop: `gekko_update_session` (`:3060`). Its events, in order (`game_session.cpp:174-232, 481-521`):
   on a misprediction first `Load(min-1)`, then `Advance(f, rolling_back)` + `Save(f)` for every frame up
   to the present, then `Advance(current)` + `Save(current)`. Save(F)/Load(F) are the state after frame F.
   - Save -> `CoreRollbackSaveGameStateInto` = `M64CMD_ROLLBACK_SAVE_STATE` into GekkoNet's buffer
     (checksum 0); a Save after the real advance is deferred to `end_frame` (`:3185-3205`), because the
     real frame has not run yet.
   - Load -> `M64CMD_ROLLBACK_LOAD_STATE` (synchronous, `:1849-1887`).
   - Advance -> `latch_gekko_input` copies GekkoNet's inputs (by handle) into the per-port buffer that the
     core's input callback returns (`:2072-2168, 4745-4800`); a rolling-back advance runs at once as
     `M64CMD_ROLLBACK_RUN_FRAME` with no output (`:3244-3260`); the real one returns 1 and the core runs
     it with output (`:3364-3370, 3388-3457`).
   - No events (the window is used up, waiting for the peer): sleep 100 us and try again (`:3461-3475`).
4. `end_frame` (`:3478-3549`) runs the deferred saves.
The game reads its controllers through the PIF; the core asks the callback once per frame and gives
every read in that frame the same latched words (`pif.c:327-410`).

### 5. Time sync and pacing

- GekkoNet's `gekko_frames_ahead` = average over the last 26 frames of (local advantage - remote
  advantage) / 2, the advantages being "current frame - newest frame received from the other + own
  delay" (`backend.cpp:918-951`, `game_session.cpp:9-24, 240-255, 601-629`); the remote advantage arrives
  in every InputAck.
- RMG-K's model (asymmetric, the only one now, `rmgk_gekko.cpp:2242-2332`, constants `:113-133`):
  every 30 frames a target speed: behind by more than 0.015 frames -> up to +1 % (full at 3 frames);
  ahead by more than 0.48 -> down to -0.5 % (full at 3 frames); the speed moves 15 % of the way to the
  target each frame and goes to the core (`M64CMD_ROLLBACK_SET_TIMESYNC_SCALE`, clamped 0.99..1.01).
- Pacing: the core's limiter is bypassed in a rollback game; `pace_before_present` (`:4413-4590`), called
  by the video plugin's rendering callback right before the swap, waits for the next deadline of a
  phase-locked clock of (VI rate, 60 Hz by default) x scale; a stall of a period or more re-bases it.

### 6. Ending

- A player who stops the game: the dialog sends n02 DROP (`p2p_drop_game`, `p2p_core.cpp:440-465`),
  which the peer's dialog handles in `p2p_rollback_process_control` (called by its UI timer while a
  rollback game runs, `KailleraP2PDialog.cpp:3925-3932`, `p2p_core.cpp:545-596`) by ending its game too.
  EXIT leaves the lobby. GekkoNet itself has no end message.

### 7. What a port has to match, in short

The lobby strings and the instruction framing (section 1), the tunnel prefix, the zpp datagrams and
GekkoNet's sync/ack/prediction rules (section 3), the 2-player session with handles 0/1, input size 4
and the word layout, the save/load/advance mapping (section 4), and the same emulated machine
(settings of section 2, new_dynarec, blank saves, ports 3-4 unplugged). Delay and prediction window are
per side (GekkoNet balances unequal delays); the time-sync constants only affect smoothness, not sync.

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

### First run on the console (Erista, 2091 MHz), 2026-11-12

`NX_IP=<console> tools/nx-run.sh` with `build/switch/znx-bench.nro`, vanilla US ROM, no picture, 7,200 frames.

- It ran on the first attempt: the recompiler's code cache inside the image with a writable view from
  `svcMapProcessMemory` works under hbloader with a game's memory (the core got its full 512 MB layout).
- All twelve RDRAM hashes (every 600 frames to 7,200) equal the Mac ARM64 build's: the Switch and the Mac
  emulate bit for bit the same. The open cross-architecture question is only ARM64 against x86-64.
- Speed: 0.96 ms per frame over the intro, 0.80 ms per frame in the CPU demo fight (1,157 frames per
  second overall). Worst single frames: 177 ms at frame 1,647 and 25.6 ms at 5,010 (scene loads; to be
  attributed).
- Rollback: save 1.76 ms (8,438,120 bytes), load 15.5 ms, re-running 60 frames 92 ms (1.54 ms per frame,
  which includes recompiling after the load); the re-run is exact (61 of 61 hashes).
- Verdict: go. The emulation itself leaves room (0.8 ms of 16.7). The rollback load does not: 15.5 ms
  spends a whole frame before anything is re-run. The load invalidates the entire 32 MB code cache
  (`invalidate_all_pages`), and the frames after it pay for recompiling. First optimisation target: keep
  compiled blocks across a rollback load when the code they came from did not change, then cut the 8.4 MB
  copies. A five-frame rollback has to fit in about 8 ms to leave room for the frame itself and the picture.

### Rollback load fixed, 2026-11-12

Where the 15.5 ms went, from the core's own phase timers (`verbose=1`): 4 ms flushing the whole 32 MB
code cache from the instruction cache, 9 ms in `invalidate_all_pages()` (every compiled block dropped,
two sweeps over the million-entry page tables), under 2 ms copying RDRAM.

- `patches/0004`: a full invalidation flushes only the pages whose jumps it rewrote (they were already
  tracked in `needs_clear_cache`); also fixes `do_clear_cache()` skipping a run of pages that reached the
  last bit of a bitmap word.
- `patches/0005`: a rollback load whose TLB entries did not change keeps compiled code. Before RDRAM is
  overwritten it notes which pages holding compiled code differ from the incoming state and afterwards
  invalidates only those, the way a guest write would; code from outside RDRAM is always dropped.

Rollback stress (`--rollback-stress K D`, `rollback_stress=K,D` in bench.ini): every K frames rewind
the last D and re-run them, comparing a checksum of RDRAM after every frame with the first pass.
9,000 frames (boot, intro, scene loads, demo fight), a 7-frame rollback every 10 frames:

| | Mac (M5) | Switch (2091 MHz) |
|---|---|---|
| rollbacks, all exact | 900 of 900 | 900 of 900 |
| save | 0.12 ms | 1.69 ms (worst 2.04) |
| load | 0.15 ms | 1.98 ms (worst 2.32) |
| re-run, per frame | 0.11 ms | 0.95 ms |
| load + 7 frames | 0.89 ms (worst 2.74) | 8.61 ms (worst 22.1, on a scene load) |

The periodic hashes are the same as in a run without any rollback, on both machines. Budget on the
Switch for the worst ordinary case (save 1.7 + a 7-frame rollback 8.6 + the frame itself 1.0): about
11 ms of 16.7, before any picture. Save and load are now both an 8.4 MB copy; copying only pages that
changed is the next step if the picture needs the room.

### ARM64 against x86-64 in a real match, 2026-11-12

`bench/inputs/ssb-usa-vs-mash.txt` drives the menus from power-on into a VS match (P1 Donkey Kong, P2
Kirby, default stage; the character select was scripted against `sMNPlayersVSSlots` at 0x8013BA88 from
ssb-decomp-re) and then both players mash seeded pseudo-random sticks and buttons. The match runs from
frame 5,201 to the results screen at 16,520.

Both Mac builds, 18,000 frames, `--check-every 1 --check-skip 0x80040500 0x80040E00` (a fast checksum
of RDRAM per frame, whole and without that 2.3 KB range):

- whole RDRAM differs on 3,101 of 18,000 frames (first at 266);
- without the range it differs on 3 frames (2,337 during the intro, one more before the match, 5,941 in
  the match), each equal again on the next frame.

So everything outside the saved thread contexts is the same on ARM64 and x86-64 for the whole match, and
what does differ never propagates: a divergence that reached game logic would persist, not vanish a frame
later. This is the evidence that inputs-only rollback between a Switch and a PC can hold for vanilla.
Still open: the three stray frames (probably a register save somewhere else), Smash Remix, and RMG-K's
own x86-64 build (GCC/MinGW, not Clang under Rosetta) which only step 5 can test.

### Picture (step 3a): builds, checked on the Mac, not yet run on a console, 2026-11-12

`builder/build-switch.sh game` builds `build/switch/z-cancel-nx.nro`: the bench's core, RSP plugin, forced
netplay settings and frame driver (`bench/bench_core.c`, now shared by both programs) with RMG-K's GLideN64
in place of the null video plugin. Files under `sdmc:/switch/z-cancel-nx/`: `rom.z64`, optional `inputs.txt`
(the format of `bench/inputs/`), optional `z-cancel-nx.ini` (`scale=`, `rdram_check=1`, `vsync=`, `frames=`,
`inputs=`, `cpu_mhz=`, `verbose=`), log in `latest.log`. Plus leaves. No sound, no controllers yet.

- GLideN64 is RMG-K's revision with one patch (`patches/0006`: no `<dlfcn.h>`, the core found through the
  static name table, POSIX semaphores). Built as mupen64plus-libretro-nx builds it for the Switch: desktop
  OpenGL through EGL (`-DEGL -DOS_LINUX`), no threaded renderer; and without Qt (settings come from the
  core's configuration, `mupenplus/Config_mupenplus.cpp`), texture packs and on-screen text.
- The plugin gets its window from `switch/vidext_switch.c` (the core's video extension, `CoreOverrideVidExt`):
  EGL on `nwindowGetDefault()`, 1280x720, the OpenGL 3.3 core context GLideN64 asks for (Mesa gives 4.3),
  24-bit depth, swap interval 1. Context and every GL call live on the emulation thread. The swap paces
  the game; the core's speed limiter is off (frames run without `M64FRAME_OUTPUT_PACING`), and a frame in
  which the plugin does not swap waits out a refresh by the clock.
- What RMG-K's players render with: nothing is forced or checked for a netplay session. RMG-K builds
  GLideN64 with its Qt dialog, settings in `GLideN64.ini`; a fresh install has none and gets
  `Config::resetToDefaults` (`Config.cpp:72`): frame buffer emulation off. With it off GLideN64 never writes
  RDRAM for this game (`FrameBufferList::saveBuffer` returns at `FrameBuffer.cpp:793` before a current
  buffer exists, and every copy hangs off that buffer). `GLideN64.custom.ini` has no entry for Smash.
- Check build on the Mac (`builder/build-mac.sh arm64 game`, `build/mac-arm64/znx-game`): the same driver,
  settings and patched GLideN64 drawing into an offscreen OpenGL 4.1 context, no window. The title, the
  intro and the demo fight render correctly, 4:3 centred in 1280x720, and RDRAM hashes equal znx-bench's
  at every 600th frame to 7,200, without input and through the scripted VS match: with frame buffer
  emulation off the picture does not touch the emulated machine.
- With frame buffer emulation on (`scale=1` or `2`), every copy option off, RDRAM differs from the bench
  from frame 600 on (the plugin clears colour and depth images in RDRAM itself, `FrameBufferList::fillRDRAM`,
  `FrameBuffer.cpp:1708`); scale 1 and 2 give the same hashes as each other. So a PC player who turned
  frame buffer emulation on has different bytes in the framebuffer areas of RDRAM than one who did not.
  Whether that can reach game logic is not known; it is another reason a raw RDRAM hash is not a desync
  check. The Switch default is off, like a fresh RMG-K.
- Untested: everything that is Horizon or Mesa: EGL on the console, nouveau with GLideN64's shaders,
  the 4.3 code paths the Mac's 4.1 does not take (buffer storage), vsync pacing, speed with a picture.


### Picture on the console, and Smash Remix, 2026-11-12

Picture (`bash builder/build-switch.sh game` -> `build/switch/z-cancel-nx.nro`): GLideN64 on the
console's OpenGL (nouveau NV120, OpenGL 4.3 core, Mesa 20.1), 1280x720, vsync. First run on the Erista:

- the picture is right (How to Play demo and title checked from console screenshots);
- every RDRAM hash to 7,200 equals the headless bench's, so the renderer does not touch the machine;
- the emulation thread (core, RSP, GLideN64, GL driver's CPU side) runs 2.2 to 4.4 ms per frame; the
  frame is paced by vsync at 59 to 60 fps. The driver waits for vsync somewhere other than
  `eglSwapBuffers`, so "swap wait" in the log is near zero and the thread's own running time
  (`the emulation thread ran ...`) is the number to read;
- first-time spikes of 30 to 500 ms where new shaders compile (frame 118, scene changes): to fix with
  a shader cache or by compiling ahead.

Smash Remix: `xdelta3 -d -s <vanilla> <patch>` on the release patches gives ROMs whose MD5s are the ones
in the releases' own ROM database entries and in RMG-K's `mupen64plus.ini` (Remix 2.0.1:
2B2D6B295106C54216B7FC7A2F14346E, 65,536,000 bytes; Remix 2.0.1 +EXTRA 0.6.0:
823DFA3414285939A44FD715413421F6, 80,421,504 bytes; both CountPerOp 1). Both patches are against the
vanilla US ROM (MD5 F7C52568A31AADF26E14DC2B6416B2ED). On the console, 9,000 frames with a 7-frame
rollback every 10 frames: 900 of 900 exact for each, load + 7 frames 8.6 and 8.7 ms (vanilla 8.6), and
the hashes equal the Mac's. Their intro/demo frames cost about 1.25 ms without a picture (vanilla 1.0).


### Sound and controllers, 2026-11-12

Both are in `z-cancel-nx.nro` and checked on the console.

- Sound (`switch/audio_switch.c`, an audio plugin linked like the others): the game's 32 kHz blocks go
  into a ring; a feeder thread on core 1 resamples to 48 kHz into 5 ms audout buffers and leans the rate
  up to 1% to hold 40 ms in the ring. After the ring runs dry it stays silent until it is back at the
  target. The feeder's priority must be within what hbloader allows an application (0x1C; 0x1A was
  refused). In the intro's first minute the output ran dry 21 times, at the frames where new shaders
  compile; that goes away with the shader hitches.
- Controllers (`switch/input_switch.c`): polled once a frame on the emulation thread and handed to the
  core's rollback input callback. The callback's word is the controller's reply bytes with the first
  byte highest (buttons, buttons, stick x, stick y), not `BUTTONS.Value`. GameCube and Pro layouts share
  one table (the system reports GameCube L/R as ZL/ZR and Z as R); full tilt reads 84 (`stick_range`).
  Plus is Start, so quitting is Minus, or L + R + Z + Start held half a second.
- Checked with sys-botbase: Start at the title opens Mode Select, A enters 1P Mode; the quit hold returns
  to the Homebrew Menu. The bot's first press after its controller detaches is lost: send a throwaway
  press first, and never send presses after the program has exited (a stray Minus opened hbmenu's theme
  menu).

### Shader cache, 2026-11-12

`EnableShadersStorage` is on: GLideN64 saves the combiners a game used when it closes
(`<user cache>/shaders`) and compiles them at the next start. The Switch's Mesa offers 0 program
binary formats, so it is the key list that is saved and the compile happens at start, not a binary
load. On the console, the intro's first 7,200 frames went from 50 frames over 16.7 ms (worst 504) to
20 (worst 181), and sound ran dry 9 times instead of 28.

The scripted VS match on the console with the picture (`inputs=` the mash script), the second time
(combiners cached): from the stage load to the results screen not one frame over 16.7 ms after the
first second; worst 14.2 ms, typical 3.7 ms of the emulation thread per frame. The first time it had
one 113 ms stop (a combiner seen for the first time). The remaining spikes (frame 1,647 of the intro:
181 ms; the match start: 29 ms) are also there without any picture, so they are the recompiler
compiling newly loaded game code, at scene loads.

### Rollback netplay between two copies on the Mac (step 4a), 2026-11-12

`build/mac-arm64/znx-net` (sources in `net/`, built by `builder/build-mac.sh`) runs an RMG-K P2P
rollback game headless: RMG-K's n02 P2P client, its session driver `rmgk_gekko.cpp` with
`RollbackNetcode.cpp`, and GekkoNet (without asio), all compiled unchanged from the `build/src` copy
of `ref/RMG-K`; `net/` adds only stand-ins for the rest of RMG-Core (`rmgk_glue.cpp`), the n02 UI
callbacks (`n02_glue.cpp`), the dialog's part (host/join, `RMGK:` settings, ready), an input plugin fed
from a script (`input-net.c`) and a bad-network simulator on the n02 socket (`netsim.cpp`, k_socket's
`sendto`/`bind` renamed with -D; loopback only). `bench/bench_core.c` sets the core up as for any
session and hands over to `rmgk_gekko::execute()` (new `znx_host.input`/`session_run`).

    znx-net --rom ROM --host 27886 --inputs bench/inputs/ssb-usa-vs-mash.txt --player 1 \
            --frames 17000 --check-skip 0x80040500 0x80040E00 --sim-latency 60 --sim-jitter 20 --sim-loss 2
    znx-net --rom ROM --join 127.0.0.1:27886 --inputs ... --player 2  (same options)

Each side plays its player's lines of the script (looking `delay` frames ahead, since GekkoNet files
an input added on frame F under F + delay) and prints `check <frames> <whole> <rest>` for every 60th
frame once that frame is final (from the rollback saves, `gekko_confirmed_frame`). `--sim-latency` is
added by each sender (round trip = 2x), jitter only on GekkoNet's datagrams, loss only on GekkoNet's
datagrams (the n02 lobby has no retransmission). The end: when every frame below `--frames` is final a
side sends n02 DROP and stops when the peer's DROP arrives.

Result, vanilla US ROM, delay 2, window 7, 60 ms +- 20 one way, 2 % loss, RMG-K's 60 Hz pacer:
- the 284 check lines (60..17,000) are identical on both sides, and identical to
  `znx-bench --inputs ... --input-players 2 --check-every 20` (RMG-K's P2P synchronizes 2 ports, so
  the reference must too); final `check 17000 2c0196363c2d2922 976c1aa7e40df3c7` on all three;
- 59.8 / 58.8 frames per second over 284 / 294 s (the joiner boots ~5 s later, the host waits);
  measured RTT 134 ms, jitter 17 ms;
- rollbacks 1,518 (host) / 1,580 (joiner), 3,186 / 3,293 frames re-simulated, deepest 4 / 7, mostly
  2-3 frames; 1,218 / 1,294 GekkoNet datagrams dropped of ~61,500; no waiting for input after the start.

Not yet compatible or not tested: real RMG-K (Windows, x86-64 build) has not been on the other end;
connect codes (nat.smash64.net), the public list and the lobby are not implemented; nothing checks
desyncs on the wire (GekkoNet's checksum is RMG-K's 0). For the Switch: the n02 code needs BSD sockets
(`socketInitializeDefault`, `select`, `ioctl(FIONBIO)` to verify) and one thread polling
`p2p_rollback_process_control`; GekkoNet needs no asio; `PreciseWait.hpp` falls to its portable branch;
`kGekkoStateCapacity` makes GekkoNet allocate 10 x 24 MiB (the state is 8.4 MB; the size is local, not on
the wire, so a smaller one is safe); the pacer must replace vsync pacing, because RMG-K's time sync
speeds a lagging side up by up to 1 %.


### Start screen and ROM preparation (step 6): built, previewed on the Mac, not yet run on a console, 2026-11-12

`z-cancel-nx.nro` opens on a start screen (`menu/`, drawn with OpenGL 3.3 on the console's EGL by
`switch/menu_switch.c`): Super Smash Bros., Smash Remix 2.0.1 and Smash Remix 2.0.1 +EXTRA 0.6.0, each with
its state (ready / built from your ROM / needs the patch / needs your ROM), then Play or Online play
(`menu/online.c`, a stub the netplay work replaces), and Settings (`sound`, `stick_range` 60..100 in
`z-cancel-nx.ini`, other lines kept). `menu=0` in the ini, or a ROM path as the NRO's first argument, skips it.

- Vanilla ROM: any .z64/.n64/.v64 in `sdmc:/switch/z-cancel-nx/` or its `roms/`, converted to big-endian
  order in memory and identified by MD5 F7C52568A31AADF26E14DC2B6416B2ED; anything else is listed with its
  name, size and MD5 and why it is not used. MD5s already computed are kept in `rom-cache.txt` (path, size,
  time); only matches are trusted from it, after the header CRC agrees.
- Remix: the patch is looked for in `patches/` (by size; MD5 checked before use); if missing, A downloads
  the release zip from GitHub (libcurl; on the Switch the portlib's libnx backend, i.e. the system's TLS
  service and certificate store), checks its size and MD5, takes only the .xdelta out (the core's minizip),
  checks it, deletes the zip. xdelta3 3.2.2's decoder (Apache-2.0, `ref/xdelta` via
  `builder/fetch-menu-deps.sh`, no secondary compressors: the patches use none) writes
  `roms/smashremix-2.0.1.z64` / `roms/smashremix-2.0.1-extra-0.6.0.z64` while hashing it; a wrong MD5 or
  size deletes it. On the Mac, the decoder gives exactly RMG-K's MD5s for both, and the whole path
  (file:// "download" of the real release zips, unpack, checks, patch) ends on the game's screen.
- Back to the start screen after a game: the core, GLideN64 and the audio plugin are shut down and started
  again in the same process. Two things made that safe: `bench_core.c` now clears its per-run state
  (`l_rdram` above all: a second run skipped `first_frame_setup`, so the core's speed limiter stayed on and
  the old RDRAM pointer was hashed: seen on the Mac as a second run at 60 fps); and the program keeps one EGL
  display and window surface for its whole life (`switch/screen_switch.c`), shared by the menu and the
  video extension, so only OpenGL contexts are created and destroyed per game. Checked: `znx-game --repeat 3`
  (core + GLideN64 + context, three times in one process) and the Switch recipe's self-test with `3600x2`
  (core and the recompiler's code-cache view mapped twice) print the same hashes to 3,600 every run.
- Preview: `build/mac-arm64/znx-game --menu-shot out.png --menu-base DIR --menu-keys "down,a,..."` renders the
  same menu code offscreen to PNG (`game/mac/menu_mac.c`); `--menu-build` and `--menu-notice` show the
  build screen and a message with made-up content.

### Start screen checked on the console, 2026-11-12

`z-cancel-nx.nro` opens on the start screen. On the Erista, driven with sys-botbase and checked from
screenshots and the log:

- the vanilla ROM is found by MD5 (it is named rom.z64 on that card);
- Smash Remix 2.0.1 +EXTRA 0.6.0 and Smash Remix 2.0.1: A downloaded each release zip from GitHub
  (44 and 63 MB), kept only the patch, built the ROM on the console and checked it: MD5s
  823DFA3414285939A44FD715413421F6 and 2B2D6B295106C54216B7FC7A2F14346E, as RMG-K's database has them;
  about two minutes each over Wi-Fi including the download;
- three games in one session (+EXTRA, vanilla, Remix), each back to the start screen with the quit hold,
  and B on the start screen back to the Homebrew Menu: the core, GLideN64's context and audout restart
  cleanly.

## RMG-K lobby, in detail (read from its source, 2f5454a = tag v0.9.16)

What a Switch build has to reproduce to play in RMG-K's "Join Rollback Netplay" lobby. Paths are
relative to `ref/RMG-K/Source`. Short names: **LC** `RMG/UserInterface/Dialog/Lobby/LobbyClient.cpp`
(the WebSocket protocol, Qt), **RLD** `.../Lobby/RollbackLobbyDialog.cpp` (the dialog: start rule,
delay, match start), **LCD** `.../Lobby/LobbyConnectDialog.cpp`, **ICE** `RMG-Core/LobbyIce.cpp` (the
libjuice mesh, no Qt), **RG** `RMG-Core/rmgk_gekko.cpp`. The server (`rmgk-lobby`) is not public:
everything about it below is what the client sends and accepts. `net/lobby/` implements this (end of
this section).

### The parts

- A WebSocket to the lobby server carries everything that is not gameplay: login, presence, room
  list, rooms, chat, ICE signalling, match start/end. One JSON text frame per message.
- An ICE mesh (libjuice 1.7.3, `3rdParty/libjuice`) between the members of a room: one agent per
  other member, created as soon as both are in the room, not at match start (LC:1154-1240). Pings,
  the pre-match manifest and GekkoNet's datagrams all travel over it, each datagram prefixed by a
  channel byte. ICE-capable clients do not open the old UDP "anchor" (port 6364) at all (LC:1037-1038).
- GekkoNet over the mesh: `rmgk_gekko::start_lobby_session` with the "ice:" adapter (RG:1555-1634,
  3737-3990), compiled only with `-DNETPLAY` (RG:3808, 3891).

### Server and envelope

- URL: release builds `ws://216.128.157.98:8080/ws`, builds from an untagged commit (`vdev-N`)
  `ws://64.177.116.159:8080/ws` (LCD:31-35, root `CMakeLists.txt:70-72`); the environment variable
  `RMGK_LOBBY_URL` overrides it (LCD:40-46). Plain `ws://`: no TLS anywhere.
- Username 2-20 characters of `[A-Za-z0-9_.-]` (LCD:76-80, 108), remembered in QSettings.
- Envelope (LC:668-682): `{"type":"<TYPE>","id":"<id>","data":{...}}`, compact. `id` only on HELLO
  (`"hello-1"`); `data` left out when empty (ROOM_LEAVE, ROOM_START, QUICK_MATCH_CANCEL). Ids are JSON
  numbers (Qt reads them with `toDouble()`, so they stay below 2^53). Server limit 1 MiB per message
  (LC:3515). Unknown types are ignored (LC:958).

### Connection state machine (LC:48-55, 580-660, 817-1049)

`Disconnected -> Connecting` (socket opening) `-> Authenticating` (WebSocket open: HELLO sent at once)
`-> Connected` (HELLO_OK) or `-> Failed` (HELLO_FAIL, socket error). Connect is allowed from
Disconnected or Failed only, and is refused while a match owns the ICE mesh (HELLO_OK reconfigures it,
LC:583-591). On HELLO_OK: ICE configured, heartbeat every 15 s starts. A dropped WebSocket clears
pings, probes and routes (LC:856-900); a running match keeps its ICE agents (teardown deferred until
the match ends, LC:1282-1321) and plays on without chat (RLD:2628-2640).

### Client -> server

| type | data | when |
|---|---|---|
| HELLO | `username`, `clientVersion` (`CORE_VERSION`, the git tag: `"v0.9.16"`), `romHashes` (array of strings; RMG-K 0.9.16 sends `[]`, RLD:2619), `iceSupported:true`, `lateJoin:true`, `saveData:true`, optional `connection` (`"lan"`/`"wifi"`), `anonymous:true`, `tzOffset` (standard UTC offset in seconds, non-zero only), `localIp` (private IPv4) | WebSocket open (LC:817-854) |
| HEARTBEAT | `away` (bool: no input for 5 min) | every 15 s while Connected (LC:65, 2288-2297) |
| CHAT_SEND | `channel` (`"lobby"`, `"room"`, `"spectate"`), `message` | (LC:3300-3306) |
| CHAT_HISTORY | `channel` | (LC:3308-3313) |
| ROOM_CREATE | `name`, `rom{name,md5,region}`, `maxPlayers` (2-4), `delay`, `prediction`, `pacing` (always 1, RLD:3374-3376), optional `password` | (LC:3359-3379) |
| ROOM_JOIN | `roomId`, optional `password` | (LC:3381-3388) |
| ROOM_LEAVE | none | |
| ROOM_START | none | host, Start button (RLD:4656-4661) |
| ROOM_UPDATE_DELAY | `delay`, `delayAuto` | own delay changed (each player picks their own; LC:3429-3435, RLD:4896-4945) |
| ROOM_UPDATE_PREDICTION | `prediction` (1-9), `predictionAuto` | host (LC:3437-3443) |
| ROOM_UPDATE_LIVE_REPLAY / ROOM_UPDATE_LATE_JOIN | `enabled` | host |
| ROOM_UPDATE_SAVE_DATA | `enabled`, and when on `label`, `kinds[]`, `bytes`, `hash` | host (LC:3414-3427) |
| ROOM_KICK | `userId` | host |
| ROOM_SWAP_SLOTS | `slotA`, `slotB` | host, waiting room (LC:3452-3461) |
| ICE_SIGNAL | `targetUserId`, `roomId`, `kind` (`"description"`, `"candidate"`, `"gathering_done"`, `"restart"`), `generation`, `value` (SDP text; absent for gathering_done/restart) | ICE (LC:1508-1529, 1425-1437) |
| MATCH_CONNECTED | `matchId`, `peerUserId` (0) | emulation running (RLD:4472) |
| MATCH_PUNCH_FAILED | `matchId`, `peerUserId` | legacy path |
| MATCH_FINISHED | `matchId` | emulation ended, or match start aborted (RLD:4503, 5757-5758) |
| MATCH_ADMIT / MATCH_ADMIT_SIGNAL | `matchId`,`userId` / `matchId`,`admitId`,`step`,`frame`?,`ok`?,`reason`? or blob chunks `index`,`count`,`data` (base64, 384 KiB) | late join (LC:2102-2147) |
| BROADCAST_BEGIN / _DATA / _KEYFRAME / _END | `matchId` + `manifest` (base64) / `data`,`frame` / `frame`,`chunkIndex`,`chunkCount`,`data` (64 KiB chunks, "RMGKZ1" zlib) / - | live replay upload (LC:3513-3578, 726-759) |
| SPECTATE_START / SPECTATE_STOP | `matchId` | |
| QUICK_MATCH_JOIN / _CANCEL | `rom{name,md5,region}` / - | |
| ADMIN_AUTH / MOD_ACTION | `password` / `action`,`target`,`duration`?,`reason`? | moderators |

### Server -> client

| type | data (as read) |
|---|---|
| HELLO_OK | `userId`, `observedIp`, `region`, `ice{stun{host,port}, turn[{host,port,username,password}]}`, `udpAnchor` (legacy) (LC:963-1039). Missing STUN falls back to `stun.l.google.com:19302`; TURN port defaults to 3478. |
| HELLO_FAIL | `reason` (`username_taken`, `invalid_hello`, `invalid_payload`, `version_mismatch` = older than 0.9.13 / vdev-2367, `server_full`; RLD:2738-2747), optional `message`. Client closes. |
| HEARTBEAT_ACK | ignored (`serverTime`) |
| PRESENCE_FULL | `users[]` |
| PRESENCE_DELTA | `added[]`, `updated[]`, `removed[]` (ids). A user: `id`, `username`, `state`, `region` (LobbyRegions.hpp codes), `country`, `clientVersion`, `connection`, `anonymous`, `pingToServer`, `currentRoomId`, `currentRoomName`, `searchingRom` (LC:3608-3625) |
| ROOM_LIST | `rooms[]`: `id`, `name`, `hostId`, `hostName`, `rom{name,md5}`, `players`, `maxPlayers`, `state` (`waiting`/`in_game`), `hasPassword`, `startedAt`, `broadcasting`, `matchId`, `viewerCount`, `lateJoin`, `playerNames[]` (LC:1090-1118). The whole list each time. |
| ROOM_CREATED / ROOM_CREATE_FAIL | `roomId` / `reason`. The creator then gets ROOM_STATE. |
| ROOM_JOIN_OK | a full room state (as ROOM_STATE) |
| ROOM_JOIN_FAIL | `reason` |
| ROOM_STATE | `id`, `name`, `hostId`, `rom{name,md5,region}`, `maxPlayers`, `state`, `prediction` (1-9, default 7), `predictionAuto`, `pacing`, `liveReplayEnabled`, `inGameSettings`, `lateJoin`, `lateJoinEnabled`, `canStartAlone`, `saveData{enabled,label,kinds[],bytes,hash}`, `players[]`: `userId`, `username`, `slot` (1-4), `frameDelay`, `iceSupported`, `inMatch`, `lateJoin`, `saveData` (RLD:3596-3700, 3770-3910; LC:1154-1240) |
| ROOM_LEFT | `reason` |
| CHAT_MSG | `channel`, `fromUserId`, `fromUsername`, `message`, `serverTime`, `admin` (LC:1635-1645) |
| CHAT_HISTORY_REPLY | `channel`, `messages[]` (same fields) |
| ICE_SIGNAL | `roomId`, `fromUserId`, `kind`, `generation`, `value` (LC:1323-1413) |
| MATCH_BEGIN | `matchId`, `peers[]`: `userId`, `username`, `publicIp`, `publicPort`, `localIp`, `slot`, `iceSupported`; `seats` (ports a late-join match emulates, 0 if any member cannot late-join), `keyframeIntervalSec` (LC:2006-2031). Arrives after the ROOM_STATE that set `in_game` (RLD:5849-5851). |
| MATCH_PEER_LEFT | `matchId`, `userId`, `slot`, `reason`: that port goes idle (RLD:4629) |
| MATCH_ADMIT_BEGIN / _SIGNAL / _FAIL | late join (LC:2033-2100) |
| PING_PROBE_REPLY / PING_PROBE_INCOMING | legacy UDP probing (`targetUserId`,`targetEndpoint` / `fromUserId`,`fromEndpoint`) |
| SPECTATE_BEGIN / _DATA / _KEYFRAME / _END / _FAIL, BROADCAST_VIEWER_COUNT | live replay viewing |
| QUICK_MATCH_STATUS | `searching`, `queueSize` |
| ADMIN_AUTH_OK / _FAIL, MOD_NOTICE (`severity`, `text`), MOD_LIST | moderation |

### Rooms, delay, readiness

- There is no "ready" message. The host's Start button is enabled when (RLD:4370-4399): host, room
  `waiting`, at least 2 seated (or 1 with `canStartAlone`), every pair of seated players has a
  measured ping (`worstRoomPingMs` complete, LC:3230-3270), and if save data is on every seated
  player has `saveData:true`.
- Pings run over the ICE path: every 3 s to each seated peer (RLD:749, 4663-4680) and at once when a
  peer's ICE connects (RLD:4726-4751). A probe is channel 1 `[op=1][nonce u64 BE]`; the reply is the
  same with op 2, echoed from libjuice's receive callback (ICE:193-205); RTT measured from the send
  (ICE:586-599, 210-222). 4 attempts 300 ms apart (LC:147-150, 1907-1949). Non-host pairs: the lower
  user id reports to the host, channel 1 `[op=3][target u64 BE][rtt u16 BE]` (LC:1475-1495, 1531-1559).
- Delay is per player: Auto picks from the worst measured ping, -1 -> 2, <=50 ms -> 1, <=100 -> 2,
  <=150 -> 3, <=220 -> 4, else 5 (RLD:113-121), published with ROOM_UPDATE_DELAY; playing alone uses 0
  (RLD:5807-5812). Prediction is the room's (host), default 7, range 1-9; a ROOM_STATE during a match
  changes it live (RLD:3790-3800). Pacing is always 1 ("smooth").

### ICE (ICE, LC:1154-1633)

- Agent config (ICE:415-429): `JUICE_CONCURRENCY_MODE_POLL` (one libjuice thread for all agents),
  STUN from HELLO_OK, TURN servers from HELLO_OK, local ports alternating per peer between 49152-57343
  and 57344-65535 so a restart never reuses its port (ICE:57-71). Host candidates: every non-loopback
  interface (libjuice `udp.c:520-552`).
- Signalling: `add_peer` creates the agent, queues its local description as a `description` signal
  and starts gathering (ICE:372-467); each candidate is trickled as a `candidate` signal
  (`a=candidate:... typ host|srflx|relay`), then `gathering_done`. Signals are sent only while the
  WebSocket is open and we are in a room (LC:1508). Incoming: candidates before the description are
  held (ICE:264-276); signals for a peer without an agent yet are queued until it exists (ICE:554-565).
  Both sides create their description first, so both start controlling and libjuice settles it by ICE
  role-conflict resolution (seen as "ICE role conflict (both controlling)" in the test).
- Generations: when an agent fails, the client restarts it with generation+1, telling the peer with an
  ICE_SIGNAL `kind:"restart"`; signals of an older generation are dropped; a description with a newer
  generation counts as a restart (LC:1323-1464, 1594-1632). 19 automatic restarts (20 attempts, about
  3 s each) before the UI shows the failure (LC:160-164). No restarts of agents a match is using.
- Datagrams: `[channel u8][payload <= 4000 bytes]` (ICE:567-609). Channels (LobbyIce.hpp:16-39):
  1 ping, 2 pre-match, 3 GekkoNet (first session of a match), 4 lag stats (once a second during a
  match), 0x10-0x1f GekkoNet after the n-th late join. Unknown channels are dropped (ICE:182-191).

### Match start (RLD:5765-6001, LC:2530-2749)

1. Host sends ROOM_START; server sets the room `in_game` (ROOM_STATE) and sends MATCH_BEGIN to every
   seated player.
2. Each client waits until every peer's ICE is connected, re-checking every 100 ms, at most 15 s
   ("direct ICE negotiation timed out. No TURN relay is configured."); a peer without `iceSupported`
   fails at once (RLD:5767-5793).
3. Pre-match sync over channel 2, 10 s limit (LC:2575). The sync authority is the room's host, not
   seat 1 (LC:279-286). Host: builds the manifest, sends it to every peer that has not acknowledged,
   all fragments again every 100 ms, until every ACK with the right hash arrived; then applies it
   itself. Others: reassemble, check sender and FNV-1a, apply, send the ACK 8 times (LC:2733-2739).
4. `Rollback_PacingMode` = room pacing (RLD:5994), the match takes ownership of the ICE agents
   (RLD:5999), and emulation starts with the address
   `LOBBY|<delay>|<prediction>|<slot>,ice,<userId>|...[|seats=N]` (RLD:5920-5943, parsed in
   `RMG-Core/Emulation.cpp:151-260`): deterministic settings as for P2P (`Emulation.cpp:1069-1078`), saves
   in memory from the manifest's save section, CPU = new_dynarec, `start_lobby_session` with
   `players` = the highest occupied seat (or `seats`) (`Emulation.cpp:1168-1215`).
5. MATCH_CONNECTED once emulation runs (RLD:4472); MATCH_FINISHED when it stops (RLD:4503) or when
   any step above fails (RLD:5734-5763), which puts the room back to `waiting`.

Pre-match packet formats (all on channel 2; LC:121-129, 418-471):

- manifest packet: `"RMGK"` `06` `senderUserId u64 BE` `manifestHash u64 LE` `manifest`
- ACK: `"RMGK"` `07` `senderUserId u64 BE` `manifestHash u64 LE` (21 bytes)
- a manifest packet over 1000 bytes goes as fragments: `"RMGK"` `08` `sender u64 BE` `hash u64 LE`
  `index u16 LE` `count u16 LE` `totalBytes u32 LE` then bytes `[index*1000, +1000)` of the whole
  manifest packet (count <= 2048, total <= 2 MiB)
- manifestHash: FNV-1a 64 over the manifest (LC:247-256)
- manifest (LC:334-416): `"RMGKPMAN"` `version u32 LE = 1` `sections u32 LE` (bit 0 cheats, always;
  bit 1 save data) `cheatLen u32` cheats [`saveLen u32` save] and nothing after. Cheats
  (`RMG-Core/Cheats.cpp:1174-1205`): `"RMGKCHT1"` `count u32`, each cheat: strings (`u32 len` + bytes)
  name, author, note, `hasOptions u8`, `u32` options (name, `value u32`, `size i32`), `u32` codes
  (`address u32`, `value i32`, `useOptions u8`, `optionIndex i32`, `optionSize i32`). Save section:
  Qt `qCompress` (big-endian `u32` raw length + zlib stream) of `"RMGKSAV1"` `u32 3` then for EEPROM,
  SRAM, FlashRAM `u32 len` + bytes (at most 0x800, 0x8000, 0x20000; `RMG-Core/NetplaySave.cpp:28-31, 402-450`).
  The empty manifest (no cheats, blank save) is 32 bytes.

### The GekkoNet session of a lobby match (RG:2441-2537, 3737-3990)

Actors = remote players + 1, added in seat order on every peer (empty seats get none and zero
input); remote addresses `"ice:<userId>"`; input 4 bytes; prediction clamped 1-10, local delay
0-10; `desync_detection` on, `check_distance` 10, runahead 0; disconnect timeout 30 s
(`kGekkoLobbyPeerTimeoutMs`, RG:81), because the lobby decides drops (MATCH_PEER_LEFT ->
`request_disconnect_player`). The adapter sends each GekkoNet packet as one channel-3 datagram and
receives channel 3 (and drops the previous epoch's channel) (RG:1580-1623). No GekkoNet spectators:
watching is the server's krec stream (BROADCAST/SPECTATE).

### Timers

| what | value | where |
|---|---|---|
| heartbeat | 15 s | LC:65 |
| ICE event poll (Qt) | 25 ms | LC:549-552 |
| seat ping refresh | 3 s | RLD:749 |
| ping retries | 4 attempts, 300 ms | LC:147-150 |
| ICE attempts | 20 (19 restarts), each a fresh agent | LC:160-164 |
| wait for ICE after MATCH_BEGIN | 15 s, checked every 100 ms | RLD:5776-5793 |
| pre-match sync | 10 s; host resend every 100 ms; ACK sent 8 times | LC:2575, 2625, 2735 |
| GekkoNet peer silence | 30 s | RG:81 |
| away | 5 min without input | LC:119 |

### What Qt does there, and what replaces it

QWebSocket -> a small RFC 6455 client on a non-blocking BSD socket (`net/lobby/ws.cpp`); QJsonDocument
-> `net/lobby/json.cpp`; QTimer / the nested `processEvents` loop of the pre-match sync -> deadlines
checked in `znx_lobby_poll`; QNetworkInterface (localIp, lan/wifi), QTimeZone (tzOffset), QSettings
(username) -> fields the caller fills; QRandomGenerator -> `random_u64`; `qCompress` -> zlib, needed only
by whoever applies the save section. LobbyIce.cpp has no Qt; it is followed in `net/lobby/ice_mesh.cpp`.

### net/lobby (built 2026-11-12)

- `znx_lobby.h`: the C API. `znx_lobby_create(config)`, `znx_lobby_connect(lobby, url, username,
  rom_md5, client_version)`, `znx_lobby_poll(lobby, &event)` (connected, hello failed, disconnected,
  users, room list, room created/joined/state/left and failures, chat, peer ICE state, peer ping, match
  begin, match ready, match failed, peer left, notice), readers for users, rooms, the current room
  (seats with delay, ICE state and ping) and the match (slots, total seats, delay, prediction, peers,
  manifest), `create_room / join_room / leave_room / send_chat / set_delay / set_prediction /
  can_start / start_match / swap_seats / kick / set_prematch_manifest / match_started /
  match_finished`, datagrams `znx_lobby_send / znx_lobby_receive` per channel, and
  `znx_lobby_gekko_adapter(lobby, 3)` (a GekkoNetAdapter, byte-compatible with RMG-K's "ice:" adapter).
  Config: STUN (server's / given / off), TURN from the server on/off, the ICE bind address.
- `lobby.cpp` protocol and state machine; `ws.cpp` WebSocket; `json.cpp`; `ice_mesh.cpp` the mesh on
  libjuice; `net_posix.cpp` the only platform file (sockets, clock); `net_io.h` puts TLS behind a
  `Stream` interface (no backend: every RMG-K URL is `ws://`); `lobby_gekko.cpp` the adapter;
  `lobby_ice_rmgk.cpp` defines the three `LobbyIce` functions RG compiled with `-DNETPLAY` calls (send,
  take_packets, peer_connected; checked with `nm`), forwarding to a bound lobby, so RMG-K's own
  `start_lobby_session` can run over this library unchanged.
- Left out: late join (HELLO says `lateJoin:false`, so the server sends `seats:0`), spectating and
  live replay, quick match, moderation, lag-stats reports (channel 4), the legacy UDP anchor.
  `saveData` in HELLO is off unless the caller sets `config.save_data` (it must then apply the
  manifest's save section).
- libjuice 1.7.3 from RMG-K's tree, copied to `build/src/libjuice` and compiled unchanged with RMG-K's
  options (`NO_SERVER`, `USE_NETTLE=0`); MPL-2.0 (THIRD_PARTY_NOTICES.md).

Test: `bash builder/build-mac.sh arm64` builds `build/mac-arm64/znx-lobby-test`;
`build/mac-arm64/znx-lobby-test` (from the repo root) starts `net/lobby/test/mock_lobby.py` (a mock of the
server side above, Python standard library, 127.0.0.1 only) and runs two clients in one process with no
STUN, no TURN and ICE bound to 127.0.0.1. Result: every step passes: upgrade and HELLO_OK, presence,
create, room list, join, room chat, ICE through the relayed signals (`127.0.0.1:51241 -> 127.0.0.1:51242`,
host candidates), pings both ways, published delays in ROOM_STATE, the start rule (host yes, guest no),
MATCH_BEGIN, pre-match sync of a 6 kB manifest (7 fragments) received byte for byte, 200 datagrams each
way, then a GekkoNet session of 1200 frames between the two over the lobby adapter (a small
deterministic stand-in game, local delay 0 so remote input is always predicted: 40 to 300 rollbacks per run)
with identical state checksums on every confirmed frame and no desync event, MATCH_FINISHED back to
`waiting`, ROOM_LEAVE. Four runs in a row passed.

Not verified against the real server (nothing was sent to it): the server-side field names and
ordering are inferred from what the client reads; whether the server checks `romHashes`, how it
orders ROOM_CREATED / ROOM_STATE and MATCH_BEGIN, its rate limits ("flood budget", RLD:4689-4693), and
what it does with a client that says `lateJoin:false`. Interop with a real RMG-K client (ICE between
libjuice builds, the manifest, the GekkoNet session with the real emulator) is untested.

Switch integration needs: libjuice on libnx sockets (BSD API, `poll`, pthreads: present in
devkitPro's newlib; libnx has no `getifaddrs`, so build with `NO_IFADDRS` and check its `SIOCGIFCONF`
path, or bind the agents to the console's address from `gethostid()` via `config.bind_address`);
`socketInitialize` with enough UDP buffers for one agent per peer; the libjuice thread (POLL mode);
calling `znx_lobby_poll` every frame from the UI thread and the adapter from the emulation thread
(both are locked); RG compiled a second way with `-DNETPLAY` plus `lobby_ice_rmgk.cpp`
(`znx_lobby_bind_rmgk`) to use RMG-K's own `start_lobby_session`, with `LOBBY|` semantics
(total seats, slot order) from `znx_lobby_match`; applying the manifest's cheats and save section to
the core before frame 0 (RMG-Core's `CoreDeserializeNetplayCheats` / `CoreSetNetplaySaveData` have no
Qt); no TLS (all `ws://`); DNS is not needed (IP literals).


## Netplay on the Switch (step 4b), 2026-11-12: built and tested off the console, not yet run on one

`z-cancel-nx.nro` plays RMG-K's "Peer to Peer" rollback mode from the start screen: a game's "Online play"
row opens Host a game, Join with a code, Join by IP address, Public games and Lobby (the last calls
`znx_lobby_open`, `menu/lobby_entry.c`, a stub for the lobby work in `net/lobby/`).

### The parts

- `net/p2p/znx_p2p.cpp` (C API in `znx_p2p.h`): what RMG-K keeps in `KailleraP2PDialog`, without Qt, plus the
  session. One lobby thread polls n02 every millisecond (`p2p_step`; during a match `p2p_rollback_process_control`
  every 2 ms) and does the dialog's one-second housekeeping (`onTravTimer`). Host: `p2p_core_initialize` on
  the `p2p_port` setting (27886), connect code from the traversal server (CLAIM|AUTO -> CLAIMOK -> CLAIMACK +
  HOSTOPEN -> HOSTOK, HOSTKEEP every 10 s, PEER -> ten PUNCH a second for 15 s, HOSTCLOSE when a peer joins,
  HOSTOPEN again when it leaves; NOAUTH clears the saved code, NOSESSION reopens), code and owner token kept in
  `z-cancel-nx.ini` (`p2p_code`, `p2p_token`, as RMG-K's `Kaillera_P2PStaticCode`/`OwnerToken`); after four
  unanswered tries it shows the address from WHATISMYIP and keeps asking every 30 s. Public list (setting, off by
  default): ENLISP `game|app {CC:code}|user|port` (ENLIST without a code on 27886), every 30 s while waiting,
  UNENLIST when a peer joins and on leaving; PINGRQ answered. Join: a code (normalised as
  `normalizeTraversalCode`) sends JOIN every 3 s; HOST -> PUNCH + `p2p_core_connect` every second, the host's
  address on 27886 after three tries, 15 s in all; IP[:port] logs in every second for 7 s. The public list is
  `GET /plist.txt` on a plain socket (`http_list.cpp`), parsed as both RMG-K parsers do (lines or one flat pipe
  stream, four fields per game, only games with ` {CC:` kept); the screen shows only games whose name is the
  chosen ROM's GoodName.
- Lobby (`KailleraP2PDialog.cpp:2090-2136, 3195-3256, 3482-3520, 3816-3841`): app string `RMG-K v0.9.16`; the host
  sends `RMGK:MODE:RB`, `RMGK:RBDELAY:<0 auto|3 custom>:<d>`, `RMGK:RBPRED:0`, `RMGK:RBPACE:1` on join and on
  ready, and `RBDELAY` again when the automatic delay changes (75th percentile of the last 20 s of pings,
  first after five pings or 5 s, then every 10 s; `<=50` ms 1 ... `>220` 5); the joiner adopts the host's
  delay and window, and may press ready only once it knows the host's game, the game is its own and the mode is
  RB (otherwise the screen says which). The peer's ready state is read from n02's "<name> is ready" lines, as
  the dialog does. Game names: the GoodName of each MD5 in RMG-K's `mupen64plus.ini`: `Super Smash Bros. (U) [!]`,
  `SmashRemix2.0.1`, `SmashRemix2.0.1 +EXTRA (0.6.0)` (`menu/online.c`).
- Session (`znx_p2p_run_session`, the `session_run` of `bench_core.c`): `set_deterministic`, `start_p2p_session(
  "rmgk-gekko", 2, 4, player, port, peer, delay, window)` (host 1, joiner 2, ports 3-4 unplugged by the core),
  `execute`. A control thread ends it on the peer's DROP or EXIT, when GekkoNet reports the peer disconnected
  (5 s of silence), or after 20 s without a frame; the quit hold (or the system closing the program) sends n02
  DROP (`znx_p2p_leave_game`). After a DROP both stay in the lobby (the screen before the match comes back);
  after EXIT or a timeout the start screen shows why.
- Input: `net/input-net.c` is linked as a fifth static plugin (`input_net`); its source polls the console and
  returns controller 1's word, whichever player this console is. Note: the generated name tables used to drop
  every `SDL_*` export (meant for the core's SDL stand-ins); an input plugin's `SDL_KeyDown/KeyUp` are its API,
  and without them the core refuses the plugin ("broken Input plugin"), so the filter is now the core's only.
- Memory: `patches/0007` lets `RMGK_GEKKO_STATE_CAPACITY` set GekkoNet's slot size; the Switch build passes
  10 MiB (the state is 8,438,120 bytes): about 100 MB for a 7-frame window instead of 240 MB. Local only.
- Sockets: `socketInitializeDefault` is already done by `znx_front_begin`. libnx's `ioctl` passes FIONBIO to the
  bsd service unchanged; whether Horizon honours it is undocumented, while `fcntl(F_SETFL, O_NONBLOCK)` maps to
  the service's Fcntl (what other homebrew uses). `k_socket.cpp` is compiled with `-Dioctl=znx_ioctl`
  (`net/p2p/socket_switch.c`), which does FIONBIO with fcntl and logs `net: n02 socket N non-blocking by fcntl:
  yes|NO` once. `select`, `gethostbyname`, `getaddrinfo` are libnx's. RMG-Core's `Library.cpp` (dlopen) is
  replaced by `net/p2p/library_static.cpp` (the static name tables).

### Pacing

RMG-K's time sync runs a lagging side up to 1 % fast and an ahead side up to 0.5 % slow. The display's vsync
cannot do that, so an online match uses RMG-K's own pacer: the core's frame output keeps PACING on, so
GLideN64's rendering callback calls `rmgk_gekko::pace_before_present` right before the swap (a phase-locked
deadline at 60 Hz x speed, re-based after a stall), and the core's limiter, at the same speed, paces a frame
without a swap; the swap interval is 0 (`vidext_switch.c` ignores the plugin's request during a match). Why that
should not tear: the Switch shows a window through the system compositor (vi/nvnflinger buffer queue); a queued
buffer is taken whole at the display's refresh, so swap interval 0 can only replace a buffer that was not shown
yet or let the display repeat the last one, which is what a game at 60.6 Hz or 59.7 Hz must look like on a 60 Hz
panel anyway (at full correction one repeated or dropped frame every 1.7 s); it never shows two halves. This is
the expectation, not a measurement: if the console tears, `net_vsync=1` in the ini keeps swap interval 1 and
follows the same 60 Hz x speed timeline by skipping a swap when this side is a frame behind it (two frames in
one refresh) or waiting a refresh before a swap when it is a frame ahead (re-anchored after a stall of three
frames, so a wait for the peer is not caught up in a burst). Local play keeps swap interval 1 and the display.

### Overlay

`net_overlay=1` (Settings, "Match overlay", default off): ping, delay and rollbacks in the black margin left of the
4:3 picture, drawn by `switch/overlay_switch.c` with the start screen's renderer in GLideN64's context inside the
buffer swap, after the frame is complete. It writes only the window's framebuffer, never RDRAM; every piece of
OpenGL state it touches is read before and restored after, so GLideN64's state cache stays true.

### Tests (Mac M5 and the container; no public host contacted)

- Two Mac processes, the same lobby/session code, vanilla US ROM, `bench/inputs/ssb-usa-vs-mash.txt`, 60 ms +- 20
  each way, 2 % of GekkoNet's datagrams lost, host on automatic delay (`--delay 0 --ready-after 7`): the host's
  automatic delay went from 2 to 3 (ping 139 ms) and the joiner adopted it; 284 of 284 check lines identical,
  `check 17000 2c0196363c2d2922 976c1aa7e40df3c7` on both (the bench's reference); 59.82 fps over 284 s;
  1,176 / 1,234 rollbacks, deepest 3.
- Connect codes and the public list against `tools/mock-smash64.py` (a local stand-in written from the client's
  side of the protocol): CLAIM, CLAIMOK, CLAIMACK, HOSTOPEN, HOSTOK, ENLISP with ` {CC:ZNX@7}`, plist.txt with
  the listing, JOIN `znx7` (typed lowercase, normalised to ZNX@7), PEER + HOST, HOSTCLOSE + UNENLIST on join;
  code and token saved and reused. 10 of 10 check lines identical.
- `builder/build-switch.sh --selftest-net ROM ...`: the netplay code compiled by the container's GCC for Linux
  ARM64 (with the core's `__SWITCH__` paths and the static tables) as `znx-net-linux`, joining znx-net on the
  Mac through the container's NAT (`host.docker.internal`), 30 ms +- 10 each way, 1 % loss: 284 of 284 check
  lines identical, `check 17000 2c0196363c2d2922 976c1aa7e40df3c7` on both, 59.9 / 58.9 fps, 694 / 691 rollbacks.
  The first try found the name-table `SDL_*` bug above (the input plugin refused); the bench still gives
  `hash 3600 1499ebb25c8606c4`.
- Start screen previews from the real code against the mock server and znx-net (`build/menu-preview/online/`).

Untested: everything on the console (sockets, fcntl, swkbd, pacing and tearing, the overlay over GLideN64,
memory), real RMG-K on the other end, the real nat/list servers (the mock follows the client, not the server),
a rematch after a DROP (the code path exists; GekkoNet's session is closed and started again).

### Switch against Mac over a real LAN, 2026-11-12

The console (Wi-Fi) hosted from the online screen; `znx-net --join <console>:27886 --bind-any` on the Mac
(Ethernet) joined, played P2 with the mash script while sys-botbase pressed buttons for P1 on the console.
With `rdram_check=1` the console logs the same final-frame `check` lines as `znx-net`:

- 9,000 frames, ping 33 ms, delay 1 (auto); console 88 rollbacks (177 frames re-run, deepest 7), Mac 92
  (deepest 7); all 15 check lines identical on both sides;
- the pre-match screen, ready, start, the peer leaving ("Mac left. Waiting for another player."), and B
  back out to the Homebrew Menu all worked; with the code server unreachable the host card shows the
  address after a few seconds;
- Mac hosting and the console joining by IP did not connect: macOS's application firewall blocks
  incoming UDP to the unsigned `znx-net`. The console's "Could not join / Nobody answered" screen is
  what showed. Not a console problem; the firewall setting was left alone.


## Lobby on the Switch, 2026-11-12: built and tested on the Mac, not yet run on a console

The online menu's "Lobby" row now opens RMG-K's "Join Rollback Netplay" lobby (`net/lobby/`, above) with its own
screens, and a lobby match runs through RMG-K's own session driver.

### The parts

- `net/lobby_play.cpp` (C API `net/lobby_play.h`): what RollbackLobbyDialog does around LobbyClient, without Qt or
  screens, shared by the console's screens and `znx-net --lobby` (the Mac test client): connect (HELLO with
  `clientVersion` `v0.9.16`, `romHashes` `[]` as RMG-K 0.9.16 sends it, `saveData:true`), the room list of the chosen
  ROM only (room ROM MD5 = ours, case-insensitive; ping column = RMG-K's region estimate, `LobbyRegions.hpp`), create
  (CreateRoomDialog's defaults: `"<name>'s Room"`, 2 seats, delay 2, prediction 7, ROM name = the GoodName, region "")
  / join / leave, room chat, own delay (Auto or fixed, ROOM_UPDATE_DELAY), the host's Start (`znx_lobby_can_start`),
  MATCH_BEGIN -> ICE wait -> pre-match sync -> READY, problems for the player (server unreachable, version mismatch,
  name refused, kicked, room closed, ICE failed after 20 attempts, match not started). The lobby is polled every
  5 ms by its own thread while open (heartbeats and ICE signalling go on while the menu shows a message or the
  system keyboard, and during the match). Username: the `name` setting made into RMG-K's 2-20 `[A-Za-z0-9_.-]`.
- The match: `znx_p2p_run_lobby_session` (`net/p2p/znx_p2p.cpp`) shares everything with the P2P session (hooks,
  statistics, overlay numbers, the quit hold through `znx_p2p_leave_game`, `rdram_check` checkpoints, pacing) and
  starts GekkoNet the way CoreStartEmulation's `LOBBY|` branch does: `set_deterministic(true)`,
  `start_lobby_session("rmgk-gekko", players = max(local seat, seats, remote seats), 4, seat, 0, remotes
  {slot, userId}, delay, prediction)`; the address RMG-K would build is logged (`LOBBY|2|7|1,ice,1`). `rmgk_gekko.cpp`
  is now compiled with `-DNETPLAY` everywhere (Mac and console; the P2P path is unchanged by it, RMG-K always builds
  it so), and `net/lobby/lobby_ice_rmgk.cpp` supplies its three `LobbyIce` functions. Ends: the other players gone
  (MATCH_PEER_LEFT -> `request_disconnect_player`, then the game closes when no one is left: RMG-K's
  onMatchWatchTick, also for the host since there is no late join), lobby lost and no frame for 10 s, no frame for
  20 s, GekkoNet's 30 s silence, the local quit (MATCH_FINISHED). MATCH_CONNECTED once the first frame ran.
  The console's controller is port 1 of the console whatever seat it plays (`submit_local_input` samples
  controller 1).
- The pre-match manifest is applied before frame 0 with RMG-Core's own code, compiled unchanged: `Cheats.cpp`
  (CoreDeserializeNetplayCheats, CoreSetNetplayCheats, CoreApplyNetplayCheats) and `NetplaySave.cpp`
  (qUncompress with zlib, CoreDeserializeNetplaySaveData, CoreApplyNetplaySaveData(true)), through a new
  `znx_host.session_prepare` hook in `bench_core.c` that replaces the blank saves. NetplaySave's three in-match
  tracker hooks are renamed at compile time so `rmgk_glue.cpp` keeps them; the RMG-Core functions those files reach
  but a match never calls (cheat files, ROM cache, settings) are stubs in `rmgk_glue.cpp`. Netplay cheats are
  cleared after the match.
- Settings (read only, `menu/settings.h`): `lobby_server=ws://host:port/ws` (default `ws://216.128.157.98:8080/ws`),
  `lobby_stun=off|host:port` (default: the server's, as RMG-K), `lobby_bind=` (default: the console's own address).
- Switch specifics: libjuice compiled unchanged with `NO_IFADDRS`, an empty `<ifaddrs.h>`, `__LITTLE_ENDIAN__`
  (picohash wants `<endian.h>`), and `conn_poll.c` with `-Dpipe=znx_juice_pipe` (`switch/juice_switch.c`: libnx has
  no pipe, so the poll thread's wake-up is a connected pair of loopback UDP sockets, or on the console's address if
  loopback cannot be bound). The agents are bound to the console's address (the UDP-connect trick of the host
  screen), which makes it the host candidate. `socketInitialize` with 8 BSD sessions (libjuice's poll thread
  blocks inside poll) and doubled UDP buffers. libjuice's own log goes to latest.log (`juice info: ...`).
  The Linux self-test stays peer to peer (its GCC 12 lacks `<format>` for Cheats.cpp; `switch/selftest/lobby_none.cpp`).
- `ws.cpp` fix: frames that arrive together with the server's close (HELLO_FAIL then hang-up) are now delivered
  before the close (before, a version mismatch showed as "server did not answer").
- Mock server (`net/lobby/test/mock_lobby.py`): `--host 0.0.0.0`, `--stun host:port|none`, `--min-version`,
  `--kick-after S`, and MATCH_FINISHED per player (the others get MATCH_PEER_LEFT "finished", the room is back to
  waiting when all finished; the real server's behaviour here is unknown, the client handles both).

### Screens (`menu/lobby_entry.c`, previews in `build/menu-preview/lobby/`)

Connecting (server URL, name, "Speaks as RMG-K v0.9.16"), Rooms (first row opens a room: name through the system
keyboard, 2-4 seats with left/right; rooms of this ROM with host, players, state and ping estimate; a note counts the
hidden rooms of other games), Room (seats with ICE state and attempt, ping, published delay, host/you; the room chat's
last lines; Y writes through the keyboard; left/right change our delay, saved as `input_delay`; A starts for the host
when RMG-K's rule allows), Starting (direct connections with a countdown, pre-match sync, start). Errors are message
screens. `real-*.png` come from the real code against the mock server and a `znx-net --lobby` peer; `debug-*.png` from
`znx-game --menu-online 0,room:<which>`.

### Tests (Mac only; nothing left 127.0.0.1)

- `tools/lobby-match-test.sh ROM 17000 --sim-latency 60 --sim-jitter 20 --sim-loss 2`: mock server + two znx-net
  through `lobby_play.cpp`, ICE on 127.0.0.1 without STUN/TURN, a real emulator match through `start_lobby_session`
  (delay 2, window 7; latency, jitter and loss added where the session hands datagrams to the mesh,
  `znx_lobby_rmgk_netsim`): 284 of 284 check lines identical on both sides and equal to `znx-bench --input-players 2`,
  final `check 17000 2c0196363c2d2922 976c1aa7e40df3c7` (the P2P reference); 59.82 fps over 284 s; rollbacks
  1,530 / 1,550, deepest 4 / 5; 1,218 / 1,292 of ~61,500 GekkoNet datagrams dropped.
- The same with `--manifest-cheat 90 --manifest-save` (a 663-byte manifest: one cheat writing 0x5A at 0x80700000 and a
  32 KB SRAM save): both sides log "1 cheat, save ... SRAM 32768", 30 of 30 check lines identical, and different from
  the blank reference from frame 120 on, so cheats and saves reach the core before the game reads them.
- `znx-lobby-test` (library alone) still passes; `znx-bench` still gives `hash 3600 1499ebb25c8606c4`;
  `builder/build-switch.sh all` links `z-cancel-nx.nro` with no unresolved symbols.

### Console against the Mac (to run)

The Mac's firewall blocks incoming connections to unsigned programs, so the mock server runs under Apple's own
`/usr/bin/python3` (signed: allowed in), and the Mac's ICE agent sends its connectivity checks out first or at the same
time as the console's; ICE retransmits checks, so once the Mac's outbound checks have passed its firewall the
console's get answered (the same reason the P2P LAN test worked with the Mac joining).

    /usr/bin/python3 -I net/lobby/test/mock_lobby.py --host 0.0.0.0 --port 18080
    # console z-cancel-nx.ini:  lobby_server=ws://<Mac LAN IP>:18080/ws   lobby_stun=off   rdram_check=1
    # console: Online play -> Lobby -> Open a room
    build/mac-arm64/znx-net --rom ROM --lobby ws://127.0.0.1:18080/ws --join-room --bind-any --ice-bind <Mac LAN IP> \
        --inputs bench/inputs/ssb-usa-vs-mash.txt --frames 9000 --check-every 600 --check-skip 0x80040500 0x80040E00

Untested against the real server: everything the server decides (field names as inferred from the client, the order
of ROOM_STATE / MATCH_BEGIN, what it does on MATCH_FINISHED from one player, rate limits, whether it accepts
`lateJoin:false`), STUN/TURN through the server's configuration, a real RMG-K client on the other end (libjuice
interop, the manifest from RMG-K's serializer, a 3-4 player room), and everything on the console.

### Lobby checked on the console, 2026-11-12

Console (Wi-Fi) against the Mac over the LAN, with `net/lobby/test/mock_lobby.py` on the Mac standing in
for the lobby server (`lobby_server=ws://<mac>:18080/ws`, `lobby_stun=off`, `rdram_check=1`): the console
connected, opened a room (name from the on-screen keyboard), the Mac's `znx-net --lobby ... --join-room`
joined, libjuice connected the two directly (18 ms), the console started the match. 9,000 frames, the
console 55 rollbacks (deepest 7), the Mac 59 (deepest 5): all 15 check lines identical. The WebSocket
client, libjuice (its wake-up pipe is a loopback UDP pair on Horizon) and the pre-match manifest all work
on libnx. The real RMG-K lobby server and a real RMG-K peer are untested.

## Switch-style UI, 2026-11-12: built and previewed on the Mac, not yet run on a console

The start screen, the online and lobby screens and the match overlay now follow the console's own system
software (System Settings), so the program reads as part of the Switch. Behaviour, navigation and the meaning of
every text are unchanged; copy was shortened only where a row needed it.

- **Themes.** Basic White and Basic Black (`menu/draw.c` `znx_theme`), from the console's setting
  (`setsysGetColorSetId`, `switch/sysfont_switch.c`); `theme=auto|light|dark` in `z-cancel-nx.ini` overrides it
  (read only, default auto). The colours are the usual approximations of the system's (THIRD_PARTY_NOTICES.md).
- **Type.** The console's shared system font and its Nintendo Extended font with the button glyphs, read at run
  time with `plGetSharedFontByType` and handed to FreeType from memory; nothing of Nintendo's is in the NRO. If
  pl does not give the font, or FreeType refuses it, the bundled Barlow font is used, and the hints are drawn as
  discs with the button's letter. Sizes: 18, 20, 22, 28 and 40 pixels.
- **Layout (1280x720).** Header: the mark on its dark tile and the page's name at 28 px, a thin rule 30 px from
  the edges at y 88. Left: a list of 70 px rows with thin lines, 22 px text, the value at the right in the accent
  colour (On in the accent, Off quiet), status dots in green, red or grey. Right: the detail pane on the panel
  colour (key/value lines, a paragraph, a quieter note; an MD5 on two lines). Footer: a rule at y 647, small
  status text at the left, the hints at the right in the system's order (A outermost, then B, X, Y, D-pad).
  Long lists scroll, with a thin bar beside them; steps (building, joining, starting) are rows with Done or a
  figure at the right and a thin accent progress bar under the list; a message has its text, the details as a
  plain list and a selected OK row.
- **Selection.** The system's fill and a 5 px rounded outline whose colour moves between the two highlight
  colours every 72 frames (1.2 s): the one animation. It is drawn after the rows so their lines do not cross it.
- **Button glyphs.** `znx_draw_button` (draw.c): A U+E0E0, B U+E0E1, X U+E0E2, Y U+E0E3, D-pad left U+E0ED and
  right U+E0EE in the footer; the table also knows L, R, ZL, ZR (E0E4-E0E7), Plus E0EF, Minus E0F0, D-pad E0EA,
  left stick E0C1. They are the code points the open Switch libraries use; none could be checked off the
  console. `debug_glyphs=1` in the ini opens a page with all 96 glyphs from U+E0A0 and their codes, the ones in
  use in the accent colour, so one screenshot settles it.
- **Overlay.** `znx_ui_overlay`: a small dark translucent rounded rectangle with white text at the top left, in
  the system font, over the black margin. It is still drawn only into the window's framebuffer after GLideN64's
  frame, with every piece of OpenGL state restored; it never touches the game.
- **Log lines to look for in `latest.log`:** `menu: theme dark (console setting)` / `menu: theme light
  (theme=light in z-cancel-nx.ini)` / `menu: theme light (console setting unavailable: 0x...)`;
  `menu: system font loaded (N bytes)` or `menu: system font unavailable: 0x..., using the bundled font`;
  `menu: system button glyphs loaded (N bytes)` or `... unavailable: 0x..., drawing stand-ins`;
  `menu: drawing with the system font, button glyphs from the system`; `menu: the system font cannot be used
  (...), using the bundled font`.
- **Preview.** `znx-game --menu-shot` takes `--menu-theme light|dark`, `--menu-font FILE|barlow`, `--menu-glyphs`
  and `--menu-overlay "Ping=41 ms|Delay=2|Rollbacks=3"`. A Mac has neither the console's font nor its glyphs: the
  preview draws with Hiragino Sans GB from the Mac's system folder (close in width; Helvetica, then Barlow, if it
  is missing), shows the stand-in discs, and holds the outline's pulse at its middle. Every screen in both themes
  is in `build/menu-preview/switch-style/{light,dark}/`.

### Where the slow frames come from, 2026-11-12

Stage matrix on the console (`matrix=<file>` in the ini: forced four-CPU VS matches one after another,
each a fresh start, 3,000 frames timed from frame 660, picture on). Vanilla, all 14 stages that start:
the emulation thread runs 4.7 to 5.6 ms per frame on average, p99 7.7 to 10.4 ms, but every stage has 5
to 12 frames over 16.7 ms and 2 to 6 over 33 ms, and a second pass with every shader cached gives the
same numbers. The headless bench shows the same frames without any picture, and patch 0008's
`DebugDynarecStats` attributes them: the forced match on Congo Jungle compiles 1,165 blocks after frame
660 at 0.485 ms per block on the console (0.077 ms on the Mac); frame 661 takes 59.5 ms of which 57.6
compiling 68 blocks, 869 takes 40.6 of which 38.7, 2,087 takes 31.6 of which 30.1. So the only frames
that miss the display in a match are frames in which the recompiler meets code for the first time.
Next: compile ahead of need from a saved list of blocks, and make a compile cheaper.

The console's menus were restyled after the system's own (Basic White and Basic Black from the console's
setting, the shared system font and button glyphs loaded at run time, System Settings' layout); checked
on the console in both themes.
