#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

class ComputePipeline;
class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;
struct RendererObjectGpuResources;
struct ResolvedRenderScene;

namespace Skinning
{

// 逐帧 GPU 预蒙皮 compute pass。每个蒙皮对象持有一份 device-local 输出 buffer
// （跨帧复用，dispatch 前后的屏障覆盖 RAW/WAR）；本 pass 只拥有 pipeline、
// descriptor pool 和逐对象 descriptor set 缓存，不拥有对象 buffer/palette。
//
// 生命周期与 HairSimulationPass 一致：随 RenderGraph 候选创建，SwapState 移交，
// Shutdown 按 GPU epoch 退休；pipeline 走 CreateComputePipelineCandidate，不注册
// shader 热重载 participant。
class SkinDeformPass
{
public:
    ~SkinDeformPass();
    SkinDeformPass() = default;
    SkinDeformPass(const SkinDeformPass&) = delete;
    SkinDeformPass& operator=(const SkinDeformPass&) = delete;

    void Initialize(PipelineFactory& factory, RendererBackendVulkan& backend);
    // 只判断本帧是否存在已分配 pre-skin 输出 buffer 的对象。调用方必须在写
    // skinnedDeform timestamp 之前调用，空帧不占 query 槽位。
    bool HasWork(const VL::ResolvedRenderScene& scene) const;
    // 调用方负责在本函数之前完成本帧 palette 上传（compute 读同一份 host-visible
    // palette）。imageIndex 同时索引 palette 和输出 buffer 的 swapchain image 槽位。
    void Record(vk::CommandBuffer commandBuffer,
        const VL::ResolvedRenderScene& scene,
        uint32_t imageIndex);

private:
    struct ObjectDescriptors
    {
        std::vector<vk::DescriptorSet> sets;
        // 缓存创建时的 buffer 句柄；对象资源包换代后旧 set 不能复用。
        std::vector<vk::Buffer> paletteBuffers;
        // 输出与参数是单份 buffer，所有 image 槽位的 set 指向同一份。
        vk::Buffer deformedBuffer = nullptr;
        vk::Buffer influences = nullptr;
        vk::Buffer sourceVertices = nullptr;
        vk::Buffer params = nullptr;
    };

    struct DescriptorPoolBudget
    {
        vk::DescriptorPool pool = nullptr;
        uint32_t remainingSets = 0;
    };

    std::vector<vk::DescriptorSet> AcquireDescriptorSets(uint32_t count);
    ObjectDescriptors& EnsureObjectDescriptors(
        const VL::RendererObjectGpuResources& resources);
    void CreateObjectDescriptors(
        const VL::RendererObjectGpuResources& resources,
        ObjectDescriptors& descriptors);
    void ResetDescriptorCache() noexcept;

    RendererBackendVulkan* backend = nullptr;
    std::shared_ptr<ComputePipeline> pipeline;
    std::vector<DescriptorPoolBudget> descriptorPools;
    std::vector<std::pair<const VL::RendererObjectGpuResources*, ObjectDescriptors>>
        objectDescriptors;
    bool initialized = false;
    bool reportedFirstDispatch = false;
};

} // namespace Skinning
} // namespace VL
