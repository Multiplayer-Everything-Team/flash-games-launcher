# Multiplayer Everything: Flash Launcher

One window for every multiplayer Flash game in the *Multiplayer Everything*
family. Pick a game, press **Play**, **Host online** or **Join online**, and the
launcher starts it in Ruffle or Flash Player with the right file and settings.
Native C++20/Win32, no dependencies, one small `.exe`.

![Library](docs/screenshots/01-library.png)

## What it does

- **Finds everything itself.** Ruffle (its own `players\` folder, `Program Files`,
  PATH), Flashpoint's Flash Player 32, and each game's `.swf` in `games\` or your
  Flashpoint folder. Anything missing gets a button that fixes it: *Locate game
  file...* or *Install Ruffle*.
- **Installs Ruffle for you.** With no Flash player on the PC, the launcher offers
  to install Ruffle: it downloads the latest stable release from Ruffle's official
  GitHub page, checks it against the published SHA-256 checksum, and unpacks it
  into `players\ruffle\`. Nothing is downloaded without your OK.
- **Player slots.** Every game shows how many players it takes, in the players'
  own colors (P1 red, P2 blue, P3 green, P4 yellow). While you host, the slots fill
  with the names of the people who joined.
- **Hosting without extra windows.** *Host online* starts a built-in relay (the
  same protocol as the mods' `gm2_relay.py` / `sh2_relay.py`, so no Python is
  needed) and shows who is connected plus the addresses friends should type,
  including VPN adapters such as Radmin VPN, ZeroTier or Tailscale.
- **Smooth.** One native process. Games run in their own player process; the
  launcher only watches them, so it never slows a match down. The game grid is a
  double-buffered list with art pre-scaled once per size.
- **Keeps track.** Favorites, play count, play time, last played, and which
  player you prefer per game.
- **Everything in `launcher.json`.** Games, actions, players, colors and every
  visible string. Adding a game needs no C++ (see [CONFIG.md](CONFIG.md)).

## Games

| Game | Actions | Notes |
|---|---|---|
| Gun Mayhem 2: More Mayhem | Play, Host online, Join online | Up to 4 players. Online play uses the Gun Mayhem 2 Online mod (relay port 7777). |
| Stealth Hunter 2 | Play, Host co-op, Join co-op | The original is single player; the Stealth Hunter 2 Online mod adds co-op for up to 4 (relay port 7778). |
| Gun Mayhem | Play | Local multiplayer on one keyboard. |
| Fireboy and Watergirl in the Forest Temple | Play | Two-player co-op on one keyboard. |
| Bad Ice-Cream | Play | Co-op on one keyboard. |
| Boxhead: 2Play Rooms | Play | Co-op on one keyboard. |

**No game files are included.** Put your own `.swf` files in the `games\`
folder (names are listed in `games\README.txt`), point the launcher at them with
*Locate game file...*, or install Flashpoint: its copies are picked up
automatically.

## Install and play

1. Download `MultiplayerFlashLauncher.zip` from Releases (or build it, below) and unzip it anywhere.
2. Run `FlashLauncher.exe`. If no Flash player is found it offers to install
   Ruffle; say yes. (Or install [Ruffle](https://ruffle.rs/downloads) yourself.)
   Flashpoint users already have Flash Player 32; the launcher finds it.

Flash Player itself can't be installed automatically: Adobe stopped distributing
it, so there is no official download left. Install [Flashpoint](https://flashpointarchive.org/downloads)
if a game needs Flash Player instead of Ruffle.

**Hosting:** select the game, press *Host online*, then in the game press **F9**
and click **HOST**. Give friends an address from the *Hosting* box. Windows may
ask to allow `FlashLauncher.exe` through the firewall the first time: allow it,
or friends can't connect. Over the internet, everyone joins the same virtual LAN
(Radmin VPN, ZeroTier, Tailscale) or the host forwards the game's TCP port.

**Joining:** press *Join online*, then **F9** in the game, type the host's
address and click **JOIN**.

Keyboard: arrow keys move through the grid, **Enter** plays, **Ctrl+F**
searches, **F5** rescans for players and game files, **Tab** reaches every button.

## Command line

```
FlashLauncher.exe --list                       every game and whether it can start
FlashLauncher.exe --launch gunmayhem2:host     start an action without the window
FlashLauncher.exe --relay 7777                 run only the relay (a dedicated relay PC)
FlashLauncher.exe --install-player ruffle      install Ruffle into players\ruffle
FlashLauncher.exe --validate                   check launcher.json
```

`FlashLauncher.exe --help` lists everything. Settings and the log live in
`%LOCALAPPDATA%\MultiplayerEverything\FlashLauncher\`.

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload.

```bat
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
powershell -ExecutionPolicy Bypass -File build.ps1 -Dist
```

`-Test` runs the C++ tests (config, file lookup, command lines, settings, and
the relay against real sockets) and `tests\e2e.ps1`, which drives the real exe
and its window through the accessibility API with a fake player, and installs a
fake player from a local test server (checksum, failed reinstall, first-run prompt). `-Dist` makes
`dist\MultiplayerFlashLauncher\` and its `.zip`, and refuses to package any
`.swf` or player binary.

## Repository

| Path | Contents |
|---|---|
| `src/core/` | No UI: config, library (players, files, command lines), process watching, relay, player install, settings |
| `src/ui/` | The launcher window and its theme |
| `src/app/` | Entry point and command line |
| `launcher/` | What ships next to the exe: `launcher.json`, `games\`, `players\` |
| `tests/` | C++ tests and the end-to-end script |
| [CONFIG.md](CONFIG.md) | Every `launcher.json` option |

Ruffle is open source ([ruffle.rs](https://ruffle.rs)). Flash Player and the
games belong to their owners; this project ships neither.
