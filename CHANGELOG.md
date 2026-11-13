# Changelog

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
