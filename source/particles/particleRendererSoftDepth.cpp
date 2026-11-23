#include "particles/particleRendererInternal.h"

namespace VL
{

namespace
{
// 稳定的 64 位混合：同一组 handle 在任意帧序下都得到同一签名。
uint64_t MixSoftDepthSignature(uint64_t value, uint64_t entry, uint64_t weight)
{
    return (value ^ (entry * weight)) * 1099511628211ull;
}
}

void ParticleRenderer::DestroySoftDepthResources()
{
    if (backend == nullptr)
    {
        return;
    }
    if (softDepthResources.descriptorPool)
    {
        backend->DestroyDescriptorPool(
            softDepthResources.descriptorPool);
        softDepthResources.descriptorPool = nullptr;
    }
    for (GpuBuffer& uniform : softDepthResources.uniforms)
    {
        DestroyBuffer(*backend, uniform);
    }
    softDepthResources.uniforms.clear();
    softDepthResources.sets.clear();
    softDepthResources.signature = 0;
}

void ParticleRenderer::RecordSoftDepth(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass,
    const RenderGraph& graph,
    const RenderScene& renderScene)
{
    if (!initialized || !softDepthPipeline ||
        renderPass.width == 0 || renderPass.height == 0)
    {
        return;
    }
    const auto& resources = graph.GetResourcesResolve();
    const auto depthIt = resources.find("sceneDepth");
    const auto softIt = resources.find("particleSoftDepth");
    if (depthIt == resources.end() || softIt == resources.end() ||
        depthIt->second.empty() || softIt->second.empty())
    {
        return;
    }
    const auto& depthList = depthIt->second;
    const auto& softList = softIt->second;
    const auto& depth = depthList.at(
        std::min<size_t>(imageIndex, depthList.size() - 1));
    const auto& soft = softList.at(
        std::min<size_t>(imageIndex, softList.size() - 1));
    if (!depth.imageView || !soft.imageView)
    {
        return;
    }

    const uint32_t imageCount = backend->GetSwapchainImageCount();
    // 资源按 swapchain image 分配，签名必须覆盖所有 image；只算当前 image 会让
    // imageIndex 交替时每帧误判资源变化，进而每帧销毁/重建描述符池，
    // 而上一帧的 command buffer 可能仍在引用这些 descriptor set。
    uint64_t signature = static_cast<uint64_t>(imageCount) * 97ull;
    for (uint32_t image = 0; image < imageCount; ++image)
    {
        const auto& imageDepth = depthList.at(
            std::min<size_t>(image, depthList.size() - 1));
        const auto& imageSoft = softList.at(
            std::min<size_t>(image, softList.size() - 1));
        signature = MixSoftDepthSignature(signature,
            reinterpret_cast<uintptr_t>(
                static_cast<VkImageView>(imageDepth.GetShaderDescriptorView())),
            1315423911ull);
        signature = MixSoftDepthSignature(signature,
            reinterpret_cast<uintptr_t>(
                static_cast<VkSampler>(imageDepth.sampler)),
            2654435761ull);
        signature = MixSoftDepthSignature(signature,
            reinterpret_cast<uintptr_t>(
                static_cast<VkImageView>(imageSoft.imageView)),
            40503ull);
        signature = MixSoftDepthSignature(signature, image, 31ull);
    }
    if (softDepthResources.signature != signature ||
        softDepthResources.sets.size() != imageCount)
    {
        // 真正的 view/尺寸变化很少见；旧 pool 的 set 可能仍被在途帧引用。
        if (softDepthResources.descriptorPool)
            backend->WaitIdle();
        DestroySoftDepthResources();

        std::vector<vk::DescriptorPoolSize> poolSizes{
            {vk::DescriptorType::eCombinedImageSampler, imageCount},
            {vk::DescriptorType::eStorageImage, imageCount},
            {vk::DescriptorType::eUniformBuffer, imageCount}};
        vk::DescriptorPoolCreateInfo poolInfo;
        poolInfo
            .setPoolSizes(poolSizes)
            .setMaxSets(imageCount);
        softDepthResources.descriptorPool =
            backend->CreateDescriptorPool(
                poolInfo, "ParticleSoftDepthDescriptors");
        if (!softDepthResources.descriptorPool)
        {
            return;
        }

        const std::vector<vk::DescriptorSetLayout> layouts(
            imageCount,
            softDepthPipeline->GetDescriptorSetLayouts().at(0));
        vk::DescriptorSetAllocateInfo allocate;
        allocate
            .setDescriptorPool(
                softDepthResources.descriptorPool)
            .setSetLayouts(layouts);
        softDepthResources.sets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, softDepthResources.sets);
        softDepthResources.uniforms.resize(imageCount);

        struct SoftDepthParams
        {
            Eigen::Matrix4f invProjection;
            Eigen::Vector4f params;
        };
        static_assert(
            sizeof(SoftDepthParams) == 80,
            "Soft depth UBO must match particleSoftDepth.comp");
        for (uint32_t image = 0; image < imageCount; ++image)
        {
            const auto allocation = backend->CreateBuffer(
                sizeof(SoftDepthParams),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "ParticleSoftDepthParams:" +
                    std::to_string(image));
            GpuBuffer uniform;
            uniform.buffer = allocation.first;
            uniform.memory = allocation.second;
            uniform.mapped = backend->MapMemory(
                uniform.memory, sizeof(SoftDepthParams));
            softDepthResources.uniforms[image] = uniform;
            if (uniform.mapped == nullptr)
            {
                DestroySoftDepthResources();
                return;
            }

            const auto& imageDepth = depthList.at(
                std::min<size_t>(image, depthList.size() - 1));
            const auto& imageSoft = softList.at(
                std::min<size_t>(image, softList.size() - 1));
            vk::DescriptorImageInfo sceneDepthInfo(
                imageDepth.sampler,
                imageDepth.GetShaderDescriptorView(),
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
            vk::DescriptorImageInfo softDepthInfo(
                vk::Sampler{},
                imageSoft.imageView,
                vk::ImageLayout::eGeneral);
            vk::DescriptorBufferInfo paramsInfo(
                uniform.buffer, 0, sizeof(SoftDepthParams));
            std::array<vk::WriteDescriptorSet, 3> writes{};
            writes[0]
                .setDstSet(softDepthResources.sets.at(image))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(sceneDepthInfo);
            writes[1]
                .setDstSet(softDepthResources.sets.at(image))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eStorageImage)
                .setImageInfo(softDepthInfo);
            writes[2]
                .setDstSet(softDepthResources.sets.at(image))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramsInfo);
            backend->UpdateDescriptorSets(
                {writes[0], writes[1], writes[2]});
        }
        softDepthResources.signature = signature;
        softDepthResources.width = renderPass.width;
        softDepthResources.height = renderPass.height;
    }
    if (imageIndex >= softDepthResources.sets.size() ||
        imageIndex >= softDepthResources.uniforms.size())
    {
        return;
    }
    struct SoftDepthParams
    {
        Eigen::Matrix4f invProjection;
        Eigen::Vector4f params;
    };
    SoftDepthParams params{};
    params.invProjection =
        renderScene.camera.projection.inverse();
    params.params = Eigen::Vector4f(50.0f, 0.0f, 0.0f, 0.0f);
    std::memcpy(
        softDepthResources.uniforms.at(imageIndex).mapped,
        &params,
        sizeof(params));

    commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute,
        softDepthPipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        softDepthPipeline->GetPipelineLayout(),
        0,
        softDepthResources.sets.at(imageIndex),
        nullptr);
    constexpr uint32_t groupSize = 8;
    commandBuffer.dispatch(
        (renderPass.width + groupSize - 1) / groupSize,
        (renderPass.height + groupSize - 1) / groupSize,
        1);
}

} // namespace VL
