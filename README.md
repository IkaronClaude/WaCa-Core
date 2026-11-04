# WaCa Core

The shared base for mods for **Warrior Cats: Clans of the Forest** (Unreal Engine 5.7): one native DLL and one Lua
library that every WaCa mod - and **your** mod - builds on. On its own it fixes a few of the game's text bugs; mostly it
is what the other WaCa mods need:

| Mod | What it does |
| --- | --- |
| **WaCa Core** (this) | the API, the event bus, the save sidecars, cross-mod events, text fixes |
| [WaCa Keybinds & Mouse](https://github.com/IkaronClaude/WaCa-KeybindsMouse) | rebind every key, **a real mouse**: cursor in menus, click the menus' buttons, mouse look |
| [WaCa Looks](https://github.com/IkaronClaude/WaCa-Looks) | any colour, pattern packs, looks, your own name - in the game's character creator |
| [WaCa Clan Life](https://github.com/IkaronClaude/WaCa-ClanLife) | day / night, seasons, weather, the clan's daily life and a story system for writers |
| [WaCa Hunting](https://github.com/IkaronClaude/WaCa-Hunting) | hunting by skill and chance, scent trails, fishing, wading |
| [WaCa Graphics](https://github.com/IkaronClaude/WaCa-Graphics) | more graphics settings than the options screen |
| [WaCa Cutscenes](https://github.com/IkaronClaude/WaCa-Cutscenes) | cutscenes from plain text: cats on marks, beats, camera shots |

Each is its own UE4SS mod: install the ones you want, plus this one.

## Install (players)
1. **UE4SS** - [UE4SS For Warrior Cats](https://www.nexusmods.com/warriorcatsclansoftheforest/mods/2) on Nexus Mods
   (UE4SS 3.0.1-1181-g83330635), installed as its page says. You only install it once for all mods.
2. **WaCa Core**: extract the zip into the game's folder (`...\steamapps\common\Warrior Cats Clans of the Forest`), so
   that `Wrc\Binaries\Win64\ue4ss\Mods\WaCaCore` exists. It turns itself on (`enabled.txt`) - no `mods.txt` edit.
3. Any other WaCa mods the same way.

**Coming from the old all-in-one "WaCa ModKit" (0.7 and older)?** Delete `Wrc\Binaries\Win64\ue4ss\Mods\WaCaModKit`
first (keep a copy of its `config` folder if you changed settings - the same files now live in the mod they belong
to). Your saves keep their custom names and looks: WaCa Looks reads the same sidecar files.

Uninstall: delete the mod's folder. No game file is ever changed; the game's saves only ever get the game's own values,
so they stay loadable without any mod.

## Text fixes (config/text.ini)
- `[Placeholders]`: the game misspells some name placeholders in its own lines (`{THE WARROR FATHER}` ...), so those
  lines never show the cat's name - corrected at load.
- `[Text] KEY = new text`: replace any text the game shows (the rogue "Chesnut" is spelled Chestnut). Other mods can add
  their own in `<TheirMod>/content/text.ini`.
- Lines the game shows with an unfilled placeholder are logged (`UE4SS.log`, `dialogue: UNFILLED`) so they can be fixed.

## For modders
Your mod is a normal UE4SS Lua mod that starts with `local WaCa = require("WaCa")` and ends with `WaCa.Ready()`.
- [docs/API.md](docs/API.md) - the library: events instead of polling, any game function as an event (`WaCa.Watch`),
  text, data tables, assets from your own pak, cat appearance, per-save data (sidecars), content files several mods
  can add to without editing each other's (`WaCa.ReadContent`).
- [docs/EVENTS.md](docs/EVENTS.md) - **cross-mod events**: every WaCa mod is its own Lua state; `WaCa.Emit` /
  `WaCa.Publish` reach every other mod (JSON payloads), and the documented topics of the WaCa mods.
- [examples/HelloWaCa](examples/HelloWaCa) - a complete small mod to copy.

## How it works
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) loads the mods: Lua scripts, plus this native DLL (a UE4SS C++ mod).
- Event-driven, no timers or polling: the DLL turns UE4SS's own ProcessEvent callbacks into events for every Lua mod
  (`WaCa.On`), so Blueprint events Lua's `RegisterHook` can't reach (a screen's `Construct` / `Destruct`) work too, and
  hands events from one mod's Lua state to the others (`WaCaNative_Broadcast`).
- Game code UE4SS has no hook for is found by byte signature and detoured with MinHook (key bindings, text and key
  tables, the cat's look, the camera, hunting, saves). `tools/check_hooks.py <unpacked exe>` checks them against a
  new game build.

## Building
- `native\build.bat` builds `native\out\main.dll` against the UE4SS SDK: a RE-UE4SS source checkout (with submodules)
  at commit `83330635` in `tools\bin\src\RE-UE4SS`, or `WACA_UE4SS_DIR`. Needs Visual Studio's C++ tools and Rust.
  UE4SS's Unreal submodule is private: it needs a GitHub account linked to Epic Games (the release workflow uses the
  secret `UE4SS_SOURCE_TOKEN`). The first build takes about 10 minutes.
- `tools\package.ps1` builds `dist\WaCa-Core-<version>.zip` (the layout to extract into the game's folder).
- `tools\check_lua.py Mods examples` - the release check of any WaCa repo's Lua: it compiles, no accidental globals
  (a local function called above its definition is a nil global in Lua). `pip install lupa luaparser`.

No game files or game data are in this repository (BYO: the game's paks, text and binaries stay on your machine).

## Contributing
Pull requests, bug reports and ideas are very welcome - see [CONTRIBUTING.md](CONTRIBUTING.md). New to modding this
game? The API docs are written to get you started; ask in an issue.

## AI disclosure
Much of this code and documentation was written with AI assistance (Claude, by Anthropic), directed, play-tested and
released by the author. No AI-generated art or audio: the mods reuse the game's own assets.

## License
Apache License 2.0 (see LICENSE). Third party: UE4SS (MIT - installed separately), MinHook (BSD-2,
`native/third_party/minhook`).
