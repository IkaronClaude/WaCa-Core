# The WaCa API (Mods/shared/WaCa/WaCa.lua)

A WaCa mod is a normal [UE4SS](https://docs.ue4ss.com) Lua mod: a folder in `ue4ss\Mods` with `Scripts\main.lua`
(and an `enabled.txt` so UE4SS turns it on). It needs WaCa Core installed - list it as a requirement.

```lua
local WaCa = require("WaCa")
if not WaCaNative_Command then print("[MyMod] WaCa Core is missing\n") return end

WaCa.On("ScreenShown", function(screen) ... end)
WaCa.Ready()          -- the LAST line: events are held back until then; then Started fires and the screens already
                      -- up arrive as ScreenShown
```
See [examples/HelloWaCa](../examples/HelloWaCa) for a complete one.

## Events instead of polling
Handlers run on the game thread. Never run Lua off the game thread in a mod that uses events (`LoopAsync`,
`ExecuteAsync`): two threads in one Lua state corrupt it. For a delay use `ExecuteInGameThreadWithDelay(ms, fn)`.
```lua
WaCa.On("ScreenShown", function(screen) end)   -- also ScreenHidden, ScreenFocusIn / ScreenFocusOut, CatSpawned, GameSaved
WaCa.Watch("ReceiveBeginPlay", "/Script/Engine.Pawn", "PawnSpawned")      -- ANY UFunction by name, Blueprint ones too
WaCa.On("PawnSpawned", function(pawn) end)
WaCa.Watch("SequencerStarted", "/Script/SwtEngine.SwtSequencerManager", "MyEvent", "post", "sequenceId")
WaCa.On("MyEvent", function(manager, id) end) -- a 5th argument hands one parameter of the call to the handler: an
                                              -- object, or text (a name, a gameplay tag, a string, a number, a bool)
```
`WaCa.Watch(function, class, event, when)`: `when` = `"pre"` (before the function runs) or `"post"` (default).
Events between mods: [EVENTS.md](EVENTS.md).

## Menus or gameplay?
`WaCa.InMenu()` is true while a menu is up (any screen that isn't the gameplay screen or an overlay over it, or a
popup on the gameplay screen - the pause menu, the map). `WaCa.MenuShowing(leaving)` the same with `nil` while no
screen is up at all (the frame or two between two screens). `WaCa.IsGameplayScreen(screen)`: is that screen part of
gameplay (the HUD, an overlay) rather than a menu.

## Text
All of the game's text is one string table (`SwtStrings`, from `Gameplay/Strings/NonAsset/Strings_<LANG>.csv`).
```lua
WaCa.SetText("STR_MAINMENU_PROPHECY_MODE", "Story Mode")   -- any text, by its key
print(WaCa.GetText("STR_CAT_NAME1_PAW"))                    -- "Woodpaw": cat names are STR_CAT_NAME<n><suffix id>
```
Without code: `<YourMod>/content/text.ini` with `[Text] KEY = text` (read by WaCa Core with every mod's).

## Per-save data (sidecars)
One small ini file per mod and save slot, next to the game's save (`%LOCALAPPDATA%\Wrc\Saved\SaveGames\
WrcGame.<save>-<character slot>.<MyMod>.ini`). The game's own save is never touched, so it stays loadable without
any mod.
```lua
local file = WaCa.SlotFile("MyMod")         -- nil until a save slot is active; WaCa.CurrentSlot() = the slot's id
local data = WaCa.ReadIni(file) or {}       -- { [section] = { key = value, __order = { keys } } }
WaCa.WriteIni(file, data, "; MyMod - this save's data")
WaCa.On("GameSaved", function() ... end)   -- write yours when the game saves (quitting without saving drops it, like
                                            -- the game's own changes)
```

## The character being played
```lua
WaCa.CurrentSlot()     -- "<save id>-<cat slot>": the save's own CurrentCatSlot (the cat picked in "Select Your Cat")
WaCa.SaveCharacter()   -- that cat's record as the game last saved it (GameplayProgression, QuestProgression ...)
WaCa.GameProgress()    -- its GameplayProgression
WaCa.PlayerRank()      -- 1 Apprentice ... 4 Leader, and where from: 1 + the rank quests done, or a promotion just now
WaCa.PlayerClan()      -- "RiverClan" ...: the cat in the world's, else the save's
WaCa.PartyRoles()      -- the story's party (the PARTY page's cats): their ENarrativeRole names, from the save
WaCa.Subscribe("player.rank", function(r) end)   -- { rank, name, slot, from } on load and on every promotion
```

## Content files several mods can add to
`WaCa.ReadContent(basePath, name)` reads one mod's file (`basePath`, e.g. `WaCaClanLife/config/clanevents.ini`) and
then `<Mod>/content/<name>.ini` of every enabled mod on top:
- a section only one file has is added as it is (two mods' `[Event.Rogues]` / `[Event.Moonhigh]` both exist);
- a section several files have: **numbered** keys are appended (`Step1`, `Line1`, `Morning1` ... renumbered - two
  mods can add beats to the same story day or lines to the same dialogue); a plain key is set by the later mod
  (logged in UE4SS.log);
- `Off = 1` in your copy of a section turns another mod's off (the readers honour it);
- `section.__mods` lists the mods that touched a section.
So a story mod is a folder with `enabled.txt` and a `content` folder - no code, no edits to anyone else's files.
`WaCa.EnabledMods(modsDir)` lists the enabled mods (mods.txt order, then `enabled.txt` ones by name).

## Assets from your own pak
```lua
local mi = WaCa.LoadAsset("/Game/Mods/MyMod/MI_Thing.MI_Thing")   -- also from your own pak in Content\Paks\~mods
```
(UE4SS's own `LoadAsset` only finds assets in the game's AssetRegistry, so it returns nil for anything in a mod pak.)
Making a pak: cook in a project named `Wrc` with Unreal Engine 5.7.4, the game's renderer settings, Nanite off on
meshes, shader code not shared - WaCa Clan Life's `tools/build_content.ps1` does it headless.

## Data tables
```lua
WaCaNative_CloneTableRow(table:GetAddress(), "FromRow", "NewRow")
WaCaNative_SetTableField(table:GetAddress(), "NewRow", "Field", "value")  -- text, /Game/... asset paths, enum names,
                                                                         -- numbers, bools
```

## Cat appearance
Any cat's `SwtAdventureCustomizableMeshComponent`:
```lua
WaCa.SetCatColor(cat, "Fur", WaCa.ParseColor("#EFE6D6"))
WaCa.SetPartColor(cat, "Tail", "Fur", WaCa.ParseColor("#3E2B22"))
WaCa.SetCatMask(cat, "Markings", "game:M12_Mask")        -- or a folder of PNG masks
for _, cat in ipairs(WaCa.PlayerCats()) do ... end       -- the player's cat + the creation preview
for _, c in ipairs(WaCa.Cats()) do ... end               -- the world's clan cats, nearest first:
                                                         --   { actor, name, clan, giver, story, kind, dist }
WaCa.CatName(cat)                                        -- a cat's name as the game shows it
WaCa.PlayerName()                                        -- your cat's name
WaCa.RebuildLook(comp)                                   -- the game rebuilds a cat's look (every mod's on top)
WaCa.CopyLook(to, from)                                  -- dress one cat as another (cats or look components)
WaCa.ZonePlace("I-")                                     -- the middle of one of the game's zones ("I-" the Island,
                                                         --   "R-05" a camp, or a name id): { X, Y, Z } or nil
```
The look is applied where the game builds it: `WaCa.On("CatLookPart", ...)` runs right after the game writes a colour
or mask to one part of a cat - put your value on the same part there and it is part of every rebuild.

## The game's own scenes
```lua
WaCaNative_PlaySequence(FindFirstOf("SwtQuestSequencerManager"):GetAddress(), "Gameplay.Narrative.RankUp.RiverClan", 3, "")
```
plays a sequence the game registered under a tag (type 3: the narrative progression table - the rank-up ceremonies,
`...RankUp.<Clan>` and `...RankUp.Leader`) through the sequencer manager's own play: placed and anchored the game's way,
with its HOLD SKIP. Nothing of the game's rank-up runs (no promotion). Its cats are stand-ins the game dresses as the
clan's cats when it starts (`WrcCatSkeletalMeshWithCustomization`: `Clan`, `GiverRole`, `Index`) - `WaCa.CopyLook` on
them afterwards re-casts the scene; WaCa Cutscenes' `Scene =` does all of it.

## The game's popups
`WaCaNative_OpenPopup(screen:GetAddress(), id)` opens one of the game's popups (`EWidgetsIds`: 86 = the fresh-kill
pile) on a screen - the HUD's `WBP_GuiHudGameWrc_C` in the world - the way a dialogue's create-screen action does. A
popup is driven with its own commands: `popup:OnListenCommand(command, 0, controller)` then `(command, 1, controller)`
(pressed, released; `ECommands`: 1-4 the arrows, 6 Back, 9 / 10 ShoulderL / R, 26 Confirm).

## The game's text fill
`WaCaNative_FillText(text [, asShown])` -> the text as the game fills it in this save (its {PLAYER} / {Clan} / story
role fill - every dialogue and quest line goes through it; asShown: with WaCa Core's fallback). text.ini `[Fill]
Fallback = 1` (default): a role this save has no cat for is shown in words ("the ambitious apprentice"), not braces.

## Menu keys
`WaCa.SetUIKey("Button_B", "Escape")` replaces a menu's keyboard key; `WaCa.SetUIIcon("Button_B", "Escape")` only its
hint icon.

## Native commands
`WaCa.Command("Verb|arg|...")` sends a command to the native half (false when it's missing or the verb is unknown):
`PressKey|Enter` (a real key press to the game window), `HoldKey|W|1` / `HoldKey|W|0` (a key held down / let go),
`SwallowKey|Enter|1` (the game doesn't get that key - the mods still do - until `|0`), `MouseLook|on|yaw|pitch`,
`CameraRelativeMove|0/1`, and the others in `native/WaCaNative/src/commands.cpp`.

## Widgets on screen
`WaCa.WidgetRect(widget)` -> left, top, right, bottom in desktop pixels (where it was last drawn - read natively: a
widget's geometry can't be read from Lua), `WaCa.MouseOver(widget)` -> is the mouse over it, and its area.
`WaCa.Latest(topic)` -> the latest value of a sticky topic.

## Rules of thumb
- React to events; don't poll (no timers that check every second).
- Read game state from the game, don't guess it; log what your mod decides, so a player's UE4SS.log tells the story.
- Never write the game's save with values the game doesn't know - keep your data in a sidecar.
- Time: `WaCa.Now()` (real seconds, fractional). Not `os.clock()` - that is the process's CPU time and runs several
  times faster than the wall in the game.
- No game files in your mod's download (text, meshes, textures of the game): reuse them by path at runtime.
