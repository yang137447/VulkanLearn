#pragma once

#include <memory>
#include <cstddef>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

#include "render/backend/rendererDrawExecutor.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"
#include "render/shadow/shadowCasterCull.h"

class MaterialInstance;
class PipelineBase;
class RenderGraph;
struct Renderpass;

namespace VL
{

// Frame-data upload and renderer-state query service used by pass recording.
// PassRuntime owns pass flow, while RenderSystem supplies updates and policy
// through this explicit boundary.
class PassRuntimeServices : public RendererDrawUploadServices
{
public:
    virtual ~PassRuntimeServices() = default;

    virtual void UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer) = 0;
    virtual void UpdateShadowGlobalUBOForPass(
        vk::CommandBuffer& commandBuffer,
        uint32_t passWidth,
        uint32_t passHeight,
        uint32_t cascadeIndex) = 0;
    virtual void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) = 0;
    virtual void UpdateObjectUBOForPass(
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket) = 0;
    virtual void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<LightSnapshot>& lights) = 0;
    virtual bool IsCsmEnabled() const = 0;
    virtual bool IsShadowCascadeActive(uint32_t cascadeIndex) const = 0;
    // 返回该级联实际用于渲染的 light view-projection；没有 CSM 的调用方返回 false。
    // 阴影 pass 用它构造与 shadow map 逐 texel 对齐的 caster 剔除体。
    virtual bool GetShadowCascadeLightViewProjection(
        uint32_t,
        Eigen::Matrix4f&) const
    {
        return false;
    }
    // 返回该级联接收面在光源空间的 XY 范围；没有 CSM 的调用方返回 false。
    virtual bool GetShadowCascadeCasterReceiverBounds(
        uint32_t,
        ShadowCasterReceiverBounds&) const
    {
        return false;
    }
    // 编辑器参考网格的几何与缓冲归 RenderSystem 所有；pass 流程只请求提交一次
    // 网格绘制，因此网格顶点生命周期不会泄漏到 pass 层。
    virtual void DrawEditorGridGeometry(vk::CommandBuffer& commandBuffer) = 0;
};

struct PassRuntimeContext
{
    vk::CommandBuffer& commandBuffer;
    const Renderpass& renderPass;
    const RenderGraph& renderGraph;
    size_t passIndex = 0;
    uint32_t swapChainImageIndex = 0;
    const RenderScene& renderScene;
    const ResolvedRenderScene& resolvedRenderScene;
    PassRuntimeServices& services;
    // 仅重定向输出 swapChain 的 pass，其他图资源和完整 backing extent 不变。
    // LOAD 前调用方必须保证目标已有有效内容并处于 ColorAttachmentOptimal。
    bool renderToViewport = false;
    // 可选诊断输出：只有阴影 pass 会写，渲染系统聚合后按 --gpu-stats 周期打印。
    ShadowCasterCullStats* shadowCasterCullStats = nullptr;
};

// Records one compiled Renderpass using render-domain data. FrameGraph and
// PassRuntime own pass flow separately from RenderSystem frame orchestration.
class PassRuntime
{
public:
    void RecordPass(const std::string& passName, PassRuntimeContext& context) const;

private:
    void RecordShadowPass(PassRuntimeContext& context) const;
    void RecordGeometryPass(PassRuntimeContext& context) const;
    void RecordForwardOpaquePass(PassRuntimeContext& context) const;
    void RecordForwardTransparentPass(PassRuntimeContext& context) const;
    void RecordPostProcessPass(PassRuntimeContext& context) const;
    void RecordEditorGridPass(PassRuntimeContext& context) const;

    void PreparePassResources(PassRuntimeContext& context) const;
    void BeginConfiguredRenderPass(PassRuntimeContext& context) const;
    // PostProcess 与 EditorGrid 都要"绑定 pass material 管线 + 描述符集"，
    // 区别只在最后提交的几何；返回 false 表示该 pass 没有可用材质，不进入 render pass。
    bool BindPassMaterialPipeline(PassRuntimeContext& context) const;
    void UpdateSceneGlobalUBO(PassRuntimeContext& context) const;

    RendererDrawExecutor drawExecutor;
};

} // namespace VL
