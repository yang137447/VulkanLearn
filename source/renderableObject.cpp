#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "animation/skeletalPlayback.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

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
        dynamicVertices = section.vertices;
        const size_t pointCount = section.tressfxInverseMass.size();
        const size_t pointsPerGuide = section.tressfxPointsPerStrand;
        if (pointCount != 0 && pointsPerGuide >= 2 &&
            pointCount % pointsPerGuide == 0 &&
            section.vertices.size() == pointCount * 2)
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
            if (section.tressfxSimulation.enabled)
            {
                tressfxSimulation = std::make_unique<VL::Hair::GuideSimulation>(
                    std::move(restPositions),
                    pointCount / pointsPerGuide,
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
    }
    catch (...)
    {
        if (rendererBackend && tressfxPositionBuffer)
            rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
        if (rendererBackend && skinInfluenceBuffer)
            rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
        DestroyIndexBuffer();
        DestroyVertexBuffer();
        throw;
    }
}

const std::vector<Vertex>& RenderableObject::GetVertices() const
{
    if (!dynamicVertices.empty())
        return dynamicVertices;
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
    if (rendererBackend && tressfxPositionBuffer)
        rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
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

void RenderableObject::CreateTressfxPositionBuffer()
{
    if (!IsTressfx())
        return;

    std::vector<Eigen::Vector3f> staticPositions;
    const std::vector<Eigen::Vector3f>* positions = nullptr;
    if (tressfxSimulation)
    {
        positions = &tressfxSimulation->Positions();
    }
    else
    {
        const auto& section = imported->modelResource.sections[importedSectionIndex];
        staticPositions.reserve(section.vertices.size() / 2);
        for (size_t point = 0; point < section.vertices.size() / 2; ++point)
            staticPositions.push_back(section.vertices[point * 2].position);
        positions = &staticPositions;
    }
    const vk::DeviceSize size = positions->size() * sizeof(Eigen::Vector4f);
    std::tie(tressfxPositionBuffer, tressfxPositionMemory) = rendererBackend->CreateBuffer(
        size,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "TressFXPositions: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(tressfxPositionMemory, size);
        auto* destination = static_cast<Eigen::Vector4f*>(mapped);
        for (size_t point = 0; point < positions->size(); ++point)
            destination[point] = Eigen::Vector4f(
                (*positions)[point].x(), (*positions)[point].y(), (*positions)[point].z(), 1.0f);
        rendererBackend->UnmapMemory(tressfxPositionMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
        tressfxPositionBuffer = nullptr;
        tressfxPositionMemory = nullptr;
        throw;
    }
    tressfxPositionInfo
        .setBuffer(tressfxPositionBuffer)
        .setOffset(0)
        .setRange(size);
}

void RenderableObject::AdvanceTressfxSimulation(float dt)
{
    if (!tressfxSimulation || dynamicVertices.empty())
        return;

    tressfxSimulation->Step(dt);
    const auto& positions = tressfxSimulation->Positions();
    for (size_t point = 0; point < positions.size(); ++point)
    {
        dynamicVertices[point * 2].position = positions[point];
        dynamicVertices[point * 2 + 1].position = positions[point];
    }

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

    // 这里是游戏线程直接写 host-coherent 缓冲，没有等待在飞帧的 fence：两帧共用同一份
    // buffer，前一帧的顶点/SSBO 读取理论上可能与这次主机写入重叠。当前 CPU 原型接受
    // 该风险换取逐帧可见的动态；GPU dispatch 阶段应改为只由 compute 写 SSBO，并删除
    // 这次逐帧顶点回写（vertex factory 已从 SSBO 取中心线，顶点位置并不被消费）。
    const vk::DeviceSize vertexBytes = dynamicVertices.size() * sizeof(Vertex);
    void* mapped = rendererBackend->MapMemory(vertexBufferMemory, vertexBytes);
    std::memcpy(mapped, dynamicVertices.data(), static_cast<size_t>(vertexBytes));
    rendererBackend->UnmapMemory(vertexBufferMemory);
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
    if (IsTressfx())
    {
        std::tie(vertexBuffer, vertexBufferMemory) = rendererBackend->CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXVertexBuffer: " + name);
        void* data = rendererBackend->MapMemory(vertexBufferMemory, bufferSize);
        std::memcpy(data, sourceVertices.data(), static_cast<size_t>(bufferSize));
        rendererBackend->UnmapMemory(vertexBufferMemory);
        vertexBufferInfo
            .setBuffer(vertexBuffer)
            .setOffset(0)
            .setRange(bufferSize);
        return;
    }
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

    vk::BufferUsageFlags vertexBufferUsage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
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
