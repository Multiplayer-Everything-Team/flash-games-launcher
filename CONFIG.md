# launcher.json reference

`launcher.json` sits next to `FlashLauncher.exe` (or pass `--config <file>`).
It is JSON with comments (`//`, `/* */`) and trailing commas allowed. Unknown
keys are reported as warnings by `FlashLauncher.exe --validate`, so typos show
up. Keys starting with `_` are ignored (use them for notes).

## Variables

Any path or argument can use these:

| Variable | Value |
|---|---|
| `${LAUNCHER_DIR}` | Folder of `launcher.json` |
| `${GAMES_DIR}` | `paths.games` |
| `${PLAYERS_DIR}` | `paths.players` |
| `${DATA_DIR}` | `paths.data` (settings and log) |
| `${FLASHPOINT}` | First `paths.flashpoint` folder that contains `FPSoftware`. A candidate path using it is skipped when Flashpoint isn't installed. |
| `${GAME_ID}`, `${GAME_NAME}` | The game being started |
| `${SWF}`, `${SWF_DIR}` | The chosen game file and its folder (player arguments only) |
| `${PORT}` | The action's relay port |
| `${TITLE}`, `${SUBTITLE}` | `branding.title` / `branding.subtitle` |
| `${env:NAME}` | Environment variable, e.g. `${env:ProgramFiles}` |
| `${folder:NAME}` | Desktop, Documents, LocalAppData, RoamingAppData, ProgramFiles, Profile, Temp... |

## Top level

| Key | Default | |
|---|---|---|
| `schemaVersion` | `1` | |
| `branding` | | See below |
| `paths` | | See below |
| `behavior` | | See below |
| `players` | required | Flash players, tried in this order |
| `categories` | `[]` | Sidebar groups: `{ "id", "name" }` |
| `games` | required | The library |
| `text` | | Overrides for any UI string (run `--validate` to catch unknown keys) |

### branding

| Key | Default |
|---|---|
| `title` | `"Multiplayer Everything"` |
| `subtitle` | `"Flash Launcher"` |
| `windowTitle` | `"${TITLE} ${SUBTITLE}"` |
| `theme` | `"dark"` (or `"light"`) |
| `colors` | Overrides per role: `background`, `surface`, `surfaceHover`, `sidebar`, `sidebarText`, `sidebarTextMuted`, `primary`, `primaryHover`, `onPrimary`, `text`, `textMuted`, `border`, `focus`, `success`, `warning`, `error`, `online` (`"#RRGGBB"`). Keep text at 4.5:1 contrast or better. |
| `font`, `fontSize` | `"Segoe UI"`, `10` (points): body text |
| `displayFont` | `"Bahnschrift"`: titles, card names, buttons (falls back to `font`) |
| `playerColors` | `["#E5484D", "#3E7BFA", "#2EBD6B", "#F5B83D"]`: P1-P4, used only to mark player slots |
| `window` | `{ "width": 1180, "height": 720, "minWidth": 720, "minHeight": 520 }` (96-DPI units) |

### paths

| Key | Default |
|---|---|
| `games` | `"${LAUNCHER_DIR}\\games"` |
| `players` | `"${LAUNCHER_DIR}\\players"` |
| `data` | `"${env:LOCALAPPDATA}\\MultiplayerEverything\\FlashLauncher"` |
| `flashpoint` | `["C:\\Flashpoint", "D:\\Flashpoint", "${folder:Profile}\\Flashpoint"]` |

### behavior

| Key | Default | |
|---|---|---|
| `minimizeOnLaunch` | `false` | Flash slows down minimized windows, but the launcher's own window doesn't matter; turn on if you like a clean desktop. |
| `stopRelayWithGame` | `true` | Stop the relay when the hosting game closes. |
| `quickExitSeconds` | `4` | A player that exits with an error faster than this is reported. |

## players[]

| Key | |
|---|---|
| `id` | Letters, digits, `-`, `_`, `.` |
| `name` | Shown in the UI |
| `website` | Opened by the *Get <player>* button when the player can't be installed |
| `install` | Optional: how to install the player when it's missing (see below) |
| `paths` | Candidates, first existing wins. Absolute paths may use `*` in the file name (the highest version wins); a bare name like `ruffle.exe` is searched on PATH. |
| `args` | Arguments before the game's own. `${SWF}` is added at the end unless an argument mentions it. |

### players[].install

The launcher offers to install a missing player (on first run, and with the
*Install <player>* button or `--install-player <id>`). It unpacks the archive
with Windows' own `tar.exe`, so `.zip` files work.

| Key | |
|---|---|
| `github` | `"owner/repo"`: use the latest stable release of this repository |
| `asset` | File name pattern of the release file, e.g. `"*windows-x86_64.zip"` |
| `url`, `sha256` | Instead of `github`: a fixed `https://` download and its optional SHA-256 |
| `dir` | Folder to unpack into (replaced on reinstall; the old copy is kept until the new one is in place) |
| `exe` | The player inside the archive, e.g. `"ruffle.exe"` (found at the top or one folder down) |

GitHub releases publish a SHA-256 for each file; the download is refused when it
doesn't match.

## games[]

| Key | |
|---|---|
| `id` | Unique; also the key in settings |
| `name`, `description`, `controls` | Shown in the details panel |
| `players` | Player-count text, e.g. `"1-4 players"` |
| `maxPlayers` | Player slots to show; default: the highest number in `players` |
| `categories` | Category ids |
| `color`, `colorEnd` | Gradient of the generated cover art |
| `image` | Optional cover image (PNG/JPG), relative to `launcher.json` or absolute |
| `actions` | Buttons; the first is the primary one (Enter / double-click) |

### actions[]

| Key | |
|---|---|
| `id`, `label` | `label` is the button text |
| `description` | Second line on the button |
| `hint` | Shown in the status bar after starting (e.g. "press F9, click HOST") |
| `swf` | Candidate files (string or list); `http(s)://` URLs are passed through as-is |
| `args` | Extra player arguments for this action |
| `players` | Allowed player ids, in order; default all |
| `overrides` | Per player id: `{ "swf": [...], "args": [...] }` replacing the action's own |
| `relay` | `{ "port": 7777 }` makes this a host action: the built-in relay starts first |

Example, a new local multiplayer game:

```jsonc
{
  "id": "mygame",
  "name": "My Game",
  "players": "2 players",
  "categories": ["local"],
  "color": "#3CCB7F", "colorEnd": "#1B3A6B",
  "description": "Two players race to the exit.",
  "actions": [
    { "id": "play", "label": "Play", "swf": "${GAMES_DIR}\\mygame.swf" }
  ]
}
```

## Relay protocol

Host actions start a relay compatible with the mods' Python relays: NUL-terminated
UTF-8 messages; `H|name` makes the host, `J|name` a guest; host messages go to
every guest unchanged, guest messages reach the host as `G|<id>|<msg>`; the relay
sends `W|<id>` welcome, `J|<id>|<name>` / `L|<id>` to the host and `X` to guests
when the host leaves; `D|text` lines go to the log. It answers Flash's
`<policy-file-request/>` on the game port and on 843 when free.
