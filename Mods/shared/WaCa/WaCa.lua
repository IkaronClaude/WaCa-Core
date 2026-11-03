-- WaCa Core - the shared Lua API. Any UE4SS Lua mod can use it:
--     local WaCa = require("WaCa")
--     WaCa.SetText("STR_MAINMENU_PROPHECY_MODE", "Story Mode")     -- replace any game text
--     print(WaCa.GetText("STR_CAT_NAME1_PAW"))                     -- "Woodpaw"
-- Game text lives in one string table ("SwtStrings", from Strings_<LANG>.csv); cat names are entries too
-- (STR_CAT_NAME<n><suffix id>, e.g. STR_CAT_NAME1_PAW). Overrides change what the game shows; saves are untouched.
-- Needs WaCa Core's native part (WaCaCore/dlls/main.dll), which applies the changes on the game thread.
local WaCa = {}

-- TIMERS WAIT WHILE MODS LOAD: UE4SS runs a mod's game-thread callbacks (ExecuteInGameThread / ...WithDelay) while its
-- loader thread is still starting the next mods - both in UE4SS's shared machinery at once, the race the bridge's
-- events already wait out (VM 2026-11-01: two start-up crashes 16 s in, a Lua callback reading 0x10). In every mod's
-- state that loads this library, a callback coming due while mods load (WaCaNative_ModsLoading) waits 0.5 s and asks
-- again. Once per state.
if WaCaNative_ModsLoading and not rawget(_G, "__WaCaTimerGuard") then
    rawset(_G, "__WaCaTimerGuard", true)
    local rawDelay, rawNow = ExecuteInGameThreadWithDelay, ExecuteInGameThread
    local function guarded(fn)
        local function run()
            local ok, loading = pcall(WaCaNative_ModsLoading)
            if ok and loading and rawDelay then return rawDelay(500, run) end
            return fn()
        end
        return run
    end
    if rawDelay then ExecuteInGameThreadWithDelay = function(ms, fn) return rawDelay(ms, guarded(fn)) end end
    if rawNow then ExecuteInGameThread = function(fn) return rawNow(guarded(fn)) end end
end

local TABLE = "SwtStrings"
local stringTables = nil

local function lib()
    if not stringTables or not stringTables:IsValid() then
        stringTables = StaticFindObject("/Script/Engine.Default__KismetStringTableLibrary")
    end
    return stringTables
end

-- the text the game has for a key ("" when there is none / the table isn't loaded yet)
function WaCa.GetText(key)
    local l = lib()
    if not l or not l:IsValid() then return "" end
    return l:GetTableEntrySourceString(FName(TABLE), key):ToString()
end

-- true once the game has loaded its text
function WaCa.IsTextReady()
    return WaCa.GetText("STR_WINDOW_TITLE_BAR") ~= ""
end

-- replace the text of a key ("|" is not allowed in either); applied by the native part at once
function WaCa.SetText(key, text)
    if not WaCa.Command("SetText|" .. key .. "|" .. text) then return false end
    return WaCa.GetText(key) == text
end

-- Cat names: a cat stores a prefix id (PrefixNameId "STR_CAT_NAME12") and a suffix id (SuffixNameId "_PAW"); each
-- combination is one text entry (STR_CAT_NAME12_PAW = "Mosspaw"; all combinations exist).
-- The prefix's own text: what "<prefix>kit" and "<prefix>paw" have in common ("Woodkit", "Woodpaw" -> "Wood").
-- Remembered per prefix the first time, before any entry of it is rewritten.
local prefixTexts = {}
function WaCa.NamePrefixText(prefixId)
    if prefixTexts[prefixId] then return prefixTexts[prefixId] end
    local a, b = WaCa.GetText(prefixId .. "_KIT"), WaCa.GetText(prefixId .. "_PAW")
    local n = 0
    while n < #a and n < #b and a:sub(n + 1, n + 1) == b:sub(n + 1, n + 1) do n = n + 1 end
    if n > 0 then prefixTexts[prefixId] = a:sub(1, n) end
    return a:sub(1, n)
end

-- the suffix's own text for a combination ("STR_CAT_NAME3", "_BLAZE" -> "blaze"), or "" when unknown
function WaCa.NameSuffixText(prefixId, suffixId)
    local prefix = WaCa.NamePrefixText(prefixId)
    local full = WaCa.GetText(prefixId .. suffixId)
    if prefix == "" or full:sub(1, #prefix) ~= prefix then return "" end
    return full:sub(#prefix + 1)
end

-- the FCatCustomData fields a roster / mod may set (everything but the combat stats tag)
WaCa.CAT_DATA_FIELDS = {
    "ClanType", "Role", "BodyOffset", "FaceOffset", "FurOffset", "TailOffset", "EarOffset", "EyeOffset",
    "WhiskerOffset", "BodyValue", "FaceValue", "FurValue", "EyeValue", "WhiskerValue", "TailIdx", "EarIdx", "FurIdx",
    "FurColorIdx", "Pattern01ColorIdx", "Pattern02ColorIdx", "PatternIdx", "MarkingIdx", "EyeLeftColorIdx",
    "EyeRightColorIdx", "bOverrideColors", "Design", "PrefixNameId", "SuffixNameId", "Gender", "CatSize",
    "AccesoryNeckId", "AccesoryPawId", "AccesoryHeadId", "AccesoryBodyId", "AccesoryNeckVar", "AccesoryPawVar",
    "AccesoryHeadVar", "AccesoryBodyVar", "ScarEyeId", "ScarEarId", "ScarBodyId", "ScarChestId",
}

-- send a command to WaCa Core's native part ("Verb|arg|arg..."); false when the native part is missing or the verb
-- is unknown
function WaCa.Command(cmd)
    if not WaCaNative_Command then return false end
    return WaCaNative_Command(cmd)
end

---------------------------------------------------------------------------------------------------------------------
-- Events: the game tells the mod when something happens - nothing polls. The native part turns UE4SS's ProcessEvent
-- callbacks into events (any UFunction by name, Blueprint events included) and adds its own; handlers run on the game
-- thread.
--     WaCa.On("ScreenShown", function(screen) ... end)           a game screen / popup entered the viewport
--     WaCa.Watch("ReceiveBeginPlay", "/Script/Engine.Pawn", "PawnSpawned")   your own event from any function
--     WaCa.Ready()          LAST line of your main.lua: events are delivered from then on
-- Built-in events (watched as soon as someone listens):
--     ScreenShown(screen) / ScreenHidden(screen)    a USwtScreen (screens and popups) built / torn down: the widget's
--                                                   Construct / Destruct (hidden fires while it is still valid)
--     ScreenFocusIn(screen) / ScreenFocusOut(screen) keyboard focus entered / left the screen's widgets (a text box in
--                                                   it took / lost the keyboard: typing, Enter, focus moved away)
--     SteamTextInput(nil, submitted "1"/"0", text)   Steam's full-screen keyboard closed (WaCa.Command SteamKeyboard)
--     KeyDown(nil, keyName)                         a key / mouse button pressed ("Escape", "A", "LeftMouseButton"),
--                                                   as it reaches the game window - before the game routes it, so
--                                                   also while paused or when nothing is bound to it
--     KeyUp(nil, keyName)                           a key let go (keyboard keys)
--     MouseMove(nil, x, y)                          the mouse moved (window pixels; at most every 40 ms)
--     GameSaved(nil, context)                       right after every game save (the save manager's native save,
--                                                   whoever called it) - write per-save data (WaCa.SlotFile) here
--     Started(nil)                                  the first event after WaCa.Ready(), on the game thread; the
--                                                   screens already up arrive as ScreenShown just before it
-- Don't run Lua off the game thread in a mod that uses events (LoopAsync / ExecuteAsync): two OS threads in one Lua
-- state corrupt it.
--     CatSpawned(actor)                             a cat actor began play: the player's / an NPC's pawn, a quest
--                                                   actor, a cutscene cat
local BUILTIN = {
    ScreenShown = { { "Construct", "/Script/SwtEngine.SwtScreen", "post" } },
    ScreenHidden = { { "Destruct", "/Script/SwtEngine.SwtScreen", "pre" } },
    ScreenFocusIn = { { "OnAddedToFocusPath", "/Script/SwtEngine.SwtScreen", "post" } },
    ScreenFocusOut = { { "OnRemovedFromFocusPath", "/Script/SwtEngine.SwtScreen", "post" } },
    -- the game's sequencer (cutscenes, cinematic dialogues): handlers get (manager, id) - the sequence's / dialogue's
    -- gameplay tag as text. Story mods hook in here: WaCa.On("SequenceFinished", function(_, id) ... end)
    SequenceStarted = { { "SequencerStarted", "/Script/SwtEngine.SwtSequencerManager", "post", "sequenceId" } },
    SequenceFinished = { { "SequencerFinished", "/Script/SwtEngine.SwtSequencerManager", "post", "sequenceId" } },
    SequenceEnded = { { "OnSequenceFinished", "/Script/SwtEngine.SwtSequencerManager", "post" } },
    DialogueStarted = { { "PlaySequenceDialogue", "/Script/SwtQuest.SwtQuestSequencerManager", "pre", "SequenceDialogueId" } },
    DialogueNext = { { "SwtSequencerDialogueNext", "/Script/SwtQuest.SwtQuestSequencerManager", "post", "bIsStart" } },
    CatSpawned = {
        { "ReceiveBeginPlay", "/Script/SwtAdventure.SwtAdventureCharacter", "post" },
        { "ReceiveBeginPlay", "/Script/SwtAdventure.SwtAdventureQuestActor", "post" },
        { "ReceiveBeginPlay", "/Script/Wrc.WrcCatSkeletalMeshWithCustomization", "post" },
    },
}
local handlers = {}           -- event -> { fn, ... }
local watched = {}            -- "function|class|event" -> true once its native watch exists

local function dispatch(event, ...)
    local list = handlers[event]
    if not list then return end
    for i = 1, #list do
        local ok, err = pcall(list[i], ...)
        if not ok then print(string.format("[WaCa] %s handler: %s\n", event, tostring(err))) end
    end
end

-- called by the native part for every event this mod's state watches
local received                        -- a cross-mod event's handling (below)
local BROADCAST_MARK = "\1J"           -- a cross-mod payload's first bytes

function WaCaNative_OnEvent(event, object, ...)
    local first = ...
    if object == nil and type(first) == "string" and first:sub(1, #BROADCAST_MARK) == BROADCAST_MARK then
        return received(event, first, select(2, ...))
    end
    if event == "Started" then
        -- the first event after WaCa.Ready(): the screens already up were shown before this mod listened - replay them
        if handlers["ScreenShown"] then
            for _, s in ipairs(FindAllOf("SwtScreen") or {}) do
                if s:IsValid() and s:IsInViewport() then dispatch("ScreenShown", s) end
            end
        end
    end
    dispatch(event, object, ...)
end

-- watch a UFunction (by name, on objects of a class) as an event; when = "pre" (before it runs) or "post" (default)
-- param: a parameter of the function, handed to the handlers after the object - an object as itself (e.g. an anim
-- notify's "MeshComp" - the notify is one object shared by every cat playing the animation), anything simple as text
-- (a name, a gameplay tag's name, a string, a number, "true" / "false", an enum's number)
function WaCa.Watch(functionName, classPath, event, when, param)
    local id = functionName .. "|" .. classPath .. "|" .. event
    if watched[id] then return true end
    if not WaCaNative_Watch then return false end
    if param then WaCaNative_Watch(functionName, classPath, event, when or "post", param)
    else WaCaNative_Watch(functionName, classPath, event, when or "post") end
    watched[id] = true
    return true
end

-- listen to an event (built-in, watched, or raised with WaCa.Emit)
function WaCa.On(event, fn)
    handlers[event] = handlers[event] or {}
    table.insert(handlers[event], fn)
    for _, w in ipairs(BUILTIN[event] or {}) do WaCa.Watch(w[1], w[2], event, w[3], w[4]) end
end

-- the cat look component of an actor (pawn, quest actor, cutscene cat, creator preview), or nil
local meshCompClass = nil
function WaCa.CatComponent(actor)
    if not actor or not actor:IsValid() then return nil end
    meshCompClass = meshCompClass or StaticFindObject("/Script/SwtAdventure.SwtAdventureCustomizableMeshComponent")
    local ok, comp = pcall(function() return actor:GetComponentByClass(meshCompClass) end)
    if ok and comp and comp:IsValid() then return comp end
    -- (a cutscene stand-in - WrcCatSkeletalMeshWithCustomization - idle outside its scene: its look component isn't
    -- found by class, its MeshCustomizationComponent field holds it: VM 2026-10-31)
    ok, comp = pcall(function() return actor.MeshCustomizationComponent end)
    if ok and comp and comp:IsValid() then return comp end
    return nil
end

-- raise an event from Lua (to this mod's listeners, at once)
-- --- JSON (cross-mod payloads): numbers, strings, booleans, nil, plain tables (arrays and string-keyed) ---

local function jsonString(s)
    return '"' .. s:gsub('[%c"\\]', function(c)
        local map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
        return map[c] or string.format("\\u%04x", c:byte())
    end) .. '"'
end

-- encode a value; returns nil for anything that can't cross (a UObject, a function, a cycle)
function WaCa.JsonEncode(v, seen)
    local t = type(v)
    if v == nil then return "null" end
    if t == "boolean" then return v and "true" or "false" end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        return (math.type and math.type(v) == "integer") and tostring(v) or string.format("%.17g", v)
    end
    if t == "string" then return jsonString(v) end
    if t ~= "table" or getmetatable(v) ~= nil then return nil end
    seen = seen or {}
    if seen[v] then return nil end
    seen[v] = true
    local n = #v
    local isArray = n > 0 or next(v) == nil
    if isArray then for k in pairs(v) do if type(k) ~= "number" then isArray = false break end end end
    local parts = {}
    if isArray then
        for i = 1, n do
            local e = WaCa.JsonEncode(v[i], seen)
            if not e then return nil end
            parts[i] = e
        end
        seen[v] = nil
        return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, val in pairs(v) do
        if k ~= "__order" then
            local e = WaCa.JsonEncode(val, seen)
            if not e then return nil end
            table.insert(parts, jsonString(tostring(k)) .. ":" .. e)
        end
    end
    seen[v] = nil
    return "{" .. table.concat(parts, ",") .. "}"
end

function WaCa.JsonDecode(s)
    local i = 1
    local function ws() i = s:find("[^ \t\r\n]", i) or #s + 1 end
    local value
    local function str()
        local out, j = {}, i + 1
        while true do
            local c = s:sub(j, j)
            if c == "" then error("unterminated string") end
            if c == '"' then i = j + 1 break end
            if c == "\\" then
                local e = s:sub(j + 1, j + 1)
                local map = { n = "\n", r = "\r", t = "\t", b = "\b", f = "\f", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
                if e == "u" then
                    local code = tonumber(s:sub(j + 2, j + 5), 16)
                    table.insert(out, utf8 and utf8.char(code) or string.char(code % 256))
                    j = j + 6
                else
                    table.insert(out, map[e] or e)
                    j = j + 2
                end
            else
                table.insert(out, c)
                j = j + 1
            end
        end
        return table.concat(out)
    end
    value = function()
        ws()
        local c = s:sub(i, i)
        if c == "{" then
            i = i + 1
            local obj = {}
            ws()
            if s:sub(i, i) == "}" then i = i + 1 return obj end
            while true do
                ws()
                local k = str()
                ws()
                i = i + 1                                  -- ':'
                obj[k] = value()
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == "}" then return obj end
            end
        elseif c == "[" then
            i = i + 1
            local arr, n = {}, 0
            ws()
            if s:sub(i, i) == "]" then i = i + 1 return arr end
            while true do
                n = n + 1
                arr[n] = value()
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == "]" then return arr end
            end
        elseif c == '"' then
            return str()
        elseif s:sub(i, i + 3) == "true" then i = i + 4 return true
        elseif s:sub(i, i + 4) == "false" then i = i + 5 return false
        elseif s:sub(i, i + 3) == "null" then i = i + 4 return nil
        else
            local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
            i = i + #num
            return tonumber(num)
        end
    end
    return value()
end

-- --- events: local and ACROSS MODS ---
-- WaCa.Emit(event, ...) dispatches in this mod and - when its arguments are plain data - to every other mod using WaCa
-- (the parts of WaCa are separate UE4SS mods; the native bridge hands the event over as JSON). Handlers can tell where
-- an event came from by WaCa.EventFrom (nil = this mod). Received events are never sent on (no echo).
-- WaCa.Publish(topic, data) / WaCa.Subscribe(topic, fn(data, fromMod)): the same for documented topics with ONE table
-- payload ("<mod>.<thing>" names, e.g. "clan.thirdStarted" { day = 3, third = "Morning" } - see the Core README).
WaCa.EventFrom = nil

-- sends an event to the other mods; sticky = the native side keeps it for mods that subscribe later
local function broadcast(event, sticky, ...)
    if not WaCaNative_Broadcast then return end
    -- (the arguments one by one: a nil in the middle must stay a null, not end the list)
    local n = select("#", ...)
    local parts = {}
    for i = 1, n do
        local ok, e = pcall(WaCa.JsonEncode, (select(i, ...)))
        if not ok or not e then return end          -- something that can't cross (a UObject): this mod only
        parts[i] = e
    end
    WaCaNative_Broadcast(event, BROADCAST_MARK .. '{"n":' .. n .. ',"a":[' .. table.concat(parts, ",") .. "]}", sticky)
end

-- the latest value of each sticky topic published IN this mod (a later subscriber here gets it too)
local stickyHere = {}

function WaCa.Emit(event, ...)
    dispatch(event, ...)
    broadcast(event, false, ...)
end

-- Publish(topic, data [, sticky]): sticky = a STATE rather than a moment ("clan.third" = the third it is now): a mod
-- that subscribes later - or starts after it was sent - still gets the latest one at once.
function WaCa.Publish(topic, data, sticky)
    if sticky then stickyHere[topic] = data end
    dispatch(topic, data)
    broadcast(topic, sticky == true, data)
end

-- the latest value of a sticky topic (this mod's own, else another mod's), or nil
function WaCa.Latest(topic)
    if stickyHere[topic] ~= nil then return stickyHere[topic] end
    if not WaCaNative_LastBroadcast then return nil end
    local payload = WaCaNative_LastBroadcast(topic)
    if not payload then return nil end
    local ok, msg = pcall(WaCa.JsonDecode, payload:sub(#BROADCAST_MARK + 1))
    return ok and type(msg) == "table" and msg.a and msg.a[1] or nil
end

function WaCa.Subscribe(topic, fn)
    WaCa.On(topic, function(data) fn(data, WaCa.EventFrom) end)
    -- the latest sticky value: this mod's own, else another mod's (kept by the native side)
    if stickyHere[topic] ~= nil then
        fn(stickyHere[topic], nil)
    elseif WaCaNative_LastBroadcast then
        local payload, from = WaCaNative_LastBroadcast(topic)
        if payload then
            local ok, msg = pcall(WaCa.JsonDecode, payload:sub(#BROADCAST_MARK + 1))
            if ok and type(msg) == "table" and msg.a then fn(msg.a[1], from) end
        end
    end
end

-- a received cross-mod event (from WaCaNative_OnEvent): decoded and dispatched here only
received = function(event, payload, from)
    local ok, msg = pcall(WaCa.JsonDecode, payload:sub(#BROADCAST_MARK + 1))
    if not ok or type(msg) ~= "table" then return end
    local a = msg.a or {}
    WaCa.EventFrom = from
    local ok2, err = pcall(dispatch, event, table.unpack(a, 1, msg.n or #a))
    WaCa.EventFrom = nil
    if not ok2 then print(string.format("[WaCa] cross-mod event %s: %s\n", event, tostring(err))) end
end

-- A MOD'S OWN KEYS - one key file for every mod (operator 2026-10-25: keybinds.ini is "the one source"):
-- WaCa.ModKey(mod, name, default, label) -> the key. WaCa Keybinds & Mouse's keybinds.ini [ModKeys]
-- "<mod>.<name> = Key" wins over the mod's own default (its own ini's setting); without that mod, the default. Every
-- mod key is announced ("keys.mod" { mod, name, key, label }) and announced again when asked ("keys.ask") - the
-- controls screen names a clash ("F is also Clan Life's report key") whatever the load order.
local modKeys = {}
local MODS_DIR = (debug.getinfo(1, "S").source:match("^@(.+)[/\\]shared[/\\]WaCa[/\\]WaCa%.lua$"))
function WaCa.ModKey(mod, name, default, label)
    local key = default
    local ini = MODS_DIR and WaCa.ReadIni(MODS_DIR .. "/WaCaKeybinds/config/keybinds.ini")
    local v = ini and ini["ModKeys"] and ini["ModKeys"][mod .. "." .. name]
    if v and v ~= "" then key = v end
    local entry = { mod = mod, name = name, key = key, label = label or name }
    if #modKeys == 0 then
        WaCa.On("keys.ask", function()
            for _, e in ipairs(modKeys) do WaCa.Publish("keys.mod", e) end
        end)
    end
    for i, e in ipairs(modKeys) do
        if e.mod == mod and e.name == name then table.remove(modKeys, i) break end
    end
    modKeys[#modKeys + 1] = entry
    WaCa.Publish("keys.mod", entry)
    return key
end

-- THE FIGHT'S PARTY (tickets.md "The party = who you're out with"): WaCa.SetCombatParty({ 29, 43 }) - or role names,
-- { "TheRegretfulWarrior" } - and the next fights put those story cats beside your cat instead of the game's own
-- party (WaCa Core's native hook on the fight's party list; the game builds the cats its own way: looks, stats, moves).
-- nil / {} = the game's party again. Only story cats (narrative roles), as many as the game's party has room for (two).
-- -> true when it took (false: WaCa Core too old, or the fight's party list wasn't found)
-- WaCa.SetCombatParty(roles, true) sets this mod's BASE party (a story pack start's): nil then goes back to it instead
-- of the game's.
local roleValues = nil
local baseParty = nil
function WaCa.SetCombatParty(roles, base)
    if base then baseParty = roles and #roles > 0 and roles or nil end
    if roles == nil or #roles == 0 then roles = baseParty end
    local values = {}
    for _, r in ipairs(roles or {}) do
        local v = tonumber(r)
        if not v and type(r) == "string" then
            if not roleValues then
                roleValues = {}
                pcall(function()
                    StaticFindObject("/Script/SwtQuest.ENarrativeRole"):ForEachName(function(n, x)
                        roleValues[(n:ToString():gsub("^.*::", ""))] = x
                    end)
                end)
            end
            v = roleValues[r]
        end
        if v and v > 0 then values[#values + 1] = tostring(math.floor(v)) end
    end
    return WaCa.Command("Party|" .. table.concat(values, ",")) == true
end

-- the mod's main.lua is done: events may arrive from now on (they are held back while it runs on UE4SS's loader thread)
function WaCa.Ready()
    if WaCaNative_Ready then WaCaNative_Ready() end
end

-- the menus' keyboard keys + hint icons: rows of UXD/Buttons/DT_GamepadButtonsKeyboard, keyed by command name
-- (Button_A = Enter, Button_B / Button_Back = BackSpace, Button_X = E, ...). Rebinds a row; the icon is the game's
-- own for that key (T_UI_Controller_Pc_<Key>_D) unless one is given, and kept as is when there's none.
WaCa.UI_KEYS_TABLE = "/Game/Gameplay/DataTables/UXD/Buttons/DT_GamepadButtonsKeyboard.DT_GamepadButtonsKeyboard"

-- A KEY'S ICON, THE GAME'S WAY (2026-11-03): the game's own key -> icon table, DT_Keys (UIButtonsMap rows: Key, Image),
-- read once - the icons aren't all named after their keys (Up -> Arrow_Up, LeftShift -> Shift, SpaceBar -> Space,
-- One -> 1, PageUp -> Prior); a guess from the key name missed those. Its soft Image column comes out as text through
-- the engine's GetDataTableColumnAsString (a soft pointer can't be read from Lua). The texture's path, or nil.
WaCa.KEY_ICONS_TABLE = "/Game/Gameplay/DataTables/UXD/DT_Keys.DT_Keys"
local keyIcons
function WaCa.KeyIcon(key)
    if not keyIcons then
        keyIcons = {}
        pcall(function()
            local t = WaCa.LoadAsset(WaCa.KEY_ICONS_TABLE)
            local lib = StaticFindObject("/Script/Engine.Default__DataTableFunctionLibrary")
            local keys = lib:GetDataTableColumnAsString(t, FName("Key"))
            local images = lib:GetDataTableColumnAsString(t, FName("Image"))
            for i = 1, #keys do
                local k, img = keys[i]:get():ToString(), images[i] and images[i]:get():ToString() or ""
                if k ~= "" and img ~= "" and img ~= "None" then keyIcons[k] = img end
            end
        end)
    end
    return keyIcons[tostring(key)]
end
function WaCa.SetUIKey(row, key, icon)
    local tbl = StaticFindObject(WaCa.UI_KEYS_TABLE)
    if not tbl or not tbl:IsValid() then
        local ok, loaded = pcall(LoadAsset, WaCa.UI_KEYS_TABLE)
        tbl = ok and loaded or nil
    end
    if not tbl or not tbl:IsValid() then return false end
    if not icon then
        -- (the game's own icon for the key - DT_Keys; else the one named after it)
        local name = string.format("T_UI_Controller_Pc_%s_D", key)
        local path = WaCa.KeyIcon(key) or string.format("/Game/Platforms/Windows/UXD/Textures/Controllers/Gameplay/%s.%s", name, name)
        local ok, tex = pcall(LoadAsset, path)
        icon = ok and tex and tex:IsValid() and path or ""
    end
    return WaCa.Command(string.format("SetUIKey|%X|%s|%s|%s", tbl:GetAddress(), row, key, icon))
end

-- every menu-key table: the keyboard one and one per gamepad type (USwtGuiManager.GamepadButtonsMap)
function WaCa.MenuKeyTables()
    local out, seen = {}, {}
    local function add(t)
        if t and t:IsValid() and not seen[t:GetAddress()] then
            seen[t:GetAddress()] = true
            table.insert(out, t)
        end
    end
    add(StaticFindObject(WaCa.UI_KEYS_TABLE))
    local gm = FindFirstOf("SwtGuiManager")
    local map = gm and gm:IsValid() and gm.GamepadButtonsMap
    if map and map:IsValid() then
        pcall(function()
            map.DataGamepadButtonsMap:ForEach(function(_, v) add(v:get()) end)
        end)
    end
    return out
end

-- every row name of a menu-key table ("Button_A", "Button_Skip", ...)
function WaCa.UIRows(tbl)
    local rows = {}
    pcall(function()
        tbl:ForEachRow(function(name) table.insert(rows, tostring(name)) end)
    end)
    return rows
end

-- a menu row's key name in a table ("Enter", "Gamepad_FaceButton_Bottom"), or nil
function WaCa.UIRowKey(tbl, row)
    local ok, key = pcall(function() return tbl:FindRow(row).Key.KeyName:ToString() end)
    return ok and key or nil
end

-- a menu row's key in a given table, its icon untouched ("None" = no key)
function WaCa.SetUIKeyIn(tbl, row, key)
    return WaCa.Command(string.format("SetUIKey|%X|%s|%s|", tbl:GetAddress(), row, key))
end

-- only the hint icon of a menu row (its key stays): e.g. SetUIIcon("Button_B", "Escape") shows Esc for "back" while
-- BackSpace still works (pair it with a key alias Escape -> BackSpace so Esc works too)
function WaCa.SetUIIcon(row, iconKey)
    local name = string.format("T_UI_Controller_Pc_%s_D", iconKey)
    local path = string.format("/Game/Platforms/Windows/UXD/Textures/Controllers/Gameplay/%s.%s", name, name)
    local ok, tex = pcall(LoadAsset, path)
    if not (ok and tex and tex:IsValid()) then return false end
    local tbl = StaticFindObject(WaCa.UI_KEYS_TABLE)
    if not tbl or not tbl:IsValid() then
        local ok2, loaded = pcall(LoadAsset, WaCa.UI_KEYS_TABLE)
        tbl = ok2 and loaded or nil
    end
    if not tbl or not tbl:IsValid() then return false end
    return WaCa.Command(string.format("SetUIKey|%X|%s||%s", tbl:GetAddress(), row, path))
end

-- THE OBJECT IF IT IS LIVE, ELSE NIL (2026-11-02 - the P3 crash UE4SS+0x2aed57, "reading 0x10", 19 reports): where
-- there is no object, UE4SS hands back a wrapper of NULL - not nil - from GetOwner / GetParent / FindFirstOf /
-- StaticFindObject / an object field; ANY call on it (GetClass, GetFullName, a field) crashes the game natively, and
-- pcall can't catch that. IsValid itself is safe. Use: local o = WaCa.Live(comp:GetOwner()); if o then ... end
function WaCa.Live(o)
    if o ~= nil and type(o) == "userdata" and o:IsValid() then return o end
    return nil
end

-- a simple ini reader: { [section] = { key = value } }, keys in file order in section.__order; the second result is
-- the section names in file order (2026-11-02: a list of [Role.*] answers had no order to go by)
function WaCa.ReadIni(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local out, section, sections = {}, "", {}
    out[section] = { __order = {} }
    for line in f:lines() do
        line = line:gsub("^%s+", ""):gsub("%s+$", "")
        if line ~= "" and not line:match("^[;#]") then
            local s = line:match("^%[(.+)%]$")
            if s then
                section = s
                if not out[section] then sections[#sections + 1] = s end
                out[section] = out[section] or { __order = {} }
            else
                local k, v = line:match("^([^=]+)=(.*)$")
                if k then
                    k = k:gsub("%s+$", ""); v = v:gsub("^%s+", "")
                    out[section][k] = v
                    table.insert(out[section].__order, k)
                end
            end
        end
    end
    f:close()
    return out, sections
end

-- the enabled UE4SS mods, in load order: Mods/mods.txt's "<Mod> : 1" lines first, then every other folder with an
-- enabled.txt (sorted). modsDir = the Mods folder.
function WaCa.EnabledMods(modsDir)
    local out, seen = {}, {}
    local txt = io.open(modsDir .. "/mods.txt", "r")
    if txt then
        for line in txt:lines() do
            local name, on = line:match("^%s*([^:;%s][^:]-)%s*:%s*(%d)")
            if name and on == "1" and not seen[name] then
                seen[name] = true
                table.insert(out, name)
            end
        end
        txt:close()
    end
    local extra = {}
    pcall(function()
        for name, dir in pairs(IterateGameDirectories().Game.Binaries.Win64.ue4ss.Mods) do
            if type(dir) == "table" and not tostring(name):match("^__") and not seen[name] then
                local f = io.open(modsDir .. "/" .. name .. "/enabled.txt", "r")
                if f then
                    f:close()
                    table.insert(extra, name)
                end
            end
        end
    end)
    table.sort(extra)
    for _, name in ipairs(extra) do table.insert(out, name) end
    return out
end

-- CONTENT FROM EVERY MOD (operator 2026-10-28: a modding framework - overlapping mods must not have to edit the same
-- file): WaCa.ReadContent(basePath, name) reads basePath (e.g. WaCaClanLife/config/clanevents.ini), then every enabled
-- mod's own <Mod>/content/<name>.ini on top, and returns one ini table:
--   - a section only one file has: added as it is (two mods' [Event.Rogues] / [Event.Moonhigh] simply both exist);
--   - a section several files have: NUMBERED keys are ADDED after the ones already there (Step1 / Line1 / Morning1 ...
--     -> renumbered: two mods can both add beats to [Day.10] or lines to the same [Dialogue.X]);
--   - EXPLICIT OVERRIDES, NOT LOAD ORDER (operator 2026-10-28: "load order as core design choice?" - no): a mod
--     replaces another's section only by saying so - its own section with Replaces = <Mod>/<Section> takes the
--     place of that one, whole; Off = 1 turns another's section off (the readers honour it). Two mods setting the same
--     PLAIN key (On, Days, Chance ...) differently without that is a CONFLICT - logged with both mods and values
--     (WaCa.ContentConflicts) and shown once in the game - and only then does load order break the tie.
-- section.__mods lists the mods that touched it.
WaCa.ContentConflicts = WaCa.ContentConflicts or {}
local conflictShown = false
local function noteConflict(text)
    table.insert(WaCa.ContentConflicts, text)
    print("[WaCa] CONFLICT " .. text .. "\n")
    if conflictShown then return end
    conflictShown = true
    WaCa.On("ScreenShown", function(screen)
        local ok, hud = pcall(function() return screen:GetClass():GetFName():ToString():find("^WBP_GuiHudGame") ~= nil end)
        if not (ok and hud) or conflictShown == "told" then return end
        conflictShown = "told"
        ExecuteInGameThreadWithDelay(5000, function()
            WaCa.Emit("ShowLine", string.format("WaCa: %d setting(s) two mods disagree on - see UE4SS.log (CONFLICT)",
                #WaCa.ContentConflicts))
        end)
    end)
end

-- (out.__order = the section names in order: the mod's own file's, then each pack's new ones in load order)
function WaCa.ReadContent(basePath, name)
    local out, sections = WaCa.ReadIni(basePath)
    out = out or {}
    out.__order = sections or {}
    local modsDir, own = basePath:match("^(.*)[/\\]([^/\\]+)[/\\]config[/\\][^/\\]+$")
    local function owned(t, mod)
        t.__mods = { mod }
        t.__owner = {}
        for _, k in ipairs(t.__order or {}) do t.__owner[k] = mod end
    end
    for s, t in pairs(out) do if s ~= "__order" and type(t) == "table" then owned(t, own or "?") end end
    if not modsDir then return out end
    for _, mod in ipairs(WaCa.EnabledMods(modsDir)) do
        local add, addSections = nil, {}
        if mod ~= own then add, addSections = WaCa.ReadIni(modsDir .. "/" .. mod .. "/content/" .. name .. ".ini") end
        for _, section in ipairs(addSections or {}) do
            local t = add[section]
            if section ~= "" and section ~= "__order" and type(t) == "table" then
                -- (Replaces = <Mod>/<Section>: that mod's section goes, this one takes its place - whole)
                local rMod, rSection = tostring(t.Replaces or ""):match("^%s*([^/]+)/(.-)%s*$")
                if rMod then
                    local target = out[rSection]
                    local hasMod = false
                    for _, m in ipairs(target and target.__mods or {}) do if m == rMod then hasMod = true end end
                    if hasMod then
                        out[rSection] = nil
                        print(string.format("[WaCa] %s: %s's [%s] replaces %s's [%s]\n", name, mod, section, rMod, rSection))
                    else
                        print(string.format("[WaCa] %s: %s's [%s] says Replaces = %s/%s - %s has no such section here\n",
                            name, mod, section, rMod, rSection, rMod))
                    end
                end
                local into = out[section]
                if not into then
                    owned(t, mod)
                    out[section] = t
                    table.insert(out.__order, section)
                else
                    table.insert(into.__mods, mod)
                    into.__owner = into.__owner or {}
                    for _, k in ipairs(t.__order or {}) do
                        local prefix = k:match("^(%a+)%d+$")
                        if prefix then
                            local i = 1
                            while into[prefix .. i] ~= nil do i = i + 1 end
                            into[prefix .. i] = t[k]
                            into.__owner[prefix .. i] = mod
                            table.insert(into.__order, prefix .. i)
                        else
                            if into[k] ~= nil and into[k] ~= t[k] and k ~= "Off" and k ~= "Replaces" then
                                noteConflict(string.format("%s: [%s] %s - %s says %s, %s says %s (no Replaces = %s/%s; %s wins by load order)",
                                    name, section, k, into.__owner[k] or "?", tostring(into[k]), mod, tostring(t[k]),
                                    into.__owner[k] or "?", section, mod))
                            elseif into[k] == nil then
                                table.insert(into.__order, k)
                            end
                            into[k] = t[k]
                            into.__owner[k] = mod
                        end
                    end
                end
            end
        end
        if add then print(string.format("[WaCa] %s: content from %s\n", name, mod)) end
    end
    return out
end

-- write { [section] = { key = value, __order = {...} } } as an ini (sections and keys in their __order when present)
function WaCa.WriteIni(path, data, header)
    local f = io.open(path, "w")
    if not f then return false end
    if header then f:write(header, "\n\n") end
    local sections = data.__order or {}
    if #sections == 0 then for k in pairs(data) do if k ~= "__order" then table.insert(sections, k) end end end
    for _, section in ipairs(sections) do
        local t = data[section]
        if t then
            f:write("[", section, "]\n")
            local keys = t.__order or {}
            if #keys == 0 then for k in pairs(t) do if k ~= "__order" then table.insert(keys, k) end end end
            for _, k in ipairs(keys) do
                if t[k] ~= nil then f:write(k, " = ", tostring(t[k]), "\n") end
            end
            f:write("\n")
        end
    end
    f:close()
    return true
end

---------------------------------------------------------------------------------------------------------------------
-- Per-save data. The game keeps every save slot in Saved/SaveGames/WrcGame.sav (a slot = a GUID, the active one
-- USwtSaveManager.SaveGame.saveSlots[CurrentSaveSlotIndex]). Mods keep their own data per slot in a sidecar file
-- next to it - WrcGame.<save>-<character slot>.<ModName>.ini - so the game's save stays exactly as the game wrote it.

-- the active save slot's id, or nil (main menu before a save is chosen, a new game before its first save)
-- The active character's save slot: "<save GUID>-<character slot>". The game keeps ONE save (saveSlots holds its
-- GUID, the same for every character on the account) with up to MaxSlots characters in it; CurrentSaveSlotIndex is the
-- character. (Before 0.5.4 the index was taken as an index into saveSlots - only character slot 0 ever had a sidecar.)
-- the game's save file as Lua tables, read once and again after each save (GameSaved), or nil
local saveCache, saveStale = nil, true
local function saveData()
    if saveStale then
        saveCache = WaCa.ReadJson(WaCa.SaveDir() .. "/WrcGame.json")
        saveStale = false
    end
    return saveCache
end
WaCa.On("GameSaved", function() saveStale = true end)

-- WaCa.Now() -> real seconds (the engine's real time, fractional; a menu or a pause doesn't stop it). NOT os.clock():
-- that is the process's CPU time, which the game's many threads run 2-9x faster than the wall (VM 2026-10-30: a party
-- cat's "0.05 s" step read 0.4 s - 8x too far).
local nowStatics, nowCtx
function WaCa.Now()
    nowStatics = nowStatics or StaticFindObject("/Script/Engine.Default__GameplayStatics")
    local ok, t = pcall(function()
        if not (nowCtx and nowCtx:IsValid()) then
            nowCtx = nil
            for _, c in ipairs(FindAllOf("PlayerController") or {}) do
                if c:IsValid() then nowCtx = c break end
            end
        end
        return nowCtx and nowStatics:GetRealTimeSeconds(nowCtx)
    end)
    if ok and type(t) == "number" then return t end
    return os.time()
end

-- the active character: "<save id>-<cat slot>". The cat slot is the save's own Shared.General.CurrentCatSlot (the
-- character picked in "Select Your Cat"); the save game's CurrentSaveSlotIndex stayed 0 for every cat (VM 2026-10-28:
-- a new cat in slot 8 read slot 0's cat - its day, its names, its rank)
function WaCa.CurrentSlot()
    local sm = FindFirstOf("SwtSaveManager")
    if not sm or not sm:IsValid() then return nil end
    local sg = sm.SaveGame
    if not sg or not sg:IsValid() then return nil end
    local guid = #sg.saveSlots > 0 and sg.saveSlots[1]:ToString() or ""
    if guid == "" then guid = sg.GamerId:ToString() end
    if guid == "" then return nil end
    local idx
    local data = saveData()
    pcall(function() idx = tonumber(data[guid].Shared.General.CurrentCatSlot) end)
    if not idx then
        idx = sg.CurrentSaveSlotIndex
        if not idx or idx < 0 then return nil end
    end
    return string.format("%s-%d", guid, idx)
end

-- the game's save folder (%LOCALAPPDATA%/Wrc/Saved/SaveGames)
function WaCa.SaveDir()
    return (os.getenv("LOCALAPPDATA") or ".") .. "/Wrc/Saved/SaveGames"
end

-- a JSON file as Lua tables (objects -> tables by key, arrays -> 1-based tables, null -> nil), or nil
function WaCa.ReadJson(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    local pos = 1
    local value
    local function skip() pos = s:find("[^ \t\r\n]", pos) or #s + 1 end
    local escapes = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
    local function str()
        local out, i = {}, pos + 1
        while true do
            local c = s:sub(i, i)
            if c == '"' then break end
            if c == "\\" then
                local e = s:sub(i + 1, i + 1)
                if e == "u" then
                    local code = tonumber(s:sub(i + 2, i + 5), 16) or 63
                    table.insert(out, code < 128 and string.char(code) or utf8.char(code))
                    i = i + 6
                else
                    table.insert(out, escapes[e] or e)
                    i = i + 2
                end
            elseif c == "" then
                error("unterminated string")
            else
                local j = s:find('["\\]', i) or #s + 1
                table.insert(out, s:sub(i, j - 1))
                i = j
            end
        end
        pos = i + 1
        return table.concat(out)
    end
    value = function()
        skip()
        local c = s:sub(pos, pos)
        if c == "{" then
            local t = {}
            pos = pos + 1
            skip()
            if s:sub(pos, pos) == "}" then pos = pos + 1 return t end
            while true do
                skip()
                local k = str()
                skip()
                pos = pos + 1                                  -- ':'
                t[k] = value()
                skip()
                local d = s:sub(pos, pos)
                pos = pos + 1
                if d == "}" then return t end
            end
        elseif c == "[" then
            local t = {}
            pos = pos + 1
            skip()
            if s:sub(pos, pos) == "]" then pos = pos + 1 return t end
            while true do
                table.insert(t, value())
                skip()
                local d = s:sub(pos, pos)
                pos = pos + 1
                if d == "]" then return t end
            end
        elseif c == '"' then
            return str()
        elseif s:sub(pos, pos + 3) == "true" then pos = pos + 4 return true
        elseif s:sub(pos, pos + 4) == "false" then pos = pos + 5 return false
        elseif s:sub(pos, pos + 3) == "null" then pos = pos + 4 return nil
        end
        local num = s:match("^-?[%d.]+[eE]?[-+]?%d*", pos)
        if not num or num == "" then error("bad JSON at " .. pos) end
        pos = pos + #num
        return tonumber(num)
    end
    local ok, result = pcall(value)
    return ok and result or nil
end

-- THE PLAYER'S RANK as an ECharacterRank value (1 Apprentice, 2 Warrior, 3 Deputy, 4 Leader) and where it came from,
-- or nil. The game's own record of the promotions: its rank quests (QuestProgression Quest.Rank.N - the quest you do as
-- an apprentice, as a warrior, as the deputy; each one done is a promotion), so the rank is 1 + the rank quests done
-- (Status 4) - read from the game's live data for the character being played (WaCa.SaveCharacter), which works for a
-- save made before the mods too. A promotion during play (the rank-up sequence's end, OnPlayRankUpSequenceFinished -
-- WaCa Core publishes it as the sticky topic "player.rank" { rank, slot }) counts at once. (The save FILE gave a new
-- apprentice another cat's deputy rank: operator 2026-10-28.)
local QUEST_DONE = 4
function WaCa.PlayerRank()
    local slot = WaCa.CurrentSlot()
    local live = WaCa.Latest("player.rank")
    if live and live.slot == slot and tonumber(live.rank) and live.from == "promotion" then return tonumber(live.rank), "promotion" end
    local character = WaCa.SaveCharacter()
    local quests = character and character.QuestProgression
    if type(quests) ~= "table" then return nil, "unknown" end
    local done = 0
    for _, chapter in pairs(quests) do
        if type(chapter) == "table" then
            for key, q in pairs(chapter) do
                if type(key) == "string" and key:match("^Quest%.Rank%.%d+$") and type(q) == "table" and q.Status == QUEST_DONE then
                    done = done + 1
                end
            end
        end
    end
    return math.min(1 + done, 4), "rank quests"
end

-- the player's clan ("RiverClan" ...): the cat in the world's own (its CustomData.ClanType), else the save's, or nil
function WaCa.PlayerClan()
    local comp = WaCa.PlayerCats()[1]
    local value
    pcall(function() value = comp and comp.CustomData.ClanType end)
    if type(value) == "number" and value > 0 then
        local name
        pcall(function()
            StaticFindObject("/Script/SwtQuest.EClanType"):ForEachName(function(n, v)
                if v == value then name = (n:ToString():gsub("^.*::", "")) end
            end)
        end)
        if name then return name end
    end
    local p = WaCa.GameProgress()
    local me = p and p["Gameplay.Combat.Character.Cat.Player"]
    return me and type(me.Customization) == "string" and me.Customization:match("ClanType=(%w+)") or nil
end

-- THE CHARACTER BEING PLAYED, as the game last saved it: its record in WrcGame.json (<save>.<cat slot>:
-- GameplayProgression, QuestProgression ...), or nil. The game saves when a character is loaded or created, so the
-- record is the active cat's (WaCa.CurrentSlot: the save's own CurrentCatSlot - operator 2026-10-28, two cats mixed up).
function WaCa.SaveCharacter()
    local data = saveData()
    local slot = WaCa.CurrentSlot()
    if not (data and slot) then return nil end
    local save, index = slot:match("^(.*)-(%d+)$")
    local character = data[save] and data[save][index]
    return type(character) == "table" and character or nil
end

-- the character's GameplayProgression (Level, FreshKill, the player cat's Customization ...), or nil
function WaCa.GameProgress()
    local character = WaCa.SaveCharacter()
    return character and character.GameplayProgression or nil
end

-- THE SAVE'S STORY CATS of any clan - every narrative role's cat the game made for this character, the other clans'
-- included (NPCData.NarrativeRoles: "Squirrelstar" = "(ClanType=ThunderClan,Role=Leader,BodyValue=...)" - the game's
-- own text form of a cat's look). WaCa.StoryCats("ShadowClan", "Leader") -> { { key, name, clan, role, look } ... }
-- (clan / role nil = any). The look text goes to WaCa.ImportLook. {} when no save is read yet.
function WaCa.StoryCats(clan, role)
    local character = WaCa.SaveCharacter()
    local roles = character and character.NPCData and character.NPCData.NarrativeRoles
    local out = {}
    if type(roles) ~= "table" then return out end
    for key, look in pairs(roles) do
        if type(look) == "string" then
            local c, r = look:match("ClanType=(%w+)"), look:match("Role=(%w+)")
            if (not clan or c == clan) and (not role or r == role) then
                local prefix, suffix = look:match('PrefixNameId="([^"]*)"'), look:match('SuffixNameId="([^"]*)"')
                local name = prefix and WaCa.GetText(prefix .. (suffix or "")) or ""
                if name == "" or name:find("^STR_") then name = tostring(key) end
                out[#out + 1] = { key = tostring(key), name = name, clan = c, role = r, look = look }
            end
        end
    end
    table.sort(out, function(x, y) return x.key < y.key end)
    return out
end

-- dress a cat (or its look component) from a look's text form (WaCa.StoryCats' look): the game builds its whole look
-- from it. False without WaCa Core's native part.
function WaCa.ImportLook(cat, text)
    local comp = cat and cat:IsValid() and (cat:IsA(StaticFindObject("/Script/SwtAdventure.SwtAdventureCustomizableMeshComponent")) and cat
                 or WaCa.CatComponent(cat))
    if not WaCaNative_ImportLook or not comp or not text then return false end
    return WaCaNative_ImportLook(comp:GetAddress(), text)
end

-- a cat's look in the game's own text form ("(ClanType=RiverClan,BodyValue=6,...)") - what ImportLook / SpawnCat take
function WaCa.LookOf(cat)
    local comp = cat and cat:IsValid() and (cat:IsA(StaticFindObject("/Script/SwtAdventure.SwtAdventureCustomizableMeshComponent")) and cat
                 or WaCa.CatComponent(cat))
    if not WaCaNative_ExportLook or not comp then return nil end
    return WaCaNative_ExportLook(comp:GetAddress())
end

-- a look's text form split into its fields, in order: { { key, value } ... } (values may hold brackets / quotes)
local function lookFields(text)
    local body = tostring(text or ""):match("^%s*%((.*)%)%s*$") or ""
    local out, depth, quoted, start = {}, 0, false, 1
    for i = 1, #body + 1 do
        local ch = body:sub(i, i)
        if ch == '"' then quoted = not quoted
        elseif not quoted and ch == "(" then depth = depth + 1
        elseif not quoted and ch == ")" then depth = depth - 1
        elseif (ch == "," and depth == 0 and not quoted) or i > #body then
            local part = body:sub(start, i - 1)
            local k, v = part:match("^%s*([%w_]+)=(.*)$")
            if k then out[#out + 1] = { k, v } end
            start = i + 1
        end
    end
    return out
end

-- A KIT'S LOOK from its parents' (romance + kits, tickets.md 12 - pre-work): each trait from one parent or the other
-- (body, fur, colours, patterns, eyes, ears, tail ...), the first parent's clan, Role = Kit, kit-sized, a name of its
-- own (a prefix of the save's cats + "kit"). a, b = looks (text forms - WaCa.LookOf). Returns the look, the name and the
-- kit's size as an actor scale (WaCa.SpawnCat's `scale`): the look's own CatSize does NOT size a cat in the world (VM
-- 2026-10-31: CatSize 0.8 -> 2.5 + a look build / RefreshShape changed no bone; it feeds the creator preview's anim only).
WaCa.KIT_SCALE = 0.6
function WaCa.BlendLooks(a, b)
    local fa, fb = lookFields(a), lookFields(b)
    local other = {}
    for _, f in ipairs(fb) do other[f[1]] = f[2] end
    local parts = {}
    local seen = {}
    for _, f in ipairs(fa) do
        local k, v = f[1], f[2]
        seen[k] = true
        if k == "Role" then v = "Kit"
        elseif k == "ClanType" or k == "PrefixNameId" or k == "SuffixNameId" then -- (below / the first parent's)
        elseif other[k] ~= nil and math.random() < 0.5 then v = other[k] end
        if k == "CatSize" then v = string.format("%.6f", (tonumber(v) or 1) * WaCa.KIT_SCALE) end
        parts[#parts + 1] = { k, v }
    end
    for _, f in ipairs(fb) do if not seen[f[1]] then parts[#parts + 1] = f end end
    -- (a name of its own: a prefix one of the save's story cats has, not the parents' - "...kit")
    local used = {}
    for _, f in ipairs(fa) do if f[1] == "PrefixNameId" then used[f[2]] = true end end
    for _, f in ipairs(fb) do if f[1] == "PrefixNameId" then used[f[2]] = true end end
    -- (the game's own prefixes NOT yet given to a cat - WrcNPCGenerationManager.RemainingPreffixes, weighted for the
    -- kit's clan; VM 2026-11-01: a prefix taken from the story cats made "Lionkit" beside Lionstar. The story cats'
    -- prefixes only when that pool is empty)
    local clan = (a:match("ClanType=(%w+)")) or ""
    local taken = {}                 -- (the story cats' prefixes: generated cats' list doesn't know them - Lionstar)
    for _, c in ipairs(WaCa.StoryCats()) do
        local p = c.look:match('PrefixNameId="([^"]*)"')
        if p then taken[p] = true end
    end
    -- (and the names of the cats in the world - the camp's generated cats aren't story cats: 'Lionkit' beside the
    -- camp's Lionstar, 'Tigerkit' beside Tigerclaw, VM 2026-11-01)
    local living = {}
    for _, cat in ipairs(FindAllOf("SwtAdventureQuestActor") or {}) do
        pcall(function() local n = WaCa.CatName(cat) if n and n ~= "" then living[#living + 1] = n:lower() end end)
    end
    local function nameTaken(word)
        word = word:lower()
        if word == "" then return true end
        for _, n in ipairs(living) do if n:sub(1, #word) == word then return true end end
        return false
    end
    local pool, total = {}, 0
    pcall(function()
        (WaCa.Live(FindFirstOf("WrcNPCGenerationManager")) or error("no NPC generation manager")).RemainingPreffixes:ForEach(function(_, e)
            local r = e:get()
            local id = r.Preffix_ID:ToString()
            local w = 1
            pcall(function() w = tonumber(r[clan]) or 1 end)
            local kitName = WaCa.GetText(id .. "_KIT")
            if id ~= "" and w > 0 and not used['"' .. id .. '"'] and not taken[id] and kitName ~= ""
                and not nameTaken(kitName:match("^(.-)kit$") or "") then
                pool[#pool + 1] = { id = id, w = w }
                total = total + w
            end
        end)
    end)
    local prefix
    if total > 0 then
        local roll = math.random() * total
        for _, e in ipairs(pool) do
            roll = roll - e.w
            if roll <= 0 then prefix = e.id break end
        end
        prefix = prefix or pool[#pool].id
    else
        -- (the unused pool used up: the game's whole name list - STR_CAT_NAME<n>, as far as it goes - by the same
        -- rules; a story cat's prefix would be taken by definition)
        local prefixes = {}
        local misses = 0
        for n = 1, 500 do
            local id = "STR_CAT_NAME" .. n
            local kitName = WaCa.GetText(id .. "_KIT")
            if kitName == "" or kitName:find("^STR_") then
                misses = misses + 1
                if misses > 20 then break end
            else
                misses = 0
                if not used['"' .. id .. '"'] and not taken[id] and not nameTaken(kitName:match("^(.-)kit$") or "") then
                    prefixes[#prefixes + 1] = id
                end
            end
        end
        prefix = #prefixes > 0 and prefixes[math.random(#prefixes)] or nil
    end
    local hasRole, hasSize = false, false
    for _, f in ipairs(parts) do
        if f[1] == "PrefixNameId" and prefix then f[2] = '"' .. prefix .. '"' end
        if f[1] == "SuffixNameId" then f[2] = '"_KIT"' end
        if f[1] == "Role" then hasRole = true end
        if f[1] == "CatSize" then hasSize = true end
    end
    if not hasRole then parts[#parts + 1] = { "Role", "Kit" } end
    if not hasSize then parts[#parts + 1] = { "CatSize", string.format("%.6f", WaCa.KIT_SCALE) } end
    local text = {}
    for _, f in ipairs(parts) do text[#text + 1] = f[1] .. "=" .. f[2] end
    local name = prefix and WaCa.GetText(prefix .. "_KIT") or nil
    return "(" .. table.concat(text, ",") .. ")", name, WaCa.KIT_SCALE
end

-- A MONTAGE ON A CAT, null-safe (a method called on a null object is an access violation pcall cannot catch - a mesh
-- with no animation running has no anim instance): target = a skeletal mesh component, or an actor (its
-- SkeletalMeshComponent, else Mesh, else its first skeletal mesh); montage = the asset or its object path. Returns the
-- montage's length (nil: nothing played). forceMode = 1st put the mesh back on its animation blueprint.
local skelClass
local function bodyMesh(target)
    if not (target and target:IsValid()) then return nil end
    skelClass = skelClass or StaticFindObject("/Script/Engine.SkeletalMeshComponent")
    local mesh
    pcall(function() if target:IsA(skelClass) then mesh = target end end)
    for _, field in ipairs({ "SkeletalMeshComponent", "Mesh" }) do
        if not mesh then
            pcall(function() local m = target[field] if m and m:IsValid() then mesh = m end end)
        end
    end
    if not mesh then
        pcall(function() local m = target:GetComponentByClass(skelClass) if m and m:IsValid() then mesh = m end end)
    end
    return mesh
end
local function animOf(target, forceMode)
    local mesh = bodyMesh(target)
    if not mesh then return nil end
    local inst
    pcall(function()
        if forceMode then mesh:SetAnimationMode(0, true) end
        local i = mesh:GetAnimInstance()
        if i and i:IsValid() then inst = i end
    end)
    return inst
end
function WaCa.PlayMontage(target, montage, rate, forceMode)
    if type(montage) == "string" then
        local path = montage:find("%.") and montage or (montage .. "." .. montage:match("([^/]+)$"))
        montage = WaCa.LoadAsset(path)
    end
    if not (montage and montage:IsValid()) then return nil end
    local inst = animOf(target, forceMode)
    if not inst then return nil end
    local len
    pcall(function() len = inst:Montage_Play(montage, rate or 1.0, 0, 0.0, true) end)
    return len
end
function WaCa.StopMontage(target, blend)
    local inst = animOf(target)
    if not inst then return false end
    return (pcall(function() inst:Montage_Stop(blend or 0.25, nil) end))
end

-- WHAT A TRACE HIT: the name of the material on the hit component's slot 0 (a trace's out-param `hit`), or nil.
-- Every step checked - a method called on a null object is an access violation pcall cannot catch (VM 2026-10-31:
-- the game died in a walker's ground check on a hit with no material).
function WaCa.HitMaterialName(hit, slot)
    local name
    pcall(function()
        local c = hit and hit.Component
        if c and tostring(c):match("^FWeakObjectPtr") then c = c:Get() end
        if not (c and c:IsValid()) then return end
        local mat = c:GetMaterial(slot or 0)
        if not (mat and mat:IsValid()) then return end
        name = mat:GetFName():ToString()
    end)
    return name
end

-- AN EXTRA CAT in the world - one the game doesn't have here (another clan's at a Gathering, a mod's own cat): the
-- game's own cutscene cat (BP_WrcSK_Customization_Cat - a standing cat with the full look system, no AI, no name plate)
-- spawned at `at` facing `yaw`, dressed from a look's text form (WaCa.StoryCats' look) when given, `scale` times an
-- adult's size (1 when not given - a kit: BlendLooks' third value; the look's CatSize doesn't size it). Returns the actor
-- (or nil); WaCa.Despawn(cat) removes it. Its path is the way to keep it (a held wrapper can go stale). VM 2026-10-31:
-- Sunstar of ThunderClan standing by the RiverClan camp's stream.
local CAT_CLASS = "/Game/Gameplay/Blueprints/Characters/BP_WrcSK_Customization_Cat.BP_WrcSK_Customization_Cat_C"
function WaCa.SpawnCat(look, at, yaw, scale)
    local cls = StaticFindObject(CAT_CLASS)
    if not (cls and cls:IsValid()) then
        WaCa.LoadAsset(CAT_CLASS:gsub("%.[^.]+$", ""))
        cls = StaticFindObject(CAT_CLASS)
    end
    if not (cls and cls:IsValid()) or not at then return nil end
    -- (ON THE GROUND: this cat's feet are its origin - a cat spawned at your cat's place (its capsule's middle) floated
    -- 36 cm, on a slope 1.45 m: library exercise 2026-11-02. Set down on the ground below `at` - within 2 m above, 10 m
    -- below; a point with no ground there (a spawn tucked away under the map) keeps its height.)
    pcall(function()
        local K = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
        local ctx = FindFirstOf("PlayerController")
        local ignore = {}
        pcall(function() if ctx.Pawn then ignore[1] = ctx.Pawn end end)
        local hit = {}
        if K:LineTraceSingle(ctx, { X = at.X, Y = at.Y, Z = at.Z + 200 }, { X = at.X, Y = at.Y, Z = at.Z - 1000 }, 0, true,
                ignore, 0, hit, true, { R = 0, G = 0, B = 0, A = 0 }, { R = 0, G = 0, B = 0, A = 0 }, 0) then
            at = { X = at.X, Y = at.Y, Z = hit.ImpactPoint.Z }
        end
    end)
    local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    local half = math.rad((yaw or 0) / 2)
    local tf = { Translation = { X = at.X, Y = at.Y, Z = at.Z }, Rotation = { X = 0, Y = 0, Z = math.sin(half), W = math.cos(half) },
                 Scale3D = { X = scale or 1, Y = scale or 1, Z = scale or 1 } }
    local ctx
    pcall(function() ctx = FindFirstOf("PlayerController") end)
    local ok, cat = pcall(function()
        local a = gs:BeginDeferredActorSpawnFromClass(ctx, cls, tf, 1, nil, 2)   -- (always spawn; the root's own scale)
        if a and a:IsValid() then gs:FinishSpawningActor(a, tf, 2) end
        return a
    end)
    if not (ok and cat and cat:IsValid()) then return nil end
    if look then WaCa.ImportLook(cat, look) end
    return cat
end

-- A PANEL ON THE HUD (the game's current-objective look, under its quest panel - WaCa Core draws them all, stacked):
-- WaCa.HudPanel("MyMod.status", { title = "...", where = "...", activity = "...", goal = "...", got = 1, need = 3,
-- done = false, order = 100 }) shows or updates it (order: lower further up); WaCa.HudPanel(id, nil) takes it away.
-- (each mod remembers what it sent: WaCa Core asks again when it starts - a reload of Core lost every panel)
local myPanels, panelsAsked = {}, false
function WaCa.HudPanel(id, status)
    myPanels[id] = status or false
    if not panelsAsked then
        panelsAsked = true
        WaCa.On("HudPanelsAgain", function()
            for pid, s in pairs(myPanels) do if s then WaCa.Emit("HudPanel", pid, s) end end
        end)
    end
    WaCa.Emit("HudPanel", id, status)
end

-- A NAMEPLATE ON A CAT WE PUT IN THE WORLD (2026-11-03: a newcomer / a kit from SpawnCat had no name over its head - the
-- game gives plates only to its quest NPCs): the game's own plate widget (WBP_GuiQuestActorName - name, rank) on a
-- screen-space widget component of the actor, height cm over its origin. role = the rank line's text (best: copied
-- from a game cat's plate - WaCa.PlateRole(name) - so it is the game's own word in the game's language). Returns the
-- widget, or nil.
WaCa.PLATE_WIDGET = "/Game/Art/UXD/Blueprints/Controls/gui_actor_widgets/WBP_GuiQuestActorName.WBP_GuiQuestActorName_C"
function WaCa.Nameplate(actor, name, role, height)
    local a = WaCa.Live(actor)
    if not a then return nil end
    local ok, w = pcall(function()
        local cls = StaticFindObject("/Script/UMG.WidgetComponent")
        local comp
        -- (a widget component already holding a plate - from an earlier call: used again, not a second one added)
        for _, c in ipairs(a:K2_GetComponentsByClass(cls) or {}) do
            local cc = (c.get and c:get()) or c
            pcall(function()
                local held = WaCa.Live(cc) and WaCa.Live(cc:GetUserWidgetObject())
                if held and held:GetClass():GetFName():ToString() == "WBP_GuiQuestActorName_C" then comp = cc end
            end)
        end
        if not comp then
            comp = a:AddComponentByClass(cls, false, { Translation = { X = 0, Y = 0, Z = height or 90 },
                Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Scale3D = { X = 1, Y = 1, Z = 1 } }, false)
        end
        comp:SetWidgetSpace(1)                         -- (screen space: faces the camera, keeps its size)
        comp:SetDrawAtDesiredSize(true)
        local pc
        for _, c in ipairs(FindAllOf("SwtPlayerController") or {}) do if c:IsValid() and c.Player then pc = c end end
        local widget = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary"):Create(pc, WaCa.LoadAsset(WaCa.PLATE_WIDGET), pc)
        comp:SetWidget(widget)                          -- (SetWidgetClass isn't callable here; a made widget is)
        widget.mcGuiTextName:SetText(FText(tostring(name or "")))
        widget.mcGuiTextRole:SetText(FText(tostring(role or "")))
        pcall(function() widget.mcGuiTextExtraInfo:SetText(FText("")) end)
        widget:SetVisibility(4)
        comp:SetVisibility(true, true)
        return widget
    end)
    return ok and w or nil
end

-- a rank's text in the game's language, by the role name a look carries (Kit, Apprentice, Warrior, Deputy ... - the
-- game's own STR_GIVER_ROLE_<ROLE> texts, read 2026-11-03) - or nil
function WaCa.RoleText(role)
    if not role or role == "" then return nil end
    local key = "STR_GIVER_ROLE_" .. tostring(role):upper()
    local t = WaCa.GetText(key)
    return (t and t ~= "" and t ~= key) and t or nil
end

-- the rank line a game cat's nameplate shows ("Warrior" in the game's language), by the cat's name - or nil
function WaCa.PlateRole(catName)
    local role
    for _, t in ipairs(FindAllOf("TextBlock") or {}) do
        pcall(function()
            if not role and t:GetFName():ToString() == "mcGuiTextName" and t:GetText():ToString() == catName then
                local plate = t:GetOuter():GetOuter()
                if plate:GetClass():GetFName():ToString() == "WBP_GuiQuestActorName_C" then
                    role = plate.mcGuiTextRole:GetText():ToString()
                end
            end
        end)
    end
    return role ~= "" and role or nil
end

function WaCa.Despawn(cat)
    if cat and pcall(function() return cat:IsValid() end) and cat:IsValid() then pcall(function() cat:K2_DestroyActor() end) end
end

-- THE PARTY: the story's party for the character being played - its QuestProgression <chapter>.PartySetup, the cats on
-- the pause menu's PARTY page besides your own: a list of ENarrativeRole names (RiverClanKit ...) from the latest
-- chapter that sets one; {} = no party, nil = no save read yet. (The PARTY page is filled only when the pause menu opens;
-- the save has it from the start - VM 2026-10-30: Nightheart = RiverClanKit, Rippleface = RiverClanWarriorSideRace.)
function WaCa.PartyRoles()
    local character = WaCa.SaveCharacter()
    local quests = character and character.QuestProgression
    if type(quests) ~= "table" then return nil end
    local setup, chapterKey
    for key, chapter in pairs(quests) do
        if type(chapter) == "table" and type(chapter.PartySetup) == "table" and (not chapterKey or tostring(key) > chapterKey) then
            setup, chapterKey = chapter.PartySetup, tostring(key)
        end
    end
    local roles = {}
    for _, e in ipairs(setup or {}) do
        if type(e) == "table" and type(e.NarrativeRole) == "string" and e.NarrativeRole ~= "None" then roles[#roles + 1] = e.NarrativeRole end
    end
    return roles
end

-- a place from the game's own zone triggers (WrcZoneNameTrigger - the names the game shows as you walk in: "RiverClan
-- Camp", "The Island"): the middle of the zone whose code starts with `match` ("I-", "R-05") or whose name id holds it
-- ("STR_ISLAND_ZONE") - its area boxes; a zone with only a gate (the Island: one thin trigger where you step onto it)
-- that. { X, Y, Z } or nil. (Loaded far from the cat too: VM 2026-10-31, the Island's from the RiverClan camp.)
function WaCa.ZonePlace(match)
    local sums = { area = { 0, 0, 0, 0 }, gate = { 0, 0, 0, 0 } }
    for _, z in ipairs(FindAllOf("WrcZoneNameTrigger") or {}) do
        pcall(function()
            if not z:IsValid() then return end
            local code, name = z.ZoneCode:ToString(), z.ZoneNameId:ToString()
            if code:sub(1, #match) ~= match and not name:find(match, 1, true) then return end
            local comp = z.DetectionComponent
            local e = comp:GetScaledBoxExtent()
            local c = comp:K2_GetComponentLocation()
            local t = sums[math.min(e.X, e.Y) < 150 and "gate" or "area"]
            t[1], t[2], t[3], t[4] = t[1] + 1, t[2] + c.X, t[3] + c.Y, t[4] + c.Z
        end)
    end
    local t = sums.area[1] > 0 and sums.area or sums.gate
    if t[1] == 0 then return nil end
    return { X = t[2] / t[1], Y = t[3] / t[1], Z = t[4] / t[1] }
end

-- a mod's sidecar file for a slot (the active one when slot is nil)
function WaCa.SlotFile(modName, slot)
    slot = slot or WaCa.CurrentSlot()
    if not slot then return nil end
    return string.format("%s/WrcGame.%s.%s.ini", WaCa.SaveDir(), slot, modName)
end

---------------------------------------------------------------------------------------------------------------------
-- Cat appearance. A cat's look is one USwtAdventureCustomizableMeshComponent (on the player's pawn, on every NPC, on
-- the creation screen's preview); its mesh parts (body, tail, ears, fur cards) all use the material M_Characters_Base,
-- whose parameters below are the coat. Changes made here are what you SEE: the save keeps the game's choices.

-- the coat's colour slots -> material parameters
WaCa.COAT_COLORS = {
    Fur = "Fur Color", Pattern = "Pattern Mask Color", Markings = "Markings Mask Color", Paws = "Paws Color",
    Ears = "Ears Color", Nose = "Nose Color", Whiskers = "Whiskers Color",
}
-- the coat's shape textures (white = the slot's colour) -> material parameters
WaCa.COAT_MASKS = { Pattern = "Pattern Mask", Markings = "Markings Mask" }
WaCa.CAT_PARTS = { "SkelMeshComp", "TailSkelMeshComp", "EarSkelMeshComp", "FurSkelMeshComp" }

-- "#RRGGBB" / "RRGGBB" / "r, g, b" (0-255) -> a linear colour {X, Y, Z} as the material wants it (the game's colours
-- are linear: its own F8940B orange is stored as 0.93 / 0.30 / 0.004)
function WaCa.ParseColor(text)
    if not text or text == "" then return nil end
    local r, g, b
    local hex = text:match("^#?(%x%x%x%x%x%x)$")
    if hex then
        r, g, b = tonumber(hex:sub(1, 2), 16), tonumber(hex:sub(3, 4), 16), tonumber(hex:sub(5, 6), 16)
    else
        r, g, b = text:match("^(%d+)%s*,%s*(%d+)%s*,%s*(%d+)$")
        r, g, b = tonumber(r), tonumber(g), tonumber(b)
    end
    if not (r and g and b) then return nil end
    local function lin(c)
        c = c / 255
        return c <= 0.04045 and c / 12.92 or ((c + 0.055) / 1.055) ^ 2.4
    end
    return { X = lin(r), Y = lin(g), Z = lin(b) }
end

-- the mesh parts of a cat (skeletal mesh components that exist)
function WaCa.CatParts(cat)
    local out = {}
    for _, name in ipairs(WaCa.CAT_PARTS) do
        local part = cat[name]
        if part and part:IsValid() then table.insert(out, part) end
    end
    return out
end

-- a part's role from its material: MI_Cat_Body -> "Body", MI_Cat_Tails_A -> "Tails_A", MI_Cat_Fur_Bony -> "Fur_Bony"
-- (pattern packs name their mask files after it)
function WaCa.PartRole(part, slot)
    local m = part:GetMaterial(slot or 0)
    if not m or not m:IsValid() then return nil end
    local name = m:GetFName():ToString()
    if m:GetClass():GetFName():ToString() == "MaterialInstanceDynamic" and m.Parent and m.Parent:IsValid() then
        name = m.Parent:GetFName():ToString()
    end
    return name:match("^MI_Cat_(.-)_CelShaded") or name:match("^MI_Cat_(.+)$")
end

-- set one coat colour (a WaCa.COAT_COLORS slot or a raw parameter name) on every part of a cat
function WaCa.SetCatColor(cat, slot, color)
    local param = FName(WaCa.COAT_COLORS[slot] or slot)
    for _, part in ipairs(WaCa.CatParts(cat)) do part:SetVectorParameterValueOnMaterials(param, color) end
end

-- the material instance a part draws with, made dynamic if it isn't yet (so its parameters can change)
local function dynamicMaterial(part, slot)
    local m = part:GetMaterial(slot)
    if not m or not m:IsValid() then return nil end
    if m:GetClass():GetFName():ToString() == "MaterialInstanceDynamic" then return m end
    return part:CreateDynamicMaterialInstance(slot, m, FName("None"))
end

-- set a texture parameter on all of one part's materials
function WaCa.SetPartTexture(part, param, texture)
    for i = 0, part:GetNumMaterials() - 1 do
        local mid = dynamicMaterial(part, i)
        if mid and mid:IsValid() then mid:SetTextureParameterValue(FName(param), texture) end
    end
end

-- load an asset by its object path ("/Game/Mods/X/MyAsset.MyAsset"), also from a mod pak (nil when it doesn't
-- exist). UE4SS's own LoadAsset only finds assets in the game's AssetRegistry - a mod pak's aren't in it - so this
-- falls back to the engine's own load (KismetSystemLibrary LoadAsset_Blocking, through a soft object reference).
function WaCa.LoadAsset(path)
    local ok, asset = pcall(LoadAsset, path)
    if ok and asset and asset:IsValid() then return asset end
    local k = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
    local loaded
    ok, loaded = pcall(function()
        return k:LoadAsset_Blocking(k:Conv_SoftObjPathToSoftObjRef(k:MakeSoftObjectPath(path)))
    end)
    if ok and loaded and loaded:IsValid() then return loaded end
    return nil
end

-- load a PNG / JPG / TGA from disk as a texture (cached by path; nil when it can't be read)
-- (cached by the texture's object path, found again each time: a held object can be garbage-collected and a call on
-- its stale wrapper crashes the game)
local textures = {}
function WaCa.LoadTexture(path)
    local known = textures[path] and StaticFindObject(textures[path])
    if known and known:IsValid() then return known end
    local f = io.open(path, "rb")
    if not f then return nil end
    f:close()
    local lib = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
    local tex = lib:ImportFileAsTexture2D(FindFirstOf("World"), path)
    if tex and tex:IsValid() then
        textures[path] = (tex:GetFullName():gsub("^%S+ ", ""))
        return tex
    end
    return nil
end

-- the game's own mask texture for a part: a pattern (P01_Fishbone ... P23_Deluxe_Spotted) or a marking (M01_Sock ...
-- M20_Piebald) by its folder name, e.g. GameMask("M12_Mask", "Ears_A"). Loaded from the player's own game files.
function WaCa.GameMask(name, role)
    local kind = name:match("^P%d") and "Patterns" or "Markings"
    local tex = string.format("T_Cat_%s_%s_M", role, name)
    local path = string.format("/Game/Art/Characters/CatMaster/Textures/%s/%s/%s.%s", kind, name, tex, tex)
    local obj = StaticFindObject(path)
    if not obj or not obj:IsValid() then
        -- LoadAsset wants the full object path (Package.Object) and must run on the game thread (main.lua's callers do)
        local ok, loaded = pcall(LoadAsset, path)
        obj = ok and loaded or nil
    end
    return obj and obj:IsValid() and obj or nil
end

-- set one coat mask slot ("Pattern" or "Markings") on every part of a cat from a source:
--   "game:M12_Mask"  the game's own pattern / marking of that name (all parts it exists for)
--   "<folder>"       a pattern pack: PNGs named after the part roles (Body.png, Tails_A.png, Ears_B.png,
--                    Fur_Bony.png ...; Tails.png / Ears.png / Fur.png cover all variants of a part)
-- Parts the source has no mask for keep the game's. Returns how many parts changed.
-- one part's mask for a slot from a source (see SetCatMask); true when the source has one for this part
function WaCa.SetPartMask(part, maskSlot, source)
    local param = WaCa.COAT_MASKS[maskSlot] or maskSlot
    local game = source:match("^game:(.+)$")
    local role = WaCa.PartRole(part)
    local tex
    if role and game then
        tex = WaCa.GameMask(game, role)
    elseif role then
        tex = WaCa.LoadTexture(source .. "/" .. role .. ".png")
            or WaCa.LoadTexture(source .. "/" .. (role:match("^(%a+)") or role) .. ".png")
    end
    if not tex then return false end
    WaCa.SetPartTexture(part, param, tex)
    return true
end

function WaCa.SetCatMask(cat, maskSlot, source)
    local n = 0
    for _, part in ipairs(WaCa.CatParts(cat)) do
        if WaCa.SetPartMask(part, maskSlot, source) then n = n + 1 end
    end
    return n
end

-- the part's name (Body / Tail / Ears / Tufts) of one of a cat's mesh parts, by its full name, or nil
function WaCa.PartNameOf(cat, partFullName)
    for name, field in pairs(WaCa.PART_NAMES) do
        local part = cat[field]
        if part and part:IsValid() and part:GetFullName() == partFullName then return name, part end
    end
    return nil
end

-- the game builds a cat's whole look again from its own data (colours, patterns, markings - and, through the look
-- hook, every mod's look on top); comp = a cat's look component. False without WaCa Core's native part.
function WaCa.RebuildLook(comp)
    if not WaCaNative_RebuildLook or not comp or not comp:IsValid() then return false end
    return WaCaNative_RebuildLook(comp:GetAddress())
end

-- dress one cat as another: the game builds the whole look of `to` from `from`'s data (colours, patterns, markings,
-- shape) - e.g. a ceremony's stand-in as the cat cast in its part. Cats or their look components. False without WaCa
-- Core's native part.
function WaCa.CopyLook(to, from)
    local t = to and to:IsValid() and (to:IsA(StaticFindObject("/Script/SwtAdventure.SwtAdventureCustomizableMeshComponent")) and to
              or WaCa.CatComponent(to))
    local f = from and from:IsValid() and (from:IsA(StaticFindObject("/Script/SwtAdventure.SwtAdventureCustomizableMeshComponent")) and from
              or WaCa.CatComponent(from))
    if not WaCaNative_CopyLook or not t or not f then return false end
    return WaCaNative_CopyLook(t:GetAddress(), f:GetAddress())
end

-- the parts by name, for per-part colours ("Tail.Fur = ..."): Body, Tail, Ears, Tufts (the fur cards)
WaCa.PART_NAMES = { Body = "SkelMeshComp", Tail = "TailSkelMeshComp", Ears = "EarSkelMeshComp", Tufts = "FurSkelMeshComp" }

-- set one coat colour on one named part only (e.g. a darker tail)
function WaCa.SetPartColor(cat, partName, slot, color)
    local part = cat[WaCa.PART_NAMES[partName] or partName]
    if part and part:IsValid() then part:SetVectorParameterValueOnMaterials(FName(WaCa.COAT_COLORS[slot] or slot), color) end
end

-- the player's cat: the customizable mesh on the player's pawn, the player's cat in cutscenes (bIsPlayer), and - only
-- when withPreview is true (pass true while the character creator is open) - the creator's preview cat. The game
-- reuses that preview actor on other screens ("Select Your Cat" shows other cats, e.g. Dapplepatch, with it).
-- a cat's appearance values as one string (FCatCustomData's look fields): two cats with the same fingerprint look
-- the same - the player's cat and its portraits / inventory views; nil for an empty (all-zero) record
WaCa.FINGERPRINT_FIELDS = { "ClanType", "Gender", "BodyValue", "FaceValue", "FurValue", "EyeValue", "WhiskerValue",
    "TailIdx", "EarIdx", "FurIdx", "FurColorIdx", "Pattern01ColorIdx", "Pattern02ColorIdx", "PatternIdx", "MarkingIdx",
    "EyeLeftColorIdx", "EyeRightColorIdx" }
function WaCa.CatFingerprint(d)
    local parts, any = {}, false
    for _, f in ipairs(WaCa.FINGERPRINT_FIELDS) do
        local ok, v = pcall(function() return d[f] end)
        v = ok and v or "?"
        if v ~= 0 and v ~= false and v ~= "?" then any = true end
        table.insert(parts, tostring(v))
    end
    return any and table.concat(parts, ",") or nil
end

-- the GuiCatCustomActor instances (the creator preview, the portrait / headshot and inventory views) with their
-- mesh components
function WaCa.PreviewCats()
    local out = {}
    for _, preview in ipairs(FindAllOf("GuiCatCustomActor") or {}) do
        local comp = preview:IsValid() and preview.CustomizableMeshComponent
        if comp and comp:IsValid() then table.insert(out, comp) end
    end
    return out
end

function WaCa.PlayerCats(withPreview)
    local out = {}
    local pc = FindFirstOf("PlayerController")
    local pawn = pc and pc:IsValid() and pc.Pawn
    local pawnName = pawn and pawn:IsValid() and pawn:GetFullName()
    local playerPrint
    if pawnName then
        for _, comp in ipairs(FindAllOf("SwtAdventureCustomizableMeshComponent") or {}) do
            local owner = comp:IsValid() and WaCa.Live(comp:GetOwner())
            if owner and owner:GetFullName() == pawnName then
                table.insert(out, comp)
                playerPrint = playerPrint or WaCa.CatFingerprint(comp.CustomData)
            end
        end
    end
    -- preview actors: all of them in the creator; elsewhere only those showing the player's cat (its headshots and
    -- inventory view - "Select Your Cat" reuses one for other cats, which have other values)
    for _, comp in ipairs(WaCa.PreviewCats()) do
        if withPreview or (playerPrint and WaCa.CatFingerprint(comp.CustomData) == playerPrint) then
            table.insert(out, comp)
        end
    end
    -- the player's cat as the creator and cutscenes show it (BP_WrcSK_Customization_Player and friends: a skeletal
    -- mesh actor flagged bIsPlayer)
    for _, actor in ipairs(FindAllOf("WrcCatSkeletalMeshWithCustomization") or {}) do
        if actor:IsValid() and actor.bIsPlayer then
            local comp = actor.MeshCustomizationComponent
            if comp and comp:IsValid() then table.insert(out, comp) end
        end
    end
    return out
end

-- where a UMG widget was last drawn: left, top, right, bottom in desktop pixels, or nil (not drawn / no native half).
-- Read natively: a widget's geometry can't be read from Lua (its fields aren't reflected - a copy comes back empty).
function WaCa.WidgetRect(widget)
    if not (WaCaNative_WidgetRect and widget and widget:IsValid()) then return nil end
    local ok, l, t, r, b = pcall(WaCaNative_WidgetRect, widget:GetAddress())
    if ok and l then return l, t, r, b end
    return nil
end

-- is the mouse over the widget (by where it was drawn)? and the widget's on-screen area (for the innermost of several)
function WaCa.MouseOver(widget)
    local l, t, r, b = WaCa.WidgetRect(widget)
    if not (l and WaCaNative_CursorPos) then return false end
    local x, y = WaCaNative_CursorPos()
    return x >= l and x <= r and y >= t and y <= b, (r - l) * (b - t)
end

-- --- menus vs gameplay (shared by every WaCa mod: the Keybinds & Mouse mod's cursor, the clan loop's pauses) ---
-- A "menu" = any screen up that is not the gameplay screen or an overlay over gameplay (the HUDs, the online status,
-- the combat HUD, screen transitions, the centre message), or a popup on the gameplay screen (the pause menu, the map).
-- GuiScreenOnlineOver is up for a whole session: taken as a menu, the cursor never left menu mode in the world
-- (operator 2026-10-25, VM-confirmed).
local OVERLAY_CLASSES = { "/Script/SwtAdventure.GuiScreenOnlineOver", "/Script/Wrc.GuiHudCombatOver",
                          "/Script/SwtEngine.ScreenTransitionOver", "/Script/SwtAdventure.GuiPopupCenterMessage" }
local overlayClasses, gameScreenClass = nil, nil

local function isGameplayScreen(screen)
    gameScreenClass = gameScreenClass or StaticFindObject("/Script/SwtEngine.GuiScreenGame")
    if gameScreenClass and gameScreenClass:IsValid() and screen:IsA(gameScreenClass) then return true end
    if not overlayClasses then
        overlayClasses = {}
        for _, path in ipairs(OVERLAY_CLASSES) do
            local c = StaticFindObject(path)
            if c and c:IsValid() then table.insert(overlayClasses, c) end
        end
    end
    for _, c in ipairs(overlayClasses) do
        if screen:IsA(c) then return true end
    end
    return screen:GetClass():GetFName():ToString():find("Hud", 1, true) ~= nil
end

-- true while a menu is showing, false in gameplay, nil while NO screen is up (the game tears one screen down a frame
-- or two before it builds the next - that gap is not gameplay). leaving = a screen being torn down (ScreenHidden: it
-- still counts as in the viewport)
function WaCa.MenuShowing(leaving)
    local leavingName = leaving and leaving:GetFullName()
    local any = false
    for _, s in ipairs(FindAllOf("SwtScreen") or {}) do
        if s:IsValid() and s:IsInViewport() and s:GetFullName() ~= leavingName then
            any = true
            if not isGameplayScreen(s) then return true end
            local popup = s.CurrentPopup
            if popup and popup:IsValid() and popup:GetFullName() ~= leavingName then return true end
        end
    end
    if not any then return nil end
    return false
end

-- THE CATS ABOUT (library exercise 2026-10-30: Clan Life's cast and WaCa Cutscenes both had to find cats by name / part
-- - one picture of them here). WaCa.CatName(cat) -> the name the game shows for a cat (its name ids -> the text
-- table: a WaCa Looks roster Name included), or nil. WaCa.Cats() -> every cat shown now - the camp's named (quest)
-- cats and the walking cats - nearest to your cat first: { actor, name, clan, giver, story, kind = "quest" | "walker",
-- dist } (giver: EGiverRole name - Leader, Deputy ...; story: ENarrativeRole name - RiverClanDeputy ...).
local catEnums = {}
local function catEnum(path, value)
    local cache = catEnums[path]
    if not cache then
        cache = {}
        catEnums[path] = cache
        pcall(function()
            StaticFindObject(path):ForEachName(function(n, v) cache[v] = n:ToString():gsub("^.*::", "") end)
        end)
    end
    return cache[value]
end

-- your cat's name as the game shows it now (the HUD's own label - a WaCa Looks name included; else the save's), or
-- "You"
function WaCa.PlayerName()
    local name
    for _, w in ipairs(FindAllOf("GuiExplorationCharacterSlot") or {}) do
        pcall(function()
            local t = w:IsValid() and w.mcGuiTextName and w.mcGuiTextName:GetText():ToString()
            if t and t ~= "" then name = t end
        end)
        if name then break end
    end
    if not name then pcall(function() local c = WaCa.SaveCharacter() if c and c.Name then name = c.Name end end) end
    if name and name == name:upper() then name = name:sub(1, 1) .. name:sub(2):lower() end   -- (the HUD's capitals)
    return name or "You"
end

-- a cat's look data (its MeshCustomizationComponent's CustomData - a live view), or nil. NULL-SAFE: a character
-- class with that property left empty (prey, a fight's characters) handed back a null object, and reading through it
-- crashed the game (VM 2026-11-01: twice, a probe asking every SwtAdventureCharacter its name) - pcall can't catch that
function WaCa.CustomData(cat)
    local cd
    pcall(function()
        if not (cat and cat:IsValid()) then return end
        local comp = cat.MeshCustomizationComponent
        if not (comp and comp:IsValid()) then return end
        cd = comp.CustomData
    end)
    return cd
end

function WaCa.CatName(cat)
    local name
    pcall(function()
        local cd = WaCa.CustomData(cat)
        if not cd then return end
        name = WaCa.GetText(cd.PrefixNameId:ToString() .. cd.SuffixNameId:ToString())
    end)
    return (name and name ~= "") and name or nil
end

local function catShown(a)
    local ok, yes = pcall(function()
        if a.bHidden then return false end
        local mesh = a.SkeletalMeshComponent or a.Mesh
        return not mesh or not mesh:IsValid() or mesh:IsVisible()
    end)
    return ok and yes
end

function WaCa.Cats()
    local pawn
    for _, c in ipairs(FindAllOf("SwtPlayerController") or {}) do
        if c:IsValid() and c.Player and c.Pawn and c.Pawn:IsValid() then pawn = c.Pawn end
    end
    local at = pawn and pawn:K2_GetActorLocation()
    local out = {}
    local function add(a, giver, story, kind)
        local q = a:K2_GetActorLocation()
        local clan
        pcall(function() clan = a.ClanType end)
        if type(clan) ~= "number" then pcall(function() local cd = WaCa.CustomData(a) if cd then clan = cd.ClanType end end) end
        out[#out + 1] = {
            actor = a, name = WaCa.CatName(a) or a:GetFName():ToString(),
            clan = type(clan) == "number" and catEnum("/Script/SwtQuest.EClanType", clan) or nil,
            giver = giver or "", story = story or "", kind = kind,
            dist = at and math.sqrt((q.X - at.X) ^ 2 + (q.Y - at.Y) ^ 2) or 0,
        }
    end
    for _, a in ipairs(FindAllOf("SwtAdventureQuestActor") or {}) do
        pcall(function()
            if a:IsValid() and catShown(a) then
                add(a, catEnum("/Script/SwtQuest.EGiverRole", a.GiverRole), catEnum("/Script/SwtQuest.ENarrativeRole", a.narrativeRole), "quest")
            end
        end)
    end
    for _, a in ipairs(FindAllOf("SwtAdventureWalkingNPC") or {}) do
        pcall(function() if a:IsValid() and catShown(a) then add(a, "", "", "walker") end end)
    end
    -- (cats a mod put in the world - WaCa.SpawnCat: a newcomer, a kit - by their name; a cutscene's B = Emberheart found
    -- nobody, 2026-11-03)
    for _, a in ipairs(FindAllOf("BP_WrcSK_Customization_Cat_C") or {}) do
        pcall(function()
            if a:IsValid() and catShown(a) and WaCa.CatName(a) then add(a, "", "", "spawned") end
        end)
    end
    table.sort(out, function(x, y) return x.dist < y.dist end)
    return out
end

-- a menu is up now (no screen at all = gameplay)
function WaCa.InMenu() return WaCa.MenuShowing() == true end

-- is this screen part of gameplay (the HUD, an overlay - not a menu)?
function WaCa.IsGameplayScreen(screen) return isGameplayScreen(screen) end

return WaCa
