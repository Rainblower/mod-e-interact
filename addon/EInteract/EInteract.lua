-- EInteract: one key (E by default) to loot, gather and talk to NPCs without the mouse,
-- page through quest dialogs and answer group loot rolls.
-- Looting, gathering and talking are done by the server module mod-e-interact: the 3.3.5 client
-- cannot look for units around the player and InteractUnit is protected, so the addon only
-- sends an addon whisper to itself. Quest dialogs and loot rolls work on any server.

local defaults = { enabled = true, key = "E", autoloot = true, dialog = true, roll = true }

local L = {
    NOTHING = "Nothing to interact with nearby",
    NO_SERVER = "the server has no mod-e-interact: looting, gathering and talking to NPCs are unavailable, quest dialogs and loot rolls still work.",
    SERVER_OK = "server module found",
    IN_COMBAT = "in combat, will apply after combat.",
    ENABLED = "enabled, key %s",
    DISABLED = "disabled",
    KEY = "key: %s",
    AUTOLOOT = "autoloot %s",
    DIALOG = "quest dialogs: %s",
    ROLL = "loot rolls: %s",
    DEBUG = "debug %s",
    ON = "on",
    OFF = "off",
    USAGE = "/ei on | off | key <key> | loot on|off | dialog on|off | roll on|off | debug",
    STATUS = "now: %s, key %s, autoloot %s, dialogs %s, rolls %s, server module %s",
    SERVER_STATE = { [true] = "found", [false] = "missing" },
    CHECKING = "checking",
}

if GetLocale() == "ruRU" then
    L = {
        NOTHING = "Рядом не с кем взаимодействовать",
        NO_SERVER = "на сервере нет mod-e-interact: лут, сбор и разговор с NPC недоступны, квестовые диалоги и розыгрыш добычи работают.",
        SERVER_OK = "серверный модуль найден",
        IN_COMBAT = "в бою, применю после выхода из боя.",
        ENABLED = "включён, клавиша %s",
        DISABLED = "выключен",
        KEY = "клавиша: %s",
        AUTOLOOT = "автолут %s",
        DIALOG = "квестовые диалоги: %s",
        ROLL = "розыгрыш добычи: %s",
        DEBUG = "отладка %s",
        ON = "вкл",
        OFF = "выкл",
        USAGE = "/ei on | off | key <клавиша> | loot on|off | dialog on|off | roll on|off | debug",
        STATUS = "сейчас: %s, клавиша %s, автолут %s, диалоги %s, розыгрыш %s, серверный модуль %s",
        SERVER_STATE = { [true] = "есть", [false] = "нет" },
        CHECKING = "проверяется",
    }
end

local function Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cff33ff99EInteract:|r " .. msg)
end

local function OnOff(value)
    return value and L.ON or L.OFF
end

local function Send(what)
    SendAddonMessage("EINT", what, "WHISPER", UnitName("player"))
end

-- Server module: nil while checking, then true/false. Without a reply the "hello" whisper
-- just comes back to us unchanged, which is why the echoed requests are ignored below.
local server, helloAt, helloUntil, warned

local function Hello()
    Send("hello")
    helloUntil = GetTime() + 10
end

-- Quest dialog is open: E pages through it instead of looking for an NPC. In Storyline the
-- whole text is one "next" button: it finishes the text, pages through it, shows objectives or
-- rewards and then accepts or turns in the quest. It is disabled while several rewards are
-- offered — then E does nothing and the reward is picked with the mouse.
local function AdvanceDialog()
    if Storyline_NPCFrame and Storyline_NPCFrame:IsVisible() then
        if Storyline_NPCFrameChatNext:IsEnabled() == 1 then Storyline_NPCFrameChatNext:Click() end
        return true
    end
    if QuestFrame and QuestFrame:IsVisible() then
        for _, b in ipairs({ QuestFrameAcceptButton, QuestFrameCompleteButton, QuestFrameCompleteQuestButton }) do
            if b:IsVisible() then
                if b:IsEnabled() == 1 then b:Click() end
                return true
            end
        end
    end
end

-- Group loot: E answers Need on the oldest open roll, or Greed when Need is not allowed
-- (wrong class or armor type). A bind-on-pickup item asks for confirmation in a
-- CONFIRM_LOOT_ROLL popup — the next press confirms it.
local rolls = {} -- rollIDs in the order they appeared

local function ConfirmRoll()
    for i = 1, STATICPOPUP_NUMDIALOGS do
        local popup = _G["StaticPopup" .. i]
        if popup:IsVisible() and popup.which == "CONFIRM_LOOT_ROLL" then
            _G["StaticPopup" .. i .. "Button1"]:Click()
            return true
        end
    end
end

local function RollNeed()
    while rolls[1] do
        local id = tremove(rolls, 1)
        if GetLootRollTimeLeft(id) > 0 then
            local _, _, _, _, _, canNeed, canGreed = GetLootRollItemInfo(id)
            RollOnLoot(id, (canNeed == nil and canGreed == nil or canNeed) and 1 or 2)
            return true
        end
    end
end

local button = CreateFrame("Button", "EInteractButton", UIParent)
button:SetScript("OnClick", function()
    if EInteractDB.roll and (ConfirmRoll() or RollNeed()) then return end
    if EInteractDB.dialog and AdvanceDialog() then return end
    if server == false then
        UIErrorsFrame:AddMessage(L.NOTHING, 1, 0.1, 0.1)
        if not warned then
            warned = true
            Print(L.NO_SERVER)
        end
        Hello() -- the module may have been enabled since
        return
    end
    Send("near")
end)

local frame = CreateFrame("Frame")

-- Skinning loot after a server-side cast sometimes arrives without autoloot: take the rest ourselves.
local expectLoot, lootAt, closeAt = 0

local function LootAll()
    for i = GetNumLootItems(), 1, -1 do
        if GetLootSlotInfo(i) then LootSlot(i) end
    end
    closeAt = GetTime() + 0.5
end

local function LootIsEmpty()
    for i = 1, GetNumLootItems() do
        if GetLootSlotInfo(i) then return false end
    end
    return true
end

frame:SetScript("OnUpdate", function()
    local now = GetTime()
    if helloAt and now >= helloAt then
        helloAt = nil
        Hello()
    end
    if helloUntil and now >= helloUntil then
        helloUntil = nil
        if server == nil then server = false end
    end
    if lootAt and now >= lootAt then
        lootAt = nil
        LootAll()
    end
    if closeAt and now >= closeAt then
        closeAt = nil
        if LootIsEmpty() then CloseLoot() end
    end
end)

local function Apply()
    ClearOverrideBindings(frame)
    if EInteractDB.enabled then
        SetOverrideBindingClick(frame, true, EInteractDB.key, "EInteractButton")
    end
end

local pending -- bindings cannot be changed in combat

local function ApplySafe()
    if InCombatLockdown() then
        pending = true
        Print(L.IN_COMBAT)
    else
        Apply()
    end
end

frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("PLAYER_REGEN_ENABLED")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("LOOT_OPENED")
frame:RegisterEvent("START_LOOT_ROLL")
frame:RegisterEvent("CANCEL_LOOT_ROLL")
frame:SetScript("OnEvent", function(self, event, arg1, arg2, _, arg4)
    if event == "START_LOOT_ROLL" then
        tinsert(rolls, arg1)
    elseif event == "CANCEL_LOOT_ROLL" then
        for i, id in ipairs(rolls) do
            if id == arg1 then tremove(rolls, i) break end
        end
    elseif event == "CHAT_MSG_ADDON" then
        if arg1 ~= "EINT" or arg4 ~= UnitName("player") or arg2 == "near" or arg2 == "hello" then return end
        if EInteractDB.debug then Print("server: " .. tostring(arg2)) end
        if arg2 == "ready" then
            if server ~= true and (EInteractDB.debug or warned) then Print(L.SERVER_OK) end
            server, helloUntil, warned = true, nil, nil
        elseif arg2 == "gather" then
            expectLoot = GetTime()
        elseif arg2 == "none" then
            UIErrorsFrame:AddMessage(L.NOTHING, 1, 0.1, 0.1)
        end
    elseif event == "LOOT_OPENED" then
        if EInteractDB.debug then Print("LOOT_OPENED autoLoot=" .. tostring(arg1)) end
        if EInteractDB.autoloot and GetTime() - expectLoot < 6 then
            expectLoot = 0
            lootAt = GetTime() + 0.3
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        if server == nil and not helloUntil then helloAt = GetTime() + 2 end
    elseif event == "PLAYER_LOGIN" then
        EInteractDB = EInteractDB or {}
        for k, v in pairs(defaults) do
            if EInteractDB[k] == nil then EInteractDB[k] = v end
        end
        if EInteractDB.autoloot then SetCVar("autoLootDefault", "1") end
        Apply()
    elseif event == "PLAYER_REGEN_ENABLED" and pending then
        pending = nil
        Apply()
    end
end)

SLASH_EINTERACT1 = "/ei"
SLASH_EINTERACT2 = "/einteract"
SlashCmdList.EINTERACT = function(msg)
    local cmd, arg = strsplit(" ", strtrim(msg or ""), 2)
    cmd = (cmd or ""):lower()
    if cmd == "on" or cmd == "off" then
        EInteractDB.enabled = (cmd == "on")
        ApplySafe()
        Print(EInteractDB.enabled and L.ENABLED:format(EInteractDB.key) or L.DISABLED)
    elseif cmd == "key" and arg and arg ~= "" then
        EInteractDB.key = arg:upper()
        ApplySafe()
        Print(L.KEY:format(EInteractDB.key))
    elseif cmd == "loot" then
        EInteractDB.autoloot = (arg == "on")
        SetCVar("autoLootDefault", EInteractDB.autoloot and "1" or "0")
        Print(L.AUTOLOOT:format(OnOff(EInteractDB.autoloot)))
    elseif cmd == "dialog" then
        EInteractDB.dialog = (arg == "on")
        Print(L.DIALOG:format(OnOff(EInteractDB.dialog)))
    elseif cmd == "roll" then
        EInteractDB.roll = (arg == "on")
        Print(L.ROLL:format(OnOff(EInteractDB.roll)))
    elseif cmd == "debug" then
        EInteractDB.debug = not EInteractDB.debug
        Print(L.DEBUG:format(OnOff(EInteractDB.debug)))
    else
        Print(L.USAGE)
        Print(L.STATUS:format(OnOff(EInteractDB.enabled), EInteractDB.key,
            OnOff(GetCVar("autoLootDefault") == "1"), OnOff(EInteractDB.dialog), OnOff(EInteractDB.roll),
            server == nil and L.CHECKING or L.SERVER_STATE[server]))
    end
end
