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
// TressFX 5.0 guide solver 的 pass 顺序，严格对应源 .usf：
//   Integration -> StrandLevel -> VSP -> LocalShape -> LengthConstraintsWind -> Collision
// 前五个与 TressFX 源文件一一对应；Collision 是 VL 在长度 pass 之后追加的胶囊投影。
enum class GuideSimulationPass : uint32_t
{
    Integration = 0,
    StrandLevel,
    Vsp,
    LocalShape,
    LengthWind,
    Collision,
    Count,
};
inline constexpr uint32_t GuideSimulationPassCount =
    static_cast<uint32_t>(GuideSimulationPass::Count);

// TressFX guide 模拟的 GPU 执行者。每个 tressfx 对象持有一组 device-local buffer：
// current（与 graphics Object Set 2 binding 3 共用）、previous、prevPrev、rest(w=逆质量)、
// restLengths、strandLevel；外加每帧改写的 params UBO 和静态 collider SSBO。
// 本类只拥有 6 个 compute pipeline、各自的 descriptor pool、逐对象的 per-pass
// descriptor set 缓存和 per-object simulationTime，不拥有对象 buffer。
//
// 生命周期与 RenderGraph 的 AutoExposure 一致：随图候选创建，SwapState 移交给活动图，
// Shutdown 时按 GPU epoch 退休。pipeline 走 CreateComputePipelineCandidate，不注册
// hot reload participant，因此 shader 热重载不会替换它们。
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
        // [pass][frame-in-flight slot]。每个 pass 的 shader 只声明自己用到的 binding，
        // 因此 descriptor set layout 按 pipeline 独立，不能跨 pass 复用同一个 set。
        std::array<std::array<vk::DescriptorSet, GuideSimulationParamBufferCount>,
            GuideSimulationPassCount>
            sets{};
        vk::Buffer currentPositions = nullptr;
        vk::Buffer previousPositions = nullptr;
        vk::Buffer previousPreviousPositions = nullptr;
        vk::Buffer restPositions = nullptr;
        vk::Buffer restLengths = nullptr;
        vk::Buffer strandLevel = nullptr;
        vk::Buffer colliders = nullptr;
        std::array<vk::Buffer, GuideSimulationParamBufferCount> params{};
        // 变量步长：每帧一次 solver step，只保留 frame-start simulationTime 供阵风使用。
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

    vk::DescriptorPool AcquireDescriptorPool(uint32_t pass);
    ObjectDescriptors& EnsureObjectDescriptors(RenderableObject& object);
    void CreateObjectDescriptors(
        RenderableObject& object,
        ObjectDescriptors& descriptors);
    void RecordPass(vk::CommandBuffer commandBuffer,
        GuideSimulationPass pass,
        vk::DescriptorSet descriptorSet,
        uint32_t groupCount,
        bool& anyDispatch);
    void ResetDescriptorCache() noexcept;

    RendererBackendVulkan* backend = nullptr;
    std::array<std::shared_ptr<ComputePipeline>, GuideSimulationPassCount> pipelines{};
    // 每个 pass 独立 descriptor pool 链；逐对象按 frame-in-flight 槽各分配一个 set。
    std::array<std::vector<DescriptorPoolBudget>, GuideSimulationPassCount> descriptorPools;
    std::unordered_map<const RenderableObject*, ObjectDescriptors> objectDescriptors;
    bool initialized = false;
    bool reportedFirstDispatch = false;
};

} // namespace Hair
} // namespace VL
