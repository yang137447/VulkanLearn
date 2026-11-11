#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "hair/tressfxGroupSimulation.h"
#include "render/renderable/tressfxSimulationBinding.h"
#include "commonFunction.h"

#include <stdexcept>
#include <utility>

// params UBO 的槽数必须与帧同步环一致：BeginFrame 先等该槽 fence，主机再写槽内参数。
static_assert(VL::Hair::GuideSimulationParamBufferCount == MAX_FRAMES_IN_FLIGHT,
    "TressFX params slots must match the frame-in-flight ring");
RenderableObject::RenderableObject(
    std::vector<Vertex> vertices,
    std::vector<uint32_t> indices,
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& name,
    bool dynamicVertices)
{
    this->rendererBackend = &rendererBackend;
    this->vertices = std::move(vertices);
    this->indices = std::move(indices);
    this->dynamicVertices = dynamicVertices;
    this->name = name;

    //更新boundingBox
    UpdateLocalBounds();
    // 创建顶点缓冲区
    CreateVertexBuffer();
    // 创建索引缓冲区
    CreateIndexBuffer();
    CreateInstanceBuffer();
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
            tressfxHasFollowBinding =
                !section.tressfxSimulationGuideIndices.empty();
            tressfxSimulationGuideCount = tressfxHasFollowBinding
                ? static_cast<uint32_t>(section.tressfxSimulationGuideIndices.size())
                : tressfxGuideCount;
            if (tressfxHasFollowBinding &&
                tressfxSimulationConfig.backend != TressfxSimulationBackend::Gpu)
                throw std::runtime_error(
                    "TressFX follow binding requires the GPU backend: " + name);
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
                    simulationConfigs.push_back(
                        VL::RenderableDetail::MakeCpuSimulationConfig(groupConfig));
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
    CreateInstanceBuffer();
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

RenderableObject::RenderableObject(
    size_t vertexCapacity,
    size_t indexCapacity,
    VL::RendererBackendVulkan& backend,
    const std::string& objectName)
    : fixedCapacity(true)
    , vertexCapacity(vertexCapacity)
    , indexCapacity(indexCapacity)
{
    if (vertexCapacity == 0 || indexCapacity == 0)
        throw std::invalid_argument("Dynamic mesh capacity must be positive");
    rendererBackend = &backend;
    name = objectName;
    dynamicVertices = true;
    CreateVertexBuffer();
    CreateIndexBuffer();
    CreateInstanceBuffer();
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
    DestroyInstanceBuffer();
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

void RenderableObject::Draw(
    vk::CommandBuffer &commandBuffer,
    uint32_t frameSlot)
{
    // viewport/scissor 由 PassRuntime 按 pass 统一设置，避免每个对象把中心
    // 视口覆盖回整张资源。
    const uint32_t slot = frameSlot % DynamicMeshFrameSlotCount;
    const vk::Buffer boundVertexBuffer =
        dynamicVertices ? dynamicVertexBuffers[slot] : vertexBuffer;
    const vk::DescriptorBufferInfo& boundVertexInfo =
        dynamicVertices ? dynamicVertexBufferInfos[slot] : vertexBufferInfo;
    const vk::Buffer boundIndexBuffer =
        fixedCapacity ? dynamicIndexBuffers[slot] : indexBuffer;
    const vk::Buffer vertexBuffers[2] = {boundVertexBuffer, instanceBuffer};
    const vk::DeviceSize vertexOffsets[2] = {boundVertexInfo.offset, 0};
    commandBuffer.bindVertexBuffers(0, 2, vertexBuffers, vertexOffsets);
    commandBuffer.bindIndexBuffer(
        boundIndexBuffer, 0, vk::IndexType::eUint32);
    const size_t count =
        fixedCapacity ? dynamicIndexCounts[slot] : GetIndices().size();
    if (count == 0) return;
    commandBuffer.drawIndexed(
        static_cast<uint32_t>(count),
        instanceCount,
        0,
        0,
        0);
}
