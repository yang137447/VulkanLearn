#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "editor/assets/editorJsonAssetTypes.h"

class PipelineFactory;
namespace VL
{
class RendererBackendVulkan;

// 粒子缩略图的确定性预览策略：当前显示 0.5s / 30 个固定步。常数放在
// 头文件里，测试可验证“缩略图实际推进的步数”而不启动 Vulkan。
struct ParticleThumbnailStepPolicy
{
    float fixedDeltaSeconds;
    float previewSeconds;
    uint32_t maxSteps;
};

inline constexpr ParticleThumbnailStepPolicy ParticleThumbnailSteps() noexcept
{
    return {1.0f / 60.0f, 0.5f, 32};
}

inline constexpr uint32_t ParticleThumbnailStepCount() noexcept
{
    const ParticleThumbnailStepPolicy policy = ParticleThumbnailSteps();
    const float ratio =
        policy.previewSeconds / policy.fixedDeltaSeconds;
    const uint32_t steps =
        static_cast<uint32_t>(ratio + 0.99999f);
    return steps > policy.maxSteps ? policy.maxSteps : steps;
}

// GT 稳定帧边界独占推进；不向 WorldManager 发布，不借用活动图的 UBO/descriptor。
class EditorThumbnailRenderSession
{
public:
    EditorThumbnailRenderSession(RendererBackendVulkan& backend, PipelineFactory& factory);
    ~EditorThumbnailRenderSession();
    // Refresh 清理会话缓存，但保留唯一 CPU 任务直至丢弃其结果，不阻塞 GT。
    void Reset();
    void Begin(const std::string& path, EditorUi::EditorAssetKind kind,
        const nlohmann::json& graph);
    // 每次只推进一个准备阶段、一个 IBL mip 或一张贴图上传；CPU 任务未完成时非阻塞返回 false。
    // 返回 true 表示完成（包括失败）。
    bool Step();
    std::shared_ptr<const RendererGpuImageToken> Image() const;
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
