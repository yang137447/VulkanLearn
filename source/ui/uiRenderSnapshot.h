#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace VL
{

using UiTextureId = std::uintptr_t;

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
    RuntimeUi,
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
    bool runtimePageVisible = false;
    bool developerUiVisible = false;
    float cameraMoveSpeed = 10.0f;
    std::string activeWorldPath;
    std::string locale;
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
    uint32_t color = 0xffffffffu;
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
};

// Owns immutable UI geometry and texture data consumed by the render thread.
// It is produced by UiSubsystem and contains no live RmlUi or ImGui handles.
struct UiRenderSnapshot
{
    uint64_t frameIndex = 0;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    UiViewportRect sceneViewportRect;
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<UiDrawCommand> drawCommands;
    std::vector<UiTextureSnapshot> textures;
};

} // namespace VL
