#include "brdfLutGenerator.h"
#include "../commonFunction.h"
#include "../resource/image/textureIO.h"
#include "../vulkanManager.h"
#include "computePipeline.h"
#include "pipelineFactory.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

void BrdfLutGenerator::Generate(PipelineFactory& pipelineFactory)
{
    auto& vulkanManager = VulkanManager::GetInstance();
    auto& device = vulkanManager.GetDevice();
    auto& gpuMemoryProperties = vulkanManager.GetGpuMemoryProperties();
    auto& commandPool = vulkanManager.GetCommandPool();
    auto& graphicsQueue = vulkanManager.GetGraphicQueue();

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
    auto [lutImage, lutImageMemory] = CommonFunction::CreateImage(
        device,
        lutWidth, lutHeight, 1, vk::SampleCountFlagBits::e1,
        lutFormat, lutTiling,
        lutUsage,
        gpuMemoryProperties,
        lutMemFlags,
        "BrdfLutImage");
    vk::ImageView lutImageView = CommonFunction::CreateImageView(device, lutImage, 1, lutFormat, vk::ImageAspectFlagBits::eColor, "BrdfLutImageView");

    vk::DeviceSize stagingBufferSize = readbackSize;
    vk::BufferUsageFlags stagingUsage = vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags stagingMemFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    auto [stagingBuffer, stagingBufferMemory] = CommonFunction::CreateBuffer(
        device,
        stagingBufferSize,
        stagingUsage,
        gpuMemoryProperties,
        stagingMemFlags,
        "BrdfLutReadbackBuffer");

    vk::DescriptorPoolSize descriptorPoolSize;
    descriptorPoolSize
        .setType(vk::DescriptorType::eStorageImage)
        .setDescriptorCount(1);
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setPoolSizes(descriptorPoolSize)
        .setMaxSets(1);
    vk::DescriptorPool descriptorPool = device.createDescriptorPool(descriptorPoolCreateInfo);

    vk::DescriptorSetLayout setLayout = computePipeline->GetDescriptorSetLayouts()[0];
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayout);
    vk::DescriptorSet descriptorSet = device.allocateDescriptorSets(descriptorSetAllocateInfo)[0];

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
    device.updateDescriptorSets(writeDescriptorSet, nullptr);

    vk::CommandBuffer commandBuffer = CommonFunction::BeginSingleTimeCommands(device, commandPool);

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

    CommonFunction::EndSingleTimeCommands(device, commandBuffer, graphicsQueue, commandPool);
    CommonFunction::CopyImageToBuffer(device, graphicsQueue, commandPool, lutImage, stagingBuffer, lutWidth, lutHeight, true, static_cast<vk::DeviceSize>(lutWidth) * bytesPerPixel);

    void* mapped = device.mapMemory(stagingBufferMemory, 0, readbackSize);
    HostImage cpuImage;
    cpuImage.width = lutWidth;
    cpuImage.height = lutHeight;
    cpuImage.channels = 4;
    cpuImage.format = HostImage::PixelFormat::RGBA16_FLOAT;
    cpuImage.semantic = HostImage::TextureSemantic::Lut;
    cpuImage.rowStrideBytes = static_cast<uint32_t>(lutWidth * sizeof(uint16_t) * 4);
    cpuImage.data.resize(static_cast<size_t>(readbackSize));
    std::memcpy(cpuImage.data.data(), mapped, static_cast<size_t>(readbackSize));
    device.unmapMemory(stagingBufferMemory);

    TextureIO::SaveOptions saveOptions;
    saveOptions.semantic = HostImage::TextureSemantic::Lut;
    saveOptions.format = TextureIO::FileFormat::Exr;
    TextureIO::Save((std::filesystem::path(CommonFunction::GetProjectPath()) / "resources" / "generated" / "brdf_lut.exr"), cpuImage, saveOptions);

    device.destroyDescriptorPool(descriptorPool);
    device.destroyBuffer(stagingBuffer);
    device.freeMemory(stagingBufferMemory);
    device.destroyImageView(lutImageView);
    device.destroyImage(lutImage);
    device.freeMemory(lutImageMemory);
}
