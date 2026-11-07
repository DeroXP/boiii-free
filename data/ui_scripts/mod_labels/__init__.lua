-- Xerces: each camo and character icon names the Workshop mod (or custom map)
-- that supplied it, in the camo pickers and character screens, so a stack of
-- mods shows whose camo or character is whose. The owner is the zone of the
-- icon image the game uses now (game.getassetowner, read only); the game's
-- own icons get no label.
--
-- Lists, by data source name (seen in the game on 2026-11-06, docs/TESTING.md
-- "Mod labels"):
-- - GunsmithWeaponOptions: the game's camo picker (Zombies weapon kits,
--   Gunsmith, Create-a-Class). The camo's name sits at the bottom of each
--   icon, so the label goes at the top.
-- - DS4C_Camos_*: DS4C's camo screen in a match. DS4C's own BO6/BO7/MW3 and
--   Reactive tags sit at the top, so the label goes at the bottom.
-- - *CharacterOptions: DS4C's character screen in a match (and character mods
--   on the same menu template). The name sits below the portrait, so the
--   label goes at the top.
if type(game) ~= "table" or type(game.getassetowner) ~= "function" then
  return
end
if type(LUI) ~= "table" or type(LUI.UIList) ~= "table" or __xercesModLabelsInstalled then
  return
end
__xercesModLabelsInstalled = true

local LISTS = {
  { pattern = "^GunsmithWeaponOptions$", edge = "top" },
  { pattern = "^DS4C_Camos", edge = "bottom" },
  { pattern = "CharacterOptions$", edge = "top" },
}

local LABEL_HEIGHT = 16
local MARGIN = 3

local function edge_for(source)
  if type(source) ~= "string" then
    return nil
  end
  for _, list in ipairs(LISTS) do
    if string.find(source, list.pattern) then
      return list.edge
    end
  end
  return nil
end

-- "^6DS4C ^5Patched ^7v0.85" -> "DS4C Patched"; "Hyperdimension Neptunia
-- [Character Mod]" -> "Hyperdimension Neptunia". Long names are cut to fit an
-- icon.
local function short_title(title)
  local text = string.gsub(title, "%^%d", "")
  text = string.gsub(text, "%b[]", "")
  text = string.gsub(text, "%s+[vV]%d[%w%.%-]*%s*$", "")
  text = string.gsub(text, "^%s+", "")
  text = string.gsub(text, "%s+$", "")
  if #text > 22 then
    text = string.sub(text, 1, 21) .. "."
  end
  return text
end

-- Owners do not change while this Lua state lives: a mod load or a map load
-- starts a new one.
local owners = {}
local function owner_label(image)
  if type(image) ~= "string" or image == "" then
    return nil
  end
  local label = owners[image]
  if label == nil then
    label = false
    local ok, id = pcall(game.getassetowner, "image", image)
    if ok and type(id) == "string" and id ~= "" then
      local got, title = pcall(game.getmodtitle, id)
      if got and type(title) == "string" and title ~= "" then
        label = short_title(title)
      else
        label = id
      end
    end
    owners[image] = label
  end
  return label or nil
end

local function add_label(item, edge)
  if item.xercesModLabel then
    return
  end
  local back = LUI.UIImage.new()
  local text = LUI.UIText.new()
  back:setLeftRight(true, true, MARGIN, -MARGIN)
  text:setLeftRight(true, true, MARGIN + 3, -MARGIN - 3)
  if edge == "bottom" then
    back:setTopBottom(false, true, -MARGIN - LABEL_HEIGHT, -MARGIN)
    text:setTopBottom(false, true, -MARGIN - LABEL_HEIGHT + 1, -MARGIN - 1)
  else
    back:setTopBottom(true, false, MARGIN, MARGIN + LABEL_HEIGHT)
    text:setTopBottom(true, false, MARGIN + 1, MARGIN + LABEL_HEIGHT - 1)
  end
  back:setRGB(0, 0, 0)
  back:setAlpha(0)
  text:setTTF("fonts/default.ttf")
  text:setRGB(1, 1, 1)
  text:setAlpha(0)
  text:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
  text:setText("")
  item:addElement(back)
  item:addElement(text)
  item.xercesModLabel = text
  text:linkToElementModel(item, "image", true, function(model)
    local label = owner_label(Engine.GetModelValue(model))
    if label then
      text:setText(label)
      text:setAlpha(1)
      back:setAlpha(0.65)
    else
      text:setText("")
      text:setAlpha(0)
      back:setAlpha(0)
    end
  end)
end

-- Each item widget class is wrapped once, the first time a list to label
-- uses it; its later items get the label as they are made.
local wrapped = {}
local function wrap_widget(widget, edge)
  if type(widget) ~= "table" or type(widget.new) ~= "function" or wrapped[widget] then
    return
  end
  wrapped[widget] = true
  local new = widget.new
  widget.new = function(...)
    local item = new(...)
    if item then
      pcall(add_label, item, edge)
    end
    return item
  end
end

local set_widget_type = LUI.UIList.setWidgetType
LUI.UIList.setWidgetType = function(self, widget, ...)
  self.xercesItemWidget = widget
  return set_widget_type(self, widget, ...)
end

local set_data_source = LUI.UIList.setDataSource
LUI.UIList.setDataSource = function(self, source, ...)
  local edge = edge_for(source)
  if edge then
    pcall(wrap_widget, self.xercesItemWidget, edge)
  end
  return set_data_source(self, source, ...)
end
