-- WaCa Core (Lua half): what every WaCa mod shares and nothing else - the text fixes (config/text.ini), the check for
-- dialogue lines the game left unfilled, the cat's speed (one product of every mod's slow-downs) and the developer
-- traces (config/dev.ini). The API itself is Mods/shared/WaCa/WaCa.lua (require("WaCa") in any mod) on top of the
-- native half (dlls/main.dll). The other WaCa mods are separate UE4SS mods: Keybinds & Mouse, Looks, Clan Life,
-- Hunting, Graphics - each one optional.
local VERSION = "1.0.0"
local WaCa = require("WaCa")

local modDir = debug.getinfo(1, "S").source:match("^@(.+)[/\\]Scripts[/\\]main%.lua$") or "."
local modsDir = modDir:match("^(.+)[/\\][^/\\]+$") or modDir
local function log(fmt, ...) print(string.format("[WaCaCore] " .. fmt .. "\n", ...)) end
log("%s loaded", VERSION)
if WaCa.VERSION ~= VERSION then
    log("WARNING: Mods/shared/WaCa is version %s, this WaCaCore %s - a mixed install: extract WaCa Core's zip again",
        tostring(WaCa.VERSION), VERSION)
end

-- the mods running with this Core (other mods can read it: WaCa.Subscribe("core.started", fn) - sticky)
local mods = {}
for _, name in ipairs(WaCa.EnabledMods(modsDir)) do
    if name:match("^WaCa") then table.insert(mods, name) end
end
log("WaCa mods on: %s", table.concat(mods, ", "))
-- the old all-in-one WaCa ModKit (0.7 and older) does everything the split mods do: both on = everything twice
for _, name in ipairs(mods) do
    if name == "WaCaModKit" then
        log("WARNING: the old WaCa ModKit (Mods/WaCaModKit) is still on - delete that folder (or set it to 0 in " ..
            "mods.txt): it and the new WaCa mods would both run")
    end
end

-- config/text.ini [Text] KEY = new text
local function applyTextOverrides()
    local ini = WaCa.ReadContent(modDir .. "/config/text.ini", "text")
    local section = ini and ini["Text"]
    if not section then return end
    local n = 0
    for _, key in ipairs(section.__order) do
        if WaCa.SetText(key, section[key]) then n = n + 1 else log("text override %s did not apply", key) end
    end
    log("%d text overrides applied", n)
end

-- config/text.ini [Placeholders] WRONG = RIGHT: the game's own lines with a misspelled name placeholder ({THE WARROR
-- FATHER}) never get the name filled in - every line holding {WRONG} gets {RIGHT} (the game's text is read and
-- rewritten at runtime, nothing of it is shipped)
local function applyPlaceholderFixes()
    local ini = WaCa.ReadContent(modDir .. "/config/text.ini", "text")
    local section = ini and ini["Placeholders"]
    if not section or #section.__order == 0 then
        log("placeholder fixes: none configured")
        return
    end
    local ok, keys = pcall(function()
        return StaticFindObject("/Script/Engine.Default__KismetStringTableLibrary"):GetKeysFromStringTable(FName("SwtStrings"))
    end)
    if not ok or not keys then
        log("placeholder fixes: the game's text keys can't be listed (%s)", tostring(keys))
        return
    end
    local fixed = 0
    for i = 1, #keys do
        local k = keys[i]
        local key = type(k) == "string" and k or (k.ToString and k:ToString()) or (k.get and k:get():ToString()) or tostring(k)
        local text = WaCa.GetText(key)
        if text:find("{", 1, true) then
            local new = text
            for _, wrong in ipairs(section.__order) do
                new = new:gsub("{" .. wrong:gsub("%p", "%%%0") .. "}", "{" .. section[wrong]:gsub("%%", "%%%%") .. "}")
            end
            if new ~= text then
                if WaCa.SetText(key, new) then
                    fixed = fixed + 1
                    log("placeholder fix: %s", key)
                else
                    log("placeholder fix %s did not apply", key)
                end
            end
        end
    end
    log("placeholder fixes: %d lines of %d", fixed, #keys)
end

-- config/text.ini [Fill] Fallback: a role a line names that this save has no cat for is shown in words, not braces
-- (native: a hook on the game's own text fill - fill.cpp)
local function applyFillFallback()
    local ini = WaCa.ReadContent(modDir .. "/config/text.ini", "text") or {}
    local on = ((ini["Fill"] or {}).Fallback or "1") ~= "0"
    if not WaCaNative_TextFallback then return end
    log("roles with no cat in this save: %s", WaCaNative_TextFallback(on) and (on and "shown in words" or "as the game has it")
        or "the game's text fill wasn't found - as the game has it")
end
pcall(applyFillFallback)

-- Dialogue text check (the game fills the role placeholders of its lines - {THE AGING LEADER}, {PLAYER} - from data,
-- and some come out unfilled or as a number: "It seems that 1's Prophecy is true"). Every line, once shown in full
-- (the conversation widget's OnTypewriterDone), is checked for a placeholder left in braces or a bare number where a
-- name goes; a hit is logged with the dialogue node it came from, so the line can be fixed (text.ini).
WaCa.Watch("OnTypewriterDone", "/Script/SwtQuest.QuestGiverWidget", "WaCaDialogueLineShown", "post")
WaCa.On("WaCaDialogueLineShown", function(widget)
    local text, node = "", "?"
    pcall(function()
        local rich = widget.mcGuiTextParagraphRich
        if rich and rich:IsValid() then text = rich:GetText():ToString() end
        if text == "" then text = widget.mcGuiTextParagraph:GetText():ToString() end
    end)
    pcall(function()
        local n = widget.CurrentDialogueNode
        if n and n:IsValid() then node = n:GetFullName():gsub("^%S+ ", "") end
    end)
    local plain = text:gsub("<[^>]*>", "")           -- rich text markup out
    local brace = plain:match("{[^}]*}")
    local number = plain:match("%f[%w](%d+)'s%f[%W]")
    if brace or number then
        log("dialogue: UNFILLED %s in \"%s\" (node %s)", brace or (number .. "'s"), plain, node)
    end
end)

-- THE CAT'S SPEED - one owner for every mod's slow-downs: WaCa.Emit("SpeedFactor", source, factor) from any mod (Clan
-- Life's tiredness, Hunting's wading ...); the cat's time dilation is the product of them all (1 / nil = that source
-- off). (MaxWalkSpeed is set by the game every frame for its walk / run / crouch states - an override never held; the
-- cat's own time dilation slows its movement AND its animations.) Re-applied when the gameplay HUD comes back (a new
-- pawn after a level load).
local speedFactors = {}
local function playerPawn()
    for _, c in ipairs(FindAllOf("SwtPlayerController") or {}) do
        if c:IsValid() and c.Player and c.Pawn and c.Pawn:IsValid() then return c.Pawn end
    end
end
local function applySpeed()
    local speed = 1
    for _, f in pairs(speedFactors) do speed = speed * f end
    pcall(function()
        local p = playerPawn()
        if p and p.CustomTimeDilation ~= speed then p.CustomTimeDilation = speed end
    end)
end
WaCa.On("SpeedFactor", function(source, factor)
    if type(source) ~= "string" then return end
    factor = tonumber(factor)
    speedFactors[source] = (factor and factor ~= 1) and factor or nil
    applySpeed()
end)
WaCa.On("ScreenShown", function(screen)
    local ok, name = pcall(function() return screen:GetClass():GetFName():ToString() end)
    if ok and name:find("^WBP_GuiHudGame") then applySpeed() end
end)

-- developer traces (config/dev.ini - absent = off)
-- panels on the HUD for any mod (WaCa.HudPanel - panels.lua)
local Panels = require("panels")
Panels.Init({ WaCa = WaCa, log = log, modDir = modDir })
require("questtab").Init({ WaCa = WaCa, log = log, modDir = modDir, Panels = Panels })
require("questlines").Init({ WaCa = WaCa, log = log, modDir = modDir })
local Dev = require("dev")
Dev.Init({ WaCa = WaCa, log = log, modDir = modDir })

-- the game's text tables are loaded by the time a screen shows with text (the first screens are the splash)
local textApplied = false
WaCa.On("ScreenShown", function()
    if textApplied or not WaCa.IsTextReady() then return end
    textApplied = true
    applyTextOverrides()
    applyPlaceholderFixes()
    Dev.OnGameReady()
end)

-- (published on Started - the game thread; at load the engine hasn't ticked yet and nothing can be delivered)
WaCa.On("Started", function() WaCa.Publish("core.started", { version = VERSION, mods = mods }, true) end)

-- THE PLAYER'S RANK for every mod (topic "player.rank", sticky: { v = 1, rank, name, slot, from }): from the save's
-- rank quests when a character is loaded (WaCa.PlayerRank), and at once when a promotion's rank-up sequence ends
-- (the game's own QuestObjectiveRankUpTask, the new rank handed over) - the game's next save then records it.
local rankNames = {}
local function rankName(v)
    if not next(rankNames) then
        pcall(function()
            StaticFindObject("/Script/SwtQuest.ECharacterRank"):ForEachName(function(n, x) rankNames[x] = (n:ToString():gsub("^.*::", "")) end)
        end)
    end
    return rankNames[v]
end
local function publishRank(rank, from)
    local slot = WaCa.CurrentSlot()
    if not (rank and slot) then return end
    local last = WaCa.Latest("player.rank")
    if last and last.slot == slot and last.rank == rank then return end
    WaCa.Publish("player.rank", { v = 1, rank = rank, name = rankName(rank), slot = slot, from = from }, true)
    log("your cat's rank: %s (%s)", tostring(rankName(rank) or rank), from)
end
local rankSlot
WaCa.On("GameSaved", function()
    local slot = WaCa.CurrentSlot()
    if slot == rankSlot then return end
    rankSlot = slot
    local rank, from = WaCa.PlayerRank()
    publishRank(rank, from)
end)
WaCa.Watch("OnPlayRankUpSequenceFinished", "/Script/SwtQuest.QuestObjectiveRankUpTask", "WaCaRankUp", "post", "Rank")
WaCa.On("WaCaRankUp", function(_, rank)
    local value = tonumber(rank)
    rankName(1)                         -- (the names, for a rank handed over by name)
    if not value then
        for v, n in pairs(rankNames) do if n == tostring(rank):gsub("^.*::", "") then value = v end end
    end
    publishRank(value, "promotion")
end)

WaCa.Ready()
