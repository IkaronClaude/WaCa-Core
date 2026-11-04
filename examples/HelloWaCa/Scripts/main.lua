-- HelloWaCa: a complete small WaCa mod to copy. Copy this folder to ue4ss\Mods\<YourMod> (keep enabled.txt) and
-- rename it. It shows: reacting to the game's events, an event from another WaCa mod, a value kept per save slot,
-- a key of its own, a part in a story (Clan Life's Hook: step) and a game text replaced - plus content/text.ini, the
-- same without code.
local hasCore, WaCa = pcall(require, "WaCa")     -- (pcall: without WaCa Core its library is missing too)
local function log(fmt, ...) print(string.format("[HelloWaCa] " .. fmt .. "\n", ...)) end
if not (hasCore and WaCaNative_Command) then log("WaCa Core is missing - install it first") return end
if not WaCa.Needs("1.0.0", "HelloWaCa") then return end  -- the oldest WaCa Core this mod works with

-- the game: every screen that opens
WaCa.On("ScreenShown", function(screen)
    log("screen: %s", screen:GetClass():GetFName():ToString())
end)

-- another WaCa mod (WaCa Clan Life, if installed): a new day
WaCa.On("DayStarted", function(day)
    log("day %d started (from %s)", day, tostring(WaCa.EventFrom))
end)

-- per save slot: how many times this save was saved with the mod on
WaCa.On("GameSaved", function()
    local file = WaCa.SlotFile("HelloWaCa")
    if not file then return end
    local data = WaCa.ReadIni(file) or {}
    local section = data["Hello"] or { __order = { "Saves" } }
    section.Saves = tostring((tonumber(section.Saves) or 0) + 1)
    data["Hello"] = section
    data.__order = { "Hello" }
    WaCa.WriteIni(file, data, "; HelloWaCa - an example sidecar")
    log("this save has been saved %s times with HelloWaCa", section.Saves)
end)

-- your own event, for other mods (plain data: it reaches every mod using WaCa)
WaCa.On("Started", function()
    WaCa.Publish("hello.ready", { v = 1, greeting = "Hello from HelloWaCa" }, true)
end)

-- your own key (a player can move it in WaCa Keybinds & Mouse's keybinds.ini [ModKeys] HelloWaCa.Greet = <Key>)
local greetKey = WaCa.ModKey("HelloWaCa", "Greet", "H", "say hello")
local greetings = 0
WaCa.On("KeyDown", function(_, name)
    if tostring(name) ~= greetKey or WaCa.InMenu() then return end
    WaCa.Emit("ShowLine", "Hello from HelloWaCa!")              -- (a line on screen - WaCa Cutscenes / Clan Life show it)
    -- a panel of its own on the HUD (WaCa Core stacks it under the game's quest panel and any other mod's)
    greetings = greetings + 1
    WaCa.HudPanel("HelloWaCa.greetings", { title = "HelloWaCa", activity = "Say hello:", goal = "Greetings",
                                           got = math.min(greetings, 3), need = 3, done = greetings >= 3 })
end)

-- a part of a story (WaCa Clan Life): a story step "Hook: HelloGreeting" runs this, and the scene waits until it says
-- it's done. Announce the hook, then answer StoryHook for its name.
WaCa.Publish("hook.HelloGreeting", true, true)
WaCa.On("StoryHook", function(name)
    if name ~= "HelloGreeting" then return end
    log("the story asked for HelloGreeting")
    WaCa.Emit("ShowLine", "HelloWaCa's part of the story: a greeting.")
    ExecuteInGameThreadWithDelay(3000, function() WaCa.Emit("StoryHookDone", "HelloGreeting") end)
end)

WaCa.Ready()
