#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

class PipelineBase;
class PipelineFactory;
class RenderGraph;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RenderScene;

// BeforeLighting DBuffer decal 的 GPU 侧拥有者。一个 decal pass 内用
// instanced unit cube 批量绘制到 DBuffer A/B/C；实例数据只在当前 image
// 的 SSBO 中更新，不创建逐 decal 的 descriptor 或 GPU 资源。
class DecalRenderer
{
public:
    void EnsureInitialized(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void Shutdown(RendererBackendVulkan& backend);
    void InvalidatePipelines(RendererBackendVulkan& backend);
    void RecordGraphics(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass,
        const RenderScene& renderScene);

    bool IsInitialized() const noexcept { return initialized; }

private:
    struct GpuBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
    };

    struct FrameUniforms
    {
        Eigen::Matrix4f viewProjection;
        Eigen::Matrix4f invViewProjection;
    };

    // 与 shader/glsl/decal/decalCommon.glsl 的 std430 DecalInstance 一一对应。
    struct DecalInstance
    {
        Eigen::Matrix4f decalToWorld;
        Eigen::Matrix4f worldToDecal;
        Eigen::Vector4f color;
        // x = roughness, y = metallic, z = ambientOcclusion, w = opacity.
        Eigen::Vector4f surface;
        // x = edgeSoftness, y = noiseStrength, z = seed, w = reserved.
        Eigen::Vector4f params;
        // xyz = world normal, w = normal opacity. Current source leaves
        // normal opacity at 0 until a normal-capable decal material exists.
        Eigen::Vector4f normal;
    };
    static_assert(sizeof(DecalInstance) == 192,
        "DecalInstance layout must match decalCommon.glsl");

    void Initialize(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void CreateStaticGeometry(RendererBackendVulkan& backend);
    void CreateFrameResources(RendererBackendVulkan& backend);
    void DestroyResources(RendererBackendVulkan& backend);
    static void DestroyBuffer(
        RendererBackendVulkan& backend,
        GpuBuffer& buffer);

    RendererBackendVulkan* backend = nullptr;
    bool initialized = false;
    std::shared_ptr<PipelineBase> pipeline;
    GpuBuffer cubeVertices;
    GpuBuffer cubeIndices;
    std::vector<GpuBuffer> frameUniforms;
    std::vector<GpuBuffer> instanceBuffers;
    std::vector<vk::DescriptorSet> descriptorSets;
    vk::DescriptorPool descriptorPool;
};

} // namespace VL
