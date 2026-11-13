-- Xerces: the CHARACTERS screen of a Zombies match (docs/DESIGN.md 7.10, 7.12, 7.13). One screen: a tab for the character
-- list in use and one for every other character mod (and the game's own characters where a mod replaced them), the tab's
-- characters in a list on the left, and on the right the player's own character in the world, seen from the front, wearing
-- whatever the list is on.
--
-- The game gives the characters (game.getcharacterlooks, src/client/xerces/character_slots.cpp): each is a character index
-- and an outfit index of the list in use. The pick goes to the game as a menu response under StartMenu_Main
-- ("xerces_char:<index>:<style>", or "xerces_char:default"), which data/custom_scripts/zm/xerces_characters.gsc applies and
-- holds against other mods' own character scripts. The last pick is saved in the dvars xerces_char_saved and
-- xerces_char_saved_look, and the host's game puts it on again in the next match.
--
-- The preview needs the match to run, and a solo match stands still under the pause menu. So the pause menu's entry closes
-- the pause menu and opens the screen over the HUD; "xerces_char:open" starts the preview in the game script (its camera, and
-- for a player alone the same safety a pause gives), "xerces_char:preview:<index>:<style>" puts a character on while the
-- list is on it, "xerces_char:alive" says every two seconds that the screen is still open, "xerces_char:close" ends it.
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesCharactersInstalled then
  return
end
__xercesCharactersInstalled = true

require("ui.uieditor.widgets.Scrollbars.verticalScrollbar")
pcall(require, "ui.uieditor.widgets.Lobby.Common.FE_TabBar")

local TITLE = "CHARACTERS"
local MENU = "XercesCharactersMenu"
local ACCENT = { 1, 0.4157, 0.2588 }
if type(XercesTokens) == "table" and type(XercesTokens.dark) == "table" and XercesTokens.dark.accent_text then
  ACCENT = XercesTokens.dark.accent_text
end

-- The base game's Zombies characters, for a game side that does not list them (an older Xerces, or a table it could not read)
local BASE_ROSTER = {
  { name = "Dempsey", index = 0, image = "icon_dempsey" },
  { name = "Nikolai", index = 1, image = "icon_nikolai" },
  { name = "Richtofen", index = 2, image = "icon_richtofen" },
  { name = "Takeo", index = 3, image = "icon_takeo" },
  { name = "Floyd", index = 5, image = "icon_boxer" },
  { name = "Jack", index = 6, image = "icon_detective_default" },
  { name = "Jessica", index = 7, image = "icon_femme_default" },
  { name = "Nero", index = 8, image = "icon_magician" },
}

local function titleOf(id)
  if not id or id == "" or type(game) ~= "table" or type(game.getmodtitle) ~= "function" then
    return nil
  end
  local ok, title = pcall(game.getmodtitle, id)
  if ok and type(title) == "string" and title ~= "" then
    -- the colour codes of a Workshop title (^5...) are the mod's own
    return (string.gsub(title, "%^%d", ""))
  end
  return nil
end

-- A mod's Workshop title cut to fit a tab or a line: the name in front of its brackets, version and subtitle.
-- "[ Ascendance ] 0.9.9.8.5 Aesthetic" is Ascendance, "Avali: Solspace - Character Pack" is Avali: Solspace.
local function shortTitle(title, letters)
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
    if #name > letters then
      local at = string.find(name, cut, 1, true)
      if at and at > 4 then
        name = string.sub(name, 1, at - 1)
      end
    end
  end
  if #name > letters then
    name = string.sub(name, 1, letters - 2) .. ".."
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
      while
        n < back
        and n < #other
        and string.sub(first, #first - n, #first - n) == string.sub(other, #other - n, #other - n)
      do
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

-- Every character there is to pick, in groups: the list in use first (the main mod's, or the game's), then each other mod
-- in the stack's order, the game's own last. {id, title, looks = {{name, hint, image, index, style, id, inPlaceOf}}}
local function groups()
  local list = {}
  local byId = {}
  local owner = ""
  local text = nil
  if type(game) == "table" and type(game.getcharacterlooks) == "function" then
    local ok, result = pcall(game.getcharacterlooks)
    if ok and type(result) == "string" then
      text = result
    end
  end
  for line in string.gmatch(text or "", "[^\n]+") do
    local fields = {}
    for field in string.gmatch(line .. "\t", "(.-)\t") do
      table.insert(fields, field)
    end
    if fields[1] == "@" then
      -- whose list is in use
      owner = fields[2] or ""
    elseif #fields >= 9 then
      local source = fields[1]
      local group = byId[source]
      if not group then
        local title = titleOf(source) or source
        if source == "" then
          title = owner == "" and "Characters" or "Original Characters"
        end
        group = { id = source, title = title, looks = {}, inUse = source == owner }
        byId[source] = group
        table.insert(list, group)
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
        added = fields[8] == "1",
        inPlace = fields[9] == "1",
      })
    end
  end
  -- the list in use goes first, whatever its place in the stack; the others keep the stack's order
  for index, group in ipairs(list) do
    if group.inUse and index > 1 then
      table.remove(list, index)
      table.insert(list, 1, group)
      break
    end
  end
  -- a mod's own character screen names the characters of the list in use better than the list does (DS4C's
  -- CoD.CharacterTable: models.name, models.characterIndex as text, models.image)
  local roster = {}
  if type(CoD) == "table" and type(CoD.CharacterTable) == "table" then
    for _, entry in ipairs(CoD.CharacterTable) do
      local models = type(entry) == "table" and entry.models
      local index = type(models) == "table" and tonumber(models.characterIndex)
      if index and not roster[index] then
        roster[index] = { name = models.name, image = models.image }
      end
    end
  end
  for _, group in ipairs(list) do
    if group.inUse then
      for _, look in ipairs(group.looks) do
        local known = roster[look.index]
        if known and not look.added then
          if type(known.name) == "string" and known.name ~= "" then
            look.name = known.name
          end
          if type(known.image) == "string" and known.image ~= "" then
            look.image = known.image
          end
        end
      end
    end
  end
  for _, group in ipairs(list) do
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
  if #list == 0 then
    -- no list from the game: the base characters, one outfit each
    local group = { id = "", title = "Characters", looks = {}, inUse = true }
    for _, entry in ipairs(BASE_ROSTER) do
      table.insert(
        group.looks,
        { name = entry.name, hint = "", image = entry.image, index = entry.index, style = 0, id = "0" }
      )
    end
    table.insert(list, group)
  end
  return list
end

local function row(text, description, action, image, preview)
  return {
    models = {
      displayText = text,
      description = description,
      action = action,
      image = image or "",
      preview = preview or "",
    },
  }
end

-- Per controller: the pause menu the screen was opened from (only when it is open under it), what was picked, whether the
-- preview runs, the groups the open screen shows and the one its list is on
local pauseMenus = {}
local picked = {}
local previewing = {}
local wanted = {}
local sent = {}
local shown = {}
local current = {}
local openMenus = {}

local function send(controller, response)
  Engine.SendMenuResponse(controller, "StartMenu_Main", response)
end

local function closeAll(controller, menu)
  -- the pick itself ends the preview in the game script
  previewing[controller] = nil
  GoBack(menu, controller)
  if pauseMenus[controller] then
    StartMenuGoBack(pauseMenus[controller], controller)
    pauseMenus[controller] = nil
  end
end

local function choose(look, inUse)
  return function(self, element, controller, param, menu)
    local pick = look.index .. ":" .. look.style
    pcall(Engine.SetDvar, "xerces_char_saved", tostring(look.index * 16 + look.style + 1))
    -- found again by what it is, whichever mods are loaded next time: a slot alone lands on another character
    -- under another stack (review, 2026-11-13)
    pcall(Engine.SetDvar, "xerces_char_saved_look", tostring(look.id or "0"))
    picked[controller] = pick
    send(controller, "xerces_char:" .. pick)
    closeAll(controller, menu)
  end
end

local function chooseDefault(self, element, controller, param, menu)
  pcall(Engine.SetDvar, "xerces_char_saved", "0")
  pcall(Engine.SetDvar, "xerces_char_saved_look", "0")
  picked[controller] = nil
  send(controller, "xerces_char:default")
  closeAll(controller, menu)
end

local function rowsOf(controller, group)
  local rows = {
    row(
      "DEFAULT",
      "The character the match gave you. Mods' own character screens work again after this.",
      chooseDefault
    ),
  }
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
    local pick = look.index .. ":" .. look.style
    local name = look.name
    if picked[controller] == pick then
      name = "^3" .. name .. " (picked)"
    end
    table.insert(rows, row(name, description, choose(look, group.inUse), look.image, pick))
  end
  return rows
end

-- One list a controller and group, made once and read again each time it is shown
local function rowsSource(controller, index)
  local name = "XercesCharacterRows_" .. controller .. "_" .. index
  if not DataSources[name] then
    DataSources[name] = ListHelper_SetupDataSource(name, function()
      return rowsOf(controller, shown[controller] and shown[controller][index])
    end, true)
  end
  return name
end

local TAB_LETTERS = 13
local function tabsSource(controller)
  local name = "XercesCharacterTabs_" .. controller
  if not DataSources[name] then
    DataSources[name] = DataSourceHelpers.ListSetup(name, function()
      local tabs = {
        { models = { tabIcon = CoD.buttonStrings.shoulderl }, properties = { m_mouseDisabled = true } },
      }
      for index, group in ipairs(shown[controller] or {}) do
        local title = shortTitle(group.title, TAB_LETTERS)
        table.insert(tabs, {
          models = { tabName = string.upper(title), tabIcon = "" },
          properties = { tabId = "xercesGroup" .. index, xercesGroup = index },
        })
      end
      table.insert(
        tabs,
        { models = { tabIcon = CoD.buttonStrings.shoulderr }, properties = { m_mouseDisabled = true } }
      )
      return tabs
    end, true)
  end
  return name
end

-- The HUD's own menu: a screen opened from it does not stop a solo match the way one opened from the pause menu does
local function hudMenu()
  for _, root in pairs(LUI.roots) do
    local ok, kid = pcall(function()
      return root:getFirstChild()
    end)
    local count = 0
    while ok and kid and count < 64 do
      if kid.menuName == "HUD" then
        return kid
      end
      kid = kid:getNextSibling()
      count = count + 1
    end
  end
  return nil
end

-- An element's children
local function kidsOf(element)
  local kids = {}
  local ok, kid = pcall(function()
    return element:getFirstChild()
  end)
  local count = 0
  while ok and kid and count < 200 do
    kids[#kids + 1] = kid
    kid = kid:getNextSibling()
    count = count + 1
  end
  return kids
end

LUI.createMenu[MENU] = function(controller)
  local self = CoD.Menu.NewForUIEditor(MENU)
  -- a menu opened over the HUD darkens everything under it (the character stood at under half its brightness, seen
  -- 2026-11-12): with the preview this screen asks for none of that and draws its own dark panel on the left only.
  -- disableBlur must stay unset: with it the screen got no keys and Escape opened the pause menu over it.
  if previewing[controller] == true then
    self.disableDarkenElement = true
  end
  self.soundSet = "ChooseDecal"
  self:setOwner(controller)
  self:setLeftRight(true, true, 0, 0)
  self:setTopBottom(true, true, 0, 0)
  self:playSound("menu_open", controller)
  self.buttonModel = Engine.CreateModel(Engine.GetModelForController(controller), MENU .. ".buttonPrompts")
  self.anyChildUsesUpdateState = true

  shown[controller] = groups()
  current[controller] = 1
  local live = previewing[controller] == true

  -- with the preview the right of the screen is the world and the player's own character; without it (the screen opened
  -- under the pause menu) the whole screen is dark
  local panel = LUI.UIImage.new()
  if live then
    panel:setLeftRight(true, false, 0, 480)
  else
    panel:setLeftRight(true, true, 0, 0)
  end
  panel:setTopBottom(true, true, 0, 0)
  panel:setRGB(0, 0, 0)
  -- dark enough that what the game's scripts draw on the left of the HUD does not read through it
  panel:setAlpha(live and 0.95 or 0.92)
  self:addElement(panel)

  local list = LUI.UIList.new(self, controller, 2, 0, nil, false, false, 0, 0, false, false)
  list:makeFocusable()
  list:setLeftRight(true, false, 64, 444)
  list:setTopBottom(true, false, 136, 568)
  list:setWidgetType(CoD.List1ButtonLarge_PH)
  list:setVerticalCount(12)
  list:setVerticalScrollbar(CoD.verticalScrollbar)
  list:setDataSource(rowsSource(controller, 1))
  self:addElement(list)
  self.Options = list

  -- the portrait of the character the list is on, as DS4C's screen shows them (the game's eight and every table's
  -- characters have one; a mod that lists no characters gives none, and DEFAULT shows none). With the preview at the
  -- top right, clear of the character the camera shows; without it where beta.18's screen had it (2026-11-13).
  local portrait = LUI.UIImage.new()
  if live then
    portrait:setLeftRight(false, true, -216, -56)
    portrait:setTopBottom(true, false, 120, 280)
  else
    portrait:setLeftRight(true, false, 500, 756)
    portrait:setTopBottom(true, false, 230, 486)
  end
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

  local source = LUI.UIText.new()
  source:setLeftRight(true, false, 64, 460)
  source:setTopBottom(true, false, 578, 598)
  source:setTTF("fonts/default.ttf")
  source:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
  source:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
  self:addElement(source)

  local function showGroup(index)
    local group = shown[controller][index]
    if not group then
      return
    end
    current[controller] = index
    local count = #group.looks
    source:setText(group.title .. ": " .. count .. (count == 1 and " character" or " characters"))
    list:setDataSource(rowsSource(controller, index))
  end

  local description = LUI.UIText.new()
  description:setLeftRight(true, false, 64, 460)
  description:setTopBottom(true, false, 604, 622)
  description:setTTF("fonts/default.ttf")
  description:setRGB(0.75, 0.75, 0.75)
  description:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
  description:linkToElementModel(list, "description", true, function(model)
    description:setText(Engine.GetModelValue(model) or "")
  end)
  self:addElement(description)

  -- the character the list is on goes to the game, at most seven times a second, and the game hears every two seconds
  -- that the screen is still open
  panel:linkToElementModel(list, "preview", true, function(model)
    wanted[controller] = Engine.GetModelValue(model) or ""
  end)
  if live then
    sent[controller] = nil
    local ticks = 0
    self:addElement(LUI.UITimer.newElementTimer(140, false, function()
      ticks = ticks + 1
      local pick = wanted[controller]
      if pick and pick ~= "" and pick ~= sent[controller] then
        sent[controller] = pick
        send(controller, "xerces_char:preview:" .. pick)
      elseif ticks % 14 == 0 then
        send(controller, "xerces_char:alive")
      end
    end))
  end

  if live then
    -- the game's own menu frame lays a dark layer over the whole screen, which would hide the character: a title and a
    -- line of keys instead
    local title = LUI.UIText.new()
    title:setLeftRight(true, false, 64, 460)
    title:setTopBottom(true, false, 34, 74)
    title:setTTF("fonts/escom.ttf")
    title:setRGB(ACCENT[1], ACCENT[2], ACCENT[3])
    title:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
    title:setText(TITLE)
    self:addElement(title)
    local keys = LUI.UIText.new()
    keys:setLeftRight(true, false, 64, 460)
    keys:setTopBottom(true, false, 676, 692)
    keys:setTTF("fonts/default.ttf")
    keys:setRGB(0.6, 0.6, 0.6)
    keys:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
    keys:setText("Select: play as it.   Back: keep what you wear.   LB / RB or Q / E: another mod.")
    self:addElement(keys)
  else
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
  end

  -- a tab a group, when there is more than one and the game has its tab bar
  if #shown[controller] > 1 and type(CoD.FE_TabBar) == "table" then
    local tabs = CoD.FE_TabBar.new(self, controller)
    tabs:setLeftRight(true, false, 0, 2496)
    tabs:setTopBottom(true, false, 84, 123)
    tabs.Tabs.grid:setHorizontalCount(#shown[controller] + 2)
    tabs.Tabs.grid:setDataSource(tabsSource(controller))
    self:addElement(tabs)
    self.TabList = tabs
    self:registerEventHandler("list_active_changed", function(element, event)
      if element.xercesGroup then
        showGroup(element.xercesGroup)
      end
    end)
    -- the shoulder buttons, and Q and E on a keyboard, go from tab to tab
    for _, turn in ipairs({
      { button = Enum.LUIButton.LUI_KEY_LB, key = "Q", step = -1 },
      { button = Enum.LUIButton.LUI_KEY_RB, key = "E", step = 1 },
    }) do
      self:AddButtonCallbackFunction(
        self,
        controller,
        turn.button,
        turn.key,
        function(element, menu, actionController)
          local moved = pcall(function()
            if turn.step < 0 then
              tabs.Tabs.grid:navigateItemLeft()
            else
              tabs.Tabs.grid:navigateItemRight()
            end
          end)
          if not moved then
            local count = #shown[controller]
            showGroup((current[controller] - 1 + turn.step) % count + 1)
          end
          return true
        end,
        function(element, menu)
          CoD.Menu.SetButtonLabel(menu, turn.button, "")
          return true
        end,
        false
      )
    end
  end
  showGroup(1)

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
  openMenus[controller] = self
  self:processEvent({ name = "menu_loaded", controller = controller })
  self:processEvent({ name = "update_state", menu = self })
  list:processEvent({ name = "gain_focus", controller = controller })

  LUI.OverrideFunction_CallOriginalSecond(self, "close", function(element)
    if openMenus[controller] == element then
      openMenus[controller] = nil
      -- closed without a pick: the game puts back what the player wore
      if previewing[controller] then
        previewing[controller] = nil
        send(controller, "xerces_char:close")
      end
      wanted[controller] = nil
    end
    if element.MenuFrame then
      element.MenuFrame:close()
    end
    element.Options:close()
    if element.TabList then
      element.TabList:close()
    end
    -- gone already when a second screen of this controller closed first
    local prompts = Engine.GetModel(Engine.GetModelForController(controller), MENU .. ".buttonPrompts")
    if prompts then
      Engine.UnsubscribeAndFreeModel(prompts)
    end
  end)
  return self
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
  -- one screen for a controller: a second (the pause menu can come up over the first when the game loses focus) would
  -- share its rows and its button prompts
  local open = openMenus[controller]
  if open then
    -- still on screen? One that went without its close being heard must not keep the entry shut for good.
    local ok, parent = pcall(function()
      return open:getParent()
    end)
    if ok and parent then
      return
    end
    openMenus[controller] = nil
  end
  local hud = hudMenu()
  if not hud then
    -- no HUD to open it from: under the pause menu, without the preview
    pauseMenus[controller] = menu
    previewing[controller] = nil
    OpenPopup(self, MENU, controller)
    return
  end
  -- the pause menu goes so the match runs, then the game script starts the preview
  StartMenuGoBack(menu, controller)
  pauseMenus[controller] = nil
  previewing[controller] = true
  send(controller, "xerces_char:open")
  OpenPopup(hud, MENU, controller)
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
  -- a host without Xerces' scripts would take no pick: no entry then (the host's hidden text says so, a few seconds
  -- after the spawn, as for XERCES SETTINGS)
  if type(XercesHostHasScripts) == "function" and not XercesHostHasScripts() then
    return
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
