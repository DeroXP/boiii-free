-- Xerces: the CHARACTERS screen of a Zombies match's pause menu (docs/DESIGN.md 7.10). It lists every character the game's
-- character table has: DS4C's roster with its portraits and outfits when DS4C (or another mod with the same CoD.CharacterTable)
-- is loaded, the base game's characters otherwise. A character mod such as Hyperdimension Neptunia changes how some of them
-- look; the screen says which mod does. The pick goes to the game as a menu response under StartMenu_Main
-- ("xerces_char:<index>:<style>", or "xerces_char:default"), which data/custom_scripts/zm/xerces_characters.gsc applies and
-- holds against other mods' own character scripts (src/client/xerces/character_slots.cpp). The last pick is saved in the
-- dvar xerces_char_saved, which the host's game puts on again in the next match.
--
-- Every other character mod's characters, and the game's own where a mod replaced them, are slots too (DESIGN.md 7.12):
-- the game gives them as extra outfits of the table in use (game.getcharacterlooks), and the screen lists them by mod.
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
-- The screens open now, by name and controller: a pick made on a second screen closes the first too
local openMenus = {}

local function closeAll(controller, menu)
  GoBack(menu, controller)
  local first = openMenus.XercesCharactersMenu and openMenus.XercesCharactersMenu[controller]
  if first and first ~= menu then
    GoBack(first, controller)
  end
  if pauseMenus[controller] then
    StartMenuGoBack(pauseMenus[controller], controller)
    pauseMenus[controller] = nil
  end
end

local function send(controller, response)
  Engine.SendMenuResponse(controller, "StartMenu_Main", response)
end

local function choose(character, style)
  return function(self, element, controller, param, menu)
    local pick = character.index .. ":" .. style
    pcall(Engine.SetDvar, "xerces_char_saved", character.index * 16 + style + 1)
    pcall(Engine.SetDvar, "xerces_char_saved_look", 0)
    picked[controller] = pick
    send(controller, "xerces_char:" .. pick)
    closeAll(controller, menu)
  end
end

local function chooseDefault(self, element, controller, param, menu)
  pcall(Engine.SetDvar, "xerces_char_saved", 0)
  pcall(Engine.SetDvar, "xerces_char_saved_look", 0)
  picked[controller] = nil
  send(controller, "xerces_char:default")
  closeAll(controller, menu)
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

-- A mod's Workshop title cut to one row of the list (about 22 letters beside the count): the name in front of its
-- brackets, version and subtitle. "[ Ascendance ] 0.9.9.8.5 Aesthetic" is Ascendance, "Avali: Solspace - Character
-- Pack" is Avali: Solspace.
local ROW_LETTERS = 22
local function shortTitle(title)
  local name = title
  local bracketed = string.match(name, "^%s*[%[%(]%s*(.-)%s*[%]%)]")
  if bracketed and #bracketed >= 3 then
    name = bracketed
  else
    name = string.gsub(name, "%s*%b[]", "")
    name = string.gsub(name, "%s*%b()", "")
  end
  name = string.gsub(name, "^%s+", "")
  name = string.gsub(name, "%s+$", "")
  for _, cut in ipairs({ " - ", ": " }) do
    if #name > ROW_LETTERS then
      local at = string.find(name, cut, 1, true)
      if at and at > 6 then
        name = string.sub(name, 1, at - 1)
      end
    end
  end
  if #name > ROW_LETTERS then
    name = string.sub(name, 1, ROW_LETTERS - 2) .. ".."
  end
  if name == "" then
    return title
  end
  return name
end

-- A mod that replaces the base characters' models names nobody: its rows are named after what the models are made of.
-- What all of a mod's model names start and end with says nothing ("xmodel_noire", "xmodel_blanc" are Noire and Blanc;
-- "dv_coolerfz_fb", "dv_brolyfz_fb" are Cooler and Broly).
local function modelNames(hints)
  local list = {}
  local seen = {}
  for _, hint in ipairs(hints) do
    if hint ~= "" and not seen[hint] then
      seen[hint] = true
      table.insert(list, hint)
    end
  end
  local front, back = 0, 0
  if #list > 1 then
    local first = list[1]
    front = #first
    back = #first
    for i = 2, #list do
      local other = list[i]
      local n = 0
      while n < front and n < #other and string.sub(first, n + 1, n + 1) == string.sub(other, n + 1, n + 1) do
        n = n + 1
      end
      front = n
      n = 0
      while n < back and n < #other and string.sub(first, #first - n, #first - n) == string.sub(other, #other - n, #other - n) do
        n = n + 1
      end
      back = n
    end
    -- never down to nothing
    for _, hint in ipairs(list) do
      if #hint - front - back < 2 then
        back = 0
      end
    end
    for _, hint in ipairs(list) do
      if #hint - front < 2 then
        front = 0
      end
    end
  end
  local names = {}
  for _, hint in ipairs(list) do
    local name = string.sub(hint, front + 1, #hint - back)
    name = string.gsub(name, "_", " ")
    name = string.gsub(name, "^%s+", "")
    name = string.gsub(name, "%s+$", "")
    if name == "" then
      name = hint
    end
    names[hint] = string.upper(string.sub(name, 1, 1)) .. string.sub(name, 2)
  end
  return names
end

-- The other mods' characters and the game's own, as the game lists them (src/client/xerces/character_slots.hpp): one
-- group per mod, in the stack's order, the game's own last. {id, title, looks = {{name, hint, image, index, style, id}}}
local function modLooks()
  local groups = {}
  local byId = {}
  if type(game) ~= "table" or type(game.getcharacterlooks) ~= "function" then
    return groups
  end
  local ok, text = pcall(game.getcharacterlooks)
  if not ok or type(text) ~= "string" then
    return groups
  end
  for line in string.gmatch(text, "[^\n]+") do
    local fields = {}
    for field in string.gmatch(line .. "\t", "(.-)\t") do
      table.insert(fields, field)
    end
    if #fields >= 8 then
      local source = fields[1]
      local group = byId[source]
      if not group then
        local title = "Original Characters"
        if source ~= "" then
          title = titleOf(source) or source
        end
        group = { id = source, title = title, looks = {}, names = {} }
        byId[source] = group
        table.insert(groups, group)
      end
      local name = fields[2]
      if name == "" then
        name = "Character"
      elseif string.find(name, "^[A-Z0-9_]+$") and string.find(name, "_", 1, true) then
        local localized = Engine.Localize(name)
        if type(localized) == "string" and localized ~= "" then
          name = localized
        end
      end
      table.insert(group.looks, {
        name = name,
        hint = fields[3],
        image = fields[4],
        index = tonumber(fields[5]) or 0,
        style = tonumber(fields[6]) or 0,
        -- kept as text: a number this large is set as "1.75927e+09" and read back as 1
        id = fields[7],
        inPlace = fields[9] == "1",
      })
    end
  end
  for _, group in ipairs(groups) do
    local hints = {}
    for _, look in ipairs(group.looks) do
      if look.inPlace then
        table.insert(hints, look.hint)
      end
    end
    local names = modelNames(hints)
    local used = {}
    for _, look in ipairs(group.looks) do
      if look.inPlace and names[look.hint] then
        look.inPlaceOf = look.name
        look.name = names[look.hint]
      end
      -- a name the mod uses more than once is numbered
      local count = (used[look.name] or 0) + 1
      used[look.name] = count
      if count > 1 then
        look.name = look.name .. " " .. count
      end
    end
  end
  return groups
end

local openLooks
local lookGroup = {}

local function chooseLook(look)
  return function(self, element, controller, param, menu)
    local pick = look.index .. ":" .. look.style
    pcall(Engine.SetDvar, "xerces_char_saved", look.index * 16 + look.style + 1)
    pcall(Engine.SetDvar, "xerces_char_saved_look", tostring(look.id))
    picked[controller] = pick
    send(controller, "xerces_char:" .. pick)
    closeAll(controller, menu)
  end
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
    for _, group in ipairs(modLooks()) do
      local description = group.title
        .. ": "
        .. #group.looks
        .. " characters from this mod, each a slot of its own. Select to see them."
      if group.id == "" then
        description = #group.looks
          .. " characters as the game made them, where a mod changed how they look. Select to see them."
      end
      local name = shortTitle(group.title) .. " (" .. #group.looks .. ")"
      for _, look in ipairs(group.looks) do
        if picked[controller] == look.index .. ":" .. look.style then
          name = "^3" .. name
          break
        end
      end
      table.insert(
        rows,
        row(name, description, function(self, element, controller_, param, menu)
          lookGroup[controller_] = group
          openLooks(self, element, controller_, param, menu)
        end)
      )
    end
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
      local pickedIndex, pickedStyle = string.match(picked[controller] or "", "^(%d+):(%d+)$")
      -- an outfit past the character's own is another mod's character
      if tonumber(pickedIndex) == character.index and tonumber(pickedStyle) < character.styles then
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

local function looksSource(controller)
  sources = sources + 1
  local sourceName = "XercesCharacterLooks" .. sources
  DataSources[sourceName] = ListHelper_SetupDataSource(sourceName, function()
    local group = lookGroup[controller]
    local rows = {}
    if not group then
      return rows
    end
    for _, look in ipairs(group.looks) do
      local description = "Select to play as this character."
      if look.hint ~= "" then
        description = "Model: " .. look.hint .. ". " .. description
      end
      if look.inPlaceOf then
        description = "In "
          .. look.inPlaceOf
          .. "'s place: the game still names and voices you as "
          .. look.inPlaceOf
          .. ". "
          .. description
      end
      local name = look.name
      if picked[controller] == look.index .. ":" .. look.style then
        name = "^3" .. name .. " (picked)"
      end
      table.insert(rows, row(name, description, chooseLook(look), look.image))
    end
    return rows
  end, false)
  return sourceName
end

require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")

-- Like XERCES SETTINGS (data/ui_scripts/xerces_settings): a list on the left, the focused row, its description and its
-- portrait on the right; a row with an action runs it.
local function createListMenu(name, title, dataSource, alpha)
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
    background:setAlpha(alpha or 0.92)
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
    openMenus[name] = openMenus[name] or {}
    openMenus[name][controller] = self
    self:processEvent({ name = "menu_loaded", controller = controller })
    self:processEvent({ name = "update_state", menu = self })
    if not self:restoreState() then
      list:processEvent({ name = "gain_focus", controller = controller })
    end

    LUI.OverrideFunction_CallOriginalSecond(self, "close", function(element)
      if openMenus[name][controller] == element then
        openMenus[name][controller] = nil
      end
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
createListMenu("XercesCharacterStylesMenu", "OUTFITS", stylesSource, 1)
createListMenu("XercesCharacterLooksMenu", TITLE, looksSource, 1)

openStyles = function(self, element, controller, param, menu)
  OpenPopup(self, "XercesCharacterStylesMenu", controller)
end

openLooks = function(self, element, controller, param, menu)
  OpenPopup(self, "XercesCharacterLooksMenu", controller)
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
