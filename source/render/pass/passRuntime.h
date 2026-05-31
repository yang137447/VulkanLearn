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

// Temporary service bridge used while pass recording is moving out of
// RenderSystem. PassRuntime owns pass flow; services expose the remaining frame
// data uploads until descriptor-backed draw packets are fully backend-owned.
class PassRuntimeServices : public RendererDrawUploadServices
{
public:
    virtual ~PassRuntimeServices() = default;

    virtual void UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer) = 0;
    virtual void UpdateShadowGlobalUBOForPass(
        vk::CommandBuffer& commandBuffer,
        uint32_t passWidth,
        uint32_t passHeight) = 0;
    virtual void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) = 0;
    virtual void UpdateObjectUBOForPass(
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket) = 0;
    virtual void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<LightSnapshot>& lights) = 0;
};

struct PassRuntimeContext
{
    vk::CommandBuffer& commandBuffer;
    const Renderpass& renderPass;
    RenderGraph& renderGraph;
    size_t passIndex = 0;
    uint32_t swapChainImageIndex = 0;
    const RenderScene& renderScene;
    const ResolvedRenderScene& resolvedRenderScene;
    PassRuntimeServices& services;
};

// Records one compiled Renderpass using render-domain data. This is still a
// bridge over the existing Vulkan draw path, but it gives FrameGraph/PassRuntime
// a real ownership point separate from RenderSystem::Render().
class PassRuntime
{
public:
    void RecordPass(const std::string& passName, PassRuntimeContext& context) const;

private:
    void RecordShadowPass(PassRuntimeContext& context) const;
    void RecordGeometryPass(PassRuntimeContext& context) const;
    void RecordPostProcessPass(PassRuntimeContext& context) const;

    void PreparePassResources(PassRuntimeContext& context) const;
    void BeginConfiguredRenderPass(PassRuntimeContext& context) const;

    RendererDrawExecutor drawExecutor;
};

} // namespace VL
