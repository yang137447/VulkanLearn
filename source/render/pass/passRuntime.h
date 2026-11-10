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
    // LocalShadow pass 由 RenderSystem 每帧选灯后激活。缩略图/离线等没有
    // 局部阴影生产者的会话保持默认 false，仍需 begin/end 完成 layout 转换。
    virtual bool IsLocalShadowActive(uint32_t localShadowIndex) const
    {
        (void)localShadowIndex;
        return false;
    }
    // 0 = 透视聚光，1 = 八面体点光。PassRuntime 用它决定一个 pass 内
    // 录制一次还是八个象限各录制一次。
    virtual uint32_t GetLocalShadowMode(uint32_t localShadowIndex) const
    {
        (void)localShadowIndex;
        return 0;
    }
    virtual void UpdateLocalShadowUBOForPass(
        vk::CommandBuffer& commandBuffer,
        uint32_t passWidth,
        uint32_t passHeight,
        uint32_t lightIndex,
        uint32_t octant,
        uint32_t mode)
    {
        (void)commandBuffer;
        (void)passWidth;
        (void)passHeight;
        (void)lightIndex;
        (void)octant;
        (void)mode;
    }
    // mesh 粒子 caster 的 UBO 更新必须在 render pass 外进行：同一
    // per-image UBO 要在一个 pass 内按 CSM 级联/八面体象限多次覆盖，
    // 只能依赖有序的 vkCmdUpdateBuffer。Record 阶段只做管线/描述符/绘制。
    virtual void PrepareParticleShadowCasters(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex,
        uint32_t octant)
    {
        (void)commandBuffer;
        (void)renderPass;
        (void)swapChainImageIndex;
        (void)octant;
    }
    virtual void RecordParticleShadowCasters(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex)
    {
        (void)commandBuffer;
        (void)renderPass;
        (void)swapChainImageIndex;
    }
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
    // 粒子发射器资源归 RenderSystem/ParticleRenderer 所有；pass 只提供已开始的
    // sceneColor 合成窗口和 sceneDepth 输入描述符集，避免在 RenderGraph 里
    // 再建一套材质合同。
    virtual void RecordParticlePass(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex) = 0;
    // 屏幕空间扰动与粒子共享 emitter 资源，但写独立的 distortion 缓冲；
    // pass 只提供已清空的颜色窗口和 sceneDepth 输入描述符集。
    virtual void RecordDistortionPass(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex) = 0;
    // 体积雾合成的 pipeline/descriptor 归 VolumetricFogRenderer 所有；
    // pass 层只提供已开始的 sceneColor 窗口与 viewport。没有体积雾的会话
    // （缩略图/离线）保持空录制。
    virtual void RecordFogCompositePass(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex)
    {
        (void)commandBuffer;
        (void)renderPass;
        (void)swapChainImageIndex;
    }
    // DecalRenderer 拥有 unit cube、实例 SSBO 与 decal pipeline；pass 仅提供
    // 已开始的 DBuffer A/B/C/D + Mask 窗口和 sceneDepth 输入描述符集。
    // 主渲染路径必须覆写；没有 DecalRenderer 的缩略图会话保持空录制，
    // 其 decal pass 本身也是空录制，因此不需要 stencil mask。
    virtual void RecordDecalStencilPass(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex,
        const RenderScene& renderScene)
    {
        (void)commandBuffer;
        (void)renderPass;
        (void)swapChainImageIndex;
        (void)renderScene;
    }
    virtual void RecordDecalPass(
        vk::CommandBuffer& commandBuffer,
        const Renderpass& renderPass,
        uint32_t swapChainImageIndex,
        const RenderScene& renderScene) = 0;
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
    void RecordLocalShadowPass(PassRuntimeContext& context) const;
    void RecordGeometryPass(PassRuntimeContext& context) const;
    void RecordForwardOpaquePass(PassRuntimeContext& context) const;
    void RecordForwardTransparentPass(PassRuntimeContext& context) const;
    void RecordPostProcessPass(PassRuntimeContext& context) const;
    void RecordEditorGridPass(PassRuntimeContext& context) const;
    void RecordParticlePass(PassRuntimeContext& context) const;
    void RecordDistortionPass(PassRuntimeContext& context) const;
    void RecordFogCompositePass(PassRuntimeContext& context) const;
    void RecordDecalStencilPass(PassRuntimeContext& context) const;
    void RecordDecalPass(PassRuntimeContext& context) const;
    void RecordTerrainFeedbackPass(PassRuntimeContext& context) const;

    void PreparePassResources(PassRuntimeContext& context) const;
    void BeginConfiguredRenderPass(PassRuntimeContext& context) const;
    // PostProcess 与 EditorGrid 都要"绑定 pass material 管线 + 描述符集"，
    // 区别只在最后提交的几何；返回 false 表示该 pass 没有可用材质，不进入 render pass。
    bool BindPassMaterialPipeline(PassRuntimeContext& context) const;
    void UpdateSceneGlobalUBO(PassRuntimeContext& context) const;

    RendererDrawExecutor drawExecutor;
};

} // namespace VL
