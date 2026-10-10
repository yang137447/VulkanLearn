#pragma once

#include <string>
#include <string_view>

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
    SetCameraExposure,
    SetOfflineMode,
    SetOfflineSetting,
    SetOfflineAov,
    SetOfflineExposure,
    ControlOfflineSession,
    ExportOffline,
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

// UI 与控制台命令共用值域；名称和值都在命令执行边界校验。
inline const char* OfflineSettingValidationError(std::string_view name, int value)
{
    if (name == "samples")
        return value >= 1 && value <= 1000000 ? nullptr : "Samples must be 1..1000000";
    if (name == "bounces")
        return value >= 0 && value <= 64 ? nullptr : "Bounces must be 0..64";
    if (name == "lightSamples")
        return value >= 1 && value <= 64 ? nullptr : "Light samples must be 1..64";
    if (name == "seed")
        return value >= 0 ? nullptr : "Seed must be non-negative";
    return "Unknown offline setting";
}

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
