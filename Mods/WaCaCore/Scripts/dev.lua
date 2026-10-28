-- Developer tracing, off unless config/dev.ini exists. For finding the game code a feature has to hook, from a real
-- play session (the native side logs to WaCaNative.log, "[dev]").
--   [Dev]
--   MenuKeyTrace = 1   the native call stacks of the first key presses and menu commands, and every instruction that
--                      reads the menu "back" row's key (DT_GamepadButtonsKeyboard Button_B) - press BackSpace in a menu
local Dev = {}

local WaCa, log
local cfg = {}
local stacks = 0

local function trace(label)
    if stacks >= 12 then return end
    stacks = stacks + 1
    WaCa.Command("DevStack|" .. label)
end

-- once the game's tables are loaded (main.lua: with the game's text)
local watched = false
function Dev.OnGameReady()
    if cfg.MenuKeyTrace ~= "1" or watched then return end
    local t = StaticFindObject(WaCa.UI_KEYS_TABLE)
    if not t or not t:IsValid() then return end
    watched = true
    WaCa.Command(string.format("DevWatchRow|%X|Button_B|8|8", t:GetAddress()))
    log("dev: watching the menu back row's key - press BackSpace in a menu")
end

function Dev.Init(c)
    WaCa, log = c.WaCa, c.log
    local ini = WaCa.ReadIni(c.modDir .. "/config/dev.ini")
    cfg = ini and ini["Dev"] or {}
    if cfg.MenuKeyTrace ~= "1" then return end
    log("dev: MenuKeyTrace on")
    pcall(RegisterHook, "/Script/SwtEngine.SwtPlayerController:OnKeyPressed", function(_, key)
        local ok, name = pcall(function() return key:get().KeyName:ToString() end)
        trace("SwtPlayerController.OnKeyPressed " .. (ok and name or "?"))
    end)
    pcall(RegisterHook, "/Script/SwtEngine.SwtScreen:OnListenCommand", function(self, command)
        local ok, v = pcall(function() return command:get() end)
        trace("SwtScreen.OnListenCommand " .. (ok and tostring(v) or "?"))
    end)
    pcall(RegisterHook, "/Script/SwtEngine.SwtPlayerController:OnListenCommand", function(self, command)
        local ok, v = pcall(function() return command:get() end)
        trace("SwtPlayerController.OnListenCommand " .. (ok and tostring(v) or "?"))
    end)
end

return Dev
