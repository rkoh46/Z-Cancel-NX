<p align="center"><img src="assets/logo.png" width="128" alt="Z-Cancel NX"></p>

# Z-Cancel NX

Super Smash Bros. (N64) and Smash Remix with rollback netplay on a Nintendo Switch, built to play
against the PC players who use [RMG-K](https://github.com/Jay-Day/RMG-K). You bring your own ROM;
nothing from a game is in this repository.

Status: early. The emulator core runs headless on a Switch and on a Mac with working, exact rollback and identical results on both; there is no picture, sound or netplay yet. The plan, what was learned from RMG-K's source and the
order things get tested in are in [docs/PLAN.md](docs/PLAN.md).

| Step | State |
|---|---|
| 1. Speed test: RMG-K's emulator core on the Switch, no picture | done: 0.8 ms per frame in a fight; a 7-frame rollback costs 8.6 ms |
| 2. Determinism rig: ARM64 against x86-64, same inputs, state hashed every frame | first results: see the plan |
| 3. Picture, sound, controllers | not started |
| 4. Netplay: rollback, connect codes, lobby | not started |
| 5. Cross-play against RMG-K on a PC | not started |
| 6. ROM picker and on-device Remix patching | not started |

Z-Cancel NX is a port of RMG-K (Rosalie's Mupen GUI with Kaillera and GekkoNet rollback, GPL-3.0) and
of mupen64plus. It is licensed under the GNU General Public License v3.0; see [LICENSE](LICENSE).
It is not affiliated with Nintendo, the RMG-K authors or the Smash Remix team.
