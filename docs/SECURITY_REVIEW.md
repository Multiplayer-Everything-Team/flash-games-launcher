# Security review: what the website's data can reach

Done in the order of the "security-reviewer" skill (https://github.com/jeffallan/claude-skills): scope, scan, review by
hand, classify, report. Scope: the code added for the website's `games.json` (core/site, core/guide, the covers, the About
and Guide pages). Authorization: this is our own code in our own repository; nothing was run against anything else.

## Scope: what comes from outside

The launcher reads `https://www.multiplayereverything.com/games.json` and keeps a copy. A game's `id`, `repo`, `cover`,
`color` and texts come from it. If the site were wrong, hacked or just edited carelessly, those values are the attack
surface: they end up in a file name, a link the launcher opens, a download address and on screen.

## Findings

| ID | Severity | Where | Finding | Fix |
|---|---|---|---|---|
| SEC-1 | High | `siteCoverFile` | The cover was saved as `site-<id>.png` with the website's `id` in the name. An id like `..\..\x` could place the file outside the covers folder. (Only a PNG or JPEG could be written, but the place is the problem.) | An id may only hold letters, digits, `-` and `_` (up to 48); any other entry is dropped when the list is read. |
| SEC-2 | Medium | Guide page / link button | The GitHub link was `https://github.com/` plus the site's `repo`. A value with `/`, `..`, spaces or `?` could point somewhere else on github.com or break the link. | `repo` must be exactly `owner/name` of letters, digits, `-`, `_` and `.`; anything else is dropped, and checked again where the link is made. |
| SEC-3 | Medium | `fetchSiteCovers` | The cover path was only checked for `..`. A path like `https://evil/x.png` or `covers/../../x` could steer the download address. | Only `covers/<name>.png` or `.jpg` (letters, digits, `-`, `_`, `.`) is accepted. Pictures are fetched from the same host as `games.json`, over https, at most 3 MB, and are deleted unless they start like a PNG or JPEG. |
| SEC-4 | Low | `loadSite` | A site that answers with something that is not a games list. | Nothing from it is used; the last good copy keeps serving. |
| SEC-5 | Info | Texts from the site | Descriptions, names and notes are shown as text only. Nothing is run, expanded as a variable or used as a path. | None needed. |
| SEC-6 | Info | Guide links in `launcher.json` | A `guide.links` address could be `http://` or something else. | `--validate` rejects any link that is not `https://`; at most four are shown. |

## Checked and fine

- Package downloads (the PC mods) are unchanged by this work: GitHub's SHA-256 is checked before anything is installed.
- The website's list never decides what is installed or run; it only decorates cards and pages.
- Redirects from https to http are refused by the HTTP layer.

## Tests

`the_website_cannot_steer_a_file_name_or_a_link_the_launcher_uses` (tests/test_site.cpp) feeds the launcher ids, repos and
covers built to misbehave and checks that none of them survives.

## Left open

- The website's own server is outside this review.
- The launcher trusts `games.json` for names, grades and descriptions. A wrong grade would show wrong; it can't do more.
