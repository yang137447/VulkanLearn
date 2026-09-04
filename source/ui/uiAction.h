#pragma once

#include <string>

namespace VL
{

enum class UiActionType
{
    ToggleRuntimePage,
    CloseRuntimePage,
    ToggleDeveloperUi,
    SetLocale,
    LoadWorld,
    SetCameraMoveSpeed,
    SetDebugViewMode,
    SetToneMappingMode,
    SetBloomStrength,
    SetBloomThreshold,
    SetBloomKnee,
    SetBloomClamp,
    SetCsmCastShadows,
    SetCsmDynamicShadowDistance,
    SetCsmDynamicShadowCascades,
    SetCsmCascadeDistributionExponent,
    SetCsmCascadeTransitionFraction,
    SetCsmShadowDistanceFadeoutFraction,
    SetCsmShadowBias,
    SetCsmShadowSlopeBias,
    SetCsmShadowCascadeBiasDistribution,
    SaveCsmSettingsToScene,
    SetEnvironmentIntensity,
    SetSunIntensity,
    SetSpeedTreeStrength,
    SetSpeedTreeGustingEnabled,
    ForceSpeedTreeGust,
    Quit
};

// Carries typed UI-to-engine mutation requests for CommandBus and EngineLoop.
// It does not mutate renderer or gameplay state itself.
struct UiAction
{
    UiActionType type = UiActionType::ToggleRuntimePage;
    int intValue = 0;
    float floatValue = 0.0f;
    std::string stringValue;
};

} // namespace VL
