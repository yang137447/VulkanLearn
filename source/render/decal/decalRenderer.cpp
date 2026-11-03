#include "render/decal/decalRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include <Eigen/Geometry>

#include "commonFunction.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/pipelineBase.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/frontend/renderScene.h"
#include "render/rendergraph/renderGraphPassType.h"
#include "renderGraph.h"
#include "VertexDataStruct.h"

namespace VL
{
namespace
{

constexpr uint32_t kDecalCubeVertexCount = 8;
constexpr uint32_t kDecalCubeIndexCount = 36;
constexpr uint32_t kMaxDecalInstances = 64;

const Renderpass* FindDecalPass(const RenderGraph& graph)
{
    for (const auto& [name, pass] : graph.GetRenderpasses())
    {
        (void)name;
        if (pass.type == RenderGraphPassType::Decal)
            return &pass;
    }
    return nullptr;
}

std::array<Vertex, kDecalCubeVertexCount> UnitCubeVertices()
{
    std::array<Vertex, kDecalCubeVertexCount> vertices{};
    const std::array<Eigen::Vector3f, kDecalCubeVertexCount> positions = {
        Eigen::Vector3f(-1.0f, -1.0f, -1.0f),
        Eigen::Vector3f( 1.0f, -1.0f, -1.0f),
        Eigen::Vector3f( 1.0f,  1.0f, -1.0f),
        Eigen::Vector3f(-1.0f,  1.0f, -1.0f),
        Eigen::Vector3f(-1.0f, -1.0f,  1.0f),
        Eigen::Vector3f( 1.0f, -1.0f,  1.0f),
        Eigen::Vector3f( 1.0f,  1.0f,  1.0f),
        Eigen::Vector3f(-1.0f,  1.0f,  1.0f)};
    for (uint32_t index = 0; index < kDecalCubeVertexCount; ++index)
    {
        vertices[index].position = positions[index];
        vertices[index].normal = Eigen::Vector3f::Zero();
        vertices[index].color = Eigen::Vector4f::Ones();
        vertices[index].texCoord = Eigen::Vector2f::Zero();
        vertices[index].tangent = Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f);
    }
    return vertices;
}

std::array<uint32_t, kDecalCubeIndexCount> UnitCubeIndices()
{
    return {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4,
        2, 3, 7, 2, 7, 6,
        1, 2, 6, 1, 6, 5,
        3, 0, 4, 3, 4, 7};
}

} // namespace

void DecalRenderer::EnsureInitialized(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    if (initialized)
        return;
    Initialize(factory, renderer, graph);
}

void DecalRenderer::Initialize(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    backend = &renderer;
    const Renderpass* decalPass = FindDecalPass(graph);
    if (decalPass == nullptr)
    {
        initialized = false;
        return;
    }

    ShaderVariantKey variant;
    variant.shaderName = "decal/decal";
    variant.renderMode = RenderMode::Opaque;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        factory.PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        decalPass->pipelineContractKey;
    contract.useVertexInput = true;
    contract.depthTestEnable = false;
    contract.depthWriteEnable = false;
    contract.depthCompareOp = vk::CompareOp::eAlways;
    Renderpass* mutableDecalPass = const_cast<Renderpass*>(decalPass);
    pipeline = factory.CreateGraphicsPipeline(
        &mutableDecalPass->renderPass,
        contract,
        artifact,
        // 闭合 cube 只保留正面；否则同一 sceneDepth 表面会被前后两面
        // 各混合一次。索引绕序与 pipeline 的 clockwise front face 对齐。
        vk::CullModeFlagBits::eBack,
        GraphicsPipelineBlendMode::DecalColor);
    if (!pipeline)
        throw std::runtime_error("DecalRenderer failed to create its graphics pipeline");

    CreateStaticGeometry(renderer);
    CreateFrameResources(renderer);
    initialized = true;
}

void DecalRenderer::CreateStaticGeometry(
    RendererBackendVulkan& renderer)
{
    const auto vertices = UnitCubeVertices();
    const auto indices = UnitCubeIndices();
    const vk::DeviceSize vertexBytes = sizeof(Vertex) * vertices.size();
    const vk::DeviceSize indexBytes = sizeof(uint32_t) * indices.size();

    auto createHostBuffer = [&renderer](
        vk::DeviceSize bytes,
        vk::BufferUsageFlags usage,
        const char* name)
    {
        GpuBuffer buffer{};
        std::tie(buffer.buffer, buffer.memory) = renderer.CreateBuffer(
            bytes,
            usage,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            name);
        buffer.mapped = renderer.MapMemory(buffer.memory, bytes);
        if (buffer.mapped == nullptr)
            throw std::runtime_error(std::string("DecalRenderer failed to map ") + name);
        return buffer;
    };

    cubeVertices = createHostBuffer(
        vertexBytes,
        vk::BufferUsageFlagBits::eVertexBuffer,
        "DecalCubeVertices");
    cubeIndices = createHostBuffer(
        indexBytes,
        vk::BufferUsageFlagBits::eIndexBuffer,
        "DecalCubeIndices");
    std::memcpy(cubeVertices.mapped, vertices.data(), vertexBytes);
    std::memcpy(cubeIndices.mapped, indices.data(), indexBytes);
    renderer.UnmapMemory(cubeVertices.memory);
    renderer.UnmapMemory(cubeIndices.memory);
    cubeVertices.mapped = nullptr;
    cubeIndices.mapped = nullptr;
}

void DecalRenderer::CreateFrameResources(
    RendererBackendVulkan& renderer)
{
    if (!pipeline)
        throw std::runtime_error("DecalRenderer has no pipeline");

    const uint32_t imageCount = renderer.GetSwapchainImageCount();
    frameUniforms.resize(imageCount);
    instanceBuffers.resize(imageCount);
    descriptorSets.resize(imageCount);

    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer, imageCount},
        {vk::DescriptorType::eStorageBuffer, imageCount}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount);
    descriptorPool = renderer.CreateDescriptorPool(
        poolInfo,
        "DecalRendererDescriptors");

    const auto& layouts = pipeline->GetDescriptorSetLayouts();
    if (layouts.empty())
        throw std::runtime_error("DecalRenderer shader has no set 0 layout");

    vk::DescriptorSetAllocateInfo allocateInfo;
    const std::vector<vk::DescriptorSetLayout> setLayouts(
        imageCount,
        layouts.at(0));
    allocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts)
        .setDescriptorSetCount(imageCount);
    renderer.AllocateDescriptorSets(allocateInfo, descriptorSets);

    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const auto createMapped = [&renderer, imageIndex](
            vk::DeviceSize bytes,
            vk::BufferUsageFlags usage,
            const char* name)
        {
            GpuBuffer buffer{};
            std::tie(buffer.buffer, buffer.memory) = renderer.CreateBuffer(
                bytes,
                usage,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                std::string(name) + ":" + std::to_string(imageIndex));
            buffer.mapped = renderer.MapMemory(buffer.memory, bytes);
            if (buffer.mapped == nullptr)
                throw std::runtime_error(
                    std::string("DecalRenderer failed to map ") + name);
            return buffer;
        };

        frameUniforms[imageIndex] = createMapped(
            sizeof(FrameUniforms),
            vk::BufferUsageFlagBits::eUniformBuffer,
            "DecalFrameUniforms");
        instanceBuffers[imageIndex] = createMapped(
            sizeof(DecalInstance) * kMaxDecalInstances,
            vk::BufferUsageFlagBits::eStorageBuffer,
            "DecalInstances");

        const vk::DescriptorBufferInfo uniformInfo(
            frameUniforms[imageIndex].buffer,
            0,
            sizeof(FrameUniforms));
        const vk::DescriptorBufferInfo instanceInfo(
            instanceBuffers[imageIndex].buffer,
            0,
            sizeof(DecalInstance) * kMaxDecalInstances);
        vk::WriteDescriptorSet uniformWrite;
        uniformWrite
            .setDstSet(descriptorSets[imageIndex])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setBufferInfo(uniformInfo);
        vk::WriteDescriptorSet instanceWrite = uniformWrite;
        instanceWrite
            .setDstBinding(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(instanceInfo);
        renderer.UpdateDescriptorSets({uniformWrite, instanceWrite});
    }
}

void DecalRenderer::Shutdown(RendererBackendVulkan& renderer)
{
    if (!initialized &&
        static_cast<VkDescriptorPool>(descriptorPool) == VK_NULL_HANDLE)
        return;
    DestroyResources(renderer);
    initialized = false;
    backend = nullptr;
}

void DecalRenderer::InvalidatePipelines(RendererBackendVulkan& renderer)
{
    if (!initialized)
        return;
    renderer.WaitIdle();
    Shutdown(renderer);
}

void DecalRenderer::DestroyResources(RendererBackendVulkan& renderer)
{
    for (GpuBuffer& buffer : frameUniforms)
        DestroyBuffer(renderer, buffer);
    for (GpuBuffer& buffer : instanceBuffers)
        DestroyBuffer(renderer, buffer);
    frameUniforms.clear();
    instanceBuffers.clear();
    DestroyBuffer(renderer, cubeVertices);
    DestroyBuffer(renderer, cubeIndices);
    if (static_cast<VkDescriptorPool>(descriptorPool) != VK_NULL_HANDLE)
    {
        renderer.DestroyDescriptorPool(descriptorPool);
        descriptorPool = nullptr;
    }
    descriptorSets.clear();
    pipeline.reset();
}

void DecalRenderer::DestroyBuffer(
    RendererBackendVulkan& renderer,
    GpuBuffer& buffer)
{
    if (buffer.mapped != nullptr)
    {
        renderer.UnmapMemory(buffer.memory);
        buffer.mapped = nullptr;
    }
    if (static_cast<VkBuffer>(buffer.buffer) != VK_NULL_HANDLE)
    {
        renderer.DestroyBuffer(buffer.buffer, buffer.memory);
        buffer.buffer = nullptr;
        buffer.memory = nullptr;
    }
}

void DecalRenderer::RecordGraphics(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass,
    const RenderScene& renderScene)
{
    if (!initialized || !pipeline ||
        imageIndex >= descriptorSets.size() ||
        imageIndex >= frameUniforms.size() ||
        imageIndex >= instanceBuffers.size())
        return;

    std::vector<const DecalSnapshot*> visibleDecals;
    visibleDecals.reserve(renderScene.decals.size());
    for (const DecalSnapshot& decal : renderScene.decals)
    {
        if (decal.visible)
            visibleDecals.push_back(&decal);
    }
    if (visibleDecals.empty())
        return;
    std::sort(
        visibleDecals.begin(),
        visibleDecals.end(),
        [](const DecalSnapshot* left, const DecalSnapshot* right)
        {
            if (left->sortOrder != right->sortOrder)
                return left->sortOrder < right->sortOrder;
            return left->decalId < right->decalId;
        });
    if (visibleDecals.size() > kMaxDecalInstances)
        visibleDecals.resize(kMaxDecalInstances);

    FrameUniforms frame{};
    frame.viewProjection = renderScene.camera.viewProjection;
    frame.invViewProjection =
        renderScene.camera.viewProjection.inverse();
    std::memcpy(
        frameUniforms[imageIndex].mapped,
        &frame,
        sizeof(frame));

    auto* instances = static_cast<DecalInstance*>(
        instanceBuffers[imageIndex].mapped);
    for (size_t index = 0; index < visibleDecals.size(); ++index)
    {
        const DecalSnapshot& source = *visibleDecals[index];
        DecalInstance& target = instances[index];
        target.decalToWorld = source.decalToWorld;
        target.worldToDecal = source.decalToWorld.inverse();
        target.color = source.color;
        target.params = Eigen::Vector4f(
            source.opacity,
            source.edgeSoftness,
            source.noiseStrength,
            source.randomSeed);
    }

    commandBuffer.bindPipeline(
        pipeline->GetBindPoint(),
        pipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        pipeline->GetPipelineLayout(),
        0,
        descriptorSets[imageIndex],
        nullptr);
    const auto& passSets = renderPass.GetDescriptorSets();
    if (imageIndex < passSets.size() &&
        PassSetIndex < passSets[imageIndex].size())
    {
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            PassSetIndex,
            passSets[imageIndex][PassSetIndex],
            nullptr);
    }
    commandBuffer.bindVertexBuffers(
        0,
        cubeVertices.buffer,
        vk::DeviceSize{0});
    commandBuffer.bindIndexBuffer(
        cubeIndices.buffer,
        0,
        vk::IndexType::eUint32);
    commandBuffer.drawIndexed(
        kDecalCubeIndexCount,
        static_cast<uint32_t>(visibleDecals.size()),
        0,
        0,
        0);
}

} // namespace VL
