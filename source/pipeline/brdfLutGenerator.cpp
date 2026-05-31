#include "brdfLutGenerator.h"
#include "../commonFunction.h"
#include "../render/backend/rendererBackendVulkan.h"
#include "../resource/image/textureIO.h"
#include "../texture.h"
#include "computePipeline.h"
#include "pipelineFactory.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
#if !defined(NDEBUG)
    constexpr bool kEnableDebugBrdfDump = true;
#else
    constexpr bool kEnableDebugBrdfDump = false;
#endif

    vk::Sampler CreateBrdfLutSampler(VL::RendererBackendVulkan& rendererBackend)
    {
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
            .setAnisotropyEnable(VK_FALSE)
            .setMaxAnisotropy(1.0f)
            .setBorderColor(vk::BorderColor::eFloatOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(0.0f);
        return rendererBackend.CreateSampler(samplerInfo, "BrdfLutSampler");
    }
}

std::shared_ptr<Texture> BrdfLutGenerator::Generate(
    PipelineFactory& pipelineFactory,
    VL::RendererBackendVulkan& rendererBackend)
{
    auto computePipeline = pipelineFactory.CreateComputePipeline("generator/brfdLut");
    const uint32_t lutWidth = 512;
    const uint32_t lutHeight = 512;
    const uint32_t channelCount = 4;
    const vk::DeviceSize bytesPerPixel = sizeof(uint16_t) * channelCount;
    const vk::DeviceSize readbackSize = static_cast<vk::DeviceSize>(lutWidth) * lutHeight * bytesPerPixel;

    vk::Format lutFormat = vk::Format::eR16G16B16A16Sfloat;
    vk::ImageTiling lutTiling = vk::ImageTiling::eOptimal;
    vk::ImageUsageFlags lutUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    vk::MemoryPropertyFlags lutMemFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    auto [lutImage, lutImageMemory] = rendererBackend.CreateImage(
        lutWidth,
        lutHeight,
        1,
        vk::SampleCountFlagBits::e1,
        lutFormat,
        lutTiling,
        lutUsage,
        lutMemFlags,
        "BrdfLutImage");
    vk::ImageView lutImageView = rendererBackend.Create2DImageView(
        lutImage,
        1,
        lutFormat,
        vk::ImageAspectFlagBits::eColor,
        "BrdfLutImageView");

    vk::DescriptorPoolSize descriptorPoolSize;
    descriptorPoolSize
        .setType(vk::DescriptorType::eStorageImage)
        .setDescriptorCount(1);
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setPoolSizes(descriptorPoolSize)
        .setMaxSets(1);
    vk::DescriptorPool descriptorPool = rendererBackend.CreateDescriptorPool(
        descriptorPoolCreateInfo,
        "DescriptorPool: BrdfLut");

    vk::DescriptorSetLayout setLayout = computePipeline->GetDescriptorSetLayouts()[0];
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayout);
    std::vector<vk::DescriptorSet> descriptorSets(1);
    rendererBackend.AllocateDescriptorSets(descriptorSetAllocateInfo, descriptorSets);
    vk::DescriptorSet descriptorSet = descriptorSets[0];

    vk::DescriptorImageInfo imageInfo;
    imageInfo
        .setSampler(vk::Sampler())
        .setImageView(lutImageView)
        .setImageLayout(vk::ImageLayout::eGeneral);
    vk::WriteDescriptorSet writeDescriptorSet;
    writeDescriptorSet
        .setDstSet(descriptorSet)
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(imageInfo);
    rendererBackend.UpdateDescriptorSets(std::vector<vk::WriteDescriptorSet>{ writeDescriptorSet });

    vk::CommandBuffer commandBuffer = rendererBackend.BeginSingleTimeCommands();

    vk::ImageMemoryBarrier barrierToGeneral;
    barrierToGeneral
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(lutImage)
        .setSubresourceRange(vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setSrcAccessMask(vk::AccessFlagBits::eNone)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr, nullptr,
        barrierToGeneral);

    computePipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        computePipeline->GetPipelineLayout(),
        0,
        descriptorSet,
        nullptr);
    uint32_t groupX = (lutWidth + 7) / 8;
    uint32_t groupY = (lutHeight + 7) / 8;
    computePipeline->Dispatch(commandBuffer, groupX, groupY, 1);

    if constexpr (kEnableDebugBrdfDump)
    {
        vk::ImageMemoryBarrier barrierToTransferSrc;
        barrierToTransferSrc
            .setOldLayout(vk::ImageLayout::eGeneral)
            .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(lutImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrierToTransferSrc);
    }
    else
    {
        vk::ImageMemoryBarrier barrierToSample;
        barrierToSample
            .setOldLayout(vk::ImageLayout::eGeneral)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(lutImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrierToSample);
    }

    rendererBackend.EndSingleTimeCommands(commandBuffer);

    if constexpr (kEnableDebugBrdfDump)
    {
        vk::Buffer stagingBuffer;
        vk::DeviceMemory stagingBufferMemory;
        vk::DeviceSize stagingBufferSize = readbackSize;
        vk::BufferUsageFlags stagingUsage = vk::BufferUsageFlagBits::eTransferDst;
        vk::MemoryPropertyFlags stagingMemoryFlags =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        std::tie(stagingBuffer, stagingBufferMemory) = rendererBackend.CreateBuffer(
            stagingBufferSize,
            stagingUsage,
            stagingMemoryFlags,
            "BrdfLutReadbackBuffer");

        rendererBackend.CopyImageToBuffer(
            lutImage,
            stagingBuffer,
            lutWidth,
            lutHeight,
            true,
            static_cast<vk::DeviceSize>(lutWidth) * bytesPerPixel);

        void* mapped = rendererBackend.MapMemory(stagingBufferMemory, readbackSize);
        HostImage cpuImage;
        cpuImage.width = lutWidth;
        cpuImage.height = lutHeight;
        cpuImage.channels = 4;
        cpuImage.format = HostImage::PixelFormat::RGBA16_FLOAT;
        cpuImage.semantic = HostImage::TextureSemantic::Lut;
        cpuImage.rowStrideBytes = static_cast<uint32_t>(lutWidth * sizeof(uint16_t) * 4);
        cpuImage.data.resize(static_cast<size_t>(readbackSize));
        std::memcpy(cpuImage.data.data(), mapped, static_cast<size_t>(readbackSize));
        rendererBackend.UnmapMemory(stagingBufferMemory);

        TextureIO::SaveOptions saveOptions;
        saveOptions.semantic = HostImage::TextureSemantic::Lut;
        saveOptions.format = TextureIO::FileFormat::Exr;
        TextureIO::Save((std::filesystem::path(CommonFunction::GetResourcePath()) / "generated" / "brdf_lut.exr"), cpuImage, saveOptions);

        rendererBackend.DestroyBuffer(stagingBuffer, stagingBufferMemory);

        vk::CommandBuffer layoutCommandBuffer = rendererBackend.BeginSingleTimeCommands();
        vk::ImageMemoryBarrier barrierToSample;
        barrierToSample
            .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(lutImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        layoutCommandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrierToSample);
        rendererBackend.EndSingleTimeCommands(layoutCommandBuffer);
    }

    rendererBackend.DestroyDescriptorPool(descriptorPool);
    vk::Sampler lutSampler = CreateBrdfLutSampler(rendererBackend);
    return std::make_shared<Texture>(
        rendererBackend,
        lutImage,
        lutImageMemory,
        lutImageView,
        lutSampler,
        1,
        lutFormat);
}
