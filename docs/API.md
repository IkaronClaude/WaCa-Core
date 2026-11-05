# The WaCa API (Mods/shared/WaCa/WaCa.lua)

A WaCa mod is a normal [UE4SS](https://docs.ue4ss.com) Lua mod: a folder in `ue4ss\Mods` with `Scripts\main.lua`
(and an `enabled.txt` so UE4SS turns it on). It needs WaCa Core installed - list it as a requirement.

```lua
local hasCore, WaCa = pcall(require, "WaCa")       -- (pcall: without WaCa Core the library itself is missing)
if not (hasCore and WaCaNative_Command) then print("[MyMod] WaCa Core is missing\n") return end
if not WaCa.Needs("1.0.0", "MyMod") then return end -- the oldest WaCa Core your mod works with

WaCa.On("ScreenShown", function(screen) ... end)
WaCa.Ready()          -- the LAST line: events are held back until then; then Started fires and the screens already
                      -- up arrive as ScreenShown
```
See [examples/HelloWaCa](../examples/HelloWaCa) for a complete one.

`WaCa.VERSION` is the installed Core's version; `WaCa.AtLeast("1.2")` says whether it is that or newer (for a feature
you use only when it is there); `WaCa.Needs(version, mod)` logs "needs WaCa Core ... or newer" and returns false when
it is older.

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

## The ground below a point - `WaCa.OnGround(at)`

`{X, Y, Z}` set down on the ground below `at` (a trace from 2 m above to 10 m below; no ground there = `at` unchanged) -
what `WaCa.Spawn` / `WaCa.SpawnCat` do to their spot. For moving an actor somewhere it must stand: a cat's origin is its
feet. Core 1.1.0+ (`WaCa.Needs("1.1.0", ...)`).

## Any of the game's objects, spawned - `WaCa.Spawn(classPath, at[, yaw, scale])`

The class by its object path (`/Game/.../BP_Thing.BP_Thing_C` - loaded when it isn't yet), set down on the ground
below `at`, and spawned even where it overlaps something (the game's own spawn refuses a spot that collides). The actor,
or nil. `WaCa.SpawnCat(look, at, yaw, scale)` is this plus a look (the look's text form - `WaCa.LookOf(cat)` of any
cat). Core 1.1.0+.

## The game's Interact prompt anywhere - `WaCa.InteractSpot(at, onInteract[, { reach = cm }])`

The game's own "Interact" prompt at a place, and `onInteract(spot)` when your cat uses it there - within `reach`
(250 cm by default) of the spot; with several in reach, the nearest. One press calls it once. Returns the spot -
`spot:Remove()`, `spot:MoveTo(at)` - or nil. Use it for "show, not tell": the player does the thing there, a duty
doesn't just tick on arrival. Core 1.1.0+.

## A line over a cat's head - `WaCa.Say(catName, text[, seconds])` / `WaCa.SayOrShow(...)`

The cat's own nameplate (the game's, or one `WaCa.Nameplate` made) shows the line under its name for `seconds`
(default by length), then its own text again. True when shown; false when that cat has no plate up (too far, hidden).
`WaCa.SayOrShow` does the same, and when there's no plate shows "<cat>: <line>" as an on-screen line instead.
Core 1.1.0+.

A cat of your own that answers when spoken to (tested on the VM, 2026-11-05):
```lua
local at = { X = ..., Y = ..., Z = ... }
local donor = WaCa.Cats()[1].actor                               -- any cat, for a look to borrow
local cat = WaCa.SpawnCat(WaCa.LookOf(donor), at, 180)
WaCa.Nameplate(cat, "Driftpaw", WaCa.PlateRole("Nightheart"))    -- the game's own word for that rank
WaCa.InteractSpot(at, function() WaCa.Say("Driftpaw", "You found me! I was watching the river.") end)
```

## A nameplate on a cat you spawned - `WaCa.Nameplate(actor, name, role[, height])`

The game floats a name and rank only over its own quest NPCs; a cat from `WaCa.SpawnCat` has none. `WaCa.Nameplate`
puts the game's own plate widget (`WBP_GuiQuestActorName`) on the actor - a screen-space widget component, `height` cm
over its origin (90 by default) - and returns the widget; called again it reuses the component. For the rank line use
`WaCa.PlateRole("<a game cat's name>")`: the text that cat's plate shows, so it is the game's own word in the player's
language.

## No object is not nil - `WaCa.Live(o)`

Where there is no object, UE4SS hands back a *wrapper of NULL* - not `nil` - from `GetOwner()`, `GetParent()`,
`FindFirstOf`, `StaticFindObject` and object fields (a component's owner while a level unloads, the sixth parent of a
widget, a manager on the main menu). **Any call on such a wrapper** - `GetClass()`, `GetFullName()`, a field - crashes
the game natively (UE4SS+0x2aed57, "reading address 0x10"), and **`pcall` cannot catch it**. `IsValid()` itself is
safe. So never chain through an object that might not be there:

```lua
local owner = WaCa.Live(comp:GetOwner())          -- the object, or nil
if owner then log("%s", owner:GetFullName()) end
-- inside a pcall, turn "no object" into a Lua error it CAN catch:
(WaCa.Live(FindFirstOf("SwtQuestSequencerManager")) or error("no manager")).ActiveSequences:ForEach(...)
```

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
  mods can add beats to the same story day or lines to the same dialogue);
- **explicit overrides, not load order**: `Replaces = <Mod>/<Section>` in your section takes the place of that mod's
  section, whole; `Off = 1` in your copy of a section turns another mod's off (the readers honour it). Two mods
  setting the same plain key differently without that is a **conflict** - logged (`[WaCa] CONFLICT ...` with both
  mods and values, `WaCa.ContentConflicts`) and announced once in the game; only then does load order break the tie;
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
WaCa.StoryCats("ShadowClan", "Leader")                  -- the save's story cats of a clan / part (any clan):
                                                         --   { key, name, clan, role, look } - look = the game's text form
WaCa.ImportLook(cat, look)                               -- dress a cat from such a look
WaCa.SpawnCat(look, at, yaw, scale) / WaCa.Despawn(cat)        -- an extra cat in the world (the game's cutscene cat, no AI),
                                                         --   set down on the ground below `at` (none there: at `at`)
WaCa.HitMaterialName(hit[, slot])                        -- the material name on what a trace hit, or nil (null-safe)
WaCa.PlayMontage(target, montage[, rate, forceMode])     -- a montage on a cat or mesh, null-safe -> its length or nil
WaCa.StopMontage(target[, blend])                        -- stop it, null-safe
WaCa.LookOf(cat)                                         -- a cat's look as text (the form ImportLook / SpawnCat take)
WaCa.BlendLooks(a, b)                                    -- a kit's look from two parents' looks (+ a kit name, + its scale for SpawnCat)
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

## A mod's own keys
`WaCa.ModKey(mod, name, default, label)` -> the key your mod should listen for. It comes from WaCa Keybinds & Mouse's
`keybinds.ini` `[ModKeys]` (`<mod>.<name> = Key`, every mod's keys in one file) when set there, else your own default.
The key is announced (`keys.mod` `{ mod, name, key, label }`) so the controls screen can warn when a player gives a
game command the same key ("F is also ClanLife's key to report a finished task"). Call it at load, for keys that are on.
A binding may list several keys, comma-separated (`SpaceBar, Gamepad_FaceButton_Right`): compare a pressed key with
`WaCa.KeyIs(key, binding)`, never `==`.

**Gamepad buttons** arrive as `KeyDown` / `KeyUp` too (`Gamepad_FaceButton_Bottom` ... - Unreal's key names): WaCa
Core raises them from the game screens' own `OnKeyPressed` / `OnKeyReleased`, once per press however many screens get
it. (Keyboard and mouse come from the game window.) For menu-style moves, the game's screen commands
(`OnListenCommand` - Button_Up / Down / A) cover keyboard and pad alike. `WaCa.Watch` can hand on several parameters,
comma-separated (`"Command,InputEvent"`), and an `FKey` parameter comes as its key name.

## A part in a story (WaCa Clan Life's `Hook:` step)
Every UE4SS mod runs in its own Lua state, so a story step can't call your functions directly. Announce the hook and
answer for it: `WaCa.Publish("hook.<Name>", true, true)`, then `WaCa.On("StoryHook", function(name) ... end)`; when your
part is over, `WaCa.Emit("StoryHookDone", name)`. A story's `Hook: <Name>` waits for that (at most Clan Life's
`events.ini [Events] HookWait`, 60 s); a hook nobody announced is logged and skipped. HelloWaCa has one.

## The fight's party
`WaCa.SetCombatParty({ "TheRegretfulWarrior", 31 })` (role names or values) - the next fights put those story cats
beside your cat instead of the game's own party; the game builds them its own way (looks, stats, moves). `nil` gives
the game's party back. Only story cats (narrative roles), up to the room the game's party has (two).

## Native commands
`WaCa.Command("Verb|arg|...")` sends a command to the native half (false when it's missing or the verb is unknown):
`PressKey|Enter` (a real key press to the game window), `HoldKey|W|1` / `HoldKey|W|0` (a key held down / let go),
`SwallowKey|Enter|1` (the game doesn't get that key - the mods still do - until `|0`), `SwallowAll|1` (no key, mouse
button or wheel reaches the game until `|0` - for a mod's own menu on top of the game's), `ApplyBindings`
(`WaCaKeybinds\config\keybinds.ini` applied to the live bindings now; the key icons and hints follow it too),
`MouseLook|on|yaw|pitch`, `CameraRelativeMove|0/1`, and the others in `native/WaCaNative/src/commands.cpp`.

## A panel on the HUD
`WaCa.HudPanel(id, { title, where, activity, goal, got, need, done, order, icon })` shows a panel in the game's own quest
look, under the game's quest panel; WaCa Core draws every mod's panels, stacked (`order`: lower further up, 100 when
not given), and has every mod send them again when it reloads. `WaCa.HudPanel(id, nil)` takes it away. Spacing:
WaCaCore `config/panels.ini`. Room is short (the game's quest panel takes the top of the column): more panels than fit
are scaled down together, to 60% at most; beyond that the last in the order wait (on the VM's 16:9 screen two fit). WaCa Clan Life's day status is one (order 0); HelloWaCa has one.
`icon`: the goal line's icon, a game texture's object path (`/Game/.../T_UI_Icon_X.T_UI_Icon_X`) - none when not given.
The same panels show in the pause menu's Quest page, on a tab of their own after Side Quests (`config/panels.ini`
[QuestTab]: On, Title - "Clan" -, DetailDown): TriggerR past Side Quests or a click opens it, the first panel by order on it.

## A line in the pause menu's quest tab
`WaCa.QuestLine(id, { title, activity, location, order })` puts a line in the pause menu's QUEST tab, SIDE QUESTS list,
after the game's own side quests (an unused item of the list, filled the game's way). Display only: the game's
navigation never lands on it and it can't be tracked; its detail (the title, the activity, the place) is shown on the
panel beside the list while the mouse is over it, and on its own when the game has no side quest. Up to four lines
share the room the game's quests leave. `WaCa.QuestLine(id, nil)` takes it away; like the HUD panels, WaCa Core draws
them (`questlines.lua`) and every mod sends its lines again when Core reloads. WaCa Clan Life's pack stories use it.

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
