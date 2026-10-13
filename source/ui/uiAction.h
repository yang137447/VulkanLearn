#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace VL
{

enum class UiActionType
{
    ToggleDeveloperUi,
    SetLocale,
    LoadWorld,
    // stringValue 是资源根相对模型路径；GT 将其转为中性场景的 LoadWorld 注入请求。
    PreviewModel,
    PreviewMaterial,
    NavigateModelPreview,
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
    // 逐 actor 可见性：stringValue 携带大纲的稳定身份（"pointLight|Key_Point_Light"），
    // intValue 是 0/1。身份在执行边界解析并校验，UI 回调不接触 World。
    SetActorVisible,
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
enum class ModelPreviewNavigation { Orbit, Pan, Zoom, Frame };

// 预览输入绑定已发布的 World 代数；切标签后的旧鼠标事件不能修改新关卡。
struct ModelPreviewCameraInput
{
    ModelPreviewNavigation mode = ModelPreviewNavigation::Orbit;
    uint64_t worldGeneration = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct UiAction
{
    UiActionType type = UiActionType::ToggleDeveloperUi;
    int intValue = 0;
    float floatValue = 0.0f;
    std::string stringValue;
    ModelPreviewCameraInput previewCamera;
};

} // namespace VL
