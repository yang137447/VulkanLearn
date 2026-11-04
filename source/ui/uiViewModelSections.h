#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logStore.h"
#include "core/performanceSnapshot.h"
#include "editor/assets/editorJsonAssetTypes.h"
#include "editor/ui/editorSceneActorTypes.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "render/camera/cameraExposure.h"
#include "script/scriptTypes.h"
#include "ui/uiRenderSnapshot.h"

namespace VL
{

// 视图模型分节：载荷在发布后不可变（shared_ptr<const T>），version 只在
// 该节的输入真正变化时递增。消费者按 version 判断“这一节要不要重建/重脏”，
// 而不是整份快照比较。UI 线程内部只读；跨 GT→UI 的冻结值同样不暴露 live 对象。
template <class T>
struct UiSnapshotSection
{
    std::shared_ptr<const T> data;
    uint64_t version = 0;

    bool Published() const noexcept { return data != nullptr; }
};

// 分节标识。顺序固定，供位掩码、版本数组和诊断计数共用。
enum class UiSectionId : uint32_t
{
    Scripts = 0,
    Animation,
    SceneOutliner,
    Assets,
    Performance,
    ViewportCamera,
    EnvironmentLighting,
    Offline,
    Status,
    Count
};

inline constexpr size_t UiSectionCount = static_cast<size_t>(UiSectionId::Count);
using UiSectionMask = uint32_t;

constexpr UiSectionMask UiSectionBit(UiSectionId id)
{
    return UiSectionMask{1} << static_cast<uint32_t>(id);
}

// 全部节：只用于 World 换代/首次发布这种“所有输入都换了一份”的事件。
inline constexpr UiSectionMask UiSectionAll =
    (UiSectionMask{1} << static_cast<uint32_t>(UiSectionId::Count)) - 1;

inline const char* UiSectionName(UiSectionId id)
{
    switch (id)
    {
    case UiSectionId::Scripts: return "scripts";
    case UiSectionId::Animation: return "animation";
    case UiSectionId::SceneOutliner: return "sceneOutliner";
    case UiSectionId::Assets: return "assets";
    case UiSectionId::Performance: return "performance";
    case UiSectionId::ViewportCamera: return "viewportCamera";
    case UiSectionId::EnvironmentLighting: return "environmentLighting";
    case UiSectionId::Offline: return "offline";
    case UiSectionId::Status: return "status";
    case UiSectionId::Count: break;
    }
    return "unknown";
}

// 脚本视图的节。归属：EngineLoop（脚本运行时 + 编辑器文档历史）。
// 版本源：World 代数、脚本播放/编译状态、文档历史修订；仅在事件强制刷新时重建，
// 因此静止帧不会重新执行 CanUndo/CanRedo 的路径规范化。
struct UiScriptsSection
{
    ScriptRuntimeView scripts;
};

// 动画预览节。time/playing 是时间相关值，随 10 Hz 刷新节拍变化；
// 静止/暂停且数值不变时版本不递增（按值比较）。
struct UiAnimationPreviewSection
{
    uint64_t worldGeneration = 0;
    std::vector<std::string> clips;
    size_t clip = 0;
    float time = 0;
    float duration = 0;
    bool playing = false;
    size_t jointCount = 0;
    std::vector<std::string> featureChannels;
    std::vector<float> features;
    std::string featureStatus;

    bool operator==(const UiAnimationPreviewSection& other) const
    {
        return worldGeneration == other.worldGeneration && clips == other.clips &&
            clip == other.clip && time == other.time && duration == other.duration &&
            playing == other.playing && jointCount == other.jointCount &&
            featureChannels == other.featureChannels && features == other.features &&
            featureStatus == other.featureStatus;
    }
};

// 大纲节。归属：UiSubsystem（它同时拥有 actor 列表、MI 模型列表与选中态）。
// 版本源：UiSubsystem 的 outlinerRevision，只在指针替换或选中态变化时推进。
struct UiSceneOutlinerSection
{
    std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>> actors;
    std::shared_ptr<const std::vector<EditorUi::MaterialInstanceModelSnapshot>> models;
    std::string selectedActorIdentity;
    std::optional<EditorUi::MaterialInstanceModelSnapshot> selectedModel;
    std::optional<uint32_t> selectedMaterialSlotIndex;
};

// 资产节。归属：UiSubsystem（日志存储、JSON 资产库、缩略图库都由 GT 轮询）。
// 载荷本身就是各库已经发布好的不可变快照；同一 revision 重复 Poll 得到同一个
// shared_ptr，因此“N 次构建无变化”只发布一次。
struct UiAssetsSection
{
    std::shared_ptr<const LogSnapshot> logs;
    std::shared_ptr<const EditorUi::EditorJsonAssetCatalogSnapshot> assetCatalog;
    std::shared_ptr<const EditorUi::EditorJsonDocumentSnapshot> jsonDocument;
    std::shared_ptr<const EditorUi::EditorThumbnailSnapshot> thumbnails;
    bool assetCatalogLoading = false;
    bool jsonDocumentLoading = false;

    bool operator==(const UiAssetsSection& other) const
    {
        return logs == other.logs && assetCatalog == other.assetCatalog &&
            jsonDocument == other.jsonDocument && thumbnails == other.thumbnails &&
            assetCatalogLoading == other.assetCatalogLoading &&
            jsonDocumentLoading == other.jsonDocumentLoading;
    }
};

// 性能节。归属：EngineLoop（RenderSystem 的冻结性能快照 + 脚本运行时的时间序列）。
// 数值是时间序列：采样窗口每推进一次就发布一次新版本；面板按自己的 5 Hz
// 取样，不因这一节变化而重建整份面板。
struct UiPerformanceSection
{
    PerformanceSnapshot performance;
    // Log 的 Scripts 页消费的脚本时间/计数器 JSON（与 ScriptRuntimeView 同源）。
    nlohmann::json scriptPerformance = nlohmann::json::object();
};

// 视口/相机节。归属：EngineLoop。版本按值比较；debugDraw/motionDebug 是
// 冻结共享指针，同一帧内不会被改写。
struct UiViewportCameraSection
{
    bool hasCamera = false;
    CameraExposure cameraExposure;
    int debugViewMode = 0;
    int toneMappingMode = 0;
    uint64_t motionWorldGeneration = 0;
    std::shared_ptr<const Animation::MotionDebugView> motionDebug;
    // 调试叠层每帧都会产生新的冻结快照（几何变化），但面板只关心
    // 开关/类别/owner/计数。用这些字段的签名做版本输入，避免整份相机节
    // 每个 10 Hz 窗口都判脏。
    uint64_t debugDrawSignature = 0;
    std::shared_ptr<const Debug::Snapshot> debugDraw;
    UiCameraProjection motionCamera;
    float cameraMoveSpeed = 1000.0f;

    bool operator==(const UiViewportCameraSection& other) const;
};

// 环境/光照作者参数节。归属：EngineLoop（RenderSystem 设置与环境更新诊断）。
struct UiEnvironmentLightingSection
{
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

    bool operator==(const UiEnvironmentLightingSection& other) const;
};

// 离线渲染节。归属：EngineLoop（RenderSystem 离线会话）。
// UI 侧“已请求模式/提示文本”仍由 UiSubsystem 自己保存，不进入冻结载荷。
struct UiOfflineSection
{
    bool offlineMode = false;
    int state = 0;
    int samples = 32;
    int completedSamples = 0;
    int bounces = 2;
    int lightSamples = 1;
    int seed = 0;
    int aov = 0;
    float exposure = 1.0f;
    std::string error;
    std::optional<OfflineModeCommandFeedback> modeFeedback;

    bool operator==(const UiOfflineSection& other) const;
};

// 状态节。归属：UiSubsystem（locale/热重载状态）加上逐帧冻结的 World 路径。
// 只在显示内容真正变化时提升版本，world_text 绑定也只在这一节变化时重脏。
struct UiStatusSection
{
    std::string locale;
    std::string hotReloadStatus;
    bool developerUiVisible = false;
    std::string activeWorldPath;

    bool operator==(const UiStatusSection& other) const
    {
        return locale == other.locale && hotReloadStatus == other.hotReloadStatus &&
            developerUiVisible == other.developerUiVisible &&
            activeWorldPath == other.activeWorldPath;
    }
};

// 分节聚合。每个消费者只读自己需要的节；没有“半更新”的中间态：
// 一次发布只替换某一节的 shared_ptr，旧载荷仍可被在途消费者安全持有。
struct UiViewModelSections
{
    UiSnapshotSection<UiScriptsSection> scripts;
    UiSnapshotSection<UiAnimationPreviewSection> animation;
    UiSnapshotSection<UiSceneOutlinerSection> sceneOutliner;
    UiSnapshotSection<UiAssetsSection> assets;
    UiSnapshotSection<UiPerformanceSection> performance;
    UiSnapshotSection<UiViewportCameraSection> viewportCamera;
    UiSnapshotSection<UiEnvironmentLightingSection> environmentLighting;
    UiSnapshotSection<UiOfflineSection> offline;
    UiSnapshotSection<UiStatusSection> status;
};

inline bool SameCameraExposure(const CameraExposure& left, const CameraExposure& right)
{
    return std::tie(left.mode, left.ev100, left.aperture, left.shutterSeconds, left.iso,
               left.compensation, left.autoMinEv100, left.autoMaxEv100, left.autoSpeedUp,
               left.autoSpeedDown, left.autoTargetGray) ==
        std::tie(right.mode, right.ev100, right.aperture, right.shutterSeconds, right.iso,
               right.compensation, right.autoMinEv100, right.autoMaxEv100, right.autoSpeedUp,
               right.autoSpeedDown, right.autoTargetGray);
}

inline bool UiViewportCameraSection::operator==(const UiViewportCameraSection& other) const
{
    return hasCamera == other.hasCamera && SameCameraExposure(cameraExposure, other.cameraExposure) &&
        debugViewMode == other.debugViewMode && toneMappingMode == other.toneMappingMode &&
        motionWorldGeneration == other.motionWorldGeneration &&
        motionDebug == other.motionDebug &&
        debugDrawSignature == other.debugDrawSignature &&
        motionCamera.position == other.motionCamera.position &&
        motionCamera.forward == other.motionCamera.forward &&
        motionCamera.right == other.motionCamera.right &&
        motionCamera.up == other.motionCamera.up &&
        motionCamera.horizontalFovDegrees == other.motionCamera.horizontalFovDegrees &&
        motionCamera.clipNear == other.motionCamera.clipNear &&
        motionCamera.clipFar == other.motionCamera.clipFar &&
        cameraMoveSpeed == other.cameraMoveSpeed;
}

inline bool UiEnvironmentLightingSection::operator==(
    const UiEnvironmentLightingSection& other) const
{
    return std::tie(bloomStrength, bloomThreshold, bloomKnee, bloomClamp,
               csmCastShadows, csmDynamicShadowDistance, csmDynamicShadowCascades,
               csmCascadeDistributionExponent, csmCascadeTransitionFraction,
               csmShadowDistanceFadeoutFraction, csmShadowBias, csmShadowSlopeBias,
               csmShadowCascadeBiasDistribution, environmentIntensity, sunIntensity,
               environmentActiveGeneration, environmentPendingGeneration,
               environmentUpdateStage, environmentCubemapFacesCompleted,
               environmentCubemapFaceCount, environmentShUpdatesCompleted,
               environmentPrefilterMipsCompleted, environmentPrefilterMipCount,
               environmentUsesPreviousResources, environmentGpuTimingSupported,
               environmentCubemapGpuMs, environmentShGpuMs, environmentPrefilterGpuMs,
               environmentCommitGpuMs) ==
        std::tie(other.bloomStrength, other.bloomThreshold, other.bloomKnee, other.bloomClamp,
               other.csmCastShadows, other.csmDynamicShadowDistance,
               other.csmDynamicShadowCascades, other.csmCascadeDistributionExponent,
               other.csmCascadeTransitionFraction, other.csmShadowDistanceFadeoutFraction,
               other.csmShadowBias, other.csmShadowSlopeBias,
               other.csmShadowCascadeBiasDistribution, other.environmentIntensity,
               other.sunIntensity, other.environmentActiveGeneration,
               other.environmentPendingGeneration, other.environmentUpdateStage,
               other.environmentCubemapFacesCompleted, other.environmentCubemapFaceCount,
               other.environmentShUpdatesCompleted, other.environmentPrefilterMipsCompleted,
               other.environmentPrefilterMipCount, other.environmentUsesPreviousResources,
               other.environmentGpuTimingSupported, other.environmentCubemapGpuMs,
               other.environmentShGpuMs, other.environmentPrefilterGpuMs,
               other.environmentCommitGpuMs);
}

inline bool UiOfflineSection::operator==(const UiOfflineSection& other) const
{
    const bool feedbackEqual =
        modeFeedback.has_value() == other.modeFeedback.has_value() &&
        (!modeFeedback.has_value() ||
            (modeFeedback->requestedMode == other.modeFeedback->requestedMode &&
                modeFeedback->rejectionReason == other.modeFeedback->rejectionReason));
    return offlineMode == other.offlineMode && state == other.state &&
        samples == other.samples && completedSamples == other.completedSamples &&
        bounces == other.bounces && lightSamples == other.lightSamples &&
        seed == other.seed && aov == other.aov && exposure == other.exposure &&
        error == other.error && feedbackEqual;
}

// 分节的版本快照。ChangedFrom 用位掩码返回“相比上一份哪些节变了”，
// UiSubsystem 只对掩码内的节做同步、重脏和面板重建。
struct UiSectionVersions
{
    std::array<uint64_t, UiSectionCount> versions{};

    uint64_t Version(UiSectionId id) const
    {
        return versions[static_cast<size_t>(id)];
    }

    UiSectionMask ChangedFrom(const UiSectionVersions& previous) const
    {
        UiSectionMask changed = 0;
        for (size_t index = 0; index < UiSectionCount; ++index)
        {
            if (versions[index] != previous.versions[index])
            {
                changed |= UiSectionBit(static_cast<UiSectionId>(index));
            }
        }
        return changed;
    }

    bool operator==(const UiSectionVersions& other) const
    {
        return versions == other.versions;
    }
};

inline UiSectionVersions VersionsOf(const UiViewModelSections& sections)
{
    UiSectionVersions versions;
    versions.versions[static_cast<size_t>(UiSectionId::Scripts)] = sections.scripts.version;
    versions.versions[static_cast<size_t>(UiSectionId::Animation)] = sections.animation.version;
    versions.versions[static_cast<size_t>(UiSectionId::SceneOutliner)] =
        sections.sceneOutliner.version;
    versions.versions[static_cast<size_t>(UiSectionId::Assets)] = sections.assets.version;
    versions.versions[static_cast<size_t>(UiSectionId::Performance)] = sections.performance.version;
    versions.versions[static_cast<size_t>(UiSectionId::ViewportCamera)] =
        sections.viewportCamera.version;
    versions.versions[static_cast<size_t>(UiSectionId::EnvironmentLighting)] =
        sections.environmentLighting.version;
    versions.versions[static_cast<size_t>(UiSectionId::Offline)] = sections.offline.version;
    versions.versions[static_cast<size_t>(UiSectionId::Status)] = sections.status.version;
    return versions;
}

// 两个 owner 各发布自己的节：EngineLoop 拥有脚本/动画/性能/相机/环境/离线，
// UiSubsystem 拥有大纲/资产/状态。脏集判定读取合并后的同一份版本。
inline UiSectionVersions MergeSectionVersions(
    const UiViewModelSections& engineSections,
    const UiViewModelSections& uiSections)
{
    UiSectionVersions versions = VersionsOf(engineSections);
    versions.versions[static_cast<size_t>(UiSectionId::SceneOutliner)] =
        uiSections.sceneOutliner.version;
    versions.versions[static_cast<size_t>(UiSectionId::Assets)] = uiSections.assets.version;
    versions.versions[static_cast<size_t>(UiSectionId::Status)] = uiSections.status.version;
    return versions;
}

// 面板 Build 的输入节：资产/文档、大纲、脚本类与状态文案。
// performance 不在其中：Performance 页自己按 5 Hz 取样，不触发整份面板重建。
inline constexpr UiSectionMask UiDeveloperPanelSectionMask =
    UiSectionBit(UiSectionId::Scripts) | UiSectionBit(UiSectionId::SceneOutliner) |
    UiSectionBit(UiSectionId::Assets) | UiSectionBit(UiSectionId::Status);

// UpdateRuntimeView 的输入节：脚本、动画、相机/调试叠层、环境、离线。
inline constexpr UiSectionMask UiRuntimeViewSectionMask =
    UiSectionBit(UiSectionId::Scripts) | UiSectionBit(UiSectionId::Animation) |
    UiSectionBit(UiSectionId::ViewportCamera) |
    UiSectionBit(UiSectionId::EnvironmentLighting) | UiSectionBit(UiSectionId::Offline) |
    UiSectionBit(UiSectionId::Status);

// 时间/进度驱动的节：每个 10 Hz 窗口都重新采样，但发布时仍按值判定，
// 数值没变就不提升版本（因此静止帧不会产生脏集）。
inline constexpr UiSectionMask UiCadenceSectionMask =
    UiSectionBit(UiSectionId::Animation) | UiSectionBit(UiSectionId::Performance) |
    UiSectionBit(UiSectionId::ViewportCamera) |
    UiSectionBit(UiSectionId::EnvironmentLighting) | UiSectionBit(UiSectionId::Offline);

// 本轮到期窗口要重建哪些节。事件订阅（requested）只重建自己声明的节；
// 首次发布/World 换代/可见性切换这类“所有输入都换了一份”的事件才重建全部。
inline UiSectionMask PlanUiEditorSectionRefresh(
    UiSectionMask requested,
    bool rebuildAll,
    bool firstRefresh)
{
    if (firstRefresh || rebuildAll)
    {
        return UiSectionAll;
    }
    return requested | UiCadenceSectionMask;
}

// 版本组合器：owner 用已有代数/修订号构造输入版本，不引入第二份权威状态。
// FNV-1a-64，只用于变化检测，不用于持久身份。
class UiSectionRevision
{
public:
    UiSectionRevision() = default;
    explicit UiSectionRevision(uint64_t seed) : value(seed) {}

    UiSectionRevision& Mix(uint64_t number)
    {
        value ^= number;
        value *= 1099511628211ull;
        return *this;
    }

    uint64_t Value() const noexcept { return value; }

private:
    uint64_t value = 1469598103934665603ull;
};

// 单节发布器：把“版本只随输入变化”的规则收在一处。
// - Publish(value, revision)：输入版本相同即视为无变化，不换载荷、不递增版本。
// - PublishIfChanged(value)：没有独立 revision 的节按值比较。
template <class T>
class UiSectionSlot
{
public:
    bool Publish(T value, uint64_t inputRevision)
    {
        if (revisionValid && inputRevision == revision)
        {
            return false;
        }
        section.data = std::make_shared<const T>(std::move(value));
        ++section.version;
        revision = inputRevision;
        revisionValid = true;
        return true;
    }

    bool PublishIfChanged(T value)
    {
        if (section.data != nullptr && *section.data == value)
        {
            return false;
        }
        section.data = std::make_shared<const T>(std::move(value));
        ++section.version;
        return true;
    }

    const UiSnapshotSection<T>& Section() const noexcept { return section; }

private:
    UiSnapshotSection<T> section;
    uint64_t revision = 0;
    bool revisionValid = false;
};

// GT 侧的六节发布器（脚本/动画/性能/相机/环境/离线）。
// 资产、大纲、状态由 UiSubsystem 自己的 UiSectionSlot 拥有；两者版本在
// UiSubsystem::Update 中合并成一个 UiSectionVersions 判定脏集。
class UiViewModelSectionStore
{
public:
    bool PublishScripts(UiScriptsSection value, uint64_t inputRevision)
    {
        const bool published = scripts.Publish(std::move(value), inputRevision);
        if (published) view.scripts = scripts.Section();
        return published;
    }
    bool PublishAnimation(UiAnimationPreviewSection value)
    {
        const bool published = animation.PublishIfChanged(std::move(value));
        if (published) view.animation = animation.Section();
        return published;
    }
    bool PublishSceneOutliner(UiSceneOutlinerSection value, uint64_t inputRevision)
    {
        const bool published = sceneOutliner.Publish(std::move(value), inputRevision);
        if (published) view.sceneOutliner = sceneOutliner.Section();
        return published;
    }
    bool PublishAssets(UiAssetsSection value)
    {
        const bool published = assets.PublishIfChanged(std::move(value));
        if (published) view.assets = assets.Section();
        return published;
    }
    bool PublishPerformance(UiPerformanceSection value)
    {
        // 时间序列：每次采样都是新的窗口数据，即使数值碰巧相同也要让消费者看到新样本。
        view.performance.data = std::make_shared<const UiPerformanceSection>(std::move(value));
        ++view.performance.version;
        return true;
    }
    bool PublishViewportCamera(UiViewportCameraSection value)
    {
        const bool published = viewportCamera.PublishIfChanged(std::move(value));
        if (published) view.viewportCamera = viewportCamera.Section();
        return published;
    }
    bool PublishEnvironmentLighting(UiEnvironmentLightingSection value)
    {
        const bool published = environmentLighting.PublishIfChanged(std::move(value));
        if (published) view.environmentLighting = environmentLighting.Section();
        return published;
    }
    bool PublishOffline(UiOfflineSection value)
    {
        const bool published = offline.PublishIfChanged(std::move(value));
        if (published) view.offline = offline.Section();
        return published;
    }
    bool PublishStatus(UiStatusSection value)
    {
        const bool published = status.PublishIfChanged(std::move(value));
        if (published) view.status = status.Section();
        return published;
    }

    const UiViewModelSections& Sections() const noexcept { return view; }

    UiSectionVersions Versions() const noexcept { return VL::VersionsOf(view); }

private:
    UiSectionSlot<UiScriptsSection> scripts;
    UiSectionSlot<UiAnimationPreviewSection> animation;
    UiSectionSlot<UiSceneOutlinerSection> sceneOutliner;
    UiSectionSlot<UiAssetsSection> assets;
    UiSectionSlot<UiViewportCameraSection> viewportCamera;
    UiSectionSlot<UiEnvironmentLightingSection> environmentLighting;
    UiSectionSlot<UiOfflineSection> offline;
    UiSectionSlot<UiStatusSection> status;
    // 聚合视图与各 slot 同步更新；Sections() 只返回只读引用。
    UiViewModelSections view;
};

// EngineLoop 本轮重建好的 GT 分节。std::nullopt 表示“本帧没重建这一节”，
// UiSubsystem 保留上一份已发布载荷，不做任何同步/重脏。
struct UiViewModelSectionUpdate
{
    std::optional<UiScriptsSection> scripts;
    // 脚本节由已有代数/修订号组合出的输入版本；只在重建时提供。
    uint64_t scriptsRevision = 0;
    std::optional<UiAnimationPreviewSection> animation;
    std::optional<UiPerformanceSection> performance;
    std::optional<UiViewportCameraSection> viewportCamera;
    std::optional<UiEnvironmentLightingSection> environmentLighting;
    std::optional<UiOfflineSection> offline;
};

// 每帧冻结值：这些值服务于 DrawDebug 叠层、沉浸恢复和运行时 HUD，
// 必须逐帧可见，因此不进入按版本重脏的编辑器分节。
struct UiViewModelFrameValues
{
    uint64_t frameIndex = 0;
    float deltaTimeSeconds = 0.0f;
    float framesPerSecond = 0.0f;
    float cameraMoveSpeed = 1000.0f;
    std::string activeWorldPath;
    bool offlineMode = false;
    // DrawDebug/相机冻结：与 lastViewModel 的逐帧字段一一对应。
    uint64_t motionWorldGeneration = 0;
    std::shared_ptr<const Animation::MotionDebugView> motionDebug;
    UiCameraProjection motionCamera;
    std::shared_ptr<const Debug::Snapshot> debugDraw;
    // 仅 RuntimeHud 档位：Gameplay HUD 文本逐帧冻结（编辑器可见时为空）。
    ScriptRuntimeView hudView;
};

} // namespace VL
