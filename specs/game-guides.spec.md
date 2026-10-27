# Game guides and a calmer library

Written in the order of the "feature-forge" skill (https://github.com/jeffallan/claude-skills): overview, requirements in
EARS form, acceptance criteria, error handling, a checklist. The skill's interview step was answered by the maintainer's
request: "organize the launcher and make it more friendly to use, and add instructions for each game, like a page of it".

## 1. Overview and user value

A player who picks a game today sees a card and a few buttons. They are not told what they need, what to press, how
friends join, or what to do when something is missing. This adds **a guide page for every game** (what you need, how to
play it step by step, keys, tips, what the website says about it, links), and a few changes that keep the library easy to
scan.

Two kinds of reader:
- **A first-time player** who wants to be playing with a friend in a few minutes.
- **The maintainer**, who adds a game and writes its guide in `launcher.json` without a code change.

## 2. Functional requirements (EARS)

**Guide page**
- G1. When the player presses **Guide** in a game's details panel, the launcher shall show that game's guide page in
  place of the game grid, and shall keep the details panel visible.
- G2. The guide page shall list, in this order, whichever of these the game has: *About* (the full description),
  *What you need*, *How to play*, *Keys*, *Good to know*, *From the website* (stage, quality grade, version, last update,
  how it connects, who made it, what stays in sync).
- G3. Where a game's `launcher.json` entry has no `guide` text for a section, the launcher shall fill it from a default
  for that kind of game (PC mod, Flash game with the lobby, Flash game with the F9 panel, one-keyboard play, coming soon).
- G4. Where a game has `guide.links`, the launcher shall show one button per link in a bar at the bottom of the page.
- G5. When the page is longer than its space, the launcher shall scroll it with the mouse wheel, Page Up, Page Down,
  Up, Down, Home and End, and shall show a scroll indicator.
- G6. When the player presses **Back**, presses Esc, picks another entry in the sidebar, searches, or selects another
  game, the launcher shall return to the grid.
- G7. A coming-soon game's guide shall say that it is not available yet, and shall not offer steps for playing it.

**Library**
- L1. The launcher shall keep games that are not released yet out of *All games*, the platform lists and the kind lists, and
  show them under their own *Coming soon* view. (Done in this round.)
- L2. The grid shall list favorites first, then games that are ready, then the rest, then coming-soon games. (Done.)
- L3. When a list is empty, the launcher shall say why and what to do. (Done.)

## 3. Non-functional requirements

- N1. Every control on the page (Back, the link buttons) shall be a real Win32 control with an accessible name.
- N2. Text on the page shall keep a contrast of 4.5:1 or better, using only the theme's colors.
- N3. Opening the page shall take no network request. Everything on it is already in `launcher.json` or in the copy of the
  website's list the launcher keeps.
- N4. Data that comes from the website is shown as text and never run. A link the launcher opens shall be `https://` and,
  for a repository, an `owner/name` made only of letters, digits, `-`, `_` and `.`. A game id from the website that has
  any other character is ignored, because it becomes part of a file name.
- N5. The page shall work at the minimum window size without text running outside its area.

## 4. Acceptance criteria

```
Given a PC mod that is not installed,
When the player opens its guide,
Then the page shows "What you need" with the game and the mod version rule, "How to play" starting with Install, and
     a link button for each link in its guide.
```
```
Given the guide of a game with no `guide` entry in launcher.json,
When it is opened,
Then the page shows the default steps for its kind, not an empty page.
```
```
Given a coming-soon game,
When its guide is opened,
Then the page says it is not available yet and has no "How to play" steps.
```
```
Given the guide page is open,
When the player presses Esc,
Then the grid comes back and the game stays selected.
```
```
Given the website lists a game with the id "../x",
When the launcher reads the list,
Then that entry is ignored and no file outside the covers folder is written.
```

## 5. Error handling

| Situation | What the launcher does |
|---|---|
| The website data is missing or old | The "From the website" section is left out; the rest of the page is unaffected. |
| A `guide.links` entry is not `https://` | `--validate` reports it; the launcher never shows it. |
| A repository from the website is not `owner/name` | No GitHub link is offered for it. |
| The text is longer than the page | The page scrolls; nothing is cut off. |

## 6. Checklist

- [x] Spec written (this file).
- [x] Coming soon view, grid order, empty-view texts.
- [x] `guide` in `launcher.json` and its checks (`core/config`).
- [x] The page's content built in `core/guide` (testable without a window).
- [x] The page in the window (`ui/launcher_guide.cpp`): paint, scroll, keys, Back, link bar.
- [x] Guides written for every game, from what the mods' own READMEs say.
- [x] Security review of what the website's data can reach (see `docs/SECURITY_REVIEW.md`).
- [x] Tests for the content builder, the config checks and the id rule (`tests/test_guide.cpp`, `tests/test_site.cpp`).
- [ ] Not done: a guide a player can read before installing from the website, and per-game screenshots on the page.
