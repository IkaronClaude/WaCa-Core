# Nexus Mods page - WaCa Core

<!-- Paste-ready text for the Nexus Mods upload form. Replace the TODO links once the pages exist.
     Game: Warrior Cats: Clans of the Forest (nexusmods.com/warriorcatsclansoftheforest). Category: Utilities / Modders
     resources. -->

**Name:** WaCa Core
**Version:** 1.0.0
**Category:** Modders Resources / Utilities
**Summary (short description, max 350 characters):**
The shared base of the WaCa mods (Keybinds & Mouse, Looks, Clan Life, Hunting, Graphics) and a modding API for
everyone: events instead of polling, cross-mod events, per-save data next to your save, content files many mods can add
to. Fixes a few name placeholders in the game's dialogue. Open source.

---

## Description

**WaCa Core** is the foundation the WaCa mods are built on - and an open modding framework for your own mods.

On its own it only fixes small things: some of the game's dialogue lines have misspelled name placeholders
(`{THE WARROR FATHER}`), so they never showed the cat's name - WaCa Core corrects them. Install it because a WaCa mod
needs it, or because you want to make mods.

### The WaCa mods
- **WaCa Keybinds & Mouse** - rebind every key, and a **real mouse**: a cursor in every menu, click the menus'
  buttons, PC-style mouse look, Space / click to advance dialogue. TODO: Nexus link
- **WaCa Looks** - any colour, pattern packs, look presets and your own name, right in the game's character creator.
  TODO: Nexus link
- **WaCa Clan Life** (experimental) - day / night, seasons and weather, the clan's daily life, and a story system
  for writers. TODO: Nexus link
- **WaCa Hunting** (experimental) - hunting by skill and chance, scent trails, fishing, wading. TODO: Nexus link
- **WaCa Graphics** - more graphics settings than the options screen. TODO: Nexus link
- **WaCa Cutscenes** (experimental) - cutscenes from plain text: cats on marks, beats, camera shots. TODO: Nexus link

### For modders
Every WaCa mod is an ordinary UE4SS Lua mod; WaCa Core gives yours:
- **Events instead of polling** - react to the game (`WaCa.On("ScreenShown", ...)`), and turn any game function,
  Blueprint ones included, into an event (`WaCa.Watch`).
- **Cross-mod events** - every UE4SS mod is its own Lua state; WaCa Core carries events between them (JSON payloads,
  sticky state topics), so mods can build on each other without sharing code.
- **Per-save data** - a small file next to the game's save for each mod, loaded with that save. The game's own save is
  never touched.
- **Content files several mods can add to** - two story mods can both add scenes to the same day without editing each
  other's files.
- Game text, data tables, cat appearance, assets from your own pak, menu keys.
- Documentation and a complete example mod: https://github.com/IkaronClaude/WaCa-Core/blob/main/docs/API.md and examples/HelloWaCa (also in the mod's docs and examples folders)

## Requirements
- **UE4SS for Warrior Cats** - https://www.nexusmods.com/warriorcatsclansoftheforest/mods/2 (UE4SS 3.0.1-1181-g83330635).
  WaCa Core's native part is built for exactly this UE4SS version.

## Installation
1. Install UE4SS for Warrior Cats (see its page) - once, for all mods.
2. Extract this zip into the game's folder: Steam > right-click the game > Manage > Browse local files. After
   extracting, `Wrc\Binaries\Win64\ue4ss\Mods\WaCaCore` exists.
3. Start the game. The mod turns itself on (its `enabled.txt`); nothing to edit.

**Updating from the old all-in-one "WaCa ModKit" (0.7 or older):** delete the folder
`Wrc\Binaries\Win64\ue4ss\Mods\WaCaModKit` first (keep a copy of its `config` folder if you changed settings - the
same files now live in the WaCa mod they belong to), and `Wrc\Content\Paks\~mods\WaCaModKit_P.*`. Your saves keep
their custom names and looks.

**Uninstall:** delete `ue4ss\Mods\WaCaCore` (and the WaCa mods that need it). No game file is changed.

Vortex: untested - install manually.

## Compatibility
- Tested with the Steam version of the game, October 2026 build (UE 5.7.4). A game update can move the game code the
  native part hooks; such a hook then switches itself off and says so in `ue4ss\Mods\WaCaCore\WaCaNative.log` instead
  of crashing.
- Works alongside other UE4SS mods and pak mods.
- The game's saves stay loadable without the mod.

## Known issues
- TODO: confirm WaCa Core's DLL with the Nexus UE4SS download (built for the same UE4SS commit; tested with a
  from-source build of it).

## Source code & contributing
Open source (Apache 2.0): https://github.com/IkaronClaude/WaCa-Core

**Contributions are very welcome** - bug reports, ideas, pull requests, and writing: stories, clan events and dialogue
are plain text files, no programming needed. If you'd like to help, open an issue or a pull request on GitHub, or post
in the comments here. New to modding this game? The docs are written to get you started, and questions are welcome.

Found a bug? Please attach `Wrc\Binaries\Win64\ue4ss\UE4SS.log` - the mods log what they do, so the log usually shows
the problem.

## AI disclosure
Much of this mod's code and documentation was written with AI assistance (Claude, by Anthropic), directed,
play-tested and released by the author. It contains no AI-generated art, audio or text from the game - the mods reuse
the game's own assets.

## Permissions
- Open source under the Apache License 2.0: you may use, modify, include it in your own mods and modpacks, and upload
  modified versions - keep the licence and credit, and say what you changed.
- Building your own mod on WaCa Core? List it as a requirement rather than bundling it, so players get updates.

## Credits
- UE4SS (RE-UE4SS team) - the mod loader everything runs on.
- MinHook (Tsuda Kageyu) - native function hooks.
- CaptainJackalope - UE4SS for Warrior Cats on Nexus.

## Changelog
- 1.0.0 - first release as its own mod (the shared part of the former all-in-one WaCa ModKit 0.7): cross-mod events,
  sticky topics, the shared speed of the cat, content files for text overrides.
