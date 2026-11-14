# Changelog

## 0.1.3

- GameCube controllers: L + R + Z + Start leaves a match on GameCube-style controllers that report their
  left shoulder as the Switch L (it only worked when it arrived as ZL). The log names each controller.
- Settings: L button can shield instead of taunting (Smash Remix can put taunt on the d-pad), and the
  C stick can do smash attacks on the ground and aerials in the air, as in Melee, on GameCube
  controllers or on every controller. Both change only your own controller, so they are safe online.
- Settings: the name screen says what the lobby keeps (up to 20 letters, numbers, _ - .), and the list
  scrolls now that it has more rows.

## 0.1.2

- Lobby: the console can now connect to players outside your home. It advertised its public address
  as the internet provider's hostname instead of the number (the Switch's system library ignores the
  "numeric" flag), so RMG-K on PC dropped it and every lobby connection failed. Tested on a Switch
  against a Mac over the public internet path only: a full 60-second match with no stalls.
- Lobby: rooms with a password show "Password" in the list and ask for it before joining, as RMG-K
  does. Opening a room asks for an optional password. A failed join says why in words ("Wrong
  password", "That room is gone") instead of the server's code.
- The message after a failed connection no longer blames a missing relay server.

## 0.1.1

- Frames re-run after a rollback are 11-17 % cheaper on the console (the recompiler now links
  conditional exits on ARM64; faster audio commands that give the same bytes). A full seven-frame
  rollback now fits beside a normal frame on average in all three games.
- Menus: digits in checksums and addresses are evenly spaced; Settings sits directly under the games.

## 0.1.0

First release.

- Super Smash Bros. (USA), Smash Remix 2.0.1 and Smash Remix 2.0.1 +EXTRA 0.6.0 from one start
  screen. The Remix games are built on the console from your own ROM and the official patches.
- Rollback netplay from RMG-K v0.9.16: host or join by connect code or IP address, the public
  games list, and the lobby for rooms of up to four players.
- Picture through GLideN64, sound, and Switch or GameCube controllers.
- Game code is compiled ahead of need, so matches do not hitch the first time code runs.
- Shader lists for every stage and fighter ship with the app, so effects do not hitch the first
  time they appear.
- Rollback saves and loads copy only the memory that changed: a rollback of up to five frames
  fits inside one frame on the console.
- Menus in the console's own style, following its light or dark theme.

Known limits:
- A full seven-frame rollback (about 120 ms of lag) can still make one frame late.
- Not yet tested in a match against RMG-K on a Windows PC or through the community servers.
