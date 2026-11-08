if Engine.IsUsingMods() and Engine.UsingModsUgcName() ~= "" and Engine.UsingModsUgcName() ~= "usermaps" then
  return
end

DataSources.OptionPCGraphicsCard = DataSourceHelpers.ListSetup("PC.OptionPCGraphicsCard", function()
  return {
    { models = { value = 0, valueDisplay = "Windows Default" } },
    { models = { value = 1, valueDisplay = "Power Saving" } },
    { models = { value = 2, valueDisplay = "High Performance" } },
  }
end, true)

-- The upscaler (src/client/xerces/upscale.cpp): xerces_upscaler and
-- xerces_upscalerSharpness, set when the menu's changes are applied.
-- NVIDIA DLSS is listed only when the optional DLSS module is there
-- (xerces_dlssAvailable, src/client/xerces/temporal.cpp).
DataSources.XercesUpscaler = DataSourceHelpers.ListSetup("PC.XercesUpscaler", function()
  local values = {
    { models = { value = 0, valueDisplay = "Off" } },
    { models = { value = 1, valueDisplay = "AMD FSR 1" } },
  }
  if Engine.DvarInt(nil, "xerces_dlssAvailable") == 1 then
    table.insert(values, { models = { value = 2, valueDisplay = "NVIDIA DLSS / DLAA" } })
  end
  return values
end, true)
DataSources.XercesUpscalerSharpness = DataSourceHelpers.ListSetup("PC.XercesUpscalerSharpness", function()
  local values = {}
  for tenth = 0, 10 do
    table.insert(values, { models = { value = tenth, valueDisplay = (tenth * 10) .. "%" } })
  end
  return values
end, true)

local pendingGpuPreference = nil
local pendingDvars = {}

local function pendingDvarSetting(dvar, label, description, datasource)
  return {
    models = {
      label = label,
      description = description,
      profileVarName = dvar,
      profileType = "function",
      getFunction = function()
        return pendingDvars[dvar] or Engine.DvarInt(nil, dvar)
      end,
      setFunction = function(_, value)
        pendingDvars[dvar] = value
      end,
      datasource = datasource,
      widgetType = "dropdown",
    },
    properties = CoD.PCUtil.OptionsGenericDropdownProperties,
  }
end

local applyGraphicsOptions = ApplyPCGraphicsOptions
ApplyPCGraphicsOptions = function(...)
  applyGraphicsOptions(...)
  if pendingGpuPreference ~= nil then
    local preference = pendingGpuPreference
    pendingGpuPreference = nil
    game.setgpupreference(preference)
  end
  for dvar, value in pairs(pendingDvars) do
    Engine.SetDvar(dvar, value)
  end
  pendingDvars = {}
end

local revertGraphicsOptions = CoD.PCUtil.RevertUnsavedGraphicsOptions
CoD.PCUtil.RevertUnsavedGraphicsOptions = function(...)
  pendingGpuPreference = nil
  pendingDvars = {}
  return revertGraphicsOptions(...)
end

DataSources.OptionGraphicsVideo = DataSourceHelpers.ListSetup("PC.OptionGraphicsVideo", function(controller)
  local videoSettings = {}
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_DISPLAY_MODE",
      description = "PLATFORM_DISPLAY_MODE_DESC",
      profileVarName = "r_fullscreen",
      datasource = "OptionPCGraphicsDisplayMode",
      widgetType = "dropdown",
    },
    properties = CoD.PCUtil.DependantDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_MONITOR",
      description = "PLATFORM_MONITOR_DESC",
      profileVarName = "r_monitor",
      datasource = "OptionPCGraphicsMonitor",
      widgetType = "dropdown",
      disabledFunction = function()
        return Engine.GetHardwareProfileValueAsString("r_fullscreen") == "0"
      end,
    },
    properties = CoD.PCUtil.DependantDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "Graphics Card",
      description = "Which graphics card runs the game. Pick High Performance if the game lags on a laptop. Needs a game restart to apply.",
      profileVarName = "gpu_preference",
      profileType = "function",
      getFunction = function()
        return pendingGpuPreference or game.getgpupreference()
      end,
      setFunction = function(_, value)
        pendingGpuPreference = value
      end,
      datasource = "OptionPCGraphicsCard",
      widgetType = "dropdown",
    },
    properties = CoD.PCUtil.OptionsGenericDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_SCREEN_RESOLUTION",
      description = "PLATFORM_SCREEN_RESOLUTION_DESC",
      profileVarName = "r_mode",
      datasource = "OptionPCGraphicsScreenResolution",
      widgetType = "dropdown",
      disabledFunction = function()
        return Engine.GetHardwareProfileValueAsString("r_fullscreen") == "2"
      end,
    },
    properties = CoD.PCUtil.DependantDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_REFRESH_RATE",
      description = "PLATFORM_REFRESH_RATE_DESC",
      profileVarName = "r_refreshRate",
      datasource = "OptionPCGraphicsRefreshRate",
      widgetType = "dropdown",
      disabledFunction = function()
        return Engine.GetHardwareProfileValueAsString("r_fullscreen") ~= "1"
      end,
    },
    properties = CoD.PCUtil.OptionsGenericDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_SCENE_RESOLUTION_RATIO",
      description = "PLATFORM_SCENE_RESOLUTION_RATIO_DESC",
      profileVarName = "r_sceneResolutionMultiplier",
      datasource = "OptionPCGraphicsSceneResolution",
      widgetType = "dropdown",
    },
    properties = CoD.PCUtil.DependantDropdownProperties,
  })
  table.insert(
    videoSettings,
    pendingDvarSetting(
      "xerces_upscaler",
      "Upscaler",
      "How the 3D scene is enlarged when Scene Resolution Ratio is below 100%. AMD FSR 1 keeps edges and detail sharper than the game's own stretch. NVIDIA DLSS (with the DLSS module and an RTX GPU) replaces SMAA T2x's anti-aliasing and upscales; at 100% it is DLAA. The HUD is not touched.",
      "XercesUpscaler"
    )
  )
  table.insert(
    videoSettings,
    pendingDvarSetting(
      "xerces_upscalerSharpness",
      "Upscaler Sharpness",
      "How much the scene is sharpened after AMD FSR 1 or NVIDIA DLSS, 0% for none. It also sharpens at a Scene Resolution Ratio of 100%.",
      "XercesUpscalerSharpness"
    )
  )
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_DISPLAY_GAMMA",
      description = "PLATFORM_DISPLAY_GAMMA_DESC",
      profileVarName = "r_videoMode",
      datasource = "OptionPCGraphicsDisplayGamma",
      widgetType = "dropdown",
    },
    properties = CoD.PCUtil.OptionsGenericDropdownProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_BRIGHTNESS",
      description = "PLATFORM_BRIGHTNESS_DESC",
      profileVarName = "r_sceneBrightness",
      profileType = "user",
      lowValue = -1,
      highValue = 1,
      widgetType = "slider",
    },
    properties = CoD.PCUtil.OptionsGenericSliderProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_FOV",
      description = "PLATFORM_FOV_DESC",
      profileVarName = "cg_fov_default",
      lowValue = 65,
      highValue = 120,
      useIntegerDisplay = 1,
      widgetType = "slider",
    },
    properties = CoD.PCUtil.OptionsGenericSliderProperties,
  })
  table.insert(videoSettings, {
    models = {
      widgetType = "spacer",
      height = 32,
    },
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_MAX_FPS",
      description = "PLATFORM_MAX_FPS_DESC",
      profileVarName = "com_maxfps",
      lowValue = 0,
      highValue = 500,
      useIntegerDisplay = 1,
      widgetType = "slider",
    },
    properties = CoD.PCUtil.OptionsGenericSliderProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_SYNC_EVERY_FRAME",
      description = "PLATFORM_VSYNC_DESC",
      profileVarName = "r_vsync",
      widgetType = "checkbox",
    },
    properties = CoD.PCUtil.OptionsGenericCheckboxProperties,
  })
  table.insert(videoSettings, {
    models = {
      label = "PLATFORM_DRAW_FPS",
      description = "PLATFORM_DRAW_FPS_DESC",
      profileVarName = "com_drawFPS_PC",
      widgetType = "checkbox",
    },
    properties = CoD.PCUtil.OptionsGenericCheckboxProperties,
  })
  return videoSettings
end, true)
DataSources.OptionGraphicsVideo.getWidgetTypeForItem = function(list, dataItemModel, row)
  if dataItemModel then
    local widgetType = Engine.GetModelValue(Engine.GetModel(dataItemModel, "widgetType"))
    if widgetType == "dropdown" then
      return CoD.OptionDropdown
    elseif widgetType == "checkbox" then
      return CoD.StartMenu_Options_CheckBoxOption
    elseif widgetType == "slider" then
      return CoD.StartMenu_Options_SliderBar
    elseif widgetType == "spacer" then
      return CoD.VerticalListSpacer
    end
  end
  return nil
end

require("ui.uieditor.widgets.PC.StartMenu.GraphicsOptions.StartMenu_Options_PC_Graphics_Video")

DataSources.OptionGraphicsDetail = DataSourceHelpers.ListSetup("PC.OptionGraphicsDetail", function()
  local items = {}
  for _, setting in ipairs(BoiiiLodSettings) do
    local datasource = "BoiiiLod_" .. setting.dvar
    DataSources[datasource] = DataSourceHelpers.ListSetup("PC." .. datasource, function()
      local values = {}
      for _, option in ipairs(setting.options) do
        table.insert(values, { models = { value = option.value, valueDisplay = option.option } })
      end
      return values
    end, true)
    table.insert(items, {
      models = {
        label = setting.name,
        description = setting.description,
        profileVarName = setting.dvar,
        profileType = "function",
        getFunction = function()
          return pendingDvars[setting.dvar] or Engine.DvarInt(nil, setting.dvar)
        end,
        setFunction = function(_, value)
          pendingDvars[setting.dvar] = value
        end,
        datasource = datasource,
        widgetType = "dropdown",
      },
      properties = CoD.PCUtil.OptionsGenericDropdownProperties,
    })
  end
  return items
end, true)
DataSources.OptionGraphicsDetail.getWidgetTypeForItem = DataSources.OptionGraphicsVideo.getWidgetTypeForItem

CoD.StartMenu_Options_PC_Graphics_Detail = InheritFrom(LUI.UIElement)
CoD.StartMenu_Options_PC_Graphics_Detail.new = function(menu, controller)
  local self = CoD.StartMenu_Options_PC_Graphics_Video.new(menu, controller)
  self.id = "StartMenu_Options_PC_Graphics_Detail"
  self.graphicsList:setDataSource("OptionGraphicsDetail")
  return self
end

DataSources.OptionGraphicsCategories = DataSourceHelpers.ListSetup("PC.OptionGraphicsCategories", function()
  local function tab(name, widget)
    return { models = { tabName = name, tabWidget = widget } }
  end
  return {
    { models = { tabIcon = CoD.buttonStrings.shoulderl }, properties = { m_mouseDisabled = true } },
    tab("PLATFORM_VIDEO_CAPS", "CoD.StartMenu_Options_PC_Graphics_Video"),
    tab("PLATFORM_ADVANCED_CAPS", "CoD.StartMenu_Options_PC_Graphics_Advanced"),
    tab("DETAIL", "CoD.StartMenu_Options_PC_Graphics_Detail"),
    tab("MENU_GAME_CAPS", "CoD.StartMenu_Options_PC_Graphics_Game"),
    { models = { tabIcon = CoD.buttonStrings.shoulderr }, properties = { m_mouseDisabled = true } },
  }
end, true)
