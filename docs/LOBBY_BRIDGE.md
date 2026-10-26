# The launcher's lobby and the localhost bridge (for Flash mod authors)

With the bridge a Flash mod needs **no online panel and no network code of its own**
beyond one socket to `127.0.0.1`. The launcher does the rest: rooms on the central
server or by direct IP, passwords, the version check, ready-up, the start, the "loaded"
barrier, reconnects, chat, kick and ban. The game gets the room's values at start and
talks to its own launcher; the launchers talk to each other.

```
 game (SWF) --127.0.0.1:bridgePort-- launcher ==== central server (or direct TCP) ==== launcher --127.0.0.1-- game
```

The game's networking code is the same online and by direct IP.

## Turning it on for a game

In `launcher.json`, give the game's `online` block a `lobby`:

```jsonc
"online": { "id": "gm2", "join": "online", "lobby": { "version": "1.3", "port": 7790 } }
```

| Key | Default | |
|---|---|---|
| `version` | `"1"` | The mod's version. The launcher adds the first 16 hex digits of the game file's SHA-256 (`1.3#0123456789abcdef`), so only players with the exact same build meet. |
| `port` | `7790` | Where a direct-IP host listens. |
| `enabled` | `true` | `false` turns the lobby off again without removing the block. |

*Play online* and *Quick play* then open the lobby window instead of starting the game,
and the Rooms lists show the game's rooms with their state, ping and region.

## FlashVars the game gets

| Name | Value |
|---|---|
| `lobby` | `1`: the game was started by the lobby and should connect to the bridge, with no panel of its own. |
| `room` | The room code (`ABCDE`), or `port 7790` for a direct-IP room. For display only. |
| `role` | `host` or `client`. |
| `slot` | `0` for the host (P1), `1` to `3` for the others (P2 to P4). |
| `bridgePort` | The port on `127.0.0.1` to connect to. |
| `sessionToken` | 32 hex characters; the first message must carry it. |
| `seed` | A random number every player gets, for shared randomness. |

The action's own `flashvars` (such as `name`) come too. Ruffle needs `--tcp-connections allow`
(already in the shipped player overrides).

## The bridge protocol

`XMLSocket` (or `Socket` with a NUL after each message) to `127.0.0.1:bridgePort`. Each
message is one JSON object, NUL-terminated. The bridge answers Flash's
`<policy-file-request/>` on the same port, so `Security.loadPolicyFile` isn't needed.

**Game to launcher**

| Message | |
|---|---|
| `{"t":"hello","token":"<sessionToken>"}` | First message. A wrong token closes the connection. |
| `{"t":"loaded"}` | Your game is ready to play (level loaded, players placed). Nobody gets `go` until every player's game said this. |
| `{"t":"send","to":-1,"d":<any JSON>}` | Game traffic. `to`: `-1` everyone else, or a slot number. |
| `{"t":"chat","text":"gg"}` | A line in the lobby chat. |
| `{"t":"log","text":"..."}` | A line in the launcher's log (for bug reports). |

**Launcher to game**

| Message | |
|---|---|
| `{"t":"welcome","slot":1,"role":"client","room":"ABCDE","seed":123,"name":"Ann","players":[{"slot":0,"name":"Hana"},{"slot":1,"name":"Ann"}]}` | The answer to `hello`. |
| `{"t":"go"}` | Every game is loaded: start playing now. |
| `{"t":"msg","from":0,"d":<any JSON>}` | Another player's `send`. |
| `{"t":"drop","slot":2}` / `{"t":"back","slot":2}` | A player's connection dropped / came back (they keep their slot for 30 seconds). Pause or keep their character idle. |
| `{"t":"leave","slot":2}` | A player is gone for good (left, kicked, or didn't come back). |
| `{"t":"end"}` | The host's game closed: the round is over. |
| `{"t":"closed"}` | The room closed (the host left). |

Messages from the same player arrive in order. Slots don't change during a round, even
when a player reconnects. The host is the authority; the usual pattern is the host
sending the world state and the others sending their input.

## What happens around it

- **Version check.** The server compares the version string exactly (`1.3#<hash>`), so a
  changed SWF can't join. On a mismatch the launcher offers to update the game files.
- **Passwords** are hashed on the player's PC and checked by the server; other players
  never see them. Wrong tries are rate-limited.
- **Rooms** show their state (in lobby or playing; playing rooms are greyed), size,
  whether they have a password, and the host's region and ping.
- **Heartbeat:** the launchers ping every 5 seconds; 15 seconds of silence counts as a drop.
  A dropped player has 30 seconds to come back with the same session token and slot.
- **Host leaving** closes the room; everyone goes back to the room list with a message.
- **Direct IP:** the host's launcher listens on `port`, lists the addresses to share
  (Radmin VPN, ZeroTier, Hamachi and Tailscale first), checks Windows Firewall, can open
  the port with UPnP, and asks the central server to test whether the port is reachable.

## Server side

Needs the central server's `RELAY|2` star rooms (password, size, `@META` room state,
`ROOMS` list, `PROBE` port test). On an older server the lobby falls back to `RELAY|1`:
it still works, the host checks passwords itself, and the room list shows no state.

## Trying it without a mod

`launcher_tests.exe --fake-player` plays the game's side of the bridge when it gets
`bridgePort` and `sessionToken` FlashVars (see the lobby section of `tests\e2e.ps1`), and
`tests\test_lobby.cpp` runs whole rounds against real loopback sockets.
