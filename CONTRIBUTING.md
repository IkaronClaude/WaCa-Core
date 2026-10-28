# Contributing

Thank you for wanting to help! Every kind of contribution is welcome - code, ideas, bug reports, testing, writing
(stories, dialogue, clan events), looks and patterns, translations of the docs. You don't need to be a programmer:
much of WaCa is plain ini files on purpose.

## Reporting a bug
Open an issue with:
- what you did, what you expected and what happened;
- the game's version (Steam: right-click the game > Properties > Updates) and which WaCa mods / versions you use;
- `Wrc\Binaries\Win64\ue4ss\UE4SS.log` (and `ue4ss\Mods\WaCaCore\WaCaNative.log`) from the session - WaCa logs what
  it decides, so the log usually shows the problem. If the game crashed, the newest folder under
  `%LOCALAPPDATA%\Wrc\Saved\Crashes` too.

## Pull requests
- Small and focused is easiest to review: one fix or feature per pull request.
- Match the code around it: event-driven (no polling timers), game state read from the game, every decision logged
  with the mod's prefix (`[WaCaClanLife] ...`), settings in the mod's ini files with a comment that explains them.
- **No game files or game data** in the repository - no text exported from the game, no meshes, textures, sounds or
  paks of the game. Reuse the game's assets by their path at runtime.
- Say how you tested it (in game: what you did and saw; the log lines that show it).
- Update the README in the same pull request when players or modders see a change.
- New to Lua, UE4SS or this game? Open a draft pull request or an issue early and ask - happy to help.

## Content without code
Stories, clan events, dialogue lines, looks and pattern packs are files. You can publish them as your own mod (a
folder with `enabled.txt` and a `content` folder - see WaCa Core's `docs/API.md`, "Content files several mods can add
to") or send them here as a pull request to be included.

## Licence
By contributing you agree that your contribution is licensed under the repository's licence (Apache 2.0).

## AI-assisted contributions
Welcome, the same as any other - you are responsible for what you submit: test it in game and say so.
