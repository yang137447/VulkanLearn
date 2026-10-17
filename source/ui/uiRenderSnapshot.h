#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "render/camera/cameraExposure.h"
#include "render/resource/rendererGpuImageToken.h"
#include "script/scriptTypes.h"

namespace VL
{

using UiTextureId = std::uintptr_t;
// GT 只记录逻辑纹理 ID；对应离屏图像与 descriptor 由当前 RT 帧绑定。
inline constexpr UiTextureId SceneViewportTextureId = std::numeric_limits<UiTextureId>::max();
inline constexpr const char* SceneViewportTextureSource = "vl://scene-viewport";

enum class UiInputMode
{
    GameOnly,
    UiOnly,
    GameAndUi
};

enum class UiInputOwner
{
    None,
    Game,
    DeveloperUi
};

enum class UiBlendMode
{
    StraightAlpha,
    PremultipliedAlpha
};

struct UiInputOwnershipSnapshot
{
    UiInputMode mode = UiInputMode::GameAndUi;
    UiInputOwner keyboardOwner = UiInputOwner::Game;
    UiInputOwner pointerOwner = UiInputOwner::Game;
    UiInputOwner controllerOwner = UiInputOwner::Game;
    UiInputOwner textInputOwner = UiInputOwner::None;
};

struct OfflineModeCommandFeedback
{
    int requestedMode = -1;
    std::string rejectionReason;
};

// Carries immutable game-thread state into the UI update step.
// It is assembled by EngineLoop and does not expose live renderer objects.
struct UiViewModelSnapshot
{
    uint64_t frameIndex = 0;
    float deltaTimeSeconds = 0.0f;
    float framesPerSecond = 0.0f;
    int debugViewMode = 0;
    int toneMappingMode = 0;
    bool hasCamera = false;
    // UI 只持有曝光作者状态的副本，不读取 GPU 测光历史或活动 Camera。
    CameraExposure cameraExposure;
    bool offlineMode = false;
    int offlineState = 0;
    int offlineSamples = 32;
    int offlineCompletedSamples = 0;
    int offlineBounces = 2;
    int offlineLightSamples = 1;
    int offlineSeed = 0;
    int offlineAov = 0;
    float offlineExposure = 1.0f;
    std::string offlineError;
    std::optional<OfflineModeCommandFeedback> offlineModeFeedback;
    // 以下两项仅由 UI 维护，用下一帧的引擎快照确认异步投递的模式请求。
    int offlineRequestedMode = -1;
    std::string offlineUiNotice;
    float bloomStrength = 0.0f;
    float bloomThreshold = 0.0f;
    float bloomKnee = 0.0f;
    float bloomClamp = 0.0f;
    bool csmCastShadows = false;
    float csmDynamicShadowDistance = 10.0f;
    uint32_t csmDynamicShadowCascades = 4;
    float csmCascadeDistributionExponent = 3.0f;
    float csmCascadeTransitionFraction = 0.1f;
    float csmShadowDistanceFadeoutFraction = 0.1f;
    float csmShadowBias = 0.5f;
    float csmShadowSlopeBias = 0.5f;
    float csmShadowCascadeBiasDistribution = 1.0f;
    float environmentIntensity = 1.0f;
    float sunIntensity = 1.0f;
    uint64_t environmentActiveGeneration = 0;
    uint64_t environmentPendingGeneration = 0;
    std::string environmentUpdateStage;
    uint32_t environmentCubemapFacesCompleted = 0;
    uint32_t environmentCubemapFaceCount = 0;
    uint32_t environmentShUpdatesCompleted = 0;
    uint32_t environmentPrefilterMipsCompleted = 0;
    uint32_t environmentPrefilterMipCount = 0;
    bool environmentUsesPreviousResources = false;
    bool environmentGpuTimingSupported = false;
    double environmentCubemapGpuMs = 0.0;
    double environmentShGpuMs = 0.0;
    double environmentPrefilterGpuMs = 0.0;
    double environmentCommitGpuMs = 0.0;
    float speedTreeStrength = 0.35f;
    bool speedTreeGustingEnabled = true;
    uint32_t speedTreeWindProfileCount = 0;
    bool developerUiVisible = false;
    // 与 Controller 一致，快照中的移动速度以 cm/s 表示。
    float cameraMoveSpeed = 1000.0f;
    std::string activeWorldPath;
    std::string locale;
    ScriptRuntimeView scripts;
};

inline void ReconcileOfflineModeFeedback(
    UiViewModelSnapshot& next,
    const UiViewModelSnapshot& previous)
{
    next.offlineRequestedMode = previous.offlineRequestedMode;
    next.offlineUiNotice = previous.offlineUiNotice;
    if (next.offlineMode != previous.offlineMode)
    {
        next.offlineRequestedMode = -1;
        next.offlineUiNotice.clear();
    }
    if (previous.offlineRequestedMode != -1 &&
        next.offlineModeFeedback &&
        next.offlineModeFeedback->requestedMode == previous.offlineRequestedMode)
    {
        // 只消费本次 UI 请求的执行结果；模式与有效内容始终来自引擎快照。
        next.offlineRequestedMode = -1;
        next.offlineUiNotice = next.offlineModeFeedback->rejectionReason;
    }
}

struct UiVertex
{
    float position[2] = {0.0f, 0.0f};
    float texCoord[2] = {0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct UiClipRect
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// 像素坐标系中的可渲染视口。原点位于最终 framebuffer 左上角；矩形已经
// 排除了停靠窗口的标题栏和内容 padding，renderer 可以直接把它作为安全区域。
struct UiViewportRect
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct UiDrawCommand
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    UiTextureId textureId = 0;
    UiClipRect clipRect;
    UiBlendMode blendMode = UiBlendMode::StraightAlpha;
};

struct UiTextureSnapshot
{
    UiTextureId id = 0;
    uint64_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string source;
    std::shared_ptr<const std::vector<uint8_t>> rgba8Pixels;
    // 只固定逻辑身份；成图、采样描述和 GPU 退休仍归 renderer。
    std::shared_ptr<const RendererGpuImageToken> gpuImage;
    bool linearSampling = true;
};

// GT 冻结的 UI 几何、CPU 像素或 GPU 成图 token；RT 消费，不访问 live UI。
// token 只延长 renderer 注册的身份有效期，Vulkan 对象仍由 backend 管理。
struct UiRenderSnapshot
{
    uint64_t frameIndex = 0;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    UiViewportRect sceneViewportRect;
    bool sceneViewportEnabled = false;
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<UiDrawCommand> drawCommands;
    std::vector<UiTextureSnapshot> textures;
};

} // namespace VL
