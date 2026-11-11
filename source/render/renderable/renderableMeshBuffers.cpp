#include "render/renderable/renderableMeshBuffers.h"

#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "animation/skeletalPlayback.h"
#include "mesh/loader/common/meshAssetLoader.h"

namespace VL
{
namespace RenderableDetail
{// GPU 模拟的位置 buffer 位于 device-local；初始内容只在创建时经 staging 上传一次。
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
} // namespace RenderableDetail
} // namespace VL
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

void RenderableObject::SetInstances(std::vector<InstanceVertex> instances)
{
    if (instanceData.size() == instances.size() &&
        (instanceData.empty() ||
         std::memcmp(instanceData.data(), instances.data(),
             instanceData.size() * sizeof(InstanceVertex)) == 0))
    {
        return;
    }
    instanceData = std::move(instances);
    if (instanceData.empty())
        instanceData.push_back(InstanceVertex{});
    DestroyInstanceBuffer();
    CreateInstanceBuffer();
}

void RenderableObject::CreateInstanceBuffer()
{
    const std::vector<InstanceVertex> fallback{InstanceVertex{}};
    const std::vector<InstanceVertex>& source =
        instanceData.empty() ? fallback : instanceData;
    const vk::DeviceSize bufferSize =
        static_cast<vk::DeviceSize>(source.size() * sizeof(InstanceVertex));
    std::tie(instanceBuffer, instanceBufferMemory) =
        rendererBackend->CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "InstanceBuffer (" + name + ")");
    try
    {
        void* mapped = rendererBackend->MapMemory(instanceBufferMemory, bufferSize);
        std::memcpy(mapped, source.data(), static_cast<size_t>(bufferSize));
        rendererBackend->UnmapMemory(instanceBufferMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(instanceBuffer, instanceBufferMemory);
        instanceBuffer = nullptr;
        instanceBufferMemory = nullptr;
        throw;
    }
    instanceCount = static_cast<uint32_t>(source.size());
}

void RenderableObject::DestroyInstanceBuffer()
{
    if (!rendererBackend || !instanceBuffer)
        return;
    rendererBackend->DestroyBuffer(instanceBuffer, instanceBufferMemory);
    instanceBuffer = nullptr;
    instanceBufferMemory = nullptr;
}

void RenderableObject::CreateVertexBuffer()
{
    if (dynamicVertices)
    {
        // 软体碎片拓扑固定，顶点数量不会变化；按 swapchain 槽各建一份
        // host-visible 缓冲，GT 更新永远不会写进仍在飞行的帧。
        const auto& sourceVertices = GetVertices();
        vk::DeviceSize bufferSize = fixedCapacity
            ? static_cast<vk::DeviceSize>(vertexCapacity * sizeof(Vertex))
            : static_cast<vk::DeviceSize>(
                sourceVertices.size() * sizeof(Vertex));
        bufferSize = std::max<vk::DeviceSize>(bufferSize, sizeof(Vertex));
        for (uint32_t slot = 0; slot < DynamicMeshFrameSlotCount; ++slot)
        {
            std::tie(dynamicVertexBuffers[slot],
                dynamicVertexMemories[slot]) =
                rendererBackend->CreateBuffer(
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "DynamicVertexBuffer[" + std::to_string(slot) +
                        "] (" + name + ")");
            void* data = rendererBackend->MapMemory(
                dynamicVertexMemories[slot], bufferSize);
            if (fixedCapacity)
            {
                std::memset(data, 0, static_cast<size_t>(bufferSize));
            }
            else if (!sourceVertices.empty())
            {
                std::memcpy(data, sourceVertices.data(),
                    static_cast<size_t>(bufferSize));
            }
            else
            {
                std::memset(data, 0, static_cast<size_t>(bufferSize));
            }
            rendererBackend->UnmapMemory(dynamicVertexMemories[slot]);
            dynamicVertexBufferInfos[slot]
                .setBuffer(dynamicVertexBuffers[slot])
                .setOffset(0)
                .setRange(bufferSize);
        }
        vertexBufferInfo = dynamicVertexBufferInfos[0];
        // 固定容量对象在第一次 UpdateDynamicMesh 前没有内容；非固定动态
        // 对象已经把初始顶点复制进全部槽位。
        dynamicContentInitialized.store(!fixedCapacity);
        return;
    }
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

void RenderableObject::WriteDynamicVertexSlot(
    uint32_t slot,
    const std::vector<Vertex>& updatedVertices)
{
    if (slot >= DynamicMeshFrameSlotCount || updatedVertices.empty())
        return;
    if (!dynamicVertexMemories[slot])
        throw std::logic_error("Dynamic vertex slot is not allocated");
    const vk::DeviceSize bufferSize =
        static_cast<vk::DeviceSize>(updatedVertices.size() * sizeof(Vertex));
    void* data = rendererBackend->MapMemory(
        dynamicVertexMemories[slot], bufferSize);
    std::memcpy(data, updatedVertices.data(),
        static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(dynamicVertexMemories[slot]);
}

void RenderableObject::WriteDynamicIndexSlot(
    uint32_t slot,
    const std::vector<uint32_t>& updatedIndices)
{
    if (slot >= DynamicMeshFrameSlotCount || updatedIndices.empty())
        return;
    if (!dynamicIndexMemories[slot])
        throw std::logic_error("Dynamic index slot is not allocated");
    const vk::DeviceSize bufferSize =
        static_cast<vk::DeviceSize>(
            updatedIndices.size() * sizeof(uint32_t));
    void* data = rendererBackend->MapMemory(
        dynamicIndexMemories[slot], bufferSize);
    std::memcpy(data, updatedIndices.data(),
        static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(dynamicIndexMemories[slot]);
}

void RenderableObject::ResetDynamicContentForReuse()
{
    dynamicContentInitialized.store(false, std::memory_order_release);
}

void RenderableObject::UpdateVertices(
    const std::vector<Vertex>& updatedVertices,
    uint32_t frameSlot)
{
    if (imported)
        throw std::logic_error("Imported meshes cannot be updated in place");
    if (!dynamicVertices)
        throw std::logic_error("RenderableObject is not using dynamic vertices");
    if (updatedVertices.size() != vertices.size())
        throw std::invalid_argument(
            "Dynamic vertex updates must preserve the vertex count");
    if (updatedVertices.empty()) return;

    const uint32_t slot = frameSlot % DynamicMeshFrameSlotCount;
    if (!dynamicContentInitialized.load(std::memory_order_acquire))
    {
        // 首次内容写入必须覆盖全部槽位；此前对象尚未进入任何已提交帧。
        for (uint32_t target = 0; target < DynamicMeshFrameSlotCount; ++target)
            WriteDynamicVertexSlot(target, updatedVertices);
        dynamicContentInitialized.store(true, std::memory_order_release);
    }
    else
    {
        WriteDynamicVertexSlot(slot, updatedVertices);
    }
    vertices = updatedVertices;
    UpdateLocalBounds();
}

void RenderableObject::UpdateDynamicMesh(
    const std::vector<Vertex>& updatedVertices,
    const std::vector<uint32_t>& updatedIndices,
    uint32_t frameSlot)
{
    if (imported) throw std::logic_error("Imported meshes cannot be updated in place");
    if (!fixedCapacity)
        throw std::logic_error("UpdateDynamicMesh requires a fixed-capacity mesh");
    if (updatedVertices.size() > vertexCapacity || updatedIndices.size() > indexCapacity)
        throw std::invalid_argument("Dynamic mesh update exceeds its capacity");
    const uint32_t slot = frameSlot % DynamicMeshFrameSlotCount;
    if (!dynamicContentInitialized.load(std::memory_order_acquire))
    {
        for (uint32_t target = 0; target < DynamicMeshFrameSlotCount; ++target)
        {
            WriteDynamicVertexSlot(target, updatedVertices);
            WriteDynamicIndexSlot(target, updatedIndices);
            dynamicIndexCounts[target] =
                static_cast<uint32_t>(updatedIndices.size());
        }
        dynamicContentInitialized.store(true, std::memory_order_release);
    }
    else
    {
        WriteDynamicVertexSlot(slot, updatedVertices);
        WriteDynamicIndexSlot(slot, updatedIndices);
        dynamicIndexCounts[slot] =
            static_cast<uint32_t>(updatedIndices.size());
    }
    vertices = updatedVertices;
    indices = updatedIndices;
    drawIndexCount = static_cast<uint32_t>(indices.size());
    UpdateLocalBounds();
}

void RenderableObject::DestroyVertexBuffer()
{
    if (rendererBackend == nullptr)
        return;
    if (dynamicVertices)
    {
        for (uint32_t slot = 0; slot < DynamicMeshFrameSlotCount; ++slot)
        {
            if (!dynamicVertexBuffers[slot]) continue;
            rendererBackend->DestroyBuffer(
                dynamicVertexBuffers[slot],
                dynamicVertexMemories[slot]);
            dynamicVertexBuffers[slot] = nullptr;
            dynamicVertexMemories[slot] = nullptr;
        }
        vertexBuffer = nullptr;
        vertexBufferMemory = nullptr;
        return;
    }
    if (vertexBuffer)
    {
        rendererBackend->DestroyBuffer(vertexBuffer, vertexBufferMemory);
        vertexBuffer = nullptr;
        vertexBufferMemory = nullptr;
    }
}

void RenderableObject::CreateIndexBuffer()
{
    if (fixedCapacity)
    {
        const vk::DeviceSize bufferSize =
            static_cast<vk::DeviceSize>(
                std::max<size_t>(indexCapacity, 1) * sizeof(uint32_t));
        for (uint32_t slot = 0; slot < DynamicMeshFrameSlotCount; ++slot)
        {
            std::tie(dynamicIndexBuffers[slot],
                dynamicIndexMemories[slot]) =
                rendererBackend->CreateBuffer(
                    bufferSize,
                    vk::BufferUsageFlagBits::eIndexBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "DynamicIndexBuffer[" + std::to_string(slot) +
                        "] (" + name + ")");
            void* data = rendererBackend->MapMemory(
                dynamicIndexMemories[slot], bufferSize);
            std::memset(data, 0, static_cast<size_t>(bufferSize));
            rendererBackend->UnmapMemory(dynamicIndexMemories[slot]);
            dynamicIndexBufferInfos[slot]
                .setBuffer(dynamicIndexBuffers[slot])
                .setOffset(0)
                .setRange(bufferSize);
        }
        indexBufferInfo = dynamicIndexBufferInfos[0];
        return;
    }
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

    // Storage 用途供描边法线平滑的 accumulate pass 读取三角形索引。
    vk::BufferUsageFlags indexBufferUsage = vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;
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
    if (rendererBackend == nullptr)
        return;
    if (fixedCapacity)
    {
        for (uint32_t slot = 0; slot < DynamicMeshFrameSlotCount; ++slot)
        {
            if (!dynamicIndexBuffers[slot]) continue;
            rendererBackend->DestroyBuffer(
                dynamicIndexBuffers[slot],
                dynamicIndexMemories[slot]);
            dynamicIndexBuffers[slot] = nullptr;
            dynamicIndexMemories[slot] = nullptr;
        }
        indexBuffer = nullptr;
        indexBufferMemory = nullptr;
        return;
    }
    if (indexBuffer)
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
