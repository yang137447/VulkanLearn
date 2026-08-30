#pragma once

#include <memory>
#include <cstddef>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/rendererDrawExecutor.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"

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
    void RecordForwardEyeInnerPass(PassRuntimeContext& context) const;
    void RecordForwardEyeCorneaPass(PassRuntimeContext& context) const;
    void RecordForwardTransparentPass(PassRuntimeContext& context) const;
    void RecordPostProcessPass(PassRuntimeContext& context) const;

    void PreparePassResources(PassRuntimeContext& context) const;
    void BeginConfiguredRenderPass(PassRuntimeContext& context) const;
    void UpdateSceneGlobalUBO(PassRuntimeContext& context) const;

    RendererDrawExecutor drawExecutor;
};

} // namespace VL
