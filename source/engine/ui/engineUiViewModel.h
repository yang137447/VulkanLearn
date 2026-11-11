#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "animation/motionFeatures.h"
#include "ui/uiViewModelRefresh.h"
#include "ui/viewModel/runtimeUiSections.h"

class Controller;
class RenderSystem;

namespace VL
{

class ScriptBuildService;
class ScriptRuntimeSubsystem;
class World;
class WorldManager;
struct RuntimeCommandExecutionResult;
struct WorldHandle;

namespace Animation
{
class SkeletalPlayback;
}

// 动画预览派生数据：只有 active World 换代或 preview_model 播放器身份变化时失效。
// schema/通道名/clip 列表与播放时间无关，不能每帧重算。
struct UiAnimationPreviewCache
{
    const Animation::SkeletalPlayback* player = nullptr;
    uint64_t worldGeneration = 0;
    std::vector<std::string> clipIds;
    Animation::FeatureSchema schema;
    std::vector<std::string> featureChannels;
};

// EngineLoop 权威状态的显式视图。本模块不持有第二份世界、脚本、渲染或编辑器
// 状态：所有读取都经这里传入的指针完成。
struct EngineUiViewModelContext
{
    WorldManager* worldManager = nullptr;
    ScriptRuntimeSubsystem* scriptRuntime = nullptr;
    ScriptBuildService* scriptBuild = nullptr;
    Controller* controller = nullptr;
    const RenderSystem* renderSystem = nullptr;
    bool autoCompileScripts = false;
    const std::string* scriptBuildOutput = nullptr;
    // 编辑器撤销状态由 host 提供；没有 host（game flavor）时为空。
    std::function<bool(const std::string&, bool)> canUndo;
    // EngineLoop 独占的派生缓存槽；模块只在其失效时重建，不另存一份。
    std::unique_ptr<UiAnimationPreviewCache>* animationPreviewCache = nullptr;
};

// 只填充逐帧变化的轻量字段（世界路径、离线标记、调试绘制、相机、HUD 文本）。
void FillUiViewModelLiveFrame(
    UiViewModelFrameValues& frame,
    UiViewModelBuildMode buildMode,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context);

void BuildUiScriptsSection(
    UiScriptsSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context);
void BuildUiAnimationSection(
    UiAnimationPreviewSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context);
void BuildUiPerformanceSection(
    UiPerformanceSection& section,
    const EngineUiViewModelContext& context);
void BuildUiViewportCameraSection(
    UiViewportCameraSection& section,
    const UiViewModelFrameValues& frameValues,
    const EngineUiViewModelContext& context);
void BuildUiEnvironmentLightingSection(
    UiEnvironmentLightingSection& section,
    const WorldHandle& activeWorld,
    const EngineUiViewModelContext& context);
void BuildUiOfflineSection(
    UiOfflineSection& section,
    const RuntimeCommandExecutionResult& commandResult,
    const EngineUiViewModelContext& context);

// 动画预览的派生数据按“世界代数 + 播放器身份”缓存；每帧只重算随播放时间
// 变化的值，不重复分配 clip 列表/特征通道/schema。
void CaptureUiAnimationPreview(
    UiAnimationPreviewSection& section,
    const World& world,
    uint64_t generation,
    const EngineUiViewModelContext& context);

} // namespace VL
