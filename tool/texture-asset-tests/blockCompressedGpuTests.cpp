#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "commonFunction.h"
#include "resource/image/hostImageVulkanPlan.h"

namespace
{
// 最小 headless Vulkan 环境：只创建实例/设备/队列/命令池，任何一步失败都
// 返回 false 让用例 GTEST_SKIP，而不是在没有 GPU 的机器上判失败。
struct TestVulkanContext
{
    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;
    uint32_t queueFamilyIndex = 0;

    ~TestVulkanContext()
    {
        Destroy();
    }

    bool Create()
    {
        try
        {
            vk::ApplicationInfo applicationInfo;
            applicationInfo
                .setPApplicationName("texture_asset_tests")
                .setApiVersion(VK_API_VERSION_1_1);
            vk::InstanceCreateInfo instanceInfo;
            instanceInfo.setPApplicationInfo(&applicationInfo);
            instance = vk::createInstance(instanceInfo);

            for (const vk::PhysicalDevice candidate : instance.enumeratePhysicalDevices())
            {
                const std::vector<vk::QueueFamilyProperties> families =
                    candidate.getQueueFamilyProperties();
                for (uint32_t index = 0; index < static_cast<uint32_t>(families.size()); ++index)
                {
                    const vk::QueueFlags flags = families[index].queueFlags;
                    if ((flags & vk::QueueFlagBits::eGraphics) && (flags & vk::QueueFlagBits::eTransfer))
                    {
                        physicalDevice = candidate;
                        queueFamilyIndex = index;
                        break;
                    }
                }
                if (physicalDevice)
                    break;
            }
            if (!physicalDevice)
                return false;

            const float priority = 1.0f;
            vk::DeviceQueueCreateInfo queueInfo;
            queueInfo
                .setQueueFamilyIndex(queueFamilyIndex)
                .setQueueCount(1)
                .setPQueuePriorities(&priority);
            vk::DeviceCreateInfo deviceInfo;
            deviceInfo.setQueueCreateInfos(queueInfo);
            device = physicalDevice.createDevice(deviceInfo);
            if (!device)
                return false;
            queue = device.getQueue(queueFamilyIndex, 0);

            vk::CommandPoolCreateInfo poolInfo;
            poolInfo.setQueueFamilyIndex(queueFamilyIndex);
            commandPool = device.createCommandPool(poolInfo);
            return true;
        }
        catch (const std::exception&)
        {
            Destroy();
            return false;
        }
    }

    void Destroy()
    {
        if (device)
        {
            if (commandPool)
            {
                device.destroyCommandPool(commandPool);
                commandPool = nullptr;
            }
            device.destroy();
            device = nullptr;
        }
        if (instance)
        {
            instance.destroy();
            instance = nullptr;
        }
        physicalDevice = nullptr;
        queue = nullptr;
    }
};

HostImage MakeBc7MipChain()
{
    // 16×16 + 3 级 mip（16/8/4），所有级别都是 4 的倍数；2 个数组层。
    HostImage image;
    image.width = 16;
    image.height = 16;
    image.mipLevels = 3;
    image.arrayLayers = 2;
    image.format = HostImage::PixelFormat::BC7_UNORM;
    image.channels = 4;
    image.data.resize(image.GetTotalBytes());
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
    {
        for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
        {
            const size_t offset = image.GetMipOffset(mip, layer);
            std::fill(
                image.data.begin() + static_cast<std::ptrdiff_t>(offset),
                image.data.begin() + static_cast<std::ptrdiff_t>(offset + image.GetMipLayerBytes(mip)),
                static_cast<uint8_t>(0x20 * layer + mip + 1));
        }
    }
    return image;
}

uint32_t FindMemoryType(
    const vk::PhysicalDeviceMemoryProperties& properties,
    uint32_t typeBits,
    vk::MemoryPropertyFlags required)
{
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((typeBits & (1u << index)) == 0)
            continue;
        const vk::MemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) == required)
            return index;
    }
    return UINT32_MAX;
}
}

TEST(CompressedTextureGpu, UploadsEveryBc7MipAndLayerByteExact)
{
    TestVulkanContext context;
    if (!context.Create())
        GTEST_SKIP() << "No Vulkan device with graphics+transfer queue";

    const vk::Format format = HostImageVulkan::ToVulkanFormat(HostImage::PixelFormat::BC7_UNORM);
    const vk::FormatProperties properties = context.physicalDevice.getFormatProperties(format);
    const vk::FormatFeatureFlags required =
        vk::FormatFeatureFlagBits::eTransferDst | vk::FormatFeatureFlagBits::eTransferSrc;
    if ((properties.optimalTilingFeatures & required) != required)
        GTEST_SKIP() << "Device does not support BC7 transfer copies";

    const HostImage image = MakeBc7MipChain();
    const vk::DeviceSize totalBytes = static_cast<vk::DeviceSize>(image.data.size());
    const vk::PhysicalDeviceMemoryProperties memoryProperties =
        context.physicalDevice.getMemoryProperties();
    const uint32_t hostVisibleIndex = FindMemoryType(
        memoryProperties,
        0xFFFFFFFFu,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (hostVisibleIndex == UINT32_MAX)
        GTEST_SKIP() << "No host-visible coherent memory type";

    // 显式创建并手工销毁，避免 ASSERT 提前返回时依赖析构顺序。
    vk::BufferCreateInfo stagingInfo;
    stagingInfo
        .setSize(totalBytes)
        .setUsage(vk::BufferUsageFlagBits::eTransferSrc)
        .setSharingMode(vk::SharingMode::eExclusive);
    const vk::Buffer stagingBuffer = context.device.createBuffer(stagingInfo);
    const vk::MemoryRequirements stagingRequirements =
        context.device.getBufferMemoryRequirements(stagingBuffer);
    vk::MemoryAllocateInfo stagingAllocate;
    stagingAllocate
        .setAllocationSize(stagingRequirements.size)
        .setMemoryTypeIndex(hostVisibleIndex);

    const vk::DeviceMemory stagingMemory = context.device.allocateMemory(stagingAllocate);
    context.device.bindBufferMemory(stagingBuffer, stagingMemory, 0);
    void* mapped = context.device.mapMemory(stagingMemory, 0, totalBytes);
    std::memcpy(mapped, image.data.data(), static_cast<size_t>(totalBytes));
    context.device.unmapMemory(stagingMemory);

    vk::BufferCreateInfo readbackInfo;
    readbackInfo
        .setSize(totalBytes)
        .setUsage(vk::BufferUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive);
    const vk::Buffer readbackBuffer = context.device.createBuffer(readbackInfo);
    const vk::MemoryRequirements readbackRequirements =
        context.device.getBufferMemoryRequirements(readbackBuffer);
    vk::MemoryAllocateInfo readbackAllocate;
    readbackAllocate
        .setAllocationSize(readbackRequirements.size)
        .setMemoryTypeIndex(hostVisibleIndex);
    const vk::DeviceMemory readbackMemory = context.device.allocateMemory(readbackAllocate);
    context.device.bindBufferMemory(readbackBuffer, readbackMemory, 0);

    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ image.width, image.height, 1 })
        .setMipLevels(image.mipLevels)
        .setArrayLayers(image.arrayLayers)
        .setFormat(format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    const vk::Image deviceImage = context.device.createImage(imageInfo);
    const vk::MemoryRequirements imageRequirements =
        context.device.getImageMemoryRequirements(deviceImage);
    const uint32_t deviceLocalIndex = FindMemoryType(
        memoryProperties,
        imageRequirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_NE(deviceLocalIndex, UINT32_MAX);
    vk::MemoryAllocateInfo imageAllocate;
    imageAllocate
        .setAllocationSize(imageRequirements.size)
        .setMemoryTypeIndex(deviceLocalIndex);
    const vk::DeviceMemory imageMemory = context.device.allocateMemory(imageAllocate);
    context.device.bindImageMemory(deviceImage, imageMemory, 0);

    const std::vector<vk::BufferImageCopy> regions =
        HostImageVulkan::BuildUploadRegions(image);
    ASSERT_EQ(regions.size(), image.mipLevels * image.arrayLayers);

    vk::CommandBuffer command = CommonFunction::BeginSingleTimeCommands(
        context.device, context.commandPool);
    CommonFunction::RecordTransitionImageLayout(
        command,
        deviceImage,
        image.mipLevels,
        format,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal,
        0,
        image.arrayLayers);
    CommonFunction::RecordCopyBufferToImageRegions(
        command, stagingBuffer, deviceImage, regions);
    CommonFunction::RecordTransitionImageLayout(
        command,
        deviceImage,
        image.mipLevels,
        format,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        0,
        image.arrayLayers);
    command.copyImageToBuffer(
        deviceImage,
        vk::ImageLayout::eTransferSrcOptimal,
        readbackBuffer,
        static_cast<uint32_t>(regions.size()),
        regions.data());
    CommonFunction::EndSingleTimeCommands(
        context.device, command, context.queue, context.commandPool);

    const void* readback = context.device.mapMemory(readbackMemory, 0, totalBytes);
    EXPECT_EQ(
        std::memcmp(readback, image.data.data(), static_cast<size_t>(totalBytes)),
        0) << "BC7 mip/array upload did not round-trip byte-exactly";
    context.device.unmapMemory(readbackMemory);

    context.device.destroyImage(deviceImage);
    context.device.freeMemory(imageMemory);
    context.device.destroyBuffer(stagingBuffer);
    context.device.freeMemory(stagingMemory);
    context.device.destroyBuffer(readbackBuffer);
    context.device.freeMemory(readbackMemory);
}
