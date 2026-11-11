-- Xerces: the XERCES SETTINGS menu (docs/DESIGN.md 7.9). The Zombies round
-- settings are saved dvars (src/client/xerces/round_settings.cpp) that the
-- host's game reads as a Zombies match loads
-- (data/custom_scripts/zm/xerces_rounds.gsc), so everyone in the match gets
-- the host's settings. In the front end this menu changes them; its button is
-- in the Zombies lobbies (data/ui_scripts/frontend_menus). In a Zombies match
-- each player's pause menu gets XERCES SETTINGS, showing the settings the
-- match uses, the Pack-a-Punch wheel and coin flip when they are on, SKIP
-- ROUND on a round that can be skipped, and EXFIL and KEEP GOING while the
-- round 999 vote is open.
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesSettingsInstalled then
  return
end
__xercesSettingsInstalled = true

local TITLE = "XERCES SETTINGS"
local ACCENT = { 1, 0.4157, 0.2588 }
if type(XercesTokens) == "table" and type(XercesTokens.dark) == "table" and XercesTokens.dark.accent_text then
  ACCENT = XercesTokens.dark.accent_text
end

-- bit: the setting's bit in the mask the host's game sends; tab: where the
-- front end lists it
local WHEEL_BIT = 128
local COIN_FLIP_BIT = 256
local ROUND_SKIPS_BIT = 512
local ROUND_999_BIT = 4096
local SMART_ZOMBIES_BIT = 8192
local NO_HIP_SPREAD_BIT = 16384
local SETTINGS = {
  {
    dvar = "xerces_zm_maxAmmo",
    name = "Max Ammo Every Round",
    bit = 1,
    tab = "zombies",
    description = "Everyone gets a Max Ammo at the start of each round.",
  },
  {
    dvar = "xerces_zm_autoPap",
    name = "Auto Pack-a-Punch",
    bit = 2,
    tab = "zombies",
    description = "Guns from wall buys and the mystery box come already Pack-a-Punched.",
  },
  {
    dvar = "xerces_zm_startPoints",
    name = "Starting Points",
    points = true,
    tab = "zombies",
    description = "The points each player starts the match with. Default keeps the map's own, 500 on most maps.",
  },
  {
    dvar = "xerces_zm_noPerkLimit",
    name = "No Perk Limit",
    bit = 4,
    tab = "zombies",
    description = "Players can buy every perk on the map instead of stopping at four.",
  },
  {
    dvar = "xerces_zm_boxStays",
    name = "Mystery Box Stays",
    bit = 8,
    tab = "zombies",
    description = "The mystery box never moves to another spot.",
  },
  {
    dvar = "xerces_zm_powerOn",
    name = "Power On at Start",
    bit = 16,
    tab = "zombies",
    description = "The power switch turns on when the match starts. Maps without a standard power switch are left as they are.",
  },
  {
    dvar = "xerces_zm_doorsOpen",
    name = "Doors Open at Start",
    bit = 32,
    tab = "zombies",
    description = "Every door and debris pile opens for free when the match starts. Doors that need the power open once it is on.",
  },
  {
    dvar = "xerces_zm_roundHud",
    name = "Round HUD",
    bit = 64,
    tab = "zombies",
    description = "Shows how many zombies are left in the round and how long it has taken, top left.",
  },
  {
    dvar = "xerces_zm_noHipSpread",
    name = "No Hip-Fire Spread",
    bit = NO_HIP_SPREAD_BIT,
    tab = "zombies",
    description = "Shots from the hip go where the crosshair is, for everyone in the match. Aiming down sights is unchanged.",
  },
  {
    dvar = "xerces_zm_randomLoadout",
    name = "Random Loadout",
    bit = 1024,
    tab = "zombies",
    description = "At the start of each round everyone's guns are swapped for two the mystery box picks.",
  },
  {
    dvar = "xerces_zm_selfRevives",
    name = "Self-Revives",
    bit = 2048,
    tab = "zombies",
    description = "Everyone has one self-revive, back 10 rounds after it was used. Down with one, hold Jump or Use to get up.",
  },
  {
    dvar = "xerces_zm_startRound",
    name = "Starting Round",
    rounds = true,
    tab = "rounds",
    description = "The round the match starts on, with that round's zombies: their number, health and speed. Default is round 1.",
  },
  {
    dvar = "xerces_zm_roundSkips",
    name = "Round Skips",
    bit = ROUND_SKIPS_BIT,
    tab = "rounds",
    description = "Special rounds (dogs and the like) and prime-numbered rounds can be skipped from the pause menu for 400 points each, plus a Max Ammo on a special round. In co-op more than half must vote for it.",
  },
  {
    dvar = "xerces_zm_round999",
    name = "Rounds to 999",
    bit = ROUND_999_BIT,
    tab = "rounds",
    description = "The round count goes past the game's 255. At round 999 everyone votes: exfil and win, or keep going with every perk, infinite ammo, infinite points and only smart zombies.",
  },
  {
    dvar = "xerces_zm_smartZombies",
    name = "Smart Zombies",
    bit = SMART_ZOMBIES_BIT,
    tab = "rounds",
    description = "From round 20 some zombies hunt smarter. Yellow ones gather and rush one player as a pack, green ones walk and then sprint in bursts, blue ones wait out of sight and strike when you turn away. 10% of them at round 20, rising to 99% at round 998, all of them from round 999.",
  },
  {
    dvar = "xerces_zm_wheel",
    name = "Pack-a-Punch Wheel",
    bit = WHEEL_BIT,
    tab = "gambling",
    description = "From the pause menu: 2,500 points (10 during a Fire Sale), once a round, with a Pack-a-Punched gun in hand. It lands on one of the five ammo mods or on nothing.",
  },
  {
    dvar = "xerces_zm_coinFlip",
    name = "Coin Flip",
    bit = COIN_FLIP_BIT,
    tab = "gambling",
    description = "From the pause menu, three times a round: bet points on a coin, heads wins. A win pays the bet times 2 at round 1, 0.1 more each round, up to 5.",
  },
}

-- The GRAPHICS tab: this PC's own settings (src/client/xerces/upscale.cpp),
-- not the host's, so they are kept out of SETTINGS and the pause menu's list.
-- The same two are in Graphics > Video, which a loaded mod may replace.
local GRAPHICS = {
  {
    dvar = "xerces_upscaler",
    name = "Upscaler",
    values = { 0, 1, 2 },
    default = 0,
    -- NVIDIA DLSS only with the DLSS module (xerces_dlssAvailable)
    shown = function(value)
      return value ~= 2 or Engine.DvarInt(nil, "xerces_dlssAvailable") == 1
    end,
    text = function(value)
      if value == 1 then
        return "AMD FSR 1"
      elseif value == 2 then
        return "NVIDIA DLSS / DLAA"
      end
      return "Off"
    end,
    description = "How the 3D scene is enlarged when Scene Resolution Ratio (Graphics > Video) is below 100%. AMD FSR 1 keeps edges and detail sharper than the game's own stretch. NVIDIA DLSS (with the DLSS module and an RTX GPU) replaces SMAA T2x's anti-aliasing and upscales; at 100% it is DLAA. The HUD is not touched.",
  },
  {
    dvar = "xerces_upscalerSharpness",
    name = "Upscaler Sharpness",
    values = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 },
    default = 8,
    text = function(value)
      return (value * 10) .. "%"
    end,
    description = "How much the scene is sharpened after AMD FSR 1 or NVIDIA DLSS, 0% for none. It also sharpens at a Scene Resolution Ratio of 100%.",
  },
}

-- The CONTROLLER tab: this PC's own (src/client/xerces/zm_aim_assist.cpp)
local CONTROLLER = {
  {
    dvar = "xerces_zm_aimAssist",
    name = "Aim Assist (Zombies)",
    values = { 0, 1, 2 },
    default = 0,
    text = function(value)
      if value == 1 then
        return "Modern (MW3)"
      elseif value == 2 then
        return "Modern, Stronger"
      end
      return "Game's Own"
    end,
    description = "Controller aim assist in Zombies that works like the newer Call of Duty games' (Modern Warfare III): your look speed is slowed while a zombie is near the crosshair, and the view turns with a zombie as it moves or you strafe. As in those games it needs stick input (the left stick sideways, or the right stick), has no snap to targets, and fades up close and far away. Stronger keeps the same rules with more of each. Zombies you can see only; never with a mouse, never in multiplayer.",
  },
}

-- 50000 -> "50,000"; 0 is the map's own
local function withCommas(number)
  local text = tostring(math.floor(number))
  local grouped = ""
  while #text > 3 do
    grouped = "," .. string.sub(text, -3) .. grouped
    text = string.sub(text, 1, -4)
  end
  return text .. grouped
end

local function pointsText(points)
  if points <= 0 then
    return "Default"
  end
  return withCommas(points)
end

local function roundText(round)
  if round <= 1 then
    return "Default"
  end
  return "Round " .. round
end

if Engine.GetCurrentMap() == "core_frontend" then
  require("ui.uieditor.widgets.Lobby.Common.FE_TabBar")
  require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")

  local function updateDvar(controller, optionModel, optionsList, dvarName)
    UpdateInfoModels(optionModel)
    if Engine.DvarInt(nil, dvarName) ~= optionModel.value then
      Engine.SetDvar(dvarName, optionModel.value)
    end
  end

  -- A value set outside this menu is listed too, or the menu would show the
  -- first option and overwrite the dvar.
  local function choiceOptions(dvarName, values, defaultValue, text)
    local current = Engine.DvarInt(nil, dvarName) or defaultValue
    if current < defaultValue then
      current = defaultValue
    end
    local custom = true
    for _, value in ipairs(values) do
      if value == current then
        custom = false
      end
    end
    if custom then
      table.insert(values, current)
      table.sort(values)
    end
    local options = {}
    for _, value in ipairs(values) do
      local label = text(value)
      if custom and value == current then
        label = label .. " (Custom)"
      end
      table.insert(options, { option = label, value = value, default = value == defaultValue })
    end
    return options
  end

  local function pointsOptions()
    return choiceOptions("xerces_zm_startPoints", { 0, 500, 5000, 50000 }, 0, pointsText)
  end

  local function roundOptions()
    return choiceOptions(
      "xerces_zm_startRound",
      { 1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100 },
      1,
      roundText
    )
  end

  local function settingsSource(name, tab)
    DataSources[name] = DataSourceHelpers.ListSetup(name, function(controller)
      local items = {}
      for _, setting in ipairs(SETTINGS) do
        if setting.tab == tab then
          local options = {
            { option = "MENU_DISABLED", value = 0, default = true },
            { option = "MENU_ENABLED", value = 1 },
          }
          if setting.points then
            options = pointsOptions()
          elseif setting.rounds then
            options = roundOptions()
          end
          table.insert(
            items,
            CoD.OptionsUtility.CreateDvarSettings(
              controller,
              setting.name,
              setting.description,
              "XercesSettings_" .. setting.dvar,
              setting.dvar,
              options,
              nil,
              updateDvar
            )
          )
        end
      end
      return items
    end)
  end
  settingsSource("XercesSettingsZombies", "zombies")
  settingsSource("XercesSettingsGambling", "gambling")
  settingsSource("XercesSettingsRounds", "rounds")

  local function localSource(name, list)
    DataSources[name] = DataSourceHelpers.ListSetup(name, function(controller)
      local items = {}
      for _, setting in ipairs(list) do
        local options = {}
        for _, value in ipairs(setting.values) do
          if not setting.shown or setting.shown(value) then
            table.insert(options, { option = setting.text(value), value = value, default = value == setting.default })
          end
        end
        table.insert(
          items,
          CoD.OptionsUtility.CreateDvarSettings(
            controller,
            setting.name,
            setting.description,
            "XercesSettings_" .. setting.dvar,
            setting.dvar,
            options,
            nil,
            updateDvar
          )
        )
      end
      return items
    end)
  end
  localSource("XercesSettingsGraphics", GRAPHICS)
  localSource("XercesSettingsController", CONTROLLER)

  DataSources.XercesSettingsTabs = DataSourceHelpers.ListSetup("XercesSettingsTabs", function(controller)
    local function tab(name, dataSourceName, title)
      return {
        models = { tabName = name, tabIcon = "" },
        properties = { tabId = dataSourceName, dataSourceName = dataSourceName, title = title },
      }
    end
    return {
      { models = { tabIcon = CoD.buttonStrings.shoulderl }, properties = { m_mouseDisabled = true } },
      tab("ZOMBIES", "XercesSettingsZombies", "Zombies, when you host"),
      tab("GAMBLING", "XercesSettingsGambling", "Gambling, when you host"),
      tab("ROUNDS", "XercesSettingsRounds", "Rounds, when you host"),
      tab("GRAPHICS", "XercesSettingsGraphics", "Graphics, on this PC"),
      tab("CONTROLLER", "XercesSettingsController", "Controller, on this PC"),
      { models = { tabIcon = CoD.buttonStrings.shoulderr }, properties = { m_mouseDisabled = true } },
    }
  end, true)

  LUI.createMenu.XercesSettingsMenu = function(controller)
    local self = CoD.Menu.NewForUIEditor("XercesSettingsMenu")
    if PreLoadFunc then
      PreLoadFunc(self, controller)
    end
    self.soundSet = "ChooseDecal"
    self:setOwner(controller)
    self:setLeftRight(true, true, 0, 0)
    self:setTopBottom(true, true, 0, 0)
    self:playSound("menu_open", controller)
    self.buttonModel = Engine.CreateModel(Engine.GetModelForController(controller), "XercesSettingsMenu.buttonPrompts")
    self.anyChildUsesUpdateState = true

    local background = CoD.GameSettings_Background.new(self, controller)
    background:setLeftRight(true, true, 0, 0)
    background:setTopBottom(true, true, 0, 0)
    local menuTitle = background.MenuFrame.cac3dTitleIntermediary0.FE3dTitleContainer0.MenuTitle.TextBox1.Label0
    background.MenuFrame.titleLabel:setText(TITLE)
    background.MenuFrame.titleLabel:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
    menuTitle:setText(TITLE)
    menuTitle:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
    background.GameSettingsSelectedItemInfo.GameModeInfo:setAlpha(0)
    background.GameSettingsSelectedItemInfo.GameModeName:setAlpha(0)
    self:addElement(background)
    self.GameSettingsBackground = background

    local options = CoD.Competitive_SettingsList.new(self, controller)
    options:setLeftRight(true, false, 26, 741)
    options:setTopBottom(true, false, 135, 720)
    options.Title.DescTitle:setText("Zombies, when you host")
    options.Title.DescTitle:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
    options.ButtonList:setVerticalCount(11)
    options.ButtonList:setDataSource("XercesSettingsZombies")
    options.ButtonList:setVerticalScrollbar(CoD.verticalScrollbar)
    options.ButtonList.verticalScrollbar:setLeftRight(true, false, 549, 559)
    self:addElement(options)
    self.Options = options

    local tabs = CoD.FE_TabBar.new(self, controller)
    tabs:setLeftRight(true, false, 0, 2496)
    tabs:setTopBottom(true, false, 84, 123)
    tabs.Tabs.grid:setHorizontalCount(5)
    tabs.Tabs.grid:setDataSource("XercesSettingsTabs")
    self:addElement(tabs)
    self.TabList = tabs

    self:registerEventHandler("list_active_changed", function(element, event)
      if element.dataSourceName and element.title then
        options.Title.DescTitle:setText(element.title)
        options.ButtonList:setDataSource(element.dataSourceName)
      end
    end)

    self:AddButtonCallbackFunction(
      self,
      controller,
      Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE,
      nil,
      function(element, menu, controller, model)
        GoBack(self, controller)
        SetPerControllerTableProperty(controller, "disableGameSettingsOptions", nil)
        return true
      end,
      function(element, menu, controller)
        CoD.Menu.SetButtonLabel(menu, Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE, "MENU_BACK")
        return true
      end,
      false
    )

    background.MenuFrame:setModel(self.buttonModel, controller)
    options.id = "Options"

    self:processEvent({ name = "menu_loaded", controller = controller })
    self:processEvent({ name = "update_state", menu = self })
    if not self:restoreState() then
      self.Options:processEvent({ name = "gain_focus", controller = controller })
    end

    LUI.OverrideFunction_CallOriginalSecond(self, "close", function(element)
      element.GameSettingsBackground:close()
      element.Options:close()
      element.TabList:close()
      Engine.UnsubscribeAndFreeModel(
        Engine.GetModel(Engine.GetModelForController(controller), "XercesSettingsMenu.buttonPrompts")
      )
    end)

    if PostLoadFunc then
      PostLoadFunc(self, controller)
    end

    return self
  end
  return
end

-- In a match. The host's game opens a hidden HudElementText for each player
-- whose text is "XERCES <mask> <starting points> <round> <skip> <vote>
-- <starting round>" (xerces_rounds.gsc data_text): skip is 1 on a prime round and 2 on a special
-- round that can be skipped, vote is 1 while the round 999 vote is open. The
-- game sends a script menu's text as it
-- is, where a new LUI event name would be refused (docs/DESIGN.md 7.9). The
-- wheel and coin flip go back to the game script as menu responses under
-- StartMenu_Main, which the game's own handler ignores.
local function hasBit(mask, bit)
  return math.floor(mask / bit) % 2 == 1
end

local function matchData()
  local found = nil
  local function walk(element, depth)
    if found or not element or depth > 4 then
      return
    end
    if element.menuName == "HudElementText" then
      local ok, model = pcall(function()
        return element:getModel()
      end)
      local text = ok and model and Engine.GetModel(model, "text")
      local value = text and Engine.GetModelValue(text)
      if type(value) == "string" then
        -- A host on an older Xerces sends fewer
        local mask, points, round, skip, vote, startRound =
          string.match(value, "^XERCES (%d+) (%d+) (%d+) (%d+) (%d+) (%d+)$")
        if not mask then
          mask, points, round, skip, vote = string.match(value, "^XERCES (%d+) (%d+) (%d+) (%d+) (%d+)$")
        end
        if not mask then
          mask, points, round = string.match(value, "^XERCES (%d+) (%d+) (%d+)$")
        end
        if mask then
          found = {
            mask = tonumber(mask),
            points = tonumber(points),
            round = tonumber(round),
            skip = tonumber(skip or "0"),
            vote = tonumber(vote or "0"),
            startRound = tonumber(startRound or "1"),
          }
          return
        end
      end
    end
    local kid = element:getFirstChild()
    while kid do
      walk(kid, depth + 1)
      kid = kid:getNextSibling()
    end
  end
  for _, root in pairs(LUI.roots) do
    walk(root, 0)
  end
  return found
end

-- A win pays the bet times 2 at round 1, 0.1 more each round, at most 5
-- (xerces_rounds.gsc flip_payout_tenths)
local function payoutTenths(round)
  return math.min(50, 20 + math.max(1, round) - 1)
end

local function row(text, description, action)
  return { models = { displayText = text, description = description, action = action } }
end

DataSources.XercesMatchSettings = ListHelper_SetupDataSource("XercesMatchSettings", function()
  local match = matchData()
  if not match then
    return {
      row(
        "NOT IN USE",
        "This match runs without Xerces' round settings: the host's game did not start Xerces' script, or this is a dedicated server."
      ),
    }
  end
  local rows = {}
  for _, setting in ipairs(SETTINGS) do
    local value = "Off"
    if setting.points then
      value = pointsText(match.points)
    elseif setting.rounds then
      value = roundText(match.startRound)
    elseif hasBit(match.mask, setting.bit) then
      value = "On"
    end
    table.insert(rows, row(setting.name .. ": " .. value, setting.description .. " The host sets it in the lobby."))
  end
  return rows
end, true)

-- The pause menu each controller opened the coin flip from, closed with the
-- bet menu so the game runs again (a solo game is paused under it) before
-- the bet goes in
local flipPauseMenus = {}

local function placeBet(amount)
  return function(self, element, controller, param, menu)
    GoBack(menu, controller)
    if flipPauseMenus[controller] then
      StartMenuGoBack(flipPauseMenus[controller], controller)
      flipPauseMenus[controller] = nil
    end
    -- A menu response ends at its first space
    Engine.SendMenuResponse(controller, "StartMenu_Main", "xerces_flip:" .. amount)
  end
end

DataSources.XercesCoinFlipBets = ListHelper_SetupDataSource("XercesCoinFlipBets", function()
  local match = matchData()
  local tenths = payoutTenths(match and match.round or 1)
  local times = string.format("%d.%d", math.floor(tenths / 10), tenths % 10)
  local rows = {}
  for _, bet in ipairs({ 100, 500, 1000, 2500, 5000, 10000, 25000, 50000 }) do
    table.insert(
      rows,
      row(
        "BET " .. withCommas(bet),
        "Heads wins " .. withCommas(math.floor(bet * tenths / 10)) .. " (x" .. times .. "), tails loses the bet.",
        placeBet(tostring(bet))
      )
    )
  end
  table.insert(
    rows,
    row(
      "ALL IN",
      "Bet every point you have. Heads pays it times " .. times .. ", up to the 4,194,303 points the game holds.",
      placeBet("all")
    )
  )
  return rows
end, true)

require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")

-- Like the host menus (data/ui_scripts/host_menu): a list on the left, the
-- focused row and its description on the right; a row with an action runs it.
local function createListMenu(name, title, dataSource)
  LUI.createMenu[name] = function(controller)
    local self = CoD.Menu.NewForUIEditor(name)
    self.soundSet = "ChooseDecal"
    self:setOwner(controller)
    self:setLeftRight(true, true, 0, 0)
    self:setTopBottom(true, true, 0, 0)
    self:playSound("menu_open", controller)
    self.buttonModel = Engine.CreateModel(Engine.GetModelForController(controller), name .. ".buttonPrompts")
    self.anyChildUsesUpdateState = true

    local background = LUI.UIImage.new()
    background:setLeftRight(true, true, 0, 0)
    background:setTopBottom(true, true, 0, 0)
    background:setRGB(0, 0, 0)
    background:setAlpha(0.7)
    self:addElement(background)

    local list = LUI.UIList.new(self, controller, 2, 0, nil, false, false, 0, 0, false, false)
    list:makeFocusable()
    list:setLeftRight(true, false, 64, 444)
    list:setTopBottom(true, false, 136, 640)
    list:setWidgetType(CoD.List1ButtonLarge_PH)
    list:setVerticalCount(14)
    list:setVerticalScrollbar(CoD.verticalScrollbar)
    list:setDataSource(dataSource)
    self:addElement(list)
    self.Options = list

    local heading = LUI.UIText.new()
    heading:setLeftRight(true, false, 500, 1180)
    heading:setTopBottom(true, false, 136, 172)
    heading:setTTF("fonts/escom.ttf")
    heading:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
    heading:linkToElementModel(list, "displayText", true, function(model)
      heading:setText(Engine.GetModelValue(model) or "")
    end)
    self:addElement(heading)

    local description = LUI.UIText.new()
    description:setLeftRight(true, false, 500, 1180)
    description:setTopBottom(true, false, 188, 210)
    description:setTTF("fonts/default.ttf")
    description:setRGB(0.75, 0.75, 0.75)
    description:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
    description:linkToElementModel(list, "description", true, function(model)
      description:setText(Engine.GetModelValue(model) or "")
    end)
    self:addElement(description)

    local frame = CoD.GenericMenuFrame.new(self, controller)
    frame:setLeftRight(true, true, 0, 0)
    frame:setTopBottom(true, true, 0, 0)
    frame.titleLabel:setText(title)
    pcall(function()
      frame.cac3dTitleIntermediary0.FE3dTitleContainer0.MenuTitle.TextBox1.Label0:setText(title)
    end)
    frame:setModel(self.buttonModel, controller)
    self:addElement(frame)
    self.MenuFrame = frame

    self:AddButtonCallbackFunction(
      list,
      controller,
      Enum.LUIButton.LUI_KEY_XBA_PSCROSS,
      "ENTER",
      function(element, menu, actionController)
        ProcessListAction(self, element, actionController)
        return true
      end,
      function(element, menu)
        CoD.Menu.SetButtonLabel(menu, Enum.LUIButton.LUI_KEY_XBA_PSCROSS, "MENU_SELECT")
        return true
      end,
      false
    )

    self:AddButtonCallbackFunction(
      self,
      controller,
      Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE,
      nil,
      function(element, menu, actionController)
        GoBack(self, actionController)
        return true
      end,
      function(element, menu)
        CoD.Menu.SetButtonLabel(menu, Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE, "MENU_BACK")
        return true
      end,
      false
    )

    list.id = "Options"
    self:processEvent({ name = "menu_loaded", controller = controller })
    self:processEvent({ name = "update_state", menu = self })
    if not self:restoreState() then
      list:processEvent({ name = "gain_focus", controller = controller })
    end

    LUI.OverrideFunction_CallOriginalSecond(self, "close", function(element)
      element.MenuFrame:close()
      element.Options:close()
      Engine.UnsubscribeAndFreeModel(
        Engine.GetModel(Engine.GetModelForController(controller), name .. ".buttonPrompts")
      )
    end)
    return self
  end
end

createListMenu("XercesMatchSettingsMenu", TITLE, "XercesMatchSettings")
createListMenu("XercesCoinFlipMenu", "COIN FLIP", "XercesCoinFlipBets")

-- The pause menu entries, in the game options list in use: the stock one, a
-- mod's own copy (DS4C's StartMenuGameOptions_Death) or the mod-settings
-- template's ModStartMenuGameOptions. They go above the first of these, or
-- last when none is there (as data/ui_scripts/mod_menu does).
local BELOW = {
  MENU_RESTART_LEVEL_CAPS = true,
  MENU_RESTART_MISSION_CAPS = true,
  MENU_END_GAME_CAPS = true,
  MENU_QUIT_GAME_CAPS = true,
  MENU_LEAVE_PARTY_AND_EXIT_CAPS = true,
  ["MAIN MENU"] = true,
  ["QUIT TO DESKTOP"] = true,
}

local function openSettings(self, element, controller)
  OpenPopup(self, "XercesMatchSettingsMenu", controller)
end

-- The wheel turns on the game's HUD, so the pause menu closes (and a solo game
-- runs again) before the request goes in
local function spinWheel(self, element, controller, param, menu)
  StartMenuGoBack(menu, controller)
  Engine.SendMenuResponse(controller, "StartMenu_Main", "xerces_wheel")
end

local function openCoinFlip(self, element, controller, param, menu)
  flipPauseMenus[controller] = menu
  OpenPopup(self, "XercesCoinFlipMenu", controller)
end

-- A vote that the game script counts: the pause menu closes first, as for
-- the wheel
local function sendVote(response)
  return function(self, element, controller, param, menu)
    StartMenuGoBack(menu, controller)
    Engine.SendMenuResponse(controller, "StartMenu_Main", response)
  end
end

-- The pause list keeps the height it was made with, so an added row would
-- need scrolling. BOIII sizes the stock list from BoiiiStartMenuOptionCount
-- right after it is filled (data/ui_scripts/party); a mod's own list made
-- like it (rows 33.6 high: DS4C's) grows a row per item here.
local PAUSE_ROW_HEIGHT = 33.6
local function fit_pause_list(list, key, count)
  if string.lower(key) == "startmenugameoptions" then
    if type(BoiiiStartMenuOptionCount) == "number" then
      BoiiiStartMenuOptionCount = count
    end
    return
  end
  local ok, _, top, _, bottom = pcall(list.getLocalRect, list)
  if not ok or type(top) ~= "number" or type(bottom) ~= "number" then
    return
  end
  local rows = (bottom - top) / PAUSE_ROW_HEIGHT
  local visible = math.floor(rows + 0.5)
  if math.abs(rows - visible) > 0.1 or count <= visible then
    return
  end
  pcall(function()
    list:setVerticalCount(count)
    list:setTopBottom(true, false, top, top + count * PAUSE_ROW_HEIGHT)
  end)
end

local function add_entries(controller, list)
  local key = list.customDataSourceHelper
  local items = key and list[key]
  if type(items) ~= "table" then
    return
  end
  for _, item in ipairs(items) do
    if item.xercesEntry then
      return
    end
  end
  local match = matchData()
  if not match then
    return
  end
  local entries = { { id = "xercesSettings", text = "^3" .. TITLE, action = openSettings } }
  if hasBit(match.mask, WHEEL_BIT) then
    table.insert(entries, { id = "xercesWheel", text = "^3PACK-A-PUNCH WHEEL", action = spinWheel })
  end
  if hasBit(match.mask, COIN_FLIP_BIT) then
    table.insert(entries, { id = "xercesCoinFlip", text = "^3COIN FLIP", action = openCoinFlip })
  end
  if hasBit(match.mask, ROUND_SKIPS_BIT) and match.skip > 0 then
    table.insert(entries, { id = "xercesSkip", text = "^3SKIP ROUND", action = sendVote("xerces_skip") })
  end
  if hasBit(match.mask, ROUND_999_BIT) and match.vote == 1 then
    table.insert(entries, { id = "xercesExfil", text = "^3EXFIL", action = sendVote("xerces_exfil") })
    table.insert(entries, { id = "xercesKeepGoing", text = "^3KEEP GOING", action = sendVote("xerces_keep") })
  end

  local at = #items + 1
  for index, item in ipairs(items) do
    local text = item.model and Engine.GetModel(item.model, "displayText")
    if text and BELOW[Engine.GetModelValue(text)] then
      at = index
      break
    end
  end
  local root = Engine.CreateModel(Engine.GetModelForController(controller), key)
  for offset, entry in ipairs(entries) do
    local model = Engine.GetModel(root, entry.id)
    if model then
      Engine.UnsubscribeAndFreeModel(model)
    end
    model = Engine.CreateModel(root, entry.id)
    Engine.SetModelValue(Engine.CreateModel(model, "displayText"), entry.text)
    Engine.SetModelValue(Engine.CreateModel(model, "action"), entry.action)
    table.insert(items, at + offset - 1, { model = model, xercesEntry = true })
  end
  fit_pause_list(list, key, #items)
end

-- Wrapped once, the first time a pause menu's list uses it; a mod that
-- defines it again (a map restart) gets its new table wrapped.
local function wrap_source(name)
  local source = DataSources[name]
  if type(source) ~= "table" or type(source.prepare) ~= "function" or source.xercesSettings then
    return
  end
  local prepare = source.prepare
  source.prepare = function(controller, list, ...)
    local result = prepare(controller, list, ...)
    pcall(add_entries, controller, list)
    return result
  end
  source.xercesSettings = true
end

local function is_pause_list(name)
  return type(name) == "string" and string.find(string.lower(name), "startmenugameoptions", 1, true) ~= nil
end

local set_data_source = LUI.UIList.setDataSource
LUI.UIList.setDataSource = function(self, source, ...)
  if is_pause_list(source) and Engine.IsInGame() and Engine.CurrentSessionMode() == Enum.eModes.MODE_ZOMBIES then
    pcall(wrap_source, source)
  end
  return set_data_source(self, source, ...)
end
