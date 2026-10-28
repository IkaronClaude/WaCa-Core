-- HelloWaCa: a complete small WaCa mod to copy. Copy this folder to ue4ss\Mods\<YourMod> (keep enabled.txt) and
-- rename it. It shows: reacting to the game's events, an event from another WaCa mod, a value kept per save slot,
-- and a game text replaced - plus content/text.ini, the same without code.
local WaCa = require("WaCa")
local function log(fmt, ...) print(string.format("[HelloWaCa] " .. fmt .. "\n", ...)) end
if not WaCaNative_Command then log("WaCa Core is missing - install it first") return end

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

WaCa.Ready()
