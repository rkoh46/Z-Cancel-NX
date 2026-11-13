<p align="center"><img src="assets/logo.png" width="128" alt="Z-Cancel NX"></p>

# Z-Cancel NX

Super Smash Bros. (N64) and Smash Remix with rollback netplay on a Nintendo Switch, built to play
against the PC players who use [RMG-K](https://github.com/Jay-Day/RMG-K). You bring your own ROM;
nothing from a game is in this repository.

Status: early. On a Switch it shows the game, plays sound and takes controllers, with exact rollback underneath and the same emulated state as the Mac build; there is no netplay or menu yet. The plan, what was learned from RMG-K's source and the
order things get tested in are in [docs/PLAN.md](docs/PLAN.md).

| Step | State |
|---|---|
| 1. Speed test: RMG-K's emulator core on the Switch, no picture | done: 0.8 ms per frame in a fight; a 7-frame rollback costs 8.6 ms |
| 2. Determinism rig: ARM64 against x86-64, same inputs, state hashed every frame | a 3-minute mashed VS match stays identical outside saved register slots |
| 3. Picture, sound, controllers | picture: builds for the Switch (RMG-K's GLideN64 on the console's OpenGL) and is verified on a Mac to leave the emulated state untouched; not yet run on a console |
| 4. Netplay: rollback, connect codes, lobby | not started |
| 5. Cross-play against RMG-K on a PC | not started |
| 6. ROM picker and on-device Remix patching | not started |

Z-Cancel NX is a port of RMG-K (Rosalie's Mupen GUI with Kaillera and GekkoNet rollback, GPL-3.0) and
of mupen64plus. It is licensed under the GNU General Public License v3.0; see [LICENSE](LICENSE).
It is not affiliated with Nintendo, the RMG-K authors or the Smash Remix team.
