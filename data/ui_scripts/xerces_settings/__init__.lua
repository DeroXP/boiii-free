-- Xerces: the XERCES SETTINGS menu (docs/DESIGN.md 7.9). The Zombies round
-- settings are saved dvars (src/client/xerces/round_settings.cpp) that the
-- host's game reads as a Zombies match loads
-- (data/custom_scripts/zm/xerces_rounds.gsc), so everyone in the match gets
-- the host's settings. In the front end this menu changes them; its button is
-- in the Zombies lobbies (data/ui_scripts/frontend_menus). In a Zombies match
-- the host gets the round HUD and, in the pause menu, XERCES SETTINGS showing
-- the settings the match uses, which changes nothing.
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesSettingsInstalled then
  return
end
__xercesSettingsInstalled = true

local TITLE = "XERCES SETTINGS"
local ACCENT = { 1, 0.4157, 0.2588 }
if type(XercesTokens) == "table" and type(XercesTokens.dark) == "table" and XercesTokens.dark.accent_text then
  ACCENT = XercesTokens.dark.accent_text
end

-- bit: the setting's bit in the mask the host's game sends
local SETTINGS = {
  {
    dvar = "xerces_zm_maxAmmo",
    name = "Max Ammo Every Round",
    bit = 1,
    description = "Everyone gets a Max Ammo at the start of each round.",
  },
  {
    dvar = "xerces_zm_autoPap",
    name = "Auto Pack-a-Punch",
    bit = 2,
    description = "Guns from wall buys and the mystery box come already Pack-a-Punched.",
  },
  {
    dvar = "xerces_zm_startPoints",
    name = "Starting Points",
    points = true,
    description = "The points each player starts the match with. Default keeps the map's own, 500 on most maps.",
  },
  {
    dvar = "xerces_zm_noPerkLimit",
    name = "No Perk Limit",
    bit = 4,
    description = "Players can buy every perk on the map instead of stopping at four.",
  },
  {
    dvar = "xerces_zm_boxStays",
    name = "Mystery Box Stays",
    bit = 8,
    description = "The mystery box never moves to another spot.",
  },
  {
    dvar = "xerces_zm_powerOn",
    name = "Power On at Start",
    bit = 16,
    description = "The power switch turns on when the match starts. Maps without a standard power switch are left as they are.",
  },
  {
    dvar = "xerces_zm_doorsOpen",
    name = "Doors Open at Start",
    bit = 32,
    description = "Every door and debris pile opens for free when the match starts. Doors that need the power open once it is on.",
  },
  {
    dvar = "xerces_zm_roundHud",
    name = "Round HUD",
    bit = 64,
    description = "Shows you how many zombies are left in the round and how long it has taken. Only the host sees it for now.",
  },
}

-- 50000 -> "50,000"; 0 is the map's own
local function pointsText(points)
  if points <= 0 then
    return "Default"
  end
  local text = tostring(math.floor(points))
  local grouped = ""
  while #text > 3 do
    grouped = "," .. string.sub(text, -3) .. grouped
    text = string.sub(text, 1, -4)
  end
  return text .. grouped
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
  local function pointsOptions()
    local current = Engine.DvarInt(nil, "xerces_zm_startPoints") or 0
    local values = { 0, 500, 5000, 50000 }
    local custom = current > 0
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
      local label = pointsText(value)
      if custom and value == current then
        label = label .. " (Custom)"
      end
      table.insert(options, { option = label, value = value, default = value == 0 })
    end
    return options
  end

  DataSources.XercesSettingsZombies = DataSourceHelpers.ListSetup("XercesSettingsZombies", function(controller)
    local items = {}
    for _, setting in ipairs(SETTINGS) do
      local options = {
        { option = "MENU_DISABLED", value = 0, default = true },
        { option = "MENU_ENABLED", value = 1 },
      }
      if setting.points then
        options = pointsOptions()
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

  DataSources.XercesSettingsTabs = DataSourceHelpers.ListSetup("XercesSettingsTabs", function(controller)
    return {
      { models = { tabIcon = CoD.buttonStrings.shoulderl }, properties = { m_mouseDisabled = true } },
      {
        models = { tabName = "ZOMBIES", tabIcon = "" },
        properties = {
          tabId = "XercesSettingsZombies",
          dataSourceName = "XercesSettingsZombies",
          title = "Zombies, when you host",
        },
      },
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
    tabs.Tabs.grid:setHorizontalCount(3)
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

-- In a match. Only the host's own game knows the match's settings: a listen
-- server shares its dvars with the host's client, and the game script
-- (xerces_rounds.gsc) puts what it applied in xerces_zm_match ("<mask>
-- <starting points>") and the round HUD's values in xerces_zm_hud ("<zombies
-- left> <round seconds> <1 while a round is on>"). The other players get the
-- settings as a game message when they spawn. Sending them more would mean
-- registering new strings with the game, which breaks a player's join
-- (docs/DESIGN.md 7.9).
local function isHost()
  if game and game.ishost then
    return game.ishost()
  end
  local dedicated = false
  pcall(function()
    dedicated = Dvar.cl_connected_to_dedi and Dvar.cl_connected_to_dedi:get()
  end)
  return Engine.IsLobbyHost(Enum.LobbyType.LOBBY_TYPE_GAME) and not dedicated
end

local function hostDvar(name)
  if not isHost() then
    return ""
  end
  local ok, value = pcall(Engine.DvarString, nil, name)
  return ok and type(value) == "string" and value or ""
end

local function matchSettings()
  local mask, points = string.match(hostDvar("xerces_zm_match"), "^(%d+) (%d+)$")
  if not mask then
    return nil
  end
  return { mask = tonumber(mask), points = tonumber(points) }
end

-- 83 -> "1:23"; 3723 -> "1:02:03"
local function clock(seconds)
  local hours = math.floor(seconds / 3600)
  local minutes = math.floor(seconds / 60) % 60
  local rest = seconds % 60
  if hours > 0 then
    return string.format("%d:%02d:%02d", hours, minutes, rest)
  end
  return string.format("%d:%02d", minutes, rest)
end

-- The XP bar's (data/ui_scripts/xp_bar): no round HUD over menus, the
-- scoreboard or cameras
local HIDE_BITS = {
  Enum.UIVisibilityBit.BIT_SCOREBOARD_OPEN,
  Enum.UIVisibilityBit.BIT_UI_ACTIVE,
  Enum.UIVisibilityBit.BIT_HIDE_FOR_FULLSCREEN_MENU,
  Enum.UIVisibilityBit.BIT_CAMERA_ANIM_HIDING_HUD,
  Enum.UIVisibilityBit.BIT_IN_KILLCAM,
  Enum.UIVisibilityBit.BIT_FINAL_KILLCAM,
  Enum.UIVisibilityBit.BIT_GAME_ENDED,
  Enum.UIVisibilityBit.BIT_SPECTATING_CLIENT,
  Enum.UIVisibilityBit.BIT_IS_DEMO_PLAYING,
}

local function hudHidden(controller)
  if not Engine.IsVisibilityBitSet(controller, Enum.UIVisibilityBit.BIT_HUD_VISIBLE) then
    return true
  end
  for _, bit in ipairs(HIDE_BITS) do
    if Engine.IsVisibilityBitSet(controller, bit) then
      return true
    end
  end
  return false
end

-- Zombies left and the round time, top left
local function createRoundHud(root, controller)
  local hud = LUI.UIElement.new()
  hud:setLeftRight(true, false, 24, 344)
  hud:setTopBottom(true, false, 20, 76)
  hud:setAlpha(0)
  root:addElement(hud)

  local function text(left, right, top, color)
    local element = LUI.UIText.new()
    element:setLeftRight(true, false, left, right)
    element:setTopBottom(true, false, top, top + 24)
    element:setTTF("fonts/RefrigeratorDeluxe-Regular.ttf")
    element:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
    element:setRGB(color[1], color[2], color[3])
    hud:addElement(element)
    return element
  end
  local grey = { 0.75, 0.75, 0.75 }
  local white = { 1, 1, 1 }
  text(0, 150, 0, grey):setText("ZOMBIES LEFT")
  local left = text(156, 320, 0, white)
  local timeLabel = text(0, 150, 30, grey)
  local time = text(156, 320, 30, white)

  -- On the UI root, as timers don't run inside a hidden element
  root:addElement(LUI.UITimer.newElementTimer(250, false, function()
    local count, seconds, running = string.match(hostDvar("xerces_zm_hud"), "^(%d+) (%d+) (%d)$")
    if not count or hudHidden(controller) then
      hud:setAlpha(0)
      return
    end
    left:setText(count)
    if running == "1" or seconds ~= "0" then
      timeLabel:setText(running == "1" and "ROUND TIME" or "LAST ROUND")
      time:setText(clock(tonumber(seconds)))
    else
      -- Before the first round starts
      timeLabel:setText("")
      time:setText("")
    end
    hud:setAlpha(1)
  end))
  root.xercesRoundHud = hud
end

local primary = Engine.GetPrimaryController()
local primaryRoot = LUI.roots["UIRoot" .. primary]
if primaryRoot and not primaryRoot.xercesRoundHud then
  createRoundHud(primaryRoot, primary)
end

local function row(text, description)
  return { models = { displayText = text, description = description } }
end

DataSources.XercesMatchSettings = ListHelper_SetupDataSource("XercesMatchSettings", function()
  local match = matchSettings()
  if not match then
    return {
      row(
        "NOT IN USE",
        "This match runs without Xerces' round settings: Xerces' game script did not start, or this is a dedicated server."
      ),
    }
  end
  local rows = {}
  for _, setting in ipairs(SETTINGS) do
    local value = "Off"
    if setting.points then
      value = pointsText(match.points)
    elseif math.floor(match.mask / setting.bit) % 2 == 1 then
      value = "On"
    end
    table.insert(rows, row(setting.name .. ": " .. value, setting.description .. " Change it in the lobby."))
  end
  return rows
end, true)

require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")

-- Like the host menus (data/ui_scripts/host_menu): a list on the left, the
-- focused row and its description on the right. The rows do nothing.
LUI.createMenu.XercesMatchSettingsMenu = function(controller)
  local name = "XercesMatchSettingsMenu"
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
  list:setDataSource("XercesMatchSettings")
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
  frame.titleLabel:setText(TITLE)
  pcall(function()
    frame.cac3dTitleIntermediary0.FE3dTitleContainer0.MenuTitle.TextBox1.Label0:setText(TITLE)
  end)
  frame:setModel(self.buttonModel, controller)
  self:addElement(frame)
  self.MenuFrame = frame

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
    Engine.UnsubscribeAndFreeModel(Engine.GetModel(Engine.GetModelForController(controller), name .. ".buttonPrompts"))
  end)
  return self
end

-- The host's pause menu entry, in the game options list in use: the stock
-- one, a mod's own copy (DS4C's StartMenuGameOptions_Death) or the
-- mod-settings template's ModStartMenuGameOptions. It goes above the first of
-- these, or last when none is there (as data/ui_scripts/mod_menu does).
local BELOW = {
  MENU_RESTART_LEVEL_CAPS = true,
  MENU_RESTART_MISSION_CAPS = true,
  MENU_END_GAME_CAPS = true,
  MENU_QUIT_GAME_CAPS = true,
  MENU_LEAVE_PARTY_AND_EXIT_CAPS = true,
  ["MAIN MENU"] = true,
  ["QUIT TO DESKTOP"] = true,
}

local function open_menu(self, element, controller)
  OpenPopup(self, "XercesMatchSettingsMenu", controller)
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

local function add_entry(controller, list)
  local key = list.customDataSourceHelper
  local items = key and list[key]
  if type(items) ~= "table" or not isHost() then
    return
  end
  for _, item in ipairs(items) do
    if item.xercesSettings then
      return
    end
  end
  local root = Engine.CreateModel(Engine.GetModelForController(controller), key)
  local model = Engine.GetModel(root, "xercesSettings")
  if model then
    Engine.UnsubscribeAndFreeModel(model)
  end
  model = Engine.CreateModel(root, "xercesSettings")
  Engine.SetModelValue(Engine.CreateModel(model, "displayText"), "^3" .. TITLE)
  Engine.SetModelValue(Engine.CreateModel(model, "action"), open_menu)
  local at = #items + 1
  for index, item in ipairs(items) do
    local text = item.model and Engine.GetModel(item.model, "displayText")
    if text and BELOW[Engine.GetModelValue(text)] then
      at = index
      break
    end
  end
  table.insert(items, at, { model = model, xercesSettings = true })
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
    pcall(add_entry, controller, list)
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
