-- Xerces: the CHARACTERS screen of a Zombies match's pause menu (docs/DESIGN.md 7.10). It lists every character the game's
-- character table has: DS4C's roster with its portraits and outfits when DS4C (or another mod with the same CoD.CharacterTable)
-- is loaded, the base game's characters otherwise. A character mod such as Hyperdimension Neptunia changes how some of them
-- look; the screen says which mod does. The pick goes to the game as a menu response under StartMenu_Main
-- ("xerces_char:<index>:<style>", or "xerces_char:default"), which data/custom_scripts/zm/xerces_characters.gsc applies and
-- holds against other mods' own character scripts (src/client/xerces/character_slots.cpp). The last pick is saved in the
-- dvar xerces_char_saved, which the host's game puts on again in the next match.
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesCharactersInstalled then
  return
end
__xercesCharactersInstalled = true

local TITLE = "CHARACTERS"
local ACCENT = { 1, 0.4157, 0.2588 }
if type(XercesTokens) == "table" and type(XercesTokens.dark) == "table" and XercesTokens.dark.accent_text then
  ACCENT = XercesTokens.dark.accent_text
end

-- The base game's Zombies characters, for a match without a mod that lists them
local BASE_ROSTER = {
  { name = "Dempsey", index = 0, image = "icon_dempsey" },
  { name = "Nikolai", index = 1, image = "icon_nikolai" },
  { name = "Richtofen", index = 2, image = "icon_richtofen" },
  { name = "Takeo", index = 3, image = "icon_takeo" },
  { name = "Floyd", index = 5, image = "icon_boxer", styles = 1 },
  { name = "Jack", index = 6, image = "icon_detective_default", styles = 1 },
  { name = "Jessica", index = 7, image = "icon_femme_default", styles = 1 },
  { name = "Nero", index = 8, image = "icon_magician", styles = 1 },
}

-- A model the character is made of, to find the mod that changed how it looks
local LOOK_MODELS = {
  [0] = "c_zom_dlc_waw_dempsey_mpc_body",
  [1] = "c_zom_dlc_waw_nikolai_fb",
  [2] = "c_zom_dlc_richtofen_cin_fb",
  [3] = "c_zom_dlc_waw_takeo_nohat_fb",
  [5] = "c_zom_zod_boxer_mpc_fb",
  [6] = "c_zom_zod_detective_mpc_fb",
  [7] = "c_zom_zod_femme_mpc_fb",
  [8] = "c_zom_zod_magician_mpc_fb",
}

local function lookOwner(index)
  local model = LOOK_MODELS[index]
  if not model or type(game) ~= "table" or type(game.getassetowner) ~= "function" then
    return nil
  end
  local ok, owner = pcall(game.getassetowner, "xmodel", model)
  if not ok or type(owner) ~= "string" or owner == "" then
    return nil
  end
  if type(game.getmodtitle) == "function" then
    local titleOk, title = pcall(game.getmodtitle, owner)
    if titleOk and type(title) == "string" and title ~= "" then
      -- the colour codes of a Workshop title (^5...) are the mod's own
      return (string.gsub(title, "%^%d", ""))
    end
  end
  return owner
end

-- {name, index, image, styles (count)} for each character; DS4C's CoD.CharacterTable has models.{name, characterIndex,
-- image, styles = {...}}
local function roster()
  local list = {}
  local table_ = type(CoD) == "table" and CoD.CharacterTable
  if type(table_) == "table" then
    for _, entry in ipairs(table_) do
      local models = type(entry) == "table" and entry.models
      -- DS4C's own extra characters (index 500 and up) are made by its scripts, not by the game's character table
      -- (the index is text in DS4C's table)
      local index = type(models) == "table" and tonumber(models.characterIndex)
      if index and index >= 0 and index < 64 then
        local styles = 1
        if type(models.styles) == "table" and #models.styles > 0 then
          styles = #models.styles
        end
        table.insert(list, {
          name = tostring(models.name or ("Character " .. index)),
          index = index,
          image = models.image,
          styles = styles,
        })
      end
    end
  end
  if #list == 0 then
    for _, entry in ipairs(BASE_ROSTER) do
      table.insert(list, { name = entry.name, index = entry.index, image = entry.image, styles = entry.styles or 1 })
    end
  end
  return list
end

local function row(text, description, action, image)
  return { models = { displayText = text, description = description, action = action, image = image or "" } }
end

-- The pause menu each controller opened the screen from, closed with it so the game runs again (a solo game is paused under
-- it) and the new look can be seen
local pauseMenus = {}
local picked = {}

local function send(controller, response)
  Engine.SendMenuResponse(controller, "StartMenu_Main", response)
end

local function choose(character, style)
  return function(self, element, controller, param, menu)
    local pick = character.index .. ":" .. style
    pcall(Engine.SetDvar, "xerces_char_saved", character.index * 16 + style + 1)
    picked[controller] = pick
    send(controller, "xerces_char:" .. pick)
    GoBack(menu, controller)
    if character.styles > 1 then
      GoBack(self, controller)
    end
    if pauseMenus[controller] then
      StartMenuGoBack(pauseMenus[controller], controller)
      pauseMenus[controller] = nil
    end
  end
end

local function chooseDefault(self, element, controller, param, menu)
  pcall(Engine.SetDvar, "xerces_char_saved", 0)
  picked[controller] = nil
  send(controller, "xerces_char:default")
  GoBack(menu, controller)
  if pauseMenus[controller] then
    StartMenuGoBack(pauseMenus[controller], controller)
    pauseMenus[controller] = nil
  end
end

local openStyles
local styleCharacter = {}

-- The model that made a character's portrait: the roster's own mod. A look from another mod is the character mod's.
local function imageOwner(image)
  if type(image) ~= "string" or type(game) ~= "table" or type(game.getassetowner) ~= "function" then
    return nil
  end
  local ok, owner = pcall(game.getassetowner, "image", image)
  if ok and type(owner) == "string" and owner ~= "" then
    return owner
  end
  return nil
end

local function titleOf(id)
  if not id or type(game.getmodtitle) ~= "function" then
    return nil
  end
  local ok, title = pcall(game.getmodtitle, id)
  if ok and type(title) == "string" and title ~= "" then
    return (string.gsub(title, "%^%d", ""))
  end
  return nil
end

-- A list made when the screen opens, so it shows the roster the loaded mods have now and what was picked
local sources = 0
local function charactersSource(controller)
  sources = sources + 1
  local sourceName = "XercesCharacters" .. sources
  DataSources[sourceName] = ListHelper_SetupDataSource(sourceName, function()
    local rows = {
      row(
        "DEFAULT",
        "The character the match gave you. Mods' own character screens work again after this.",
        chooseDefault
      ),
    }
    for _, character in ipairs(roster()) do
      local description = "Character " .. character.index
      if character.styles > 1 then
        description = description .. ", " .. character.styles .. " outfits. Select to pick an outfit."
      else
        description = description .. ". Select to play as this character."
      end
      local owner = lookOwner(character.index)
      if owner and owner ~= titleOf(imageOwner(character.image)) then
        -- only a look from a mod other than the one that lists the character
        description = description .. " Looks from " .. owner .. "."
      end
      local name = character.name
      if picked[controller] and picked[controller]:match("^" .. character.index .. ":") then
        name = "^3" .. name .. " (picked)"
      end
      local action
      if character.styles > 1 then
        action = function(self, element, controller_, param, menu)
          styleCharacter[controller_] = character
          openStyles(self, element, controller_, param, menu)
        end
      else
        action = choose(character, 0)
      end
      table.insert(rows, row(name, description, action, character.image))
    end
    return rows
  end, false)
  return sourceName
end

local function stylesSource(controller)
  sources = sources + 1
  local sourceName = "XercesCharacterStyles" .. sources
  DataSources[sourceName] = ListHelper_SetupDataSource(sourceName, function()
    local character = styleCharacter[controller]
    local rows = {}
    if not character then
      return rows
    end
    for style = 1, character.styles do
      table.insert(
        rows,
        row(
          character.name .. " " .. style,
          "Outfit " .. style .. " of " .. character.styles .. ".",
          choose(character, style - 1),
          character.image
        )
      )
    end
    return rows
  end, false)
  return sourceName
end

require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")

-- Like XERCES SETTINGS (data/ui_scripts/xerces_settings): a list on the left, the focused row, its description and its
-- portrait on the right; a row with an action runs it.
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
    background:setAlpha(0.92)
    self:addElement(background)

    local list = LUI.UIList.new(self, controller, 2, 0, nil, false, false, 0, 0, false, false)
    list:makeFocusable()
    list:setLeftRight(true, false, 64, 444)
    list:setTopBottom(true, false, 136, 640)
    list:setWidgetType(CoD.List1ButtonLarge_PH)
    list:setVerticalCount(14)
    list:setVerticalScrollbar(CoD.verticalScrollbar)
    list:setDataSource(type(dataSource) == "function" and dataSource(controller) or dataSource)
    self:addElement(list)
    self.Options = list

    local portrait = LUI.UIImage.new()
    portrait:setLeftRight(true, false, 500, 756)
    portrait:setTopBottom(true, false, 230, 486)
    portrait:setAlpha(0)
    portrait:linkToElementModel(list, "image", true, function(model)
      local image = Engine.GetModelValue(model)
      if type(image) == "string" and image ~= "" then
        local ok = pcall(function()
          portrait:setImage(RegisterImage(image))
        end)
        portrait:setAlpha(ok and 1 or 0)
      else
        portrait:setAlpha(0)
      end
    end)
    self:addElement(portrait)

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

createListMenu("XercesCharactersMenu", TITLE, charactersSource)
createListMenu("XercesCharacterStylesMenu", "OUTFITS", stylesSource)

openStyles = function(self, element, controller, param, menu)
  OpenPopup(self, "XercesCharacterStylesMenu", controller)
end

-- The pause menu entry, in the game options list in use (the stock one, DS4C's StartMenuGameOptions_Death or a template's),
-- above the rows that leave the match, as XERCES SETTINGS is
local BELOW = {
  MENU_RESTART_LEVEL_CAPS = true,
  MENU_RESTART_MISSION_CAPS = true,
  MENU_END_GAME_CAPS = true,
  MENU_QUIT_GAME_CAPS = true,
  MENU_LEAVE_PARTY_AND_EXIT_CAPS = true,
  ["MAIN MENU"] = true,
  ["QUIT TO DESKTOP"] = true,
}

local function openCharacters(self, element, controller, param, menu)
  pauseMenus[controller] = menu
  OpenPopup(self, "XercesCharactersMenu", controller)
end

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
  if type(items) ~= "table" then
    return
  end
  for _, item in ipairs(items) do
    if item.xercesCharactersEntry then
      return
    end
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
  local id = "xercesCharacters"
  local model = Engine.GetModel(root, id)
  if model then
    Engine.UnsubscribeAndFreeModel(model)
  end
  model = Engine.CreateModel(root, id)
  Engine.SetModelValue(Engine.CreateModel(model, "displayText"), "^3" .. TITLE)
  Engine.SetModelValue(Engine.CreateModel(model, "action"), openCharacters)
  table.insert(items, at, { model = model, xercesCharactersEntry = true })
  fit_pause_list(list, key, #items)
end

local function wrap_source(name)
  local source = DataSources[name]
  if type(source) ~= "table" or type(source.prepare) ~= "function" or source.xercesCharacters then
    return
  end
  local prepare = source.prepare
  source.prepare = function(controller, list, ...)
    local result = prepare(controller, list, ...)
    pcall(add_entry, controller, list)
    return result
  end
  source.xercesCharacters = true
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
