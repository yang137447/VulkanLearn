#include "render/editor/editorViewportGrid.h"

#include <cstring>
#include <utility>
#include <vector>

#include "render/backend/rendererBackendVulkan.h"
#include "render/editor/editorViewportGridGeometry.h"
#include "vertexDataStruct.h"

namespace VL
{

void EditorViewportGrid::EnsureCreated(RendererBackendVulkan& rendererBackend)
{
    if (vertexCount != 0)
    {
        return;
    }

    const std::vector<Vertex> vertices = BuildEditorGridVertices();
    const vk::DeviceSize bufferSize =
        static_cast<vk::DeviceSize>(vertices.size() * sizeof(Vertex));
    const std::pair<vk::Buffer, vk::DeviceMemory> created =
        rendererBackend.CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "EditorViewportGridVertices");
    // 先接管所有权再写入：映射失败时 Shutdown 仍会释放刚创建的缓冲。
    vertexBuffer = created.first;
    vertexMemory = created.second;

    // 静态几何只写一次，直接写宿主可见内存即可，不需要暂存缓冲与拷贝命令。
    void* mapped = rendererBackend.MapMemory(vertexMemory, bufferSize);
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(bufferSize));
    rendererBackend.UnmapMemory(vertexMemory);

    vertexCount = static_cast<uint32_t>(vertices.size());

    const InstanceVertex identity;
    const vk::DeviceSize instanceSize = sizeof(InstanceVertex);
    const std::pair<vk::Buffer, vk::DeviceMemory> instanceCreated =
        rendererBackend.CreateBuffer(
            instanceSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "EditorViewportGridInstances");
    instanceBuffer = instanceCreated.first;
    instanceMemory = instanceCreated.second;
    void* instanceMapped =
        rendererBackend.MapMemory(instanceMemory, instanceSize);
    std::memcpy(instanceMapped, &identity, sizeof(InstanceVertex));
    rendererBackend.UnmapMemory(instanceMemory);
}

void EditorViewportGrid::Bind(vk::CommandBuffer& commandBuffer) const
{
    const vk::Buffer buffers[2] = {vertexBuffer, instanceBuffer};
    const vk::DeviceSize offsets[2] = {0, 0};
    commandBuffer.bindVertexBuffers(0, 2, buffers, offsets);
}

void EditorViewportGrid::Draw(vk::CommandBuffer& commandBuffer) const
{
    commandBuffer.draw(vertexCount, 1, 0, 0);
}

void EditorViewportGrid::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (vertexCount == 0)
    {
        return;
    }

    rendererBackend.DestroyBuffer(vertexBuffer, vertexMemory);
    rendererBackend.DestroyBuffer(instanceBuffer, instanceMemory);
    vertexBuffer = nullptr;
    vertexMemory = nullptr;
    instanceBuffer = nullptr;
    instanceMemory = nullptr;
    vertexCount = 0;
}

} // namespace VL
