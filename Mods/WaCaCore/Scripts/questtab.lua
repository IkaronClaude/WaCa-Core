-- A "CLAN" TAB IN THE PAUSE MENU'S QUEST PAGE (operator 2026-11-04: Clan Life's quest info in a third tab next to
-- Main Quests and Side Quests): the game's own sub-tab button (its class, made like any widget) after Side Quests in
-- the page's tab row (HorizontalBox); while it is selected both quest lists are hidden and the page's detail panel
-- (mcGuiCurrentQuest - the game's current-objective look) shows the HUD panels' statuses (WaCa.HudPanel - Clan Life's
-- day: the duty, where, how far along), the first by order. Chosen by TriggerR past Side Quests (TriggerL back) or a
-- click on it; Main / Side clicked or the page left = the game's own tabs again. config/panels.ini [QuestTab] On /
-- Title. The game knows nothing of the tab: its own navigation, hidden lists and detail are put back when it closes.
local QuestTab = {}

local WaCa, log, Panels
local cfg = { on = true, title = "Clan", detailDown = 120 }
local TAB = "WaCaQuestTabClan"
local VISIBLE, COLLAPSED, HIDDEN, SELF_HIT_TEST_INVISIBLE = 0, 1, 2, 4
local TRIGGER_L, TRIGGER_R = 11, 12
local selected = false
local saved                       -- the game's detail panel + lists before ours: put back on leaving
local watching = false

local function text(w, s) if WaCa.Live(w) then w:SetText(FText(tostring(s or ""))) end end
local function vis(w, v) if WaCa.Live(w) and w:GetVisibility() ~= v then w:SetVisibility(v) end end
local function get(w) local t = "" pcall(function() t = w:GetText():ToString() end) return t end

local function questPanel()
    for _, s in ipairs(FindAllOf("WBP_GuiScreenPause_C") or {}) do
        if s:IsValid() and s:IsInViewport() then
            local p = WaCa.Live(s.mcGuiPanelQuest)
            if p and p:GetVisibility() ~= COLLAPSED and p:GetVisibility() ~= HIDDEN then return p end
            return nil, true
        end
    end
end

local function tabLook(b, on)
    if not WaCa.Live(b) then return end
    vis(b.CanvasSelected, on and SELF_HIT_TEST_INVISIBLE or HIDDEN)
    vis(b.CanvasNormal, on and HIDDEN or SELF_HIT_TEST_INVISIBLE)
end

-- our button in the tab row, made once per page - kept by its path (a made widget keeps the class's generated name; a
-- name check found nothing and every tick added one more: VM 2026-11-04, three Clan tabs); any other copy of the tab
-- class in the row that isn't the game's Main / Side (a reload's leftover) is taken out
local tabPath
local function ensureTab(panel)
    local side = WaCa.Live(panel.mcGuiButtonTabSideQuest)
    local main = WaCa.Live(panel.mcGuiButtonTabMainQuest)
    if not side then return nil end
    local row = side:GetParent()
    local rowPath = row:GetFullName()
    local mine = tabPath and StaticFindObject(tabPath)
    if not (WaCa.Live(mine) and mine:GetParent() and mine:GetParent():GetFullName() == rowPath) then mine = nil end
    local cls, sideIndex = side:GetClass(), nil
    local stale = false
    for i = row:GetChildrenCount() - 1, 0, -1 do
        local c = row:GetChildAt(i)
        if c:GetFullName() == side:GetFullName() then sideIndex = i
        elseif c:GetClass():GetFullName() == cls:GetFullName() and not (main and c:GetFullName() == main:GetFullName())
            and not (mine and c:GetFullName() == mine:GetFullName()) then
            pcall(function() c:RemoveFromParent() end)
            stale = true
        end
    end
    if mine then return mine end
    local pc
    for _, c in ipairs(FindAllOf("SwtPlayerController") or {}) do if c:IsValid() and c.Player then pc = c end end
    local b = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary"):Create(pc, cls, pc)
    if not WaCa.Live(b) then return nil end
    tabPath = (b:GetFullName():gsub("^%S+ ", ""))
    -- (right after Side Quests - the row also holds the key hints, "3 <" and "> 4", at its ends)
    sideIndex = nil
    for i = 0, row:GetChildrenCount() - 1 do if row:GetChildAt(i):GetFullName() == side:GetFullName() then sideIndex = i end end
    -- (InsertChildAt isn't callable here - VM 2026-11-04: added at the end, then moved next to Side Quests: ShiftChild,
    -- else the children after it taken off and put back after ours with their slots' padding / alignment)
    local slot = row:AddChildToHorizontalBox(b)
    if sideIndex then
        local moved = pcall(function() row:ShiftChild(sideIndex + 1, b) end)
        if not moved then
            local after = {}
            for i = sideIndex + 1, row:GetChildrenCount() - 2 do
                local c = row:GetChildAt(i)
                local s = c.Slot
                after[#after + 1] = { w = c, pad = s.Padding, h = s.HorizontalAlignment, v = s.VerticalAlignment, size = s.Size }
            end
            for _, e in ipairs(after) do row:RemoveChild(e.w) end
            for _, e in ipairs(after) do
                local s = row:AddChildToHorizontalBox(e.w)
                pcall(function() s:SetPadding(e.pad) s:SetHorizontalAlignment(e.h) s:SetVerticalAlignment(e.v) s:SetSize(e.size) end)
            end
        end
    end
    pcall(function() slot:SetPadding(side.Slot.Padding) end)
    pcall(function() slot:SetHorizontalAlignment(side.Slot.HorizontalAlignment) slot:SetVerticalAlignment(side.Slot.VerticalAlignment) end)
    for _, n in ipairs({ "mcGuiTextNormal", "mcGuiTextSelected", "mcGuiTextDisabled" }) do text(b[n], cfg.title) end
    tabLook(b, false)
    -- (the longer tab row reaches the detail panel: its "4 >" key hint sat on the detail's title, VM 2026-11-04 - the
    -- detail moves down DetailDown once per page, for every tab; not again for a reload's new tab - the old one was
    -- there, so was the move)
    if not stale then pcall(function()
        local s = panel.mcGuiCurrentQuest.Slot
        local p = s:GetPosition()
        s:SetPosition({ X = p.X, Y = p.Y + cfg.detailDown })
    end) end
    log("quest tab: the %s tab made, after Side Quests", cfg.title)
    return b
end

local function keep(panel)
    local d = panel.mcGuiCurrentQuest
    local s = { d = { title = get(d.mcGuiTitle), activity = get(d.mcGuiActivity), location = get(d.mcGuiLocation),
                      vis = d:GetVisibility() },
                main = panel.mcGuiMainQuestList:GetVisibility(), side = panel.mcGuiSideQuestList:GetVisibility() }
    return s
end
local function putBack(panel)
    if not saved then return end
    local d = WaCa.Live(panel.mcGuiCurrentQuest)
    if d then
        text(d.mcGuiTitle, saved.d.title)
        text(d.mcGuiActivity, saved.d.activity)
        text(d.mcGuiLocation, saved.d.location)
        vis(d, saved.d.vis)
    end
    vis(panel.mcGuiMainQuestList, saved.main)
    vis(panel.mcGuiSideQuestList, saved.side)
    -- (the game's own tab look again: the list it shows is its current one)
    tabLook(panel.mcGuiButtonTabMainQuest, saved.main ~= COLLAPSED and saved.main ~= HIDDEN)
    tabLook(panel.mcGuiButtonTabSideQuest, saved.side ~= COLLAPSED and saved.side ~= HIDDEN)
    saved = nil
end

local function show(panel)
    if not saved then saved = keep(panel) end
    vis(panel.mcGuiMainQuestList, HIDDEN)
    vis(panel.mcGuiSideQuestList, HIDDEN)
    tabLook(panel.mcGuiButtonTabMainQuest, false)
    tabLook(panel.mcGuiButtonTabSideQuest, false)
    local d = WaCa.Live(panel.mcGuiCurrentQuest)
    if not d then return end
    local s = Panels.Statuses()[1]
    if s then
        Panels.Fill(d, s)
    else
        text(d.mcGuiTitle, cfg.title)
        text(d.mcGuiActivity, "Nothing for the clan right now.")
        vis(d.CanvasLocation, COLLAPSED)
        for g = 1, 3 do vis(d["mcGuiGoal" .. g], COLLAPSED) end
        vis(d, VISIBLE)
    end
end

local function choose(on, why)
    if selected == on then return end
    selected = on
    log("quest tab: %s %s (%s)", cfg.title, on and "chosen" or "left", why)
end

local function tick()
    local panel, pauseUp = questPanel()
    if not panel then
        if selected then selected = false end
        saved = nil
        if not pauseUp then watching = false return end
        return ExecuteInGameThreadWithDelay(150, tick)
    end
    local b = ensureTab(panel)
    if selected then show(panel) elseif saved then putBack(panel) end
    tabLook(b, selected)
    return ExecuteInGameThreadWithDelay(150, QuestTab.Tick)
end

-- (every tick under pcall: one error logged once, the tab goes on - an error ended the loop for good, VM 2026-11-04)
local lastErr
function QuestTab.Tick()
    local ok, err = pcall(tick)
    if not ok then
        if tostring(err) ~= lastErr then lastErr = tostring(err) log("quest tab: %s", lastErr) end
        ExecuteInGameThreadWithDelay(500, QuestTab.Tick)
    end
end

local function watch()
    if watching then return end
    watching = true
    ExecuteInGameThreadWithDelay(150, QuestTab.Tick)
end

function QuestTab.Init(c)
    WaCa, log, Panels = c.WaCa, c.log, c.Panels
    local ini = WaCa.ReadIni(c.modDir .. "/config/panels.ini") or {}
    local q = ini["QuestTab"] or {}
    cfg.on = (q.On or "1") ~= "0"
    cfg.title = (q.Title or "") ~= "" and q.Title or cfg.title
    cfg.detailDown = tonumber(q.DetailDown or "") or cfg.detailDown
    if not cfg.on then log("quest tab: off (panels.ini [QuestTab] On = 0)") return end
    WaCa.On("ScreenShown", function(screen)
        local ok, n = pcall(function() return screen:GetClass():GetFName():ToString() end)
        if ok and n == "WBP_GuiScreenPause_C" then ExecuteInGameThread(watch) end
    end)
    -- (the page's own tab keys: TriggerR past Side Quests = ours; TriggerL from ours = back to Side Quests)
    WaCa.Watch("OnListenCommand", "/Script/SwtEngine.SwtScreen", "WaCaQuestTabCommand", "pre", "Command")
    WaCa.On("WaCaQuestTabCommand", function(_, command)
        command = tonumber(command)
        if command ~= TRIGGER_L and command ~= TRIGGER_R then return end
        ExecuteInGameThread(function()
            local panel = questPanel()
            if not panel then return end
            local sideOn = WaCa.Live(panel.mcGuiSideQuestList) and panel.mcGuiSideQuestList:GetVisibility() ~= COLLAPSED
                and panel.mcGuiSideQuestList:GetVisibility() ~= HIDDEN
            if command == TRIGGER_R and not selected and sideOn then choose(true, "TriggerR past Side Quests")
            elseif command == TRIGGER_L and selected then choose(false, "TriggerL") end
        end)
    end)
    -- (a click: on our tab = ours; on Main / Side while ours is up = theirs)
    WaCa.On("KeyDown", function(_, name)
        if name ~= "LeftMouseButton" then return end
        ExecuteInGameThread(function()
            local panel = questPanel()
            if not panel then return end
            local b = ensureTab(panel)
            if WaCa.MouseOver and WaCa.MouseOver(b) then choose(true, "clicked")
            elseif selected and WaCa.MouseOver and (WaCa.MouseOver(panel.mcGuiButtonTabMainQuest) or WaCa.MouseOver(panel.mcGuiButtonTabSideQuest)) then
                choose(false, "Main / Side clicked")
            end
        end)
    end)
end

return QuestTab
