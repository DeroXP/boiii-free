-- Xerces: a pause-menu entry for a mod's own menu when the pause menu in use
-- has no way to it (docs/DESIGN.md 7.6). Mods made from the common
-- mod-settings template (PEGASUS, the Avali character pack) ship a menu named
-- StartMenu_ModSettings_Main and open it from a button in their own copy of
-- the pause menu's game options list (data source ModStartMenuGameOptions).
-- A mod that brings its own pause menu leaves that list out, and the button
-- with it: DS4C's uses StartMenuGameOptions_Death. Xerces then adds
-- "<MOD> MENU" to the list in use. It opens the menu the way the mod's own
-- button does, and Back returns to the pause menu (seen in the game on
-- 2026-11-07, docs/TESTING.md "Mod menu entry").
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesModMenuInstalled then
  return
end
if Engine.GetCurrentMap() == "core_frontend" then
  return
end
__xercesModMenuInstalled = true

local MENU = "StartMenu_ModSettings_Main"
local MENU_FILE = "ui/uieditor/menus/startmenu/startmenu_modsettings_main.lua"
-- The template's own list, which has the mod's button already.
local OWN_LIST = "modstartmenugameoptions"
-- The entry goes above the first of these, or last when none is there.
local BELOW = {
  MENU_RESTART_LEVEL_CAPS = true,
  MENU_RESTART_MISSION_CAPS = true,
  MENU_END_GAME_CAPS = true,
  MENU_QUIT_GAME_CAPS = true,
  MENU_LEAVE_PARTY_AND_EXIT_CAPS = true,
  ["MAIN MENU"] = true,
  ["QUIT TO DESKTOP"] = true,
}

-- "^3[^5PZM^3] ^9PEGASUS^7 v0.2.3.9" -> "PEGASUS"; "^2Avali^7: ^8Solspace
-- ^7- ^9Character Pack" -> "AVALI". Nothing when no short name is left.
local function menu_name(title)
  local text = string.gsub(title, "%^%d", "")
  text = string.gsub(text, "%b[]", "")
  text = string.gsub(text, "%s+[vV]%d[%w%.%-]*%s*$", "")
  local cut = #text + 1
  for _, separator in ipairs({ ":", " - ", " | " }) do
    local at = string.find(text, separator, 1, true)
    if at and at < cut then
      cut = at
    end
  end
  text = string.sub(text, 1, cut - 1)
  text = string.gsub(text, "^%s+", "")
  text = string.gsub(text, "%s+$", "")
  if text == "" or #text > 16 then
    return nil
  end
  return string.upper(text)
end

-- The mod whose copy of the menu the game uses names the entry. The owner
-- does not change while this Lua state lives: a mod load or a map load
-- starts a new one.
local label = nil
local function entry_label()
  if label then
    return label
  end
  label = "^3MOD MENU"
  if type(game) == "table" and type(game.getassetowner) == "function" and type(game.getmodtitle) == "function" then
    local ok, id = pcall(game.getassetowner, "rawfile", MENU_FILE)
    if ok and type(id) == "string" and id ~= "" then
      local got, title = pcall(game.getmodtitle, id)
      local name = got and type(title) == "string" and menu_name(title)
      if name then
        label = "^3" .. name .. " MENU"
      end
    end
  end
  return label
end

-- What the mod's own button does.
local function open_menu(self, element, controller)
  NavigateToMenu(self, MENU, true, controller)
end

local function add_entry(controller, list)
  if type(LUI.createMenu[MENU]) ~= "function" then
    return
  end
  local key = list.customDataSourceHelper
  local items = key and list[key]
  if type(items) ~= "table" then
    return
  end
  for _, item in ipairs(items) do
    if item.xercesModMenu then
      return
    end
  end
  local root = Engine.CreateModel(Engine.GetModelForController(controller), key)
  local model = Engine.GetModel(root, "xercesModMenu")
  if model then
    Engine.UnsubscribeAndFreeModel(model)
  end
  model = Engine.CreateModel(root, "xercesModMenu")
  Engine.SetModelValue(Engine.CreateModel(model, "displayText"), entry_label())
  Engine.SetModelValue(Engine.CreateModel(model, "action"), open_menu)
  local at = #items + 1
  for index, item in ipairs(items) do
    local text = item.model and Engine.GetModel(item.model, "displayText")
    if text and BELOW[Engine.GetModelValue(text)] then
      at = index
      break
    end
  end
  table.insert(items, at, { model = model, xercesModMenu = true })
end

-- A data source is wrapped once, the first time a pause menu's list uses it;
-- a mod that defines it again (a map restart) gets its new table wrapped.
local function wrap_source(name)
  local source = DataSources[name]
  if type(source) ~= "table" or type(source.prepare) ~= "function" or source.xercesModMenu then
    return
  end
  local prepare = source.prepare
  source.prepare = function(controller, list, ...)
    local result = prepare(controller, list, ...)
    pcall(add_entry, controller, list)
    return result
  end
  source.xercesModMenu = true
end

local function is_pause_list(name)
  if type(name) ~= "string" then
    return false
  end
  local lower = string.lower(name)
  return lower ~= OWN_LIST and string.find(lower, "startmenugameoptions", 1, true) ~= nil
end

local set_data_source = LUI.UIList.setDataSource
LUI.UIList.setDataSource = function(self, source, ...)
  if is_pause_list(source) and Engine.IsInGame() and Engine.CurrentSessionMode() == Enum.eModes.MODE_ZOMBIES then
    pcall(wrap_source, source)
  end
  return set_data_source(self, source, ...)
end
