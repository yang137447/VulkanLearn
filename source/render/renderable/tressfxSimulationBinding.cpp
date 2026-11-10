#include "render/renderable/tressfxSimulationBinding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "renderableObject.h"
#include "render/renderable/renderableMeshBuffers.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "hair/tressfxGroupGpuData.h"
#include "hair/tressfxGroupSimulation.h"
#include "hair/tressfxLiveParamsApply.h"
#include "hair/tressfxSdfGrid.h"

namespace VL
{
namespace RenderableDetail
{VL::Hair::SimulationConfig MakeCpuSimulationConfig(
    const TressfxSimulationConfig& config)
{
    VL::Hair::SimulationConfig cpu;
    cpu.gravity = config.gravity;
    // 风不在资产里：CPU config 的 wind 保持零，由
    // AdvanceTressfxSimulation 每帧在 StepFrame 之前写入场景风向量。
    cpu.windGain = config.windGain;
    cpu.strandRadius = config.strandRadius;
    cpu.damping = config.damping;
    cpu.iterations = config.iterations;
    cpu.rootFixedPoints = config.rootFixedPoints;
    cpu.maxDt = config.maxDt;
    cpu.bendStiffness = config.bendStiffness;
    cpu.globalShapeStiffness = config.globalShapeStiffness;
    cpu.globalShapeEffectiveRange = config.globalShapeEffectiveRange;
    cpu.vspCoefficient = config.vspCoefficient;
    cpu.vspAccelThresholdMax = config.vspAccelThresholdMax;
    cpu.vspStiffness = config.vspStiffness;
    cpu.localShapeStiffness = config.localShapeStiffness;
    cpu.capsules = config.capsules;
    return cpu;
}
} // namespace RenderableDetail
} // namespace VL
bool RenderableObject::ApplyTressfxLiveParameter(
    const VL::Hair::HairLiveParameterRequest& request, std::string& error)
{
    // CPU backend 没有逐帧 params UBO/pose 通道，覆盖不会生效；这里明确拒绝，
    // 不能让调用方以为参数已经作用在 CPU 参考 solver 上。
    if (!IsTressfx() || !UsesGpuGuideSimulation())
    {
        error = "TressFX live parameters require a GPU guide object";
        return false;
    }
    // 先在所有副本上试写，成功后再一次性提交，避免某一组留下半改状态。
    // 实时覆盖是调试语义，应压过作者默认值与组覆盖；否则分组后这些控件会
    // 因为 shader 只读组参数表而完全失效。
    TressfxSimulationConfig baseCandidate = tressfxSimulationConfig;
    if (!VL::Hair::ApplyHairLiveParameter(baseCandidate, request, error))
        return false;
    std::vector<TressfxSimulationConfig> groupCandidates =
        tressfxGroupSimulationConfigs;
    for (TressfxSimulationConfig& candidate : groupCandidates)
        if (!VL::Hair::ApplyHairLiveParameter(candidate, request, error))
            return false;
    tressfxSimulationConfig = std::move(baseCandidate);
    tressfxGroupSimulationConfigs = std::move(groupCandidates);
    // 实时覆盖也算当前对象状态：后续资产重载看到不同 identity 时会恢复作者值。
    tressfxGroupSimulationKey =
        VL::Hair::MakeTressfxGroupSimulationKey(tressfxGroupSimulationConfigs);
    return true;
}

const vk::DescriptorBufferInfo& RenderableObject::GetTressfxParamsInfo(
    uint32_t frameSlot) const
{
    return tressfxParamsTargets[frameSlot].info;
}

void RenderableObject::WriteTressfxSimulationParams(
    const VL::Hair::GuideSimulationParams& params,
    uint32_t frameSlot)
{
    // host-coherent 映射：写入当前 frame-in-flight 槽即可，beginFrame 已确认该槽
    // 上一帧的 fence 完成，因此不会与仍在执行的 dispatch 重叠。
    std::memcpy(tressfxParamsTargets[frameSlot].mapped, &params, sizeof(params));
}

void RenderableObject::WriteTressfxGroupSimulationConfigs(uint32_t frameSlot)
{
    if (frameSlot >= VL::Hair::GuideSimulationParamBufferCount ||
        !tressfxGuideGroupConfigTargets[frameSlot].mapped)
        return;
    const vk::DeviceSize capacity =
        tressfxGuideGroupConfigTargets[frameSlot].info.range /
        sizeof(VL::Hair::TressfxGroupSimulationGpu);
    const std::vector<VL::Hair::TressfxGroupSimulationGpu> data =
        VL::Hair::BuildTressfxGroupSimulationGpuData(
            tressfxGroupSimulationConfigs, static_cast<size_t>(capacity));
    if (data.empty() || data.size() != capacity)
        return;
    std::memcpy(tressfxGuideGroupConfigTargets[frameSlot].mapped, data.data(),
        data.size() * sizeof(VL::Hair::TressfxGroupSimulationGpu));
}

bool RenderableObject::UpdateTressfxGroupBinding(
    const VL::Hair::TressfxGroupBinding& binding,
    const std::vector<TressfxSimulationConfig>& groupConfigs,
    std::string& error)
{
    error.clear();
    if (!IsTressfx())
    {
        error = "TressFX group binding update requires a guide renderable";
        return false;
    }
    if (!VL::Hair::ValidateTressfxGroupPreviewBinding(
            binding, tressfxGuideCount, error))
        return false;
    // 颜色/参数表在对象创建时按上限预留；descriptor 只捕获一次 buffer，
    // 组数增删都原地重写。这里先完成全部容量与配置校验，再触碰任何 GPU 数据。
    const uint32_t colorCount = static_cast<uint32_t>(binding.groupNames.size()) + 1u;
    const vk::DeviceSize colorCapacity =
        tressfxGuideGroupColorInfo.range / sizeof(VL::Hair::TressfxGroupColorGpu);
    if (!tressfxGuideGroupIdBuffer || !tressfxGuideGroupColorBuffer ||
        !tressfxGuideGroupColorMemory || colorCount > colorCapacity)
    {
        error = "TressFX group count exceeds the preview color table capacity";
        return false;
    }
    // 允许只给一份有效配置（调用方没有 per-group 解析结果时），否则必须与组数一致。
    std::vector<TressfxSimulationConfig> resolvedConfigs = groupConfigs;
    if (resolvedConfigs.size() != colorCount)
    {
        if (resolvedConfigs.size() != 1)
        {
            error = "TressFX resolved group config count does not match the group count";
            return false;
        }
        resolvedConfigs.assign(colorCount, resolvedConfigs.front());
    }
    const bool hasGroupConfigTable =
        static_cast<bool>(tressfxGuideGroupConfigTargets[0].buffer) &&
        tressfxGuideGroupConfigTargets[0].info.range != 0;
    const vk::DeviceSize groupConfigCapacity = hasGroupConfigTable
        ? tressfxGuideGroupConfigTargets[0].info.range /
            sizeof(VL::Hair::TressfxGroupSimulationGpu)
        : 0;
    if (hasGroupConfigTable && resolvedConfigs.size() > groupConfigCapacity)
    {
        error = "TressFX resolved group config count exceeds the GPU buffer capacity";
        return false;
    }

    // 1) 每 guide 组号：与几何同长度，原地覆盖 host-coherent SSBO。
    const vk::DeviceSize idBytes =
        static_cast<vk::DeviceSize>(binding.guideGroupIds.size()) * sizeof(uint32_t);
    void* idMapped = rendererBackend->MapMemory(tressfxGuideGroupIdMemory, idBytes);
    std::memcpy(idMapped, binding.guideGroupIds.data(), static_cast<size_t>(idBytes));
    rendererBackend->UnmapMemory(tressfxGuideGroupIdMemory);

    // 2) 组颜色表：名称决定色块；尾部按固定容量补默认灰，支持组数增删。
    const VL::Hair::TressfxGroupGpuData groupData = VL::Hair::BuildTressfxGroupGpuData(
        tressfxGuideCount, binding.guideGroupIds, binding.groupNames,
        VL::Hair::TressfxGroupPreviewColorCapacity);
    const vk::DeviceSize colorBytes =
        static_cast<vk::DeviceSize>(groupData.groupColors.size()) *
        sizeof(VL::Hair::TressfxGroupColorGpu);
    void* colorMapped = rendererBackend->MapMemory(
        tressfxGuideGroupColorMemory, colorBytes);
    std::memcpy(colorMapped, groupData.groupColors.data(),
        static_cast<size_t>(colorBytes));
    rendererBackend->UnmapMemory(tressfxGuideGroupColorMemory);

    // 3) CPU/GPU 组参数：实时覆盖值被新的资产解析结果替换（组编辑是资产语义）。
    tressfxSimulationConfig = resolvedConfigs.front();
    tressfxGroupSimulationConfigs = resolvedConfigs;
    if (hasGroupConfigTable)
        for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
            WriteTressfxGroupSimulationConfigs(slot);
    if (tressfxSimulation)
    {
        std::vector<VL::Hair::SimulationConfig> simulationConfigs;
        simulationConfigs.reserve(resolvedConfigs.size());
        for (const TressfxSimulationConfig& config : resolvedConfigs)
            simulationConfigs.push_back(
                VL::RenderableDetail::MakeCpuSimulationConfig(config));
        // 只换配置与映射，保留求解历史与当前姿态。
        tressfxSimulation->RebindGroups(
            std::move(simulationConfigs), binding.guideGroupIds);
    }

    tressfxGuideGroupBinding =
        std::make_shared<const VL::Hair::TressfxGroupBinding>(binding);
    tressfxGuideGroupColorCount = colorCount;
    tressfxGroupBindingKey = binding.key;
    tressfxGroupSimulationKey =
        VL::Hair::MakeTressfxGroupSimulationKey(tressfxGroupSimulationConfigs);
    return true;
}

bool RenderableObject::UpdateTressfxGroupBinding(
    const VL::Hair::TressfxGroupBinding& binding, std::string& error)
{
    error.clear();
    if (!VL::Hair::ValidateTressfxGroupPreviewBinding(
            binding, tressfxGuideCount, error))
        return false;

    // 草稿预览没有保存后的 per-group 解析结果：按名称保留已有组的配置，
    // 新建/重命名组先使用 index 0（base + placement）。保存并重载后才应用
    // 新的 simulationOverrides，这与 UI 的提示一致。
    const TressfxSimulationConfig fallback = tressfxGroupSimulationConfigs.empty()
        ? tressfxSimulationConfig : tressfxGroupSimulationConfigs.front();
    std::vector<TressfxSimulationConfig> resolved;
    resolved.reserve(binding.groupNames.size() + 1u);
    resolved.push_back(fallback);
    for (const std::string& name : binding.groupNames)
    {
        const TressfxSimulationConfig* config = nullptr;
        if (tressfxGuideGroupBinding)
        {
            const auto& oldNames = tressfxGuideGroupBinding->groupNames;
            for (size_t index = 0; index < oldNames.size(); ++index)
                if (oldNames[index] == name && index + 1u < tressfxGroupSimulationConfigs.size())
                {
                    config = &tressfxGroupSimulationConfigs[index + 1u];
                    break;
                }
        }
        resolved.push_back(config != nullptr ? *config : fallback);
    }
    return UpdateTressfxGroupBinding(binding, resolved, error);
}

bool RenderableObject::HasTressfxGroupVsp() const
{
    for (const TressfxSimulationConfig& config : tressfxGroupSimulationConfigs)
        if (config.vspCoefficient > 0.0f)
            return true;
    return false;
}

const vk::DescriptorBufferInfo& RenderableObject::GetTressfxResetParamsInfo(
    uint32_t frameSlot) const
{
    return tressfxResetParamsTargets[frameSlot].info;
}

void RenderableObject::WriteTressfxResetParams(
    const VL::Hair::GuideSimulationParams& params,
    uint32_t frameSlot)
{
    std::memcpy(tressfxResetParamsTargets[frameSlot].mapped, &params, sizeof(params));
}

void RenderableObject::WriteTressfxSkinningPalette(
    const std::vector<Eigen::Matrix4f>& jointMatrices, uint32_t frameSlot)
{
    if (!tressfxPaletteTargets[frameSlot].mapped || jointMatrices.empty())
        return;
    const size_t count = std::min<size_t>(
        jointMatrices.size(), tressfxPaletteJointCapacity);
    std::memcpy(tressfxPaletteTargets[frameSlot].mapped, jointMatrices.data(),
        count * sizeof(Eigen::Matrix4f));
}

void RenderableObject::ResetTressfxPenetrationStats(uint32_t frameSlot)
{
    if (tressfxPenetrationStatsTargets[frameSlot].mapped)
        std::memset(tressfxPenetrationStatsTargets[frameSlot].mapped, 0,
            2 * sizeof(uint32_t));
}

void RenderableObject::WriteTressfxColliders(
    const std::vector<VL::Hair::TressfxCollider>& capsules, uint32_t frameSlot)
{
    if (!tressfxGpuSimulation || frameSlot >= VL::Hair::GuideSimulationParamBufferCount)
        return;
    if (capsules.size() > VL::Hair::TressfxMaxColliders)
        throw std::runtime_error("TressFX collider count exceeds the GPU limit: " + name);
    // 与 MakeGuideColliderGpu 同一布局；未使用的槽保持零，shader 只读 counts.w 个。
    std::array<VL::Hair::GuideColliderGpu, VL::Hair::TressfxMaxColliders> gpuColliders{};
    for (size_t collider = 0; collider < capsules.size(); ++collider)
        gpuColliders[collider] = VL::Hair::MakeGuideColliderGpu(capsules[collider]);
    const vk::DeviceSize bytes = sizeof(gpuColliders);
    std::memcpy(tressfxColliderTargets[frameSlot].mapped, gpuColliders.data(),
        static_cast<size_t>(bytes));
}

bool RenderableObject::SetTressfxBrushPreview(uint32_t targetGroupId,
    VL::Hair::GroupPaintMode mode,
    const std::vector<VL::Hair::TressfxBrushGuideWeight>& weights,
    uint32_t frameSlot,
    std::string& error)
{
    error.clear();
    if (!IsTressfx() || tressfxBrushWeightBuffers.empty() || tressfxGuideCount == 0)
    {
        error = "TressFX brush preview requires a guide renderable";
        return false;
    }
    if (targetGroupId >= tressfxGuideGroupColorCount)
    {
        error = "TressFX brush preview target group is outside the saved group color table";
        return false;
    }
    const uint32_t slot = frameSlot < tressfxBrushWeightBuffers.size() ? frameSlot : 0;
    if (!tressfxBrushWeightBuffers[slot]) return false;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(tressfxGuideCount) *
        sizeof(float);
    std::vector<float> gpuWeights(tressfxGuideCount, 0.0f);
    for (const VL::Hair::TressfxBrushGuideWeight& weight : weights)
    {
        if (weight.guideIndex >= tressfxGuideCount ||
            !std::isfinite(weight.weight) || weight.weight < 0.0f ||
            weight.weight > 1.0f)
        {
            error = "TressFX brush preview weight is invalid";
            return false;
        }
        gpuWeights[weight.guideIndex] = std::max(
            gpuWeights[weight.guideIndex], weight.weight);
    }
    void* mapped = rendererBackend->MapMemory(tressfxBrushWeightMemories[slot], bytes);
    std::memcpy(mapped, gpuWeights.data(), static_cast<size_t>(bytes));
    rendererBackend->UnmapMemory(tressfxBrushWeightMemories[slot]);
    tressfxBrushActive = !weights.empty();
    tressfxBrushMode = mode;
    return true;
}

void RenderableObject::AdvanceTressfxSimulation(float dt,
    const Eigen::Vector3f& sceneWindVelocityCmPerSec)
{
    // GPU backend 的 tressfxSimulation 为空：位置 SSBO 只由 compute dispatch 写。
    if (!tressfxSimulation)
        return;

    // 场景风作者目标只在游戏线程落到 CPU config 的 wind（direction * strength，
    // 厘米/秒）。CPU 参考 backend 不复制渲染线程 SceneWindRuntime 的缓动/阵风；
    // GT 不拥有那个时钟，也不能在跨线程共享状态里另造一个。
    tressfxSimulation->SetSceneWind(sceneWindVelocityCmPerSec);

    // CPU 参考路径同样按源原生调度推进：n 个 0.0125 s substep，而不是一帧一个变量步。
    tressfxSimulation->StepFrame(dt);
    const auto& positions = tressfxSimulation->Positions();
    if (tressfxPositionBuffer)
    {
        const vk::DeviceSize size = positions.size() * sizeof(Eigen::Vector4f);
        void* mapped = rendererBackend->MapMemory(tressfxPositionMemory, size);
        auto* destination = static_cast<Eigen::Vector4f*>(mapped);
        for (size_t point = 0; point < positions.size(); ++point)
            destination[point] = Eigen::Vector4f(
                positions[point].x(), positions[point].y(), positions[point].z(), 1.0f);
        rendererBackend->UnmapMemory(tressfxPositionMemory);
    }

    // CPU 参考路径沿用接入前的同步策略：游戏线程直接写 host-coherent SSBO，没有等待
    // 在飞帧的 fence，两帧共用同一份 buffer 时主机写入可能与 GPU 顶点读取重叠。
    // GPU backend 不经过这里，SSBO 由 compute 独占写并由 dispatch 前后的 barrier 定序。
    // 顶点 buffer 不再回写：vertex factory 从 SSBO 取中心线，顶点位置不被消费。
}

void RenderableObject::SetTressfxSdfProxySkin(
    std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> transfer)
{
    if (!transfer)
        return;
    if (!tressfxSdfCollision)
        throw std::runtime_error(
            "TressFX proxy skin transfer requires collision.mesh: " + name);
    if (tressfxSdfProxySkin)
    {
        // 同一 SM 的多个场景 placement 共享同一个 RenderableObject；每个
        // placement 都会解析一次转移表。保留第一份即可（同一资产的权重相同），
        // 不重复创建缓冲，也不把正常的共享 placement 当成加载错误。
        return;
    }
    if (transfer->words.empty())
        throw std::runtime_error(
            "TressFX proxy skin transfer has no influence words: " + name);
    if (transfer->jointCount == 0)
        throw std::runtime_error(
            "TressFX proxy skin transfer has no skeleton joints: " + name);

    const vk::DeviceSize bytes =
        static_cast<vk::DeviceSize>(transfer->words.size()) * sizeof(std::uint32_t);
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    // 先建新 buffer，成功后再回收占位 buffer；失败不影响已发布状态。
    std::tie(buffer, memory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
        *rendererBackend, transfer->words.data(), bytes,
        "TressFXSdfProxyInfluences: " + name);
    if (tressfxSdfProxyInfluenceBuffer)
        rendererBackend->DestroyBuffer(tressfxSdfProxyInfluenceBuffer,
            tressfxSdfProxyInfluenceMemory);
    tressfxSdfProxyInfluenceBuffer = buffer;
    tressfxSdfProxyInfluenceMemory = memory;
    tressfxSdfProxyInfluenceInfo.setBuffer(buffer).setOffset(0).setRange(bytes);
    tressfxSdfProxySkin = std::move(transfer);

    // 代理权重可能引用多个 body section 的关节，图形 VS 的 per-section palette
    // 对未参与该 section 的关节没有有效 inverseBind，不能直接复用。这里按全骨架
    // bind→pose 重建每槽 palette（jointCount × current/previous 各 64 bytes）。
    const uint32_t jointCount = tressfxSdfProxySkin->jointCount;
    const vk::DeviceSize paletteBytes =
        static_cast<vk::DeviceSize>(jointCount) * 128u;
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        replacement{};
    const std::array<Eigen::Matrix4f, 2> identity{
        Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()};
    try
    {
        for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
        {
            TressfxParamsTarget& target = replacement[slot];
            std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
                paletteBytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "TressFXSdfProxyPalette: " + name + ":" + std::to_string(slot));
            target.mapped = rendererBackend->MapMemory(target.memory, paletteBytes);
            for (uint32_t joint = 0; joint < jointCount; ++joint)
            {
                void* destination = static_cast<unsigned char*>(target.mapped) +
                    static_cast<size_t>(joint) * 128u;
                std::memcpy(destination, identity.front().data(), 64);
                std::memcpy(static_cast<unsigned char*>(destination) + 64,
                    identity.back().data(), 64);
            }
            target.info.setBuffer(target.buffer).setOffset(0).setRange(paletteBytes);
        }
    }
    catch (...)
    {
        // 配对成功前失败：回收已建的新槽，保留旧的占位 palette 可用。
        for (TressfxParamsTarget& target : replacement)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
        }
        throw;
    }
    for (TressfxParamsTarget& target : tressfxSdfProxyPaletteTargets)
    {
        if (target.mapped)
            rendererBackend->UnmapMemory(target.memory);
        if (target.buffer)
            rendererBackend->DestroyBuffer(target.buffer, target.memory);
    }
    tressfxSdfProxyPaletteTargets = replacement;

    // 一次性加载诊断：证明权重重转移规模与绑定身体的空间距离，便于从用户日志
    // 直接判断资产是否错位（正式代理实测最大 < 1 cm）。
    std::cout << "[TressFX] SDF proxy skin transfer: object=\"" << name
              << "\" proxyVertices=" << tressfxSdfProxySkin->vertexCount
              << " influenceBytes=" << bytes
              << " maxInfluences=" << tressfxSdfProxySkin->maxInfluences
              << " jointCount=" << tressfxSdfProxySkin->jointCount
              << " paletteBytesPerSlot=" << paletteBytes
              << " uniqueBodyVertices=" << tressfxSdfProxySkin->uniqueBodyVertices
              << " maxNearestDistanceCm=" << tressfxSdfProxySkin->maxNearestDistanceCm
              << std::endl;
}

void RenderableObject::WriteTressfxSdfProxyPalette(
    const std::vector<Eigen::Matrix4f>& bindToPoseMatrices, uint32_t frameSlot)
{
    if (!tressfxSdfProxySkin || tressfxSdfProxySkin->jointCount == 0 ||
        frameSlot >= VL::Hair::GuideSimulationParamBufferCount)
        return;
    const uint32_t jointCount = tressfxSdfProxySkin->jointCount;
    auto* destination =
        static_cast<unsigned char*>(tressfxSdfProxyPaletteTargets[frameSlot].mapped);
    const Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    for (uint32_t joint = 0; joint < jointCount; ++joint)
    {
        // 骨架关节数应覆盖转移表引用上界；缺少时保留 identity，避免把越界
        // 矩阵写进 buffer（shader 只按转移表里的 joint 索引读取）。
        const Eigen::Matrix4f& matrix = joint < bindToPoseMatrices.size()
            ? bindToPoseMatrices[joint]
            : identity;
        std::memcpy(destination + static_cast<size_t>(joint) * 128u,
            matrix.data(), 64);
        std::memcpy(destination + static_cast<size_t>(joint) * 128u + 64,
            matrix.data(), 64);
    }
}

bool RenderableObject::WriteTressfxSdfParams(
    const Eigen::Matrix4f& skinningMatrix,
    const std::vector<Eigen::Matrix4f>* jointMatrices, uint32_t frameSlot)
{
    if (!tressfxSdfCollision || !imported ||
        frameSlot >= VL::Hair::GuideSimulationParamBufferCount)
        return false;
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if (!tressfxPlacementResources->collisionMesh || !tressfxSimulationConfig.collisionMesh)
        return false;

    // 有转移表且本帧拿到了源 SkinPose 时，域必须与 GPU splat 使用同一套逐顶点
    // LBS：否则域可能漏掉被多骨蒙皮推到包围盒外的代理顶点。没有转移/pose 时
    // 保持原来的单骨根矩阵路径，域也按同一矩阵计算。
    const bool useProxySkin =
        tressfxSdfProxySkin != nullptr && jointMatrices != nullptr;
    const VL::Hair::SdfTriangleMesh& proxy = *tressfxPlacementResources->collisionMesh;

    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
    bool hasBounds = false;
    for (std::size_t index = 0; index < proxy.positions.size(); ++index)
    {
        const Eigen::Vector3f& rest = proxy.positions[index];
        Eigen::Vector3f position;
        if (useProxySkin)
        {
            try
            {
                position = VL::Hair::TressfxProxySkinnedPosition(
                    *tressfxSdfProxySkin, static_cast<uint32_t>(index),
                    *jointMatrices, rest);
            }
            catch (const std::out_of_range&)
            {
                // pose 与转移表的关节空间不一致属于资产/骨架契约破坏；保留明确
                // 失败（只报告一次）而不是写一个缺骨的域。
                if (!reportedSdfDomainFailure)
                {
                    std::cerr << "[TressFX] SDF domain skipped for object \"" << name
                              << "\": proxy skin transfer joint is outside the source pose"
                              << std::endl;
                    reportedSdfDomainFailure = true;
                }
                return false;
            }
        }
        else
        {
            position = (skinningMatrix * rest.homogeneous()).head<3>();
        }
        if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
            !std::isfinite(position.z()))
        {
            if (!reportedSdfDomainFailure)
            {
                std::cerr << "[TressFX] SDF domain skipped for object \"" << name
                          << "\": skinning produced a non-finite proxy vertex"
                          << std::endl;
                reportedSdfDomainFailure = true;
            }
            return false;
        }
        if (!hasBounds)
        {
            boundsMin = position;
            boundsMax = position;
            hasBounds = true;
        }
        else
        {
            boundsMin = boundsMin.cwiseMin(position);
            boundsMax = boundsMax.cwiseMax(position);
        }
    }
    if (!hasBounds)
        return false;

    const auto domainResult = VL::Hair::MakeTressfxSdfDomain(boundsMin, boundsMax,
        tressfxSimulationConfig.collisionMesh->margin, tressfxSdfResolution);
    if (domainResult.IsFailure())
    {
        if (!reportedSdfDomainFailure)
        {
            std::cerr << "[TressFX] SDF domain skipped for object \"" << name
                      << "\": " << domainResult.Error().message << std::endl;
            reportedSdfDomainFailure = true;
        }
        return false;
    }
    const VL::Hair::TressfxSdfLayout& domain = domainResult.Value();
    const VL::Hair::TressfxSdfParams params =
        VL::Hair::MakeTressfxSdfParams(domain, tressfxSdfTriangleCount, useProxySkin);
    std::memcpy(tressfxSdfParamsTargets[frameSlot].mapped, &params, sizeof(params));

    if (!reportedSdfDomain)
    {
        // 一次性诊断：域每帧随蒙皮包围盒移动，这里只报告第一帧的实际参数；
        // 之后 origin/cellSize 每帧写入 frame slot 的 UBO。
        std::cout << "[TressFX] SDF domain: object=\"" << name
                  << "\" dims=" << domain.dims[0] << "x" << domain.dims[1] << "x"
                  << domain.dims[2] << " origin=(" << domain.origin.x() << ", "
                  << domain.origin.y() << ", " << domain.origin.z() << ")"
                  << " cellSize=" << domain.cellSize
                  << " margin=" << domain.margin
                  << " truncation=" << domain.truncation << std::endl;
        reportedSdfDomain = true;
    }
    return true;
}
