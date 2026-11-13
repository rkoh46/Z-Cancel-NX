<p align="center"><img src="assets/logo.png" width="128" alt="Z-Cancel NX"></p>

# Z-Cancel NX

Super Smash Bros. (N64) and Smash Remix with rollback netplay on a Nintendo Switch, built to play
against the PC players who use [RMG-K](https://github.com/Jay-Day/RMG-K). You bring your own ROM;
nothing from a game is in this repository.

Status: early. On a Switch it shows the game, plays sound and takes controllers, with exact rollback underneath and the same emulated state as the Mac build; it opens on a start screen that picks vanilla, Remix or Remix +EXTRA and builds the Remix ROMs itself; there is no netplay on the console yet. The plan, what was learned from RMG-K's source and the
order things get tested in are in [docs/PLAN.md](docs/PLAN.md).

| Step | State |
|---|---|
| 1. Speed test: RMG-K's emulator core on the Switch, no picture | done: 0.8 ms per frame in a fight; a 7-frame rollback costs 8.6 ms |
| 2. Determinism rig: ARM64 against x86-64, same inputs, state hashed every frame | a 3-minute mashed VS match stays identical outside saved register slots |
| 3. Picture, sound, controllers | working on the console (GLideN64 on its OpenGL, 2 to 4 ms of CPU per frame); shader-compile hitches to fix |
| 4. Netplay: rollback, connect codes, lobby | RMG-K's P2P rollback (n02 + GekkoNet) between two Mac copies: a full match identical on both sides under 120 ms RTT and 2 % loss; connect codes and lobby not started |
| 5. Cross-play against RMG-K on a PC | not started |
| 6. ROM picker and on-device Remix patching | built, previewed on the Mac, not yet run on a console: a start screen (vanilla, Remix 2.0.1, Remix +EXTRA 0.6.0), the vanilla ROM found by MD5, Remix built from it with the official xdelta patches (downloaded from GitHub if missing) and checked against RMG-K's MD5s; quitting a game returns to it |

Z-Cancel NX is a port of RMG-K (Rosalie's Mupen GUI with Kaillera and GekkoNet rollback, GPL-3.0) and
of mupen64plus. It is licensed under the GNU General Public License v3.0; see [LICENSE](LICENSE).
It is not affiliated with Nintendo, the RMG-K authors or the Smash Remix team.
