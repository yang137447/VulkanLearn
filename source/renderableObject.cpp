#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "animation/skeletalPlayback.h"
#include "hair/tressfxSdfGrid.h"
#include "hair/tressfxGroupGpuData.h"
#include "hair/tressfxGroupSimulation.h"
#include "hair/tressfxLiveParamsApply.h"
#include "commonFunction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

// params UBO 的槽数必须与帧同步环一致：BeginFrame 先等该槽 fence，主机再写槽内参数。
static_assert(VL::Hair::GuideSimulationParamBufferCount == MAX_FRAMES_IN_FLIGHT,
    "TressFX params slots must match the frame-in-flight ring");

namespace
{
// GPU 模拟的位置 buffer 位于 device-local；初始内容只在创建时经 staging 上传一次。
// 上传后由 HairSimulationPass 在本帧 dispatch 前的 transfer→compute barrier 转为可见。
// eTransferSrc 只服务于一次性 `hair trace` 的 vkCmdCopyBuffer 回读：trace 未激活时
// 没有任何拷贝，usage 位本身不产生运行时开销。
std::pair<vk::Buffer, vk::DeviceMemory> CreateUploadedDeviceLocalStorageBuffer(
    VL::RendererBackendVulkan& backend,
    const void* data,
    vk::DeviceSize size,
    const std::string& debugName)
{
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::Buffer stagingBuffer = nullptr;
    vk::DeviceMemory stagingMemory = nullptr;
    try
    {
        std::tie(buffer, memory) = backend.CreateBuffer(
            size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            debugName);
        std::tie(stagingBuffer, stagingMemory) = backend.CreateBuffer(
            size,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "StagingBuffer: " + debugName);
        void* mapped = backend.MapMemory(stagingMemory, size);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        backend.UnmapMemory(stagingMemory);
        backend.CopyBufferToBuffer(stagingBuffer, buffer, size);
        backend.DestroyBuffer(stagingBuffer, stagingMemory);
        return {buffer, memory};
    }
    catch (...)
    {
        if (stagingBuffer)
            backend.DestroyBuffer(stagingBuffer, stagingMemory);
        if (buffer)
            backend.DestroyBuffer(buffer, memory);
        throw;
    }
}

VL::Hair::SimulationConfig MakeCpuSimulationConfig(
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
} // namespace

RenderableObject::RenderableObject(
    std::vector<Vertex> vertices,
    std::vector<uint32_t> indices,
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& name)
{
    this->rendererBackend = &rendererBackend;
    this->vertices = std::move(vertices);
    this->indices = std::move(indices);
    this->name = name;

    //更新boundingBox
    UpdateLocalBounds();
    // 创建顶点缓冲区
    CreateVertexBuffer();
    // 创建索引缓冲区
    CreateIndexBuffer();
}
RenderableObject::RenderableObject(
    std::shared_ptr<const MeshAssetImportResult> importedResource,
    size_t sectionIndex,
    VL::RendererBackendVulkan& backend,
    const std::string& objectName,
    const TressfxSimulationConfig* tressfxConfig,
    std::shared_ptr<const TressfxPlacementResources> tressfxResources,
    const std::vector<TressfxSimulationConfig>* tressfxGroupConfigs,
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> tressfxGroupBinding)
    : imported(std::move(importedResource))
    , importedSectionIndex(sectionIndex)
    , tressfxPlacementResources(std::move(tressfxResources))
{
    if (!imported || sectionIndex >= imported->modelResource.sections.size())
        throw std::invalid_argument("Invalid imported mesh section");
    rendererBackend = &backend;
    name = objectName;
    const MeshSection& section = imported->modelResource.sections[sectionIndex];
    if (section.isTressfx)
    {
        if (!tressfxPlacementResources)
            tressfxPlacementResources = std::make_shared<const TressfxPlacementResources>();
        // 先复制作者参数到本对象副本，再做 GPU/CPU 资源分支：实时覆盖只写副本，
        // 共享的导入结果保持只读。
        tressfxSimulationConfig = tressfxConfig ? *tressfxConfig : imported->buildPlan.tressfxSimulation;
        // 组解析结果同样按值拷入；缺省时退化成单份有效配置（组 0）。
        if (tressfxGroupConfigs != nullptr && !tressfxGroupConfigs->empty())
            tressfxGroupSimulationConfigs = *tressfxGroupConfigs;
        else
            tressfxGroupSimulationConfigs.assign(1, tressfxSimulationConfig);
        // 分组绑定与几何分离：优先使用调用方按当前描述符构造的 binding；
        // 兼容直接构造对象的调用方，退回到导入结果的 groups（仅新对象路径）。
        if (tressfxGroupBinding)
            tressfxGuideGroupBinding = std::move(tressfxGroupBinding);
        else
        {
            std::vector<VL::Hair::TressfxGroupDeclaration> declarations;
            for (const TressfxGuideGroupPlan& group : imported->buildPlan.tressfxGuideGroups)
            {
                VL::Hair::TressfxGroupDeclaration declaration;
                declaration.name = group.name;
                declaration.ranges = group.guideRanges;
                declarations.push_back(std::move(declaration));
            }
            const uint32_t guideCount = section.tressfxGuideCount != 0
                ? section.tressfxGuideCount
                : static_cast<uint32_t>(section.tressfxRootPositions.size());
            tressfxGuideGroupBinding = VL::Hair::BuildTressfxGroupBinding(
                declarations, guideCount, imported->buildPlan.tressfxGroupBindingKey, name);
        }
        tressfxGroupBindingKey = tressfxGuideGroupBinding->key;
        tressfxGroupSimulationKey =
            VL::Hair::MakeTressfxGroupSimulationKey(tressfxGroupSimulationConfigs);
        for (const auto* curve : {&tressfxSimulationConfig.vspStiffness,
                &tressfxSimulationConfig.localShapeStiffness})
            if (!curve->empty() && curve->size() != section.tressfxPointsPerStrand)
                throw std::runtime_error("TressFX placement stiffness curve must match pointsPerStrand: " + name);
        const size_t pointCount = section.tressfxInverseMass.size();
        const size_t pointsPerGuide = section.tressfxPointsPerStrand;
        if (pointCount != 0 && pointsPerGuide >= 2 &&
            pointCount % pointsPerGuide == 0 &&
            section.vertices.size() == pointCount * 2)
        {
            tressfxGuideCount = static_cast<uint32_t>(pointCount / pointsPerGuide);
            tressfxPointsPerGuide = section.tressfxPointsPerStrand;
            std::string groupError;
            if (!VL::Hair::ValidateTressfxGroupPreviewBinding(
                    *tressfxGuideGroupBinding, tressfxGuideCount, groupError))
                throw std::runtime_error(
                    "TressFX group binding does not match imported geometry: " +
                    groupError + ": " + name);
            // GPU backend 不构造 CPU 参考 solver；位置只由 HairSimulationPass 的
            // compute dispatch 推进，避免同一份 SSBO 出现两个写入者。
            if (tressfxSimulationConfig.enabled &&
                tressfxSimulationConfig.backend == TressfxSimulationBackend::Cpu)
            {
                std::vector<Eigen::Vector3f> restPositions;
                restPositions.reserve(pointCount);
                for (size_t point = 0; point < pointCount; ++point)
                    restPositions.push_back(section.vertices[point * 2].position);

                // 4a：CPU 参考 solver 与 GPU group config 共用同一次分层解析结果；
                // 空曲线仍保持“未作者”回退语义。
                std::vector<VL::Hair::SimulationConfig> simulationConfigs;
                simulationConfigs.reserve(tressfxGroupSimulationConfigs.size());
                for (const TressfxSimulationConfig& groupConfig :
                     tressfxGroupSimulationConfigs)
                    simulationConfigs.push_back(MakeCpuSimulationConfig(groupConfig));
                tressfxSimulation = std::make_unique<VL::Hair::GuideSimulation>(
                    std::move(restPositions),
                    tressfxGuideCount,
                    pointsPerGuide,
                    section.tressfxInverseMass,
                    std::move(simulationConfigs),
                    tressfxGuideGroupBinding->guideGroupIds);
            }
        }
    }
    UpdateLocalBounds();
    CreateVertexBuffer();
    CreateIndexBuffer();
    try
    {
        CreateSkinInfluenceBuffer();
        CreateTressfxPositionBuffer();
        CreateTressfxGuideGroupBuffers();
        CreateTressfxBrushWeightBuffer();
        CreateTressfxGpuSimulationBuffers();
    }
    catch (...)
    {
        DestroyTressfxBuffers();
        if (rendererBackend && skinInfluenceBuffer)
            rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
        DestroyIndexBuffer();
        DestroyVertexBuffer();
        throw;
    }
}

const std::vector<Vertex>& RenderableObject::GetVertices() const
{
    return imported ? imported->modelResource.sections[importedSectionIndex].vertices
                    : vertices;
}

const std::vector<uint32_t>& RenderableObject::GetIndices() const
{
    return imported ? imported->modelResource.sections[importedSectionIndex].indices
                    : indices;
}

RenderableObject::~RenderableObject()
{
    DestroyTressfxBuffers();
    if (rendererBackend && skinInfluenceBuffer)
        rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
    // 销毁索引缓冲区
    DestroyIndexBuffer();
    // 销毁顶点缓冲区
    DestroyVertexBuffer();
    rendererBackend = nullptr;
}

bool RenderableObject::IsTressfx() const
{
    return imported &&
        importedSectionIndex < imported->modelResource.sections.size() &&
        imported->modelResource.sections[importedSectionIndex].isTressfx;
}

bool RenderableObject::UsesGpuGuideSimulation() const
{
    return tressfxGpuSimulation && tressfxGuideCount != 0;
}

bool RenderableObject::UsesGpuHairSkinning() const
{
    return IsTressfx() && !tressfxSimulationConfig.enabled &&
        tressfxSimulationConfig.backend == TressfxSimulationBackend::Gpu &&
        tressfxSimulationConfig.skinning.has_value() && tressfxGuideCount != 0;
}

const TressfxSimulationConfig& RenderableObject::GetTressfxSimulationConfig() const
{
    if (!IsTressfx())
    {
        // 非 tressfx 对象不应调用该接口；返回静态默认值而不是解引用空 imported。
        static const TressfxSimulationConfig defaultConfig;
        return defaultConfig;
    }
    return tressfxSimulationConfig;
}

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
            simulationConfigs.push_back(MakeCpuSimulationConfig(config));
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

void RenderableObject::SetTressfxSkinningBinding(
    const VL::Hair::TressfxSkinningBinding& binding)
{
    tressfxSkinningBinding = binding;
    if (!tressfxBoneWeightsBuffer || tressfxGuideCount == 0)
        return;
    if (!binding.strandBones.empty() &&
        binding.strandBones.size() != tressfxGuideCount)
        throw std::runtime_error(
            "TressFX .tfxbone strand count does not match guide count for \"" + name + "\"");

    std::vector<VL::Hair::TressfxBoneWeightGpu> gpu(tressfxGuideCount);
    // 裁剪/归一化/回退根的语义只在 tressfxSkinning.h 一处实现：.tfxbone 与
    // 网格派生两条来源共用，模块测试也直接测这份纯函数。
    const VL::Hair::TressfxStrandBoneWeights emptyWeights;
    for (size_t strand = 0; strand < gpu.size(); ++strand)
        gpu[strand] = VL::Hair::TressfxPackStrandBoneWeights(
            binding.strandBones.empty() ? emptyWeights : binding.strandBones[strand],
            binding.jointIndex);
    const vk::DeviceSize bytes =
        static_cast<vk::DeviceSize>(gpu.size()) * sizeof(VL::Hair::TressfxBoneWeightGpu);
    void* mapped = rendererBackend->MapMemory(tressfxBoneWeightsMemory, bytes);
    std::memcpy(mapped, gpu.data(), static_cast<size_t>(bytes));
    rendererBackend->UnmapMemory(tressfxBoneWeightsMemory);
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

std::shared_ptr<RenderableObject> RenderableObject::CloneForPlacement(
    const std::string& newName, const TressfxSimulationConfig* effectiveConfig,
    const std::vector<TressfxSimulationConfig>* groupConfigs) const
{
    if (!imported || rendererBackend == nullptr)
        throw std::runtime_error(
            "Placement renderable clone requires imported geometry and a renderer backend: " + name);
    // 构造函数按同一 imported/section 重新分配本对象的 GPU 资源与可变状态；导入几何
    // 仍共享。具体放置绑定（例如毛发的根点骨骼权重）由 resolver 按 Kind 应用，
    // 本方法保持类型无关。
    // 调用方只给有效配置、未给组表时不能复用模板的组配置：两者的 base/placement
    // 可能不同。此时退化为单份有效配置，至少保证 CPU/GPU 选到同一份标量。
    std::vector<TressfxSimulationConfig> fallbackGroups;
    const std::vector<TressfxSimulationConfig>* resolvedGroups = groupConfigs;
    if (resolvedGroups == nullptr)
    {
        if (effectiveConfig != nullptr)
        {
            fallbackGroups.assign(1, *effectiveConfig);
            resolvedGroups = &fallbackGroups;
        }
        else
        {
            resolvedGroups = &tressfxGroupSimulationConfigs;
        }
    }
    return std::make_shared<RenderableObject>(
        imported, importedSectionIndex, *rendererBackend, newName,
        effectiveConfig ? effectiveConfig : &tressfxSimulationConfig,
        tressfxPlacementResources, resolvedGroups, tressfxGuideGroupBinding);
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

void RenderableObject::CreateTressfxPositionBuffer()
{
    if (!IsTressfx())
        return;

    const auto& section = imported->modelResource.sections[importedSectionIndex];
    std::vector<Eigen::Vector3f> staticPositions;
    const std::vector<Eigen::Vector3f>* positions = nullptr;
    if (tressfxSimulation)
    {
        positions = &tressfxSimulation->Positions();
    }
    else
    {
        staticPositions.reserve(section.vertices.size() / 2);
        for (size_t point = 0; point < section.vertices.size() / 2; ++point)
            staticPositions.push_back(section.vertices[point * 2].position);
        positions = &staticPositions;
    }
    std::vector<Eigen::Vector4f> initialPositions;
    initialPositions.reserve(positions->size());
    for (const Eigen::Vector3f& position : *positions)
        initialPositions.emplace_back(position.x(), position.y(), position.z(), 1.0f);

    const vk::DeviceSize size =
        static_cast<vk::DeviceSize>(initialPositions.size()) * sizeof(Eigen::Vector4f);
    // GPU backend 的 current 只由 compute 写、vertex shader 读，创建后放在 device-local；
    // CPU backend 仍由游戏线程逐帧写 host-coherent buffer，保持既有同步策略。
    const bool gpuSimulation = (tressfxSimulationConfig.enabled ||
            tressfxSimulationConfig.skinning.has_value()) &&
        tressfxSimulationConfig.backend == TressfxSimulationBackend::Gpu &&
        tressfxGuideCount != 0;
    if (gpuSimulation)
    {
        std::tie(tressfxPositionBuffer, tressfxPositionMemory) =
            CreateUploadedDeviceLocalStorageBuffer(
                *rendererBackend, initialPositions.data(), size,
                "TressFXPositions: " + name);
    }
    else
    {
        std::tie(tressfxPositionBuffer, tressfxPositionMemory) = rendererBackend->CreateBuffer(
            size,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXPositions: " + name);
        try
        {
            void* mapped = rendererBackend->MapMemory(tressfxPositionMemory, size);
            std::memcpy(mapped, initialPositions.data(), static_cast<size_t>(size));
            rendererBackend->UnmapMemory(tressfxPositionMemory);
        }
        catch (...)
        {
            rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
            tressfxPositionBuffer = nullptr;
            tressfxPositionMemory = nullptr;
            throw;
        }
    }
    tressfxPositionInfo
        .setBuffer(tressfxPositionBuffer)
        .setOffset(0)
        .setRange(size);
}

void RenderableObject::CreateTressfxGuideGroupBuffers()
{
    if (!IsTressfx() || tressfxGuideCount == 0)
        return;

    // 组数据来自独立 binding，不读导入结果里的旧 descriptor；组编辑后
    // UpdateTressfxGroupBinding 只重写这两份 buffer。
    const std::vector<uint32_t> emptyIds;
    const std::vector<std::string> emptyNames;
    const std::vector<uint32_t>& guideGroupIds = tressfxGuideGroupBinding
        ? tressfxGuideGroupBinding->guideGroupIds : emptyIds;
    const std::vector<std::string>& groupNames = tressfxGuideGroupBinding
        ? tressfxGuideGroupBinding->groupNames : emptyNames;
    const VL::Hair::TressfxGroupGpuData data = VL::Hair::BuildTressfxGroupGpuData(
        tressfxGuideCount, guideGroupIds, groupNames,
        VL::Hair::TressfxGroupPreviewColorCapacity);

    // group id 只是作者分组数据，不随模拟更新；host-visible coherent 让对象创建后
    // 不再需要 staging/一次性上传。颜色表同样静态，但只读一次上传即可。
    const vk::DeviceSize idBytes = static_cast<vk::DeviceSize>(data.guideGroupIds.size()) *
        sizeof(uint32_t);
    std::tie(tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory) =
        rendererBackend->CreateBuffer(
            idBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGuideGroupIds: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(tressfxGuideGroupIdMemory, idBytes);
        std::memcpy(mapped, data.guideGroupIds.data(), static_cast<size_t>(idBytes));
        rendererBackend->UnmapMemory(tressfxGuideGroupIdMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory);
        tressfxGuideGroupIdBuffer = nullptr;
        tressfxGuideGroupIdMemory = nullptr;
        throw;
    }
    tressfxGuideGroupIdInfo
        .setBuffer(tressfxGuideGroupIdBuffer)
        .setOffset(0)
        .setRange(idBytes);

    // descriptor 捕获固定容量；组数变化不再重建 color buffer。
    const vk::DeviceSize colorBytes =
        static_cast<vk::DeviceSize>(VL::Hair::TressfxGroupPreviewColorCapacity) *
        sizeof(VL::Hair::TressfxGroupColorGpu);
    std::tie(tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory) =
        rendererBackend->CreateBuffer(
            colorBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGuideGroupColors: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(tressfxGuideGroupColorMemory, colorBytes);
        std::memcpy(mapped, data.groupColors.data(), static_cast<size_t>(colorBytes));
        rendererBackend->UnmapMemory(tressfxGuideGroupColorMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory);
        tressfxGuideGroupColorBuffer = nullptr;
        tressfxGuideGroupColorMemory = nullptr;
        throw;
    }
    tressfxGuideGroupColorInfo
        .setBuffer(tressfxGuideGroupColorBuffer)
        .setOffset(0)
        .setRange(colorBytes);
    tressfxGuideGroupColorCount =
        static_cast<uint32_t>(groupNames.size()) + 1u;
}

void RenderableObject::CreateTressfxBrushWeightBuffer()
{
    if (!IsTressfx() || tressfxGuideCount == 0) return;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(tressfxGuideCount) *
        sizeof(float);
    const uint32_t slots = std::max(1u, rendererBackend->GetSwapchainImageCount());
    tressfxBrushWeightBuffers.assign(slots, nullptr);
    tressfxBrushWeightMemories.assign(slots, nullptr);
    tressfxBrushWeightInfos.assign(slots, {});
    for (uint32_t slot = 0; slot < slots; ++slot)
    {
        std::tie(tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]) =
            rendererBackend->CreateBuffer(
                bytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "TressFXBrushWeights[" + std::to_string(slot) + "]: " + name);
        try
        {
            // 初值全零；UI 的第一次笔刷命令会整表覆盖，不做增量累加。
            void* mapped = rendererBackend->MapMemory(tressfxBrushWeightMemories[slot], bytes);
            std::memset(mapped, 0, static_cast<size_t>(bytes));
            rendererBackend->UnmapMemory(tressfxBrushWeightMemories[slot]);
        }
        catch (...)
        {
            rendererBackend->DestroyBuffer(
                tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]);
            tressfxBrushWeightBuffers[slot] = nullptr;
            tressfxBrushWeightMemories[slot] = nullptr;
            throw;
        }
        tressfxBrushWeightInfos[slot]
            .setBuffer(tressfxBrushWeightBuffers[slot])
            .setOffset(0)
            .setRange(bytes);
    }
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

void RenderableObject::CreateTressfxGpuSimulationBuffers()
{
    if (!IsTressfx() || tressfxSimulation)
        return;
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if ((!tressfxSimulationConfig.enabled && !tressfxSimulationConfig.skinning) ||
        tressfxSimulationConfig.backend != TressfxSimulationBackend::Gpu ||
        tressfxGuideCount == 0 || tressfxPointsPerGuide < 2)
        return;

    const size_t pointCount = section.tressfxInverseMass.size();
    const vk::DeviceSize positionBytes =
        static_cast<vk::DeviceSize>(pointCount) * sizeof(Eigen::Vector4f);

    // previous positions：初值与 current 一样来自 rest；w 不用作逆质量。
    std::vector<Eigen::Vector4f> initialPoints(pointCount);
    for (size_t point = 0; point < pointCount; ++point)
    {
        const Eigen::Vector3f& rest = section.vertices[point * 2].position;
        initialPoints[point] = Eigen::Vector4f(rest.x(), rest.y(), rest.z(), 1.0f);
    }
    if (tressfxSimulationConfig.enabled)
    {
    // 每组配置按 frame-in-flight 分槽：正常帧内容是静态的，实时参数覆盖
    // 只在当前槽 fence 完成后重写，不与在飞 compute 共享同一份可写 buffer。
    const std::vector<VL::Hair::TressfxGroupSimulationGpu> groupGpuData =
        VL::Hair::BuildTressfxGroupSimulationGpuData(tressfxGroupSimulationConfigs,
            VL::Hair::TressfxGroupPreviewColorCapacity);
    const vk::DeviceSize groupConfigBytes =
        static_cast<vk::DeviceSize>(VL::Hair::TressfxGroupPreviewColorCapacity) *
        sizeof(VL::Hair::TressfxGroupSimulationGpu);
    std::array<VL::Hair::TressfxGroupSimulationGpu, 1> emptyGroupConfig{};
    const void* groupConfigSource = groupGpuData.empty()
        ? static_cast<const void*>(emptyGroupConfig.data())
        : static_cast<const void*>(groupGpuData.data());
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxGuideGroupConfigTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            groupConfigBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGroupSimulationConfigs: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, groupConfigBytes);
        if (groupGpuData.empty())
            std::memset(target.mapped, 0, static_cast<size_t>(groupConfigBytes));
        else
            std::memcpy(target.mapped, groupConfigSource,
                static_cast<size_t>(groupConfigBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(groupConfigBytes);
    }

    std::tie(tressfxPreviousPositionBuffer, tressfxPreviousPositionMemory) =
        CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXPreviousPositions: " + name);
    tressfxPreviousPositionInfo
        .setBuffer(tressfxPreviousPositionBuffer).setOffset(0).setRange(positionBytes);

    // previous-previous positions：strand-level 伪加速度与第二阶 Verlet 状态。
    // 初值与 previous 相同，首帧的伪加速度因此为 0。
    std::tie(tressfxPreviousPreviousPositionBuffer,
        tressfxPreviousPreviousPositionMemory) =
        CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXPreviousPreviousPositions: " + name);
    tressfxPreviousPreviousPositionInfo
        .setBuffer(tressfxPreviousPreviousPositionBuffer).setOffset(0)
        .setRange(positionBytes);
    }

    // rest positions：xyz 是静止中心线，w 是导入的逆质量；integration pass 用它判断
    // 不可移动点（根前缀或逆质量 <= 0）。
    for (size_t point = 0; point < pointCount; ++point)
    {
        const Eigen::Vector3f& rest = section.vertices[point * 2].position;
        initialPoints[point] = Eigen::Vector4f(rest.x(), rest.y(), rest.z(),
            section.tressfxInverseMass[point]);
    }
    std::tie(tressfxRestPositionBuffer, tressfxRestPositionMemory) =
        CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXRestPositions: " + name);
    tressfxRestPositionInfo
        .setBuffer(tressfxRestPositionBuffer).setOffset(0).setRange(positionBytes);

    // 静止段长：第 i 项是 i -> i+1 的 rest 长度，末项 0。LengthConstraintsWind pass
    // 每根 strand 读取，创建时上传一次。
    if (tressfxSimulationConfig.enabled)
    {
    std::vector<float> restLengths(pointCount, 0.0f);
    for (size_t guide = 0; guide < tressfxGuideCount; ++guide)
    {
        const size_t base = guide * tressfxPointsPerGuide;
        for (size_t local = 0; local + 1 < tressfxPointsPerGuide; ++local)
            restLengths[base + local] = (section.vertices[(base + local + 1) * 2].position -
                section.vertices[(base + local) * 2].position).norm();
    }
    const vk::DeviceSize restLengthBytes =
        static_cast<vk::DeviceSize>(pointCount) * sizeof(float);
    std::tie(tressfxRestLengthBuffer, tressfxRestLengthMemory) =
        CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, restLengths.data(), restLengthBytes,
            "TressFXRestLengths: " + name);
    tressfxRestLengthInfo
        .setBuffer(tressfxRestLengthBuffer).setOffset(0).setRange(restLengthBytes);

    // strand-level 数据：每根 strand 3 个 vec4（VspQuat、VspTranslation + VspCoeff、
    // SkinningQuat），由 StrandLevel pass 每帧写、VSP/LocalShape pass 读。
    std::vector<Eigen::Vector4f> strandLevelData(
        static_cast<size_t>(tressfxGuideCount) * 3u, Eigen::Vector4f::Zero());
    const vk::DeviceSize strandLevelBytes =
        static_cast<vk::DeviceSize>(strandLevelData.size()) * sizeof(Eigen::Vector4f);
    std::tie(tressfxStrandLevelBuffer, tressfxStrandLevelMemory) =
        CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, strandLevelData.data(), strandLevelBytes,
            "TressFXStrandLevel: " + name);
    tressfxStrandLevelInfo
        .setBuffer(tressfxStrandLevelBuffer).setOffset(0).setRange(strandLevelBytes);
    }

    // params UBO 每帧由主机改写，保持 host-coherent；只有位置 buffer 是 device-local。
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxParamsTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            sizeof(VL::Hair::GuideSimulationParams),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSimulationParams: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(
            target.memory, sizeof(VL::Hair::GuideSimulationParams));
        target.info.setBuffer(target.buffer).setOffset(0)
            .setRange(sizeof(VL::Hair::GuideSimulationParams));
        // reset 首轮 Integration 的独立 UBO：内容只在 reset/换基帧写入，
        // 但句柄常驻，descriptor set 在对象创建时一次性绑定。
        if (tressfxSimulationConfig.enabled)
        {
        auto& resetTarget = tressfxResetParamsTargets[slot];
        std::tie(resetTarget.buffer, resetTarget.memory) = rendererBackend->CreateBuffer(
            sizeof(VL::Hair::GuideSimulationParams),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXResetParams: " + name + ":" + std::to_string(slot));
        resetTarget.mapped = rendererBackend->MapMemory(
            resetTarget.memory, sizeof(VL::Hair::GuideSimulationParams));
        resetTarget.info.setBuffer(resetTarget.buffer).setOffset(0)
            .setRange(sizeof(VL::Hair::GuideSimulationParams));
        }
    }
    // 多骨：每 strand 4 个 (joint, weight) 槽，共 32 bytes；没有 .tfxbone 时
    // 全零，shader 用 motion.w bit2 判断是否读取。palette 每槽固定 256 mat4。
    constexpr uint32_t MaxPaletteJoints = 256;
    const vk::DeviceSize boneWeightBytes =
        static_cast<vk::DeviceSize>(tressfxGuideCount) *
        sizeof(VL::Hair::TressfxBoneWeightGpu);
    std::tie(tressfxBoneWeightsBuffer, tressfxBoneWeightsMemory) =
        rendererBackend->CreateBuffer(
            std::max<vk::DeviceSize>(boneWeightBytes,
                sizeof(VL::Hair::TressfxBoneWeightGpu)),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXBoneWeights: " + name);
    {
        const vk::DeviceSize bytes = std::max<vk::DeviceSize>(boneWeightBytes,
            sizeof(VL::Hair::TressfxBoneWeightGpu));
        void* mapped = rendererBackend->MapMemory(tressfxBoneWeightsMemory, bytes);
        std::memset(mapped, 0, static_cast<size_t>(bytes));
        rendererBackend->UnmapMemory(tressfxBoneWeightsMemory);
    }
    tressfxBoneWeightsInfo.setBuffer(tressfxBoneWeightsBuffer).setOffset(0)
        .setRange(std::max<vk::DeviceSize>(boneWeightBytes,
            sizeof(VL::Hair::TressfxBoneWeightGpu)));
    tressfxPaletteJointCapacity = MaxPaletteJoints;
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxPaletteTargets[slot];
        const vk::DeviceSize bytes =
            static_cast<vk::DeviceSize>(MaxPaletteJoints) * sizeof(Eigen::Matrix4f);
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            bytes, vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSkinningPalette: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, bytes);
        target.info.setBuffer(target.buffer).setOffset(0).setRange(bytes);
    }
    // 每帧碰撞前的最大穿透深度（uint 位模式；0=无穿透）。host 清零、shader
    // atomicMax、trace 通过 transfer 拷到 readback，不参与渲染。
    // off 蒙皮只需要 current/rest/params/weights/palette，不持有求解器状态。
    if (!tressfxSimulationConfig.enabled)
        return;
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxPenetrationStatsTargets[slot];
        constexpr vk::DeviceSize bytes = 2 * sizeof(uint32_t);
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            bytes, vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXPenetrationStats: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, bytes);
        std::memset(target.mapped, 0, static_cast<size_t>(bytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(bytes);
    }
    // 解析胶囊：每个 frame-in-flight 槽一份 host-coherent SSBO。声明了 bone 的
    // 胶囊每帧由 HairSimulationPass 重写；无 bone 的胶囊内容保持创建时的作者值。
    // 空列表也保留合法 buffer，因为 shader 的 binding 7 是静态声明，不能用空
    // descriptor；count 由 params.counts.w 控制。
    const vk::DeviceSize colliderBytes =
        sizeof(VL::Hair::GuideColliderGpu) * VL::Hair::TressfxMaxColliders;
    std::array<VL::Hair::GuideColliderGpu, VL::Hair::TressfxMaxColliders> gpuColliders{};
    const auto& capsules = tressfxSimulationConfig.capsules;
    if (capsules.size() > VL::Hair::TressfxMaxColliders)
        throw std::runtime_error("TressFX collider count exceeds the GPU limit: " + name);
    for (size_t collider = 0; collider < capsules.size(); ++collider)
        gpuColliders[collider] = VL::Hair::MakeGuideColliderGpu(capsules[collider]);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxColliderTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            colliderBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXColliders: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, colliderBytes);
        std::memcpy(target.mapped, gpuColliders.data(), static_cast<size_t>(colliderBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(colliderBytes);
    }

    // `collision.mesh` 的距离场资源：只在声明了代理时创建；失败让整个
    // renderable 构造失败（构造函数的 catch 会回收已建 buffer）。
    CreateTressfxSdfBuffers();

    tressfxGpuSimulation = true;
}

void RenderableObject::CreateTressfxSdfBuffers()
{
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if (!tressfxSimulationConfig.collisionMesh)
        return;
    const TressfxCollisionMeshConfig& config =
        *tressfxSimulationConfig.collisionMesh;
    if (!tressfxPlacementResources->collisionMesh)
        throw std::runtime_error(
            "TressFX collision.mesh was declared but its OBJ was not imported: " +
            config.path);

    // 绑定姿态只用来确定固定分辨率（作者 cellSize 是目标格距）；域本身每帧
    // 从蒙皮后的代理包围盒重算，buffer 大小由分辨率决定、永不改变。
    const auto layoutResult = VL::Hair::MakeTressfxSdfLayout(
        *tressfxPlacementResources->collisionMesh, config.cellSize, config.margin);
    if (layoutResult.IsFailure())
        throw std::runtime_error("TressFX collision mesh distance field is invalid: " +
            layoutResult.Error().message + " path=" + config.path);
    const VL::Hair::TressfxSdfLayout& layout = layoutResult.Value();
    const uint32_t triangleCount =
        static_cast<uint32_t>(tressfxPlacementResources->collisionMesh->indices.size() / 3);
    const VL::Hair::TressfxSdfParams sdfParams =
        VL::Hair::MakeTressfxSdfParams(layout, triangleCount);

    // 64-byte std140 参数块按 frame-in-flight 分槽：域（origin/cellSize/truncation）
    // 每帧变化，不能与仍在飞的上帧共用一个槽。创建时先写绑定姿态域作初值。
    const vk::DeviceSize paramsBytes = sizeof(VL::Hair::TressfxSdfParams);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxSdfParamsTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            paramsBytes,
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSdfParams: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, paramsBytes);
        std::memcpy(target.mapped, &sdfParams, sizeof(sdfParams));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(paramsBytes);
    }

    // 代理顶点/索引：device-local，仅创建时经 staging 上传一次。splat pass 每帧
    // 重新蒙皮，因此这里存的是绑定姿态坐标。
    std::vector<Eigen::Vector4f> proxyPositions;
    proxyPositions.reserve(tressfxPlacementResources->collisionMesh->positions.size());
    for (const Eigen::Vector3f& position : tressfxPlacementResources->collisionMesh->positions)
        proxyPositions.emplace_back(position.x(), position.y(), position.z(), 0.0f);
    const vk::DeviceSize triangleBytes =
        static_cast<vk::DeviceSize>(proxyPositions.size()) * sizeof(Eigen::Vector4f);
    std::tie(tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory) =
        CreateUploadedDeviceLocalStorageBuffer(*rendererBackend, proxyPositions.data(),
            triangleBytes, "TressFXSdfProxyPositions: " + name);
    tressfxSdfTriangleInfo
        .setBuffer(tressfxSdfTriangleBuffer).setOffset(0).setRange(triangleBytes);

    const std::vector<std::uint32_t>& proxyIndices = tressfxPlacementResources->collisionMesh->indices;
    const vk::DeviceSize indexBytes =
        static_cast<vk::DeviceSize>(proxyIndices.size()) * sizeof(std::uint32_t);
    std::tie(tressfxSdfIndexBuffer, tressfxSdfIndexMemory) =
        CreateUploadedDeviceLocalStorageBuffer(*rendererBackend, proxyIndices.data(),
            indexBytes, "TressFXSdfProxyIndices: " + name);
    tressfxSdfIndexInfo
        .setBuffer(tressfxSdfIndexBuffer).setOffset(0).setRange(indexBytes);

    // 距离格子 buffer 只由 compute 写：clear pass 每帧先填 INITIAL_DISTANCE
    // 哨兵，因此创建时不需要上传初值。分辨率固定，所以 buffer 大小不随帧变化。
    const vk::DeviceSize gridBytes =
        static_cast<vk::DeviceSize>(layout.CellCount()) * sizeof(std::uint32_t);
    std::tie(tressfxSdfGridBuffer, tressfxSdfGridMemory) = rendererBackend->CreateBuffer(
        gridBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "TressFXSdfGrid: " + name);
    tressfxSdfGridInfo
        .setBuffer(tressfxSdfGridBuffer).setOffset(0).setRange(gridBytes);

    tressfxSdfTriangleCount = triangleCount;
    tressfxSdfCellCount = layout.CellCount();
    tressfxSdfResolution = layout.resolution;
    tressfxSdfCollision = true;

    // SDF splat 的 shader 静态声明 bindings 12/13，因此即使没有权重转移也必须
    // 提供有效 descriptor。占位影响流是 16 字节零；占位 palette 是 current/
    // previous 两个单位矩阵（128 bytes），shader 在 counts.y=0 时不读取它们。
    const std::array<std::uint32_t, 4> placeholderInfluences{0, 0, 0, 0};
    const vk::DeviceSize influenceBytes = sizeof(placeholderInfluences);
    std::tie(tressfxSdfProxyInfluenceBuffer, tressfxSdfProxyInfluenceMemory) =
        CreateUploadedDeviceLocalStorageBuffer(*rendererBackend,
            placeholderInfluences.data(), influenceBytes,
            "TressFXSdfProxyInfluences: " + name);
    tressfxSdfProxyInfluenceInfo.setBuffer(tressfxSdfProxyInfluenceBuffer)
        .setOffset(0).setRange(influenceBytes);

    const std::array<Eigen::Matrix4f, 2> identityPalette{
        Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()};
    const vk::DeviceSize paletteBytes = sizeof(identityPalette);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxSdfProxyPaletteTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            paletteBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSdfProxyPalette: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, paletteBytes);
        std::memcpy(target.mapped, identityPalette.data(),
            static_cast<size_t>(paletteBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(paletteBytes);
    }

    std::cout << "[TressFX] SDF collision buffer: object=\"" << name
              << "\" proxyTriangles=" << triangleCount
              << " resolution=" << layout.resolution << "^3"
              << " cells=" << layout.CellCount()
              << " bindCellSize=" << layout.cellSize << " margin=" << layout.margin
              << " (domain is rebuilt per frame)" << std::endl;
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
    std::tie(buffer, memory) = CreateUploadedDeviceLocalStorageBuffer(
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

void RenderableObject::DestroyTressfxBuffers()
{
    if (rendererBackend)
    {
        if (tressfxPositionBuffer)
            rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
        if (tressfxGuideGroupIdBuffer)
            rendererBackend->DestroyBuffer(
                tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory);
        if (tressfxGuideGroupColorBuffer)
            rendererBackend->DestroyBuffer(
                tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory);
        for (size_t slot = 0; slot < tressfxBrushWeightBuffers.size(); ++slot)
            if (tressfxBrushWeightBuffers[slot])
                rendererBackend->DestroyBuffer(
                    tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]);
        tressfxBrushWeightBuffers.clear();
        tressfxBrushWeightMemories.clear();
        tressfxBrushWeightInfos.clear();
        tressfxBrushActive = false;
        if (tressfxPreviousPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxPreviousPositionBuffer, tressfxPreviousPositionMemory);
        if (tressfxPreviousPreviousPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxPreviousPreviousPositionBuffer,
                tressfxPreviousPreviousPositionMemory);
        if (tressfxRestPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxRestPositionBuffer, tressfxRestPositionMemory);
        if (tressfxRestLengthBuffer)
            rendererBackend->DestroyBuffer(
                tressfxRestLengthBuffer, tressfxRestLengthMemory);
        if (tressfxStrandLevelBuffer)
            rendererBackend->DestroyBuffer(
                tressfxStrandLevelBuffer, tressfxStrandLevelMemory);
        for (auto& target : tressfxColliderTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxGuideGroupConfigTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        if (tressfxSdfGridBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfGridBuffer, tressfxSdfGridMemory);
        if (tressfxSdfTriangleBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory);
        if (tressfxSdfIndexBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfIndexBuffer, tressfxSdfIndexMemory);
        if (tressfxSdfProxyInfluenceBuffer)
            rendererBackend->DestroyBuffer(tressfxSdfProxyInfluenceBuffer,
                tressfxSdfProxyInfluenceMemory);
        for (auto& target : tressfxSdfProxyPaletteTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxSdfParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxResetParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxPaletteTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxPenetrationStatsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        if (tressfxBoneWeightsBuffer || tressfxBoneWeightsMemory)
            rendererBackend->DestroyBuffer(
                tressfxBoneWeightsBuffer, tressfxBoneWeightsMemory);
    }
    tressfxBoneWeightsBuffer = nullptr;
    tressfxBoneWeightsMemory = nullptr;
    tressfxBoneWeightsInfo = vk::DescriptorBufferInfo{};
    tressfxPaletteJointCapacity = 0;
    tressfxPositionBuffer = nullptr;
    tressfxPositionMemory = nullptr;
    tressfxPreviousPositionBuffer = nullptr;
    tressfxPreviousPositionMemory = nullptr;
    tressfxPreviousPreviousPositionBuffer = nullptr;
    tressfxPreviousPreviousPositionMemory = nullptr;
    tressfxRestPositionBuffer = nullptr;
    tressfxRestPositionMemory = nullptr;
    tressfxRestLengthBuffer = nullptr;
    tressfxRestLengthMemory = nullptr;
    tressfxStrandLevelBuffer = nullptr;
    tressfxStrandLevelMemory = nullptr;
    tressfxColliderBuffer = nullptr;
    tressfxColliderMemory = nullptr;
    tressfxSdfGridBuffer = nullptr;
    tressfxSdfGridMemory = nullptr;
    tressfxSdfTriangleBuffer = nullptr;
    tressfxSdfTriangleMemory = nullptr;
    tressfxSdfIndexBuffer = nullptr;
    tressfxSdfIndexMemory = nullptr;
    tressfxSdfTriangleCount = 0;
    tressfxSdfCellCount = 0;
    tressfxSdfResolution = 0;
    tressfxSdfCollision = false;
    reportedSdfDomain = false;
    reportedSdfDomainFailure = false;
    tressfxGpuSimulation = false;
    tressfxBrushWeightBuffers.clear();
    tressfxBrushWeightMemories.clear();
    tressfxBrushWeightInfos.clear();
}

void RenderableObject::CreateSkinInfluenceBuffer()
{
    if (!imported || !imported->modelResource.skeletalAsset) return;
    const auto words = VL::Animation::PackSkinInfluences(
        imported->modelResource.skeletalAsset->mesh.sections.at(importedSectionIndex));
    const vk::DeviceSize size = words.size() * sizeof(uint32_t);
    std::tie(skinInfluenceBuffer, skinInfluenceMemory) = rendererBackend->CreateBuffer(
        size, vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "SkinInfluences: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(skinInfluenceMemory, size);
        std::memcpy(mapped, words.data(), size);
        rendererBackend->UnmapMemory(skinInfluenceMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
        skinInfluenceBuffer = nullptr;
        skinInfluenceMemory = nullptr;
        throw;
    }
    skinInfluenceInfo.setBuffer(skinInfluenceBuffer).setOffset(0).setRange(size);
}

void RenderableObject::Draw(vk::CommandBuffer &commandBuffer)
{
    // viewport/scissor 由 PassRuntime 按 pass 统一设置，避免每个对象把中心
    // 视口覆盖回整张资源。
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);
    commandBuffer.drawIndexed(GetIndices().size(), 1, 0, 0, 0);
}

void RenderableObject::CreateVertexBuffer()
{
    const auto& sourceVertices = GetVertices();
    vk::DeviceSize bufferSize = sourceVertices.size() * sizeof(Vertex);
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        usage,
        memoryPropertyFlags,
        "StagingBuffer: Vertex (" + name + ")");

    void *data = rendererBackend->MapMemory(stagingBufferMemory, bufferSize);
    std::memcpy(data, sourceVertices.data(), static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(stagingBufferMemory);

    // eStorageBuffer 供 GPU 预蒙皮 compute 以 SSBO 读取同一份顶点流；不需要
    // 复制一份 rest position/normal/tangent。usage 位在建对象时固定，不随 flag 改变。
    vk::BufferUsageFlags vertexBufferUsage = vk::BufferUsageFlagBits::eVertexBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags vertexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(vertexBuffer, vertexBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        vertexBufferUsage,
        vertexBufferMemoryPropertyFlags,
        "VertexBuffer (" + name + ")");

    rendererBackend->CopyBufferToBuffer(stagingBuffer, vertexBuffer, bufferSize);
    rendererBackend->DestroyBuffer(stagingBuffer, stagingBufferMemory);

    vertexBufferInfo
        .setBuffer(vertexBuffer)
        .setOffset(0)
        .setRange(sourceVertices.size() * sizeof(Vertex));
}

void RenderableObject::DestroyVertexBuffer()
{
    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyBuffer(vertexBuffer, vertexBufferMemory);
        vertexBuffer = nullptr;
        vertexBufferMemory = nullptr;
    }
}

void RenderableObject::CreateIndexBuffer()
{
    const auto& sourceIndices = GetIndices();
    vk::DeviceSize bufferSize = sourceIndices.size() * sizeof(uint32_t);
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        usage,
        memoryPropertyFlags,
        "StagingBuffer: Index (" + name + ")");

    void *data = rendererBackend->MapMemory(stagingBufferMemory, bufferSize);
    std::memcpy(data, sourceIndices.data(), static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(stagingBufferMemory);

    vk::BufferUsageFlags indexBufferUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags indexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(indexBuffer, indexBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        indexBufferUsage,
        indexBufferMemoryPropertyFlags,
        "IndexBuffer (" + name + ")");

    rendererBackend->CopyBufferToBuffer(stagingBuffer, indexBuffer, bufferSize);
    rendererBackend->DestroyBuffer(stagingBuffer, stagingBufferMemory);
    
    indexBufferInfo
        .setBuffer(indexBuffer)
        .setOffset(0)
        .setRange(sourceIndices.size() * sizeof(uint32_t));
}

void RenderableObject::DestroyIndexBuffer()
{
    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyBuffer(indexBuffer, indexBufferMemory);
        indexBuffer = nullptr;
        indexBufferMemory = nullptr;
    }
}
void RenderableObject::UpdateLocalBounds()
{
    const auto& sourceVertices = GetVertices();
    if (sourceVertices.empty())
    {
        boundsMin = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        boundsMax = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        return;
    }

    boundsMin = sourceVertices[0].position;
    boundsMax = sourceVertices[0].position;
    for (const auto& vertex : sourceVertices)
    {
        boundsMin = boundsMin.cwiseMin(vertex.position);
        boundsMax = boundsMax.cwiseMax(vertex.position);
    }
    if (imported)
    {
        const float padding = imported->modelResource.sections[importedSectionIndex].boundsPadding;
        boundsMin.array() -= padding;
        boundsMax.array() += padding;
    }
}
