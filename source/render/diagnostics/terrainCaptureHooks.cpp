#include "render/diagnostics/terrainCaptureHooks.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/backend/rendererObjectGpuResources.h"
#include "render/frontend/renderScene.h"
#include "renderableObject.h"
#include "renderGraph.h"
#include "pipeline/pipelineBase.h"
#include "shaderReflect.h"
#include <atomic>
#include <stdexcept>

namespace VL
{
namespace
{
std::shared_ptr<ITerrainCaptureHooks> activeHooks;
std::atomic<bool> enabled{false};
}
std::shared_ptr<ITerrainCaptureHooks> AcquireTerrainCaptureHooks()
{
    return std::atomic_load(&activeHooks);
}
void RegisterTerrainCaptureHooks(std::shared_ptr<ITerrainCaptureHooks> hooks)
{
    if (enabled.load() || (hooks && std::atomic_load(&activeHooks)))
        throw std::runtime_error("Terrain capture hooks must register once before logical device creation");
    std::atomic_store(&activeHooks, std::move(hooks));
}
void PublishTerrainCaptureDeviceFeature(bool value)
{
    enabled.store(value, std::memory_order_release);
}
bool TerrainCaptureDeviceFeatureEnabled()
{
    return enabled.load(std::memory_order_acquire);
}
TerrainCaptureDrawInputs BuildTerrainCaptureDrawInputs(const RenderDrawPacket& packet,
    uint32_t drawPacketIndex, uint32_t frameSlot, uint32_t swapchainIndex,
    const PipelineBase& regularPipeline, const Renderpass& pass,
    std::shared_ptr<RenderableObject> renderable,
    std::shared_ptr<RendererObjectResourceEntry> objectResource,
    std::shared_ptr<Material> material, std::shared_ptr<MaterialInstance> instance)
{
    if (!renderable || !objectResource)
        throw std::runtime_error("Terrain capture lost actual resolved draw owners");
    TerrainCaptureDrawInputs inputs;
    inputs.packet = &packet;
    inputs.regularPipeline = &regularPipeline;
    inputs.drawPacketIndex = drawPacketIndex;
    inputs.frameSlot = frameSlot;
    inputs.mesh = renderable->DescribeDrawBindings(frameSlot, regularPipeline.UsesTessellation());
    inputs.renderableOwner = std::move(renderable);
    inputs.objectResourceOwner = std::move(objectResource);
    inputs.materialOwner = std::move(material);
    inputs.materialInstanceOwner = std::move(instance);
    const auto& objectSets = inputs.objectResourceOwner->GetResources().descriptorSets.at(swapchainIndex);
    for (const auto& binding : regularPipeline.GetShaderBindings())
    {
        if (binding.set >= 4)
            throw std::runtime_error("Normal draw already uses diagnostic descriptor set");
        const uint32_t set = binding.set;
        inputs.descriptorSetMask |= 1u << set;
        inputs.descriptorSets[set] = set == 3
            ? pass.GetDescriptorSets().at(swapchainIndex).at(3)
            : objectSets.at(set);
    }
    return inputs;
}
}
