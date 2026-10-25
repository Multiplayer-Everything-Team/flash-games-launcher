# Publishing a PC game mod for the launcher

For mod authors. The launcher installs a PC game mod from a **package**: a `.zip` you
publish, plus a few lines in the launcher's config and in mods-info. No code change.

## 1. The package

A `.zip` whose top level holds one folder per component, laid out the way the files must
appear in the game:

```
SpookyTogether-0.2.6.zip
  loader/          BepInEx, winhttp.dll, doorstop_config.ini ...   -> ${GAME_DIR}
  mod/             SpookyTogether.dll, README.txt ...              -> ${INSTALL_DIR}
```

- Folder names are the `components[].source` values in the mod's recipe (below).
- Do not put game files in it, and no links: a package containing a symbolic link or a path
  that leaves its folder is refused.
- Anything under the install folder that a player is likely to edit (settings files) goes in
  `install.preserve` so updates keep their changes.

## 2. Publish it

Upload the `.zip` as an asset of a GitHub release (public), then set these in your mod's file in
mods-info (`mods/<id>.json`):

| Field | |
|---|---|
| `latest_version` | The version in the zip, like `0.2.6`. |
| `download_url` | `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.zip`. Only `https://`. |
| `download_sha256` | The zip's SHA-256 (lower-case hex). Optional for a GitHub release asset: the launcher then asks GitHub for the digest it publishes. |

**The launcher never installs a package whose checksum it can't learn.** While `download_url` is
empty the mod shows *Not published yet* and its Install button is disabled.

## 3. The recipe in `launcher.json`

Add a game with a `mod` object (see [CONFIG.md](../CONFIG.md), "PC game mods"). The `spooky-together`
entry in `launcher/launcher.json` is a complete example: how to find the game, which folders of the
package go where, which files the loader creates at run time (`uninstallCleanup`), how to start the game.

Keep `mod.id` the same as the stand-alone installer's `mod.id` if you also ship one: the launcher
then recognises its installs (same install record, same folders) and can update or remove them.

## 4. Try it for real, on a copy

```bat
MultiplayerEverythingLauncher.exe --mods
MultiplayerEverythingLauncher.exe --install-mod <id> --game-dir "D:\Copy of the game"
MultiplayerEverythingLauncher.exe --repair-mod <id> --game-dir "D:\Copy of the game"
MultiplayerEverythingLauncher.exe --uninstall-mod <id> --game-dir "D:\Copy of the game"
```

After the uninstall the game folder must be byte-identical to how it started. `tests\e2e.ps1` does
exactly this against a fake game with a local server standing in for mods-info; copy its "PC game
mods" section to script your own.
