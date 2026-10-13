#pragma once

#include <cstdint>

#include <vulkan/vulkan.hpp>

namespace VL
{
class RendererBackendVulkan;

// 编辑器视口参考网格的几何所有者：一次性生成世界空间 XZ 地面网格条带与原点
// 坐标轴三角面，并持有其顶点缓冲。它不含管线、pass 状态和深度策略 —— 那些由
// renderGraphConfig 的 editorGrid pass 与 pass/editorGrid shader 决定，
// 因此这里只回答"网格长什么样"，不回答"网格怎么被画出来"。
class EditorViewportGrid
{
public:
    // 顶点数据是静态世界几何，创建一次即可；重复调用不会重建缓冲。
    void EnsureCreated(RendererBackendVulkan& rendererBackend);
    bool IsCreated() const noexcept { return vertexCount != 0; }
    void Bind(vk::CommandBuffer& commandBuffer) const;
    void Draw(vk::CommandBuffer& commandBuffer) const;
    // 调用方负责保证 GPU 已完成引用该缓冲的工作（swapchain 重建与关闭走同一处）。
    void Shutdown(RendererBackendVulkan& rendererBackend);

private:
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexMemory;
    uint32_t vertexCount = 0;
};

} // namespace VL
