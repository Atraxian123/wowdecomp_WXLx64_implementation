-- Adds an Interface Options panel with sliders for the CVars the wowdecomp client exposes:
--   farclip              view distance in yards (the original game capped this at ~1580)
--   horizonFarclipScale  how far the low detail horizon terrain reaches, as a multiple of farclip
--   fogDistanceScale     where distance fog ends relative to farclip (0 = no distance fog)

local panel = CreateFrame("Frame", "WowDecompViewDistancePanel", UIParent)
panel.name = "View Distance & Fog"

local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("View Distance & Fog")

local sliders = {}

local function MakeSlider(name, label, cvar, minValue, maxValue, step, y, format)
    local slider = CreateFrame("Slider", "WowDecompVD" .. name, panel, "OptionsSliderTemplate")
    slider:SetPoint("TOPLEFT", 24, y)
    slider:SetWidth(300)
    slider:SetMinMaxValues(minValue, maxValue)
    slider:SetValueStep(step)
    slider.cvar = cvar
    slider.label = label
    slider.format = format

    _G[slider:GetName() .. "Low"]:SetText(tostring(minValue))
    _G[slider:GetName() .. "High"]:SetText(tostring(maxValue))

    local function UpdateText(value)
        _G[slider:GetName() .. "Text"]:SetText(label .. ": " .. format(value))
    end

    slider:SetScript("OnValueChanged", function(self, value)
        value = math.floor(value / step + 0.5) * step
        UpdateText(value)

        if not self.refreshing then
            SetCVar(cvar, value)
        end
    end)

    function slider:Refresh()
        self.refreshing = true
        local value = tonumber(GetCVar(cvar)) or minValue
        self:SetValue(value)
        UpdateText(value)
        self.refreshing = false
    end

    sliders[#sliders + 1] = slider
    return slider
end

MakeSlider("Farclip", "View distance", "farclip", 177, 10000, 50, -70, function(v)
    return string.format("%d yards", v)
end)

MakeSlider("Horizon", "Horizon distance (x view distance)", "horizonFarclipScale", 3, 12, 0.5, -130, function(v)
    return string.format("x%.1f", v)
end)

MakeSlider("Fog", "Distance fog", "fogDistanceScale", 0, 1.5, 0.05, -190, function(v)
    if v <= 0 then
        return "off"
    end

    return string.format("%d%% of view distance", v * 100 + 0.5)
end)

local note = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
note:SetPoint("TOPLEFT", 24, -240)
note:SetWidth(420)
note:SetJustifyH("LEFT")
note:SetText("Large view distances load much more terrain: expect higher memory use and longer streaming. Fog above 100% shows the far clip edge of the world.")

panel.refresh = function()
    for _, slider in ipairs(sliders) do
        slider:Refresh()
    end
end

panel.okay = function() end
panel.cancel = function() end
panel.default = function()
    SetCVar("farclip", 350)
    SetCVar("horizonFarclipScale", 4)
    SetCVar("fogDistanceScale", 1)
    panel.refresh()
end

InterfaceOptions_AddCategory(panel)

SLASH_WOWDECOMPVD1 = "/viewdist"
SlashCmdList["WOWDECOMPVD"] = function(msg)
    local value = tonumber(msg)

    if value then
        SetCVar("farclip", value)
        print("View distance set to " .. GetCVar("farclip"))
    else
        InterfaceOptionsFrame_OpenToCategory(panel)
    end
end
