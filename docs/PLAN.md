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
