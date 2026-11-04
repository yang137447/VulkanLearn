#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/rendererObjectGpuResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"

class MaterialInstance;
class PipelineBase;
struct Renderpass;

namespace VL
{

// Backend 绘制阶段所需的上传回调。RenderSystem 只实现这条窄更新边界，
// 帧资源和对象描述符的所有权仍留在 renderer backend。
class RendererDrawUploadServices
{
public:
    virtual ~RendererDrawUploadServices() = default;

    virtual void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) = 0;
    virtual void UpdateObjectUBOForPass(
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket) = 0;
    virtual void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<LightSnapshot>& lights) = 0;
    // 局部阴影 caster 剔除与低频统计。默认实现关闭剔除，供缩略图/
    // 离线等没有局部阴影生产者的会话复用。
    virtual const LightSnapshot* GetLocalShadowLight(
        uint32_t localShadowIndex) const
    {
        (void)localShadowIndex;
        return nullptr;
    }
    virtual void ReportLocalShadowCasterStats(
        uint32_t localShadowIndex,
        uint32_t drawnCasters,
        uint32_t totalCasters)
    {
        (void)localShadowIndex;
        (void)drawnCasters;
        (void)totalCasters;
    }
};

struct RendererDrawContext
{
    vk::CommandBuffer& commandBuffer;
    const Renderpass& renderPass;
    uint32_t swapChainImageIndex = 0;
    const RenderScene& renderScene;
    const ResolvedRenderScene& resolvedRenderScene;
    RendererDrawUploadServices& services;
};

// Backend 侧 DrawPacket 执行器，直接消费已解析的 Renderable 句柄和 backend GPU 资源。
class RendererDrawExecutor
{
public:
    bool PipelineUsesDescriptorSet(const PipelineBase& pipeline, uint32_t setIndex) const;
    void DrawShadowScene(const PipelineBase& commonOpaquePipeline, RendererDrawContext& context) const;
    // light 为空表示调用方没有可用的冻结灯数据；此时不做剔除，保持旧的全量绘制。
    // recordCasterStats 只在点光首个象限/聚光唯一一次为 true，避免重复统计。
    void DrawLocalShadowScene(
        const PipelineBase& commonLocalShadowPipeline,
        RendererDrawContext& context,
        const LightSnapshot* light,
        bool recordCasterStats) const;
    void DrawGeometryScene(RendererDrawContext& context) const;
    void DrawForwardOpaqueScene(RendererDrawContext& context) const;
    void DrawForwardTransparentScene(RendererDrawContext& context) const;

private:
    enum class SurfaceDrawDomain
    {
        Geometry,
        ForwardOpaque
    };

    void DrawSurfaceScene(
        const char* passName,
        SurfaceDrawDomain domain,
        RendererDrawContext& context) const;
    // 透明绘制必须跨材质全局后向前排序，不能复用按 Material 分组的普通 Surface 遍历。
    void DrawSortedTransparentScene(RendererDrawContext& context) const;
};

} // namespace VL
