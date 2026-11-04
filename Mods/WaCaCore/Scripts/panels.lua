-- PANELS ON THE HUD FOR ANY MOD (library exercise 2026-11-02: a mod could show a passing line, but nothing lasting -
-- WaCa Clan Life drew its day status itself, and a second mod doing the same would have drawn over it). A mod sends
-- WaCa.HudPanel(id, status) - status = { title, where, activity, goal, got, need, done } (any of them), or nil to take
-- the panel away; this script, the only one drawing, shows each as a copy of the game's own current-objective panel
-- (WBP_GuiHudCurrentObjective_C - the game's quest look), stacked under the game's quest panel in the order the
-- panels first came, and builds them again in every new HUD (a level load). Config: WaCaCore config/panels.ini
-- [Panels] On / Below (the first panel's distance under the game's, HUD units 3840 x 2160) / Step (between panels) /
-- Right.
local Panels = {}

local WaCa, log
local cfg = { on = true, below = 760, step = 480, right = 0, height = 440, bottom = 140, offset = 120 }  -- (step: a panel with a place, an activity and a goal is ~440 units high, VM)
local order = {}                  -- panel ids, first come first
local status = {}                 -- id -> its latest status
local drawn = {}                  -- id -> its widget's full name
local hudName                     -- the HUD the panels are in
local lastFit

local VISIBLE, COLLAPSED, HIDDEN, HIT_TEST_INVISIBLE = 0, 1, 2, 4

local function setText(widget, text)
    if widget and widget:IsValid() then widget:SetText(FText(text or "")) end
end
local function show(widget, on)
    if widget and widget:IsValid() then widget:SetVisibility(on and HIT_TEST_INVISIBLE or COLLAPSED) end
end

local function fill(w, s)
    if not s then w:SetVisibility(COLLAPSED) return end
    setText(w.mcGuiTitle, s.title)
    show(w.CanvasLocation, s.where ~= nil)
    setText(w.mcGuiLocation, s.where)
    setText(w.mcGuiActivity, s.activity)
    local g = w.mcGuiGoal1
    show(g, s.goal ~= nil)
    if s.goal then
        setText(g.mcGuiGoalName, s.goal)
        setText(g.mcGuiGoalValues, s.need and string.format("%d/%d", tonumber(s.got) or 0, tonumber(s.need)) or "")
        pcall(function() g.CanvasCheck:SetVisibility(s.done and HIT_TEST_INVISIBLE or HIDDEN) end)
        -- (the goal's icon: status.icon = a game texture's object path - the template's own is a placeholder, an angry
        -- cat face (T_UI_Icon_Status_Furious) the operator saw on every duty, 2026-11-04; no icon given = none shown)
        pcall(function()
            local tex = s.icon and WaCa.LoadAsset(s.icon)
            if tex and tex:IsValid() then
                g.mcGuiIcon:SetBrushFromTexture(tex, false)
                g.mcGuiIcon:SetVisibility(HIT_TEST_INVISIBLE)
            else
                g.mcGuiIcon:SetVisibility(HIDDEN)
            end
        end)
    end
    show(w.mcGuiGoal2, false)
    show(w.mcGuiGoal3, false)
    w:SetVisibility(HIT_TEST_INVISIBLE)
end

local function gameHud()
    for _, s in ipairs(FindAllOf("SwtScreen") or {}) do
        if s:IsValid() and s:IsInViewport() and s:GetClass():GetFName():ToString():find("^WBP_GuiHudGame") then return s end
    end
end

-- every panel with a status into this HUD, stacked (a panel taken away leaves no gap)
local function draw()
    local hud = gameHud()
    if not hud then return end
    local game = hud.mcGuiQuestObjective
    if not (game and game:IsValid()) then return end
    local canvas = game:GetParent()
    local name = hud:GetFullName()
    if hudName ~= name then
        drawn, hudName = {}, name
        -- (copies of the game's panel in this HUD besides its own: from before a reload of a mod - one owner now)
        for i = canvas:GetChildrenCount() - 1, 0, -1 do
            local other = canvas:GetChildAt(i)
            if other:IsValid() and other:GetClass() == game:GetClass() and other ~= game
                and other:GetFName():ToString() ~= game:GetFName():ToString() then
                other:RemoveFromParent()
            end
        end
    end
    local pc
    for _, c in ipairs(FindAllOf("SwtPlayerController") or {}) do if c:IsValid() and c.Player then pc = c end end
    local lib = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    local k = 0
    -- (status.order first - lower higher up, 100 when not given - then the order they first came: a reload of Core
    -- has every mod send again, in whatever order they answer)
    local sorted = {}
    for i, id in ipairs(order) do sorted[#sorted + 1] = { id = id, i = i, o = tonumber(status[id] and status[id].order) or 100 } end
    table.sort(sorted, function(a, b) if a.o ~= b.o then return a.o < b.o end return a.i < b.i end)
    -- (MORE THAN FIT: the panels scaled down together so the stack ends above the screen's foot - the third of three
    -- was cut off at the bottom, VM 2026-11-02. The HUD is 2160 units high; a panel ~cfg.height; at most down to 60%)
    local shown = 0
    for _, e in ipairs(sorted) do if status[e.id] then shown = shown + 1 end end
    local top = game.Slot:GetLayout().Offsets.Top + cfg.below
    local scale, fit = 1, shown
    if shown > 0 then
        -- (the panel's content draws ~Offset units below its slot's top - measured on the VM: 1210 -> 1330)
        local room = 2160 - cfg.bottom - top - cfg.offset
        -- (the most panels that fit at 60% or more - each at the largest scale that fits that many)
        for n = shown, 1, -1 do
            local s = math.min(1, room / ((n - 1) * cfg.step + cfg.height))
            if s >= 0.6 or n == 1 then scale, fit = math.min(1, s), n break end
        end
        if fit < shown and fit ~= lastFit then
            log("panels: %d of %d fit on the screen - the rest wait for room (%.0f free, at %.0f%%)", fit, shown, room,
                scale * 100)
        end
    end
    lastFit = fit
    for _, e in ipairs(sorted) do
        local id = e.id
        local w = drawn[id] and StaticFindObject(drawn[id])
        if status[id] and k >= fit then
            if w and w:IsValid() then w:SetVisibility(COLLAPSED) end
        elseif status[id] then
            if not (w and w:IsValid()) then
                w = lib:Create(hud, game:GetClass(), pc)
                canvas:AddChildToCanvas(w)
                drawn[id] = w:GetFullName():gsub("^%S+ ", "")
                log("panels: %s on the HUD", id)
            end
            local layout = game.Slot:GetLayout()
            layout.Offsets.Left = layout.Offsets.Left + cfg.right
            layout.Offsets.Top = layout.Offsets.Top + cfg.below + k * cfg.step * scale
            w.Slot:SetLayout(layout)
            w.Slot:SetAutoSize(false)
            pcall(function()
                w:SetRenderTransformPivot({ X = 0, Y = 0 })
                w:SetRenderScale({ X = scale, Y = scale })
            end)
            fill(w, status[id])
            k = k + 1
        elseif w and w:IsValid() then
            w:RemoveFromParent()
            drawn[id] = nil
            log("panels: %s off the HUD", id)
        end
    end
end

-- (for the pause menu's Clan tab - questtab.lua: the same statuses, in the same look)
function Panels.Statuses()
    local out = {}
    for _, id in ipairs(order) do if status[id] then out[#out + 1] = status[id] end end
    table.sort(out, function(a, b) return (tonumber(a.order) or 100) < (tonumber(b.order) or 100) end)
    return out
end
Panels.Fill = function(w, s) return fill(w, s) end

function Panels.Init(c)
    WaCa, log = c.WaCa, c.log
    local p = (WaCa.ReadIni(c.modDir .. "/config/panels.ini") or {})["Panels"] or {}
    cfg.on = (p.On or "1") ~= "0"
    cfg.below = tonumber(p.Below or "") or cfg.below
    cfg.step = tonumber(p.Step or "") or cfg.step
    cfg.right = tonumber(p.Right or "") or cfg.right
    cfg.height = tonumber(p.Height or "") or cfg.height
    cfg.bottom = tonumber(p.Bottom or "") or cfg.bottom
    cfg.offset = tonumber(p.Offset or "") or cfg.offset
    if not cfg.on then log("panels: off (panels.ini [Panels] On = 0)") return end
    WaCa.On("HudPanel", function(id, s)
        id = tostring(id or "")
        if id == "" then return end
        local known = false
        for _, o in ipairs(order) do if o == id then known = true end end
        if not known then order[#order + 1] = id end
        status[id] = type(s) == "table" and s or nil
        ExecuteInGameThread(function()
            local ok, err = pcall(draw)
            if not ok then log("panels: %s", tostring(err)) end
        end)
    end)
    -- (every mod sends its panels again - Core reloaded, or started after them)
    ExecuteInGameThreadWithDelay(2000, function() WaCa.Emit("HudPanelsAgain") end)
    WaCa.On("ScreenShown", function(screen)
        local ok, n = pcall(function() return screen:GetClass():GetFName():ToString() end)
        if not (ok and n:find("^WBP_GuiHudGame")) or not next(status) then return end
        local ok2, err = pcall(draw)
        if not ok2 then log("panels: %s", tostring(err)) end
    end)
end

return Panels
