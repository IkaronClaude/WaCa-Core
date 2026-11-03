-- A MOD'S OWN LINE IN THE PAUSE MENU'S QUEST TAB (operator 2026-11-03, tickets.md 25: "Display-only line WITH detail
-- panel"): a mod sends WaCa.QuestLine(id, { title, activity, location, order }) (nil takes it away); this script, the
-- only one drawing, puts each in the SIDE QUESTS list after the game's own side quests - an unused item of the list's
-- pool (WBP_GuiItemQuestPause, four of them) filled the game's way: its title and place, the filled look (CanvasNormal /
-- CanvasSelected over CanvasEmpty). The detail panel beside the list (mcGuiCurrentQuest - the game's current-objective
-- look) shows a line while the mouse is over it, or the first line when the game has no side quest of its own (the
-- game hides the panel then). Display only: no tracking, no Enter - the game's own navigation never lands on it.
-- While the quest tab is up the list is checked every 150 ms (the game refills its pool on a tab change; the mouse
-- hover has no event here).
local QuestLines = {}

local WaCa, log
local lines = {}                  -- id -> { title, activity, location, order }
local VISIBLE, COLLAPSED, HIDDEN, SELF_HIT_TEST_INVISIBLE = 0, 1, 2, 4
local watching = false
local saved                       -- the game's own detail while ours is over it: { title, activity, location, vis ... }
local shownId                     -- the line on the detail panel now
local lastLogged

local function sorted()
    local out = {}
    for id, l in pairs(lines) do out[#out + 1] = { id = id, line = l } end
    table.sort(out, function(a, b)
        local oa, ob = tonumber(a.line.order) or 100, tonumber(b.line.order) or 100
        if oa ~= ob then return oa < ob end
        return a.id < b.id
    end)
    return out
end

local function text(w, s) if WaCa.Live(w) then w:SetText(FText(tostring(s or ""))) end end
local function vis(w, v) if WaCa.Live(w) and w:GetVisibility() ~= v then w:SetVisibility(v) end end
local function get(w) local t = "" pcall(function() t = w:GetText():ToString() end) return t end

local function pausePanel()
    for _, s in ipairs(FindAllOf("WBP_GuiScreenPause_C") or {}) do
        if s:IsValid() and s:IsInViewport() then
            local p = WaCa.Live(s.mcGuiPanelQuest)
            return s, (p and p:GetVisibility() ~= COLLAPSED and p:GetVisibility() ~= HIDDEN) and p or nil
        end
    end
end

local function items(list)
    local t = {}
    pcall(function() list.Items:ForEach(function(_, e) t[#t + 1] = e:get() end) end)
    return t
end

local function fillItem(it, l, selected)
    for _, n in ipairs({ "mcGuiTextQuestNormal", "mcGuiTextQuestSelected" }) do text(it[n], l.title) end
    for _, n in ipairs({ "mcGuiTextLocationNormal", "mcGuiTextLocationSelected" }) do text(it[n], l.location or "") end
    vis(it.CanvasEmpty, HIDDEN)
    vis(it.CanvasNormal, selected and HIDDEN or VISIBLE)
    vis(it.CanvasSelected, selected and VISIBLE or HIDDEN)
    vis(it.CanvasIcon, HIDDEN)                 -- (the tracked / new flag: ours is never tracked)
end

local function detail(d, l)
    text(d.mcGuiTitle, l.title)
    text(d.mcGuiActivity, l.activity or "")
    text(d.mcGuiLocation, l.location or "")
    vis(d.CanvasLocation, (l.location or "") ~= "" and SELF_HIT_TEST_INVISIBLE or COLLAPSED)
    for g = 1, 3 do vis(d["mcGuiGoal" .. g], COLLAPSED) end
    vis(d, VISIBLE)
end

-- the game's own detail kept while ours is on the panel, put back when the mouse leaves our line
local function keep(d)
    local s = { title = get(d.mcGuiTitle), activity = get(d.mcGuiActivity), location = get(d.mcGuiLocation),
                vis = d:GetVisibility(), loc = WaCa.Live(d.CanvasLocation) and d.CanvasLocation:GetVisibility(), goals = {} }
    for g = 1, 3 do local o = WaCa.Live(d["mcGuiGoal" .. g]) s.goals[g] = o and o:GetVisibility() end
    return s
end
local function putBack(d, s)
    text(d.mcGuiTitle, s.title)
    text(d.mcGuiActivity, s.activity)
    text(d.mcGuiLocation, s.location)
    if s.loc then vis(d.CanvasLocation, s.loc) end
    for g = 1, 3 do if s.goals[g] then vis(d["mcGuiGoal" .. g], s.goals[g]) end end
    vis(d, s.vis)
end

local function apply()
    local screen, panel = pausePanel()
    if not screen then return false end
    if not panel then return true end
    local list = WaCa.Live(panel.mcGuiSideQuestList)
    local d = WaCa.Live(panel.mcGuiCurrentQuest)
    if not (list and d) then return true end
    if list:GetVisibility() == COLLAPSED or list:GetVisibility() == HIDDEN then
        -- (the main quests' tab: the game's detail is its own again)
        if saved then saved, shownId = nil, nil end
        return true
    end
    local own = list.ListQuestsStruct:GetArrayNum()
    local pool = items(list)
    local mine = sorted()
    local hovered
    -- (by its look, not the item: the item is a 100 x 30 slot its look overflows - a 25 x 7 pixel area on the VM)
    for k, e in ipairs(mine) do
        local it = WaCa.Live(pool[own + k])
        if it and (WaCa.MouseOver(it.CanvasNormal) or WaCa.MouseOver(it.CanvasSelected)) then hovered = e end
    end
    -- (the line on the detail panel: the hovered one; the first when the game has no side quest - its panel is hidden)
    local pick = hovered or (own == 0 and mine[1]) or nil
    for k, e in ipairs(mine) do
        local it = WaCa.Live(pool[own + k])
        if it then fillItem(it, e.line, pick == e) end
    end
    local room = #pool - own
    if #mine > room and lastLogged ~= #mine .. "/" .. room then
        lastLogged = #mine .. "/" .. room
        log("quest lines: %d line(s), room for %d after the game's %d side quest(s)", #mine, math.max(0, room), own)
    end
    if pick then
        if not saved and own > 0 then saved = keep(d) end
        detail(d, pick.line)
        if shownId ~= pick.id then
            shownId = pick.id
            log("quest lines: %s on the detail panel%s", pick.id, hovered and " (the mouse over it)" or "")
        end
    elseif saved then
        putBack(d, saved)
        saved, shownId = nil, nil
    end
    return true
end

local function watch()
    if watching then return end
    watching = true
    local function tick()
        local ok, up = pcall(apply)
        if not ok then log("quest lines: %s", tostring(up)) end
        if ok and up and next(lines) then
            ExecuteInGameThreadWithDelay(150, tick)
        else
            watching, saved, shownId = false, nil, nil
        end
    end
    tick()
end

function QuestLines.Init(c)
    WaCa, log = c.WaCa, c.log
    WaCa.On("QuestLine", function(id, l)
        id = tostring(id or "")
        if id == "" then return end
        local was = lines[id] ~= nil
        lines[id] = type(l) == "table" and l or nil
        if (lines[id] ~= nil) ~= was then
            log("quest lines: %s %s", id, lines[id] and ("- " .. tostring(lines[id].title)) or "taken away")
        end
        -- (the pause menu may be up already - the watch ends by itself when it isn't)
        if lines[id] then ExecuteInGameThread(watch) end
    end)
    ExecuteInGameThreadWithDelay(2000, function() WaCa.Emit("QuestLinesAgain") end)
    WaCa.On("ScreenShown", function(screen)
        local ok, n = pcall(function() return screen:GetClass():GetFName():ToString() end)
        if ok and n == "WBP_GuiScreenPause_C" and next(lines) then ExecuteInGameThread(watch) end
    end)
end

return QuestLines
