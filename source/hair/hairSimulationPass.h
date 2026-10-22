#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "hair/tressfxSimulationParams.h"

class ComputePipeline;
class PipelineFactory;
class RenderableObject;

namespace VL
{
class RendererBackendVulkan;
struct ResolvedRenderScene;
struct RenderScene;

namespace Hair
{

// TressFX guide 模拟的 GPU 执行者。每个 tressfx 对象持有一组 host-coherent buffer：
// current(复用 graphics Object Set 2 binding 3)、previous、rest(w=逆质量) 和 params。
// 本类只拥有 compute pipeline、descriptor pool 和逐对象的 descriptor set 缓存，
// 不拥有对象 buffer，也不向顶点 buffer 回写位置。
//
// 生命周期与 RenderGraph 的 AutoExposure 一致：随图候选创建，SwapState 移交给活动图，
// Shutdown 时按 GPU epoch 退休。pipeline 走 CreateComputePipelineCandidate，不注册
// hot reload participant，因此 shader 热重载不会替换它。
class HairSimulationPass
{
public:
    ~HairSimulationPass();
    HairSimulationPass() = default;
    HairSimulationPass(const HairSimulationPass&) = delete;
    HairSimulationPass& operator=(const HairSimulationPass&) = delete;

    void Initialize(PipelineFactory& factory, RendererBackendVulkan& backend);
    // frameSlot 是 frame-in-flight 槽；调用方在每帧 pass 循环之前调用一次。
    // renderScene 是同一帧冻结的前端快照：simulation.skinning 从它的 draw packet 读取
    // SkinPose，不从头重建场景查询，也不在 pass 里持有播放器。
    void Record(vk::CommandBuffer commandBuffer,
        const VL::ResolvedRenderScene& scene,
        const VL::RenderScene& renderScene,
        uint32_t frameSlot,
        float deltaSeconds);

private:
    struct ObjectDescriptors
    {
        std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> sets{};
        vk::Buffer currentPositions = nullptr;
        vk::Buffer previousPositions = nullptr;
        vk::Buffer restPositions = nullptr;
        vk::Buffer colliders = nullptr;
        std::array<vk::Buffer, GuideSimulationParamBufferCount> params{};
        // 与 CPU GuideSimulation 完全相同的固定步 accumulator 与 frame-start 时间。
        // 未引入 GPU 侧状态 buffer，因此这份状态随 pass/图重建归零。
        float accumulator = 0.0f;
        float simulationTime = 0.0f;
        // simulation.skinning 的逐对象缓存：pose/源 draw 缺失时保留上一帧矩阵继续渲染，
        // 不用单位矩阵覆盖，也不销毁 pass/descriptor。初始单位矩阵等于历史 rest pin。
        Eigen::Matrix4f skinningMatrix = Eigen::Matrix4f::Identity();
        bool reportedMissingSkinPose = false;
    };

    struct DescriptorPoolBudget
    {
        vk::DescriptorPool pool = nullptr;
        uint32_t remainingSets = 0;
    };

    vk::DescriptorPool AcquireDescriptorPool();
    ObjectDescriptors& EnsureObjectDescriptors(RenderableObject& object);
    void CreateObjectDescriptors(
        RenderableObject& object,
        ObjectDescriptors& descriptors);
    void ResetDescriptorCache() noexcept;

    RendererBackendVulkan* backend = nullptr;
    std::shared_ptr<ComputePipeline> pipeline;
    std::vector<DescriptorPoolBudget> descriptorPools;
    std::unordered_map<const RenderableObject*, ObjectDescriptors> objectDescriptors;
    bool initialized = false;
    bool reportedFirstDispatch = false;
};

} // namespace Hair
} // namespace VL
