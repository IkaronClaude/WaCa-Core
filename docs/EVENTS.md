# Events - within a mod and across mods

Every UE4SS mod runs in its own Lua state: a table, a function or a `require`d module of one mod is invisible to all
the others. WaCa mods talk through **events**, and WaCa Core carries them from one mod to every other.

```lua
local WaCa = require("WaCa")
WaCa.On("DayStarted", function(day) print("day " .. day .. " - from " .. tostring(WaCa.EventFrom)) end)
WaCa.Emit("MyMod.PreyCounted", 12, "rabbit")      -- every mod's WaCa.On("MyMod.PreyCounted", ...) runs
WaCa.Ready()                                        -- last line of main.lua: events are held back until then
```

## How it works
- `WaCa.Emit(event, ...)` runs the handlers of **this** mod at once, then - when every argument is plain data -
  hands the event to WaCa Core's native half, which delivers it to **every other mod** using WaCa (on the game thread,
  at once - no queue, no frame of delay).
- Plain data = numbers, strings, booleans, nil and tables of those (arrays and string keys, nested). It travels as
  JSON. A UObject (an actor, a widget), a function or a table with a metatable can't cross: such an event stays in
  the mod that sent it - fine for the mod's own internal events.
- A handler sees where an event came from: `WaCa.EventFrom` is the sending mod's folder name (`"WaCaClanLife"`),
  `nil` for the mod's own events and for the game's.
- A received event is never sent on (no echoes, no loops).
- The game's events (`ScreenShown`, `CatSpawned`, `GameSaved`, `KeyDown`, `WaCa.Watch` events ...) are raised by the
  native half in every mod directly - with their UObjects, since they don't travel.

## Topics: one table, sticky state
For anything another mod may build on, prefer a **topic** - one table payload, named `<mod>.<thing>`, documented:
```lua
WaCa.Publish("mymod.mood", { cat = "Firestar", mood = "angry", v = 1 })        -- a moment
WaCa.Publish("mymod.clanSize", { ThunderClan = 21, v = 1 }, true)               -- sticky: a state
WaCa.Subscribe("mymod.clanSize", function(data, fromMod) ... end)
```
- **Sticky** topics are states rather than moments: WaCa Core keeps the latest one, and a mod that subscribes later (or
  starts after it was sent) gets it at once from `WaCa.Subscribe`.
- Put a version field (`v = 1`) in a topic's table and only ever ADD fields; when a field must change meaning, publish
  a new topic name (`mymod.mood2`). Readers ignore fields they don't know.
- Name things after what happened, not after who should react ("clan.thirdStarted", not "hunting.updateOdds").

## Events of the WaCa mods
These are stable for other mods to use (positional arguments as listed).

### WaCa Core
| Event | Arguments | |
| --- | --- | --- |
| `core.started` (topic, sticky) | `{ version, mods = { "WaCaCore", ... } }` | the WaCa mods running |
| `player.rank` (topic, sticky) | `{ v = 1, rank, name, slot, from }` | your cat's rank (ECharacterRank: 1 Apprentice ... 4 Leader) - from the save's rank quests when a character loads (`from = "rank quests"`), at once on a promotion (`"promotion"`); `WaCa.PlayerRank()` reads it |
| `SpeedFactor` (send it) | `source, factor` | slow the player's cat down: the cat's speed is the product of every source's factor; `1` / `nil` = that source off |

### The game (raised in every mod by the native half; objects don't travel)
`Started` (after your `WaCa.Ready()`), `ScreenShown` / `ScreenHidden` / `ScreenFocusIn` / `ScreenFocusOut` (screen),
`CatSpawned` (actor), `CatStep` (the cat's mesh: a footstep), `CatLookPart` (a part of a cat's look built),
`GameSaved` (context), `KeyDown` / `KeyUp` (_, key name - mouse buttons too), `MouseMove` (_, x, y),
`MouseWheel` (_, notches), `AreaOverlap` (area, other, "1" entering / "0" leaving, ...),
`HuntCaught` / `HuntMissed` / `PreyFlee` (prey, cat, ...), `HuntPounce` (prey, "1" the pounce walk starts / "0" the
leap), `HuntSequenceEnd` (prey: the catch animation is over), `SteamTextInput` (submitted, text), the sequencer:
`SequenceStarted` / `SequenceFinished` (manager, the sequence's id), `SequenceEnded`, `DialogueStarted` (manager, the
dialogue's id), `DialogueNext`. Any other game function: `WaCa.Watch` (see API.md).

### WaCa Clan Life
| Event | Arguments |
| --- | --- |
| `DayStarted` / `DayEnded` / `DayRestored` | day |
| `HourChanged` | hour, day |
| `TimeFrozen` | frozen (bool), source |
| `TimeSkipped` | days, fromDay, day |
| `SeasonChanged` | season, previous |
| `WeatherChanged` | weather, previous |
| `SetWeather` (send it) | weather name (Clear, Overcast, Rain, Snow ...), hours - that weather now |
| `WorldFactors` | hunt, fish (the season's x weather's multipliers) |
| `TiredFactors` | hunt, damage (the cat's tiredness) |
| `ClanThird` / `ClanNight` | day, third ("Morning", "Noon", "Evening" / "Night") |
| `TaskAssigned` | day, third, task kind |
| `TaskDone` | day, third, task kind |
| `TaskTurnedIn` | day, third, task kind, how ("Pile", "Partner", a role ...) |
| `ClanThirdEnded` | day, third |
| `CampEntered` / `CampLeft` | day, third |
| `DutiesJudged` | day, done, missed, standing |
| `GatheringAnnounced` / `GatheringHeld` | day, chosen |
| `PatrolOut` / `PatrolBack` | patrol id (+ cats, which stay in Clan Life) |
| `RoleBound` | role key (`S:herbalist`), the cat's object path |
| `roles.cast` (topic, sticky) | `{ ["S:<role>"] = the cat's object path }` - every mod role bound (`events.ini [Cast]`, `Events.BindRole`) |
| `map.marks` (topic, sticky) | `{ [id] = label }` - the marks Clan Life put on the world map (hunting ground, border points, den, meeting place) |
| `StoryVar` | name, value (a story variable changed) |
| `WalkerMoves` | name, actor path, "home" / "away" (a camp's named cat sets off) |
| `NpcPatrol` (send it) | n - a patrol of n camp cats walks the border points and back (it stops when it meets you) |
| `NpcHunt` (send it) | n - n of the camp's plain cats walk out to hunt (and back) |
| `TemporaryParty` (send it) | n, why - n of the camp's plain cats walk with you (behind your party); 0 = they go home |
| `WalkerWalkTo` (send it) | actor path, X, Y, Z - that cat walks there, stays a while, walks home |
| `PlayScene` (send it) | step lines joined by `\|` (`Groups: Prey\|Wait: 3\|Release`) - a scene plays now |
| `SetStoryVar` (send it) | name, value |
| `clan.status` (topic, sticky) | `{ v = 1, day, third, stage, task, turnIn, title, where, activity, goal, got, need, done, text }` - the day at a glance (Clan Life shows it on the HUD) |

A scene's `Emit: <event>` step sends `<event>` (with the scene's context table) - so a story written in ini files can
trigger your mod's code.

### WaCa Cutscenes
| Event | Arguments |
| --- | --- |
| `PlayCutscene` (send it) | name - plays it (one at a time: queued while another plays) |
| `CutsceneStarted` | name |
| `CutsceneEnded` | name, why ("its End beat", "its last beat", "MaxSeconds", "unknown", "off" ...) |
| `cutscene` (topic, sticky) | `{ playing, name }` |
| `ShowLine` (send it) | text [, seconds] - a subtitle on screen outside a cutscene (default time: by its length) |

### WaCa Hunting
| Event | Arguments |
| --- | --- |
| `RoleFactors` | hunt, fish (the cat's specialisation) |
| `SpeedFactor` | "wading", factor (sent to Core) |

## Performance
An event that crosses mods is one JSON encode in the sender and one decode per receiving mod - microseconds for the
small payloads above. Don't broadcast per frame; the events above are all per action, per hour or per day.
