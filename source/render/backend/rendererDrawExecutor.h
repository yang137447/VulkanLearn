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
    void DrawGeometryScene(RendererDrawContext& context) const;
    void DrawForwardTransparentScene(RendererDrawContext& context) const;

private:
    void DrawSurfaceScene(
        const char* passName,
        bool drawTransparent,
        RendererDrawContext& context) const;
    // 透明绘制必须跨材质全局后向前排序，不能复用按 Material 分组的普通 Surface 遍历。
    void DrawSortedTransparentScene(RendererDrawContext& context) const;
};

} // namespace VL
