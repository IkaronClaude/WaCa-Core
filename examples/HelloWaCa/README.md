# HelloWaCa - an example WaCa mod

Copy this folder into `Wrc\Binaries\Win64\ue4ss\Mods\` (next to `WaCaCore`), rename it to your mod's name, and start
the game: `UE4SS.log` (in `Wrc\Binaries\Win64\ue4ss`) shows its `[HelloWaCa]` lines.

- `enabled.txt` - UE4SS turns the mod on (no `mods.txt` edit).
- `Scripts/main.lua` - the code: events from the game and from other WaCa mods, a value kept per save, an event of
  its own for other mods, a key of its own (`H` - movable in WaCa Keybinds & Mouse's `[ModKeys]`), and a part in a
  story: a WaCa Clan Life scene step `Hook: HelloGreeting` runs it and waits for it.
- `content/text.ini` - content without code: replaces a game text ("Prophecy Mode" -> "Story Mode" in the main menu).

UE4SS can reload the Lua mods while the game runs (Ctrl+R, with hot reload on in `UE4SS-settings.ini`), so you can
edit and look again without restarting. Docs: [API.md](../../docs/API.md), [EVENTS.md](../../docs/EVENTS.md).
