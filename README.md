<p align="center"><img src="assets/logo.png" width="128" alt="Z-Cancel NX"></p>

# Z-Cancel NX

Super Smash Bros. (N64), Smash Remix and Smash Remix +EXTRA on a Nintendo Switch, with rollback
netplay that plays against PC players on [RMG-K](https://github.com/Jay-Day/RMG-K).

You bring your own ROM. Nothing from a game is in this repository or in a release.

## What you need

- A Switch running Atmosphere, with the Homebrew Menu.
- Your own dump of Super Smash Bros., USA version (`.z64`, `.n64` or `.v64`).
- For online play: the console on your network, ideally by cable.

## Install

1. Download `Z-Cancel-NX-v<version>.zip` from [Releases](../../releases) and copy its `switch`
   folder onto the root of your SD card.
2. Copy your Super Smash Bros. ROM into `switch/z-cancel-nx/`. The name does not matter.
3. Open the Homebrew Menu by holding R while starting a game, and start Z-Cancel NX. The Album
   shortcut does not give homebrew enough memory.

## Pick a game

The start screen offers three games, so you can always match what the other player runs:

| Game | Where it comes from |
|---|---|
| Super Smash Bros. | Your ROM |
| Smash Remix 2.0.1 | Built on the console from your ROM and the official patch |
| Smash Remix 2.0.1 +EXTRA 0.6.0 | Built on the console from your ROM and the official patch |

The first time you choose a Remix game the app downloads the patch from the project's GitHub
release, applies it and checks the result against the checksum RMG-K expects. Without internet,
put the `.xdelta` file in `switch/z-cancel-nx/patches/` yourself.

## Controls

| Switch | GameCube controller | N64 |
|---|---|---|
| A, B | A, B | A, B |
| ZL, ZR | L, R | Z (shield) |
| R | Z | R (grab) |
| L | | L (taunt) |
| X, Y, right stick | X, Y, C-stick | C buttons |
| Plus | Start | Start |
| Left stick | Control stick | Control stick |

Leave a game with Minus, or by holding ZL + ZR + R + Plus for half a second. Stick range and sound
are in Settings on the start screen.

## Online

Choose a game, then Online. Both players need the same game.

| Option | What it does |
|---|---|
| Host a game | Shows a connect code (or your address) to give to the other player |
| Join with a code | Type the host's code |
| Join by IP address | Type the host's address and port |
| Public games | Games hosted on the public list |
| Lobby | RMG-K's lobby, for rooms of up to four players |

A PC player on RMG-K v0.9.16 joins the same way from its Peer to Peer or lobby window.

## Status

Tested on an Erista (first model) Switch:

- Offline play with picture, sound and controllers, for all three games.
- Rollback: a 7-frame rollback costs about 9 ms; rewinding and re-running gives the identical
  state every time, in all three games.
- The Switch produces the same emulated state as a Mac running the same core, checked every
  600 frames through full matches.
- Online play, hosting and joining by address and through the lobby, against a Mac build of the
  same code on a local network.

Not yet tested: matches against RMG-K on a Windows PC over the internet, and the community's
connect-code, public-list and lobby servers. Reports are welcome.

Frame-time measurements for every stage are in [docs/STAGE_MATRIX.md](docs/STAGE_MATRIX.md).

## Build from source

Needs Docker, git and Python 3 on macOS or Linux.

```bash
builder/fetch-deps.sh       # RMG-K and xdelta3 at the pinned commits, into ref/
builder/docker-image.sh     # the devkitA64 toolchain image, once
builder/build-switch.sh     # build/switch/z-cancel-nx.nro
```

`tools/release.sh` packages a release zip. How the port works, what was learned from RMG-K's
source and the test results are in [docs/PLAN.md](docs/PLAN.md).

## Licence and credits

Z-Cancel NX is a port of [RMG-K](https://github.com/Jay-Day/RMG-K) (Rosalie's Mupen GUI with
Kaillera and GekkoNet rollback) and of mupen64plus, with GLideN64 for the picture. It is licensed
under the GNU General Public License v3.0; see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

It is not affiliated with Nintendo, the RMG-K authors or the Smash Remix team.
