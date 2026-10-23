#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "animation/skeletalPlayback.h"
#include "hair/tressfxSdfGrid.h"
#include "commonFunction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

// params UBO 的槽数必须与帧同步环一致：BeginFrame 先等该槽 fence，主机再写槽内参数。
static_assert(VL::Hair::GuideSimulationParamBufferCount == MAX_FRAMES_IN_FLIGHT,
    "TressFX params slots must match the frame-in-flight ring");

namespace
{
// GPU 模拟的位置 buffer 位于 device-local；初始内容只在创建时经 staging 上传一次。
// 上传后由 HairSimulationPass 在本帧 dispatch 前的 transfer→compute barrier 转为可见。
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
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
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
    const std::string& objectName)
    : imported(std::move(importedResource))
    , importedSectionIndex(sectionIndex)
{
    if (!imported || sectionIndex >= imported->modelResource.sections.size())
        throw std::invalid_argument("Invalid imported mesh section");
    rendererBackend = &backend;
    name = objectName;
    const MeshSection& section = imported->modelResource.sections[sectionIndex];
    if (section.isTressfx)
    {
        const size_t pointCount = section.tressfxInverseMass.size();
        const size_t pointsPerGuide = section.tressfxPointsPerStrand;
        if (pointCount != 0 && pointsPerGuide >= 2 &&
            pointCount % pointsPerGuide == 0 &&
            section.vertices.size() == pointCount * 2)
        {
            tressfxGuideCount = static_cast<uint32_t>(pointCount / pointsPerGuide);
            tressfxPointsPerGuide = section.tressfxPointsPerStrand;
            // GPU backend 不构造 CPU 参考 solver；位置只由 HairSimulationPass 的
            // compute dispatch 推进，避免同一份 SSBO 出现两个写入者。
            if (section.tressfxSimulation.enabled &&
                section.tressfxSimulation.backend == TressfxSimulationBackend::Cpu)
            {
                std::vector<Eigen::Vector3f> restPositions;
                restPositions.reserve(pointCount);
                for (size_t point = 0; point < pointCount; ++point)
                    restPositions.push_back(section.vertices[point * 2].position);

                VL::Hair::SimulationConfig simulationConfig;
                simulationConfig.gravity = section.tressfxSimulation.gravity;
                simulationConfig.wind = section.tressfxSimulation.wind;
                simulationConfig.damping = section.tressfxSimulation.damping;
                simulationConfig.iterations = section.tressfxSimulation.iterations;
                simulationConfig.rootFixedPoints = section.tressfxSimulation.rootFixedPoints;
                simulationConfig.maxDt = section.tressfxSimulation.maxDt;
                simulationConfig.bendStiffness = section.tressfxSimulation.bendStiffness;
                simulationConfig.windGust = section.tressfxSimulation.windGust;
                simulationConfig.windFrequency = section.tressfxSimulation.windFrequency;
                simulationConfig.globalShapeStiffness =
                    section.tressfxSimulation.globalShapeStiffness;
                simulationConfig.globalShapeEffectiveRange =
                    section.tressfxSimulation.globalShapeEffectiveRange;
                simulationConfig.vspCoefficient =
                    section.tressfxSimulation.vspCoefficient;
                simulationConfig.vspAccelThresholdMax =
                    section.tressfxSimulation.vspAccelThresholdMax;
                simulationConfig.capsules = section.tressfxSimulation.capsules;
                tressfxSimulation = std::make_unique<VL::Hair::GuideSimulation>(
                    std::move(restPositions),
                    tressfxGuideCount,
                    pointsPerGuide,
                    section.tressfxInverseMass,
                    simulationConfig);
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

const TressfxSimulationConfig& RenderableObject::GetTressfxSimulationConfig() const
{
    if (!IsTressfx())
    {
        // 非 tressfx 对象不应调用该接口；返回静态默认值而不是解引用空 imported。
        static const TressfxSimulationConfig defaultConfig;
        return defaultConfig;
    }
    return imported->modelResource.sections[importedSectionIndex].tressfxSimulation;
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
    const bool gpuSimulation = section.tressfxSimulation.enabled &&
        section.tressfxSimulation.backend == TressfxSimulationBackend::Gpu &&
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

void RenderableObject::AdvanceTressfxSimulation(float dt)
{
    // GPU backend 的 tressfxSimulation 为空：位置 SSBO 只由 compute dispatch 写。
    if (!tressfxSimulation)
        return;

    tressfxSimulation->Step(dt);
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
    if (!section.tressfxSimulation.enabled ||
        section.tressfxSimulation.backend != TressfxSimulationBackend::Gpu ||
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
    }
    // 静态解析胶囊：创建时上传一次。空列表也保留合法 buffer，因为 shader 的
    // binding 4 是静态声明，不能用空 descriptor；count 由 params.counts.w 控制。
    const vk::DeviceSize colliderBytes =
        sizeof(VL::Hair::GuideColliderGpu) * VL::Hair::TressfxMaxColliders;
    std::array<VL::Hair::GuideColliderGpu, VL::Hair::TressfxMaxColliders> gpuColliders{};
    const auto& capsules = section.tressfxSimulation.capsules;
    for (size_t collider = 0; collider < capsules.size(); ++collider)
        gpuColliders[collider] = VL::Hair::MakeGuideColliderGpu(capsules[collider]);
    std::tie(tressfxColliderBuffer, tressfxColliderMemory) =
        rendererBackend->CreateBuffer(
            colliderBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXColliders: " + name);
    void* colliderMapped =
        rendererBackend->MapMemory(tressfxColliderMemory, colliderBytes);
    std::memcpy(colliderMapped, gpuColliders.data(), static_cast<size_t>(colliderBytes));
    rendererBackend->UnmapMemory(tressfxColliderMemory);
    tressfxColliderInfo
        .setBuffer(tressfxColliderBuffer).setOffset(0).setRange(colliderBytes);

    // `collision.mesh` 的距离场资源：只在声明了代理时创建；失败让整个
    // renderable 构造失败（构造函数的 catch 会回收已建 buffer）。
    CreateTressfxSdfBuffers();

    tressfxGpuSimulation = true;
}

void RenderableObject::CreateTressfxSdfBuffers()
{
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if (!section.tressfxSimulation.collisionMesh)
        return;
    const TressfxCollisionMeshConfig& config =
        *section.tressfxSimulation.collisionMesh;
    if (!section.tressfxCollisionMesh)
        throw std::runtime_error(
            "TressFX collision.mesh was declared but its OBJ was not imported: " +
            config.path);

    // 绑定姿态只用来确定固定分辨率（作者 cellSize 是目标格距）；域本身每帧
    // 从蒙皮后的代理包围盒重算，buffer 大小由分辨率决定、永不改变。
    const auto layoutResult = VL::Hair::MakeTressfxSdfLayout(
        *section.tressfxCollisionMesh, config.cellSize, config.margin);
    if (layoutResult.IsFailure())
        throw std::runtime_error("TressFX collision mesh distance field is invalid: " +
            layoutResult.Error().message + " path=" + config.path);
    const VL::Hair::TressfxSdfLayout& layout = layoutResult.Value();
    const uint32_t triangleCount =
        static_cast<uint32_t>(section.tressfxCollisionMesh->indices.size() / 3);
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
    proxyPositions.reserve(section.tressfxCollisionMesh->positions.size());
    for (const Eigen::Vector3f& position : section.tressfxCollisionMesh->positions)
        proxyPositions.emplace_back(position.x(), position.y(), position.z(), 0.0f);
    const vk::DeviceSize triangleBytes =
        static_cast<vk::DeviceSize>(proxyPositions.size()) * sizeof(Eigen::Vector4f);
    std::tie(tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory) =
        CreateUploadedDeviceLocalStorageBuffer(*rendererBackend, proxyPositions.data(),
            triangleBytes, "TressFXSdfProxyPositions: " + name);
    tressfxSdfTriangleInfo
        .setBuffer(tressfxSdfTriangleBuffer).setOffset(0).setRange(triangleBytes);

    const std::vector<std::uint32_t>& proxyIndices = section.tressfxCollisionMesh->indices;
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

    std::cout << "[TressFX] SDF collision buffer: object=\"" << name
              << "\" proxyTriangles=" << triangleCount
              << " resolution=" << layout.resolution << "^3"
              << " cells=" << layout.CellCount()
              << " bindCellSize=" << layout.cellSize << " margin=" << layout.margin
              << " (domain is rebuilt per frame)" << std::endl;
}

bool RenderableObject::WriteTressfxSdfParams(
    const Eigen::Matrix4f& skinningMatrix, uint32_t frameSlot)
{
    if (!tressfxSdfCollision || !imported ||
        frameSlot >= VL::Hair::GuideSimulationParamBufferCount)
        return false;
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if (!section.tressfxCollisionMesh || !section.tressfxSimulation.collisionMesh)
        return false;

    // 逐帧域：把 2,572 个代理顶点按同一根骨矩阵蒙皮后求包围盒。CPU 成本可忽略，
    // 但能得到精确的变形后域，而不是绑定姿态包围盒的保守估计。
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
    bool hasBounds = false;
    for (const Eigen::Vector3f& rest : section.tressfxCollisionMesh->positions)
    {
        const Eigen::Vector3f position =
            (skinningMatrix * rest.homogeneous()).head<3>();
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
        section.tressfxSimulation.collisionMesh->margin, tressfxSdfResolution);
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
        VL::Hair::MakeTressfxSdfParams(domain, tressfxSdfTriangleCount);
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
        if (tressfxColliderBuffer)
            rendererBackend->DestroyBuffer(
                tressfxColliderBuffer, tressfxColliderMemory);
        if (tressfxSdfGridBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfGridBuffer, tressfxSdfGridMemory);
        if (tressfxSdfTriangleBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory);
        if (tressfxSdfIndexBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfIndexBuffer, tressfxSdfIndexMemory);
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
    }
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
