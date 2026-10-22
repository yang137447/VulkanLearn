#pragma once

#include <string>
#include <array>
#include <string_view>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "editor/scene/editorActorProperty.h"
#include "debug/debugDrawControl.h"

namespace VL
{

enum class UiActionType
{
    ToggleDeveloperUi,
    SetLocale,
    LoadWorld,
    // stringValue 是资源根相对模型路径；GT 将其转为中性场景的 LoadWorld 注入请求。
    PreviewModel,
    PreviewRetarget,
    PreviewMaterial,
    PreviewBlueprint,
    NavigateModelPreview,
    ControlAnimationPreview,
    // 被聚焦脚本控制器的时间轴：stringValue=组件身份，intValue 0=seek/1=play，floatValue=秒。
    ControlScriptAnimation,
    ControlMotionDebug,
    // intValue<0 退出历史回放，否则选择 Motion 调试历史帧。
    ControlMotionReplay,
    ControlDebugDraw,
    TransformBlueprintPreview,
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
    SetActorProperty,
    FrameEditorActor,
    ControlScripts,
    ReloadScripts,
    CompileScripts,
    SetAutoCompileScripts,
    SetScriptTimeScale,
    AddScriptBlueprint,
    SetScriptProperty,
    SetScriptComponent,
    AddScriptComponent,
    RemoveScriptComponent,
    SetScriptLogicComponent,
    AddScriptLogicComponent,
    RemoveScriptLogicComponent,
    RenameScriptBlueprint,
    DuplicateScriptBlueprint,
    DeleteScriptBlueprint,
    SetScriptPhysics,
    UndoSceneEdit,
    RedoSceneEdit,
    CreateScriptBlueprintAsset,
    OpenScriptSource,
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

// 仅作用于当前蓝图预览 World 的瞬态 TRS；不修改作者草稿，不导入资源或运行脚本。
struct BlueprintPreviewTransformInput
{
    uint64_t worldGeneration = 0;
    uint64_t documentRevision = 0;
    std::string assetPath;
    std::string componentId;
    std::string field;
    std::array<float, 3> value{0, 0, 0};
    bool reset = false;
};

struct UiAction
{
    UiActionType type = UiActionType::ToggleDeveloperUi;
    int intValue = 0;
    float floatValue = 0.0f;
    std::string stringValue;
    // PreviewRetarget: stringValue 为 RT 身份，此字段为作者指定的目标 SM；GT 核对二者绑定。
    std::string retargetPreviewModel;
    uint64_t blueprintPreviewRevision = 0;
    ModelPreviewCameraInput previewCamera;
    BlueprintPreviewTransformInput blueprintTransform;
    EditorActorPropertyEdit actorProperty;
    Debug::Control debugDrawControl;
    // 脚本作者请求只带值；class/name/modelPath 或 property/value 在 GT 校验。
    nlohmann::json script = nlohmann::json::object();
};

} // namespace VL
