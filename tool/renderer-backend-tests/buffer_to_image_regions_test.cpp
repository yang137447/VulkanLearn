#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "commonFunction.h"

namespace
{
// 无窗口最小 Vulkan 上下文：用例只关心 transfer 路径，因此只需要一个同时支持
// graphics/transfer 的队列族。任何一步失败都返回 false，让用例 Skip 而不是在
// 没有 Vulkan 驱动的机器上报假失败。
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
                .setPApplicationName("vl_renderer_backend_tests")
                .setApiVersion(VK_API_VERSION_1_1);
            vk::InstanceCreateInfo instanceInfo;
            instanceInfo.setPApplicationInfo(&applicationInfo);
            instance = vk::createInstance(instanceInfo);

            const std::vector<vk::PhysicalDevice> devices =
                instance.enumeratePhysicalDevices();
            for (const vk::PhysicalDevice candidate : devices)
            {
                const std::vector<vk::QueueFamilyProperties> families =
                    candidate.getQueueFamilyProperties();
                for (uint32_t index = 0;
                     index < static_cast<uint32_t>(families.size());
                     ++index)
                {
                    const vk::QueueFlags flags = families[index].queueFlags;
                    if ((flags & vk::QueueFlagBits::eGraphics) &&
                        (flags & vk::QueueFlagBits::eTransfer))
                    {
                        physicalDevice = candidate;
                        queueFamilyIndex = index;
                        break;
                    }
                }
                if (physicalDevice)
                {
                    break;
                }
            }
            if (!physicalDevice)
            {
                return false;
            }

            const float queuePriority = 1.0f;
            vk::DeviceQueueCreateInfo queueInfo;
            queueInfo
                .setQueueFamilyIndex(queueFamilyIndex)
                .setQueueCount(1)
                .setPQueuePriorities(&queuePriority);
            vk::DeviceCreateInfo deviceInfo;
            deviceInfo.setQueueCreateInfos(queueInfo);
            device = physicalDevice.createDevice(deviceInfo);
            if (!device)
            {
                return false;
            }
            queue = device.getQueue(queueFamilyIndex, 0);

            vk::CommandPoolCreateInfo poolInfo;
            poolInfo
                .setQueueFamilyIndex(queueFamilyIndex)
                .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
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

// 测试资源用 RAII 持有，ASSERT_* 提前返回时也不会泄漏 device memory。
struct TestImage
{
    vk::Device device;
    vk::Image image;
    vk::DeviceMemory memory;

    TestImage() = default;
    TestImage(const TestImage&) = delete;
    TestImage& operator=(const TestImage&) = delete;

    ~TestImage()
    {
        if (!device)
        {
            return;
        }
        if (image)
        {
            device.destroyImage(image);
        }
        if (memory)
        {
            device.freeMemory(memory);
        }
    }
};

struct TestBuffer
{
    vk::Device device;
    vk::Buffer buffer;
    vk::DeviceMemory memory;

    TestBuffer() = default;
    TestBuffer(const TestBuffer&) = delete;
    TestBuffer& operator=(const TestBuffer&) = delete;

    ~TestBuffer()
    {
        if (!device)
        {
            return;
        }
        if (buffer)
        {
            device.destroyBuffer(buffer);
        }
        if (memory)
        {
            device.freeMemory(memory);
        }
    }
};

uint32_t FindMemoryTypeIndex(
    const vk::PhysicalDeviceMemoryProperties& memoryProperties,
    uint32_t typeBits,
    vk::MemoryPropertyFlags required)
{
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        const bool allowed = (typeBits & (1u << index)) != 0;
        const vk::MemoryPropertyFlags properties =
            memoryProperties.memoryTypes[index].propertyFlags;
        if (allowed && (properties & required) == required)
        {
            return index;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}

void CreateColorImage(
    const TestVulkanContext& context,
    uint32_t width,
    uint32_t height,
    TestImage& result)
{
    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setExtent(vk::Extent3D{ width, height, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    result.device = context.device;
    result.image = context.device.createImage(imageInfo);

    const vk::MemoryRequirements requirements =
        context.device.getImageMemoryRequirements(result.image);
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(requirements.size)
        .setMemoryTypeIndex(FindMemoryTypeIndex(
            context.physicalDevice.getMemoryProperties(),
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eDeviceLocal));
    result.memory = context.device.allocateMemory(allocateInfo);
    context.device.bindImageMemory(result.image, result.memory, 0);
}

void CreateHostBuffer(
    const TestVulkanContext& context,
    vk::DeviceSize size,
    TestBuffer& result)
{
    vk::BufferCreateInfo bufferInfo;
    bufferInfo
        .setSize(size)
        .setUsage(vk::BufferUsageFlagBits::eTransferSrc |
                  vk::BufferUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive);

    result.device = context.device;
    result.buffer = context.device.createBuffer(bufferInfo);

    const vk::MemoryRequirements requirements =
        context.device.getBufferMemoryRequirements(result.buffer);
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(requirements.size)
        .setMemoryTypeIndex(FindMemoryTypeIndex(
            context.physicalDevice.getMemoryProperties(),
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
    result.memory = context.device.allocateMemory(allocateInfo);
    context.device.bindBufferMemory(result.buffer, result.memory, 0);
}

void TransitionImageLayout(
    const TestVulkanContext& context,
    vk::Image image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::AccessFlags sourceAccess,
    vk::AccessFlags destinationAccess,
    vk::PipelineStageFlags sourceStage,
    vk::PipelineStageFlags destinationStage)
{
    vk::Device device = context.device;
    vk::Queue queue = context.queue;
    vk::CommandPool commandPool = context.commandPool;
    vk::Image target = image;

    vk::CommandBuffer commandBuffer =
        CommonFunction::BeginSingleTimeCommands(device, commandPool);

    vk::ImageMemoryBarrier barrier;
    barrier
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess)
        .setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(target)
        .setSubresourceRange(
            vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1));
    commandBuffer.pipelineBarrier(
        sourceStage,
        destinationStage,
        vk::DependencyFlags(),
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);

    CommonFunction::EndSingleTimeCommands(device, commandBuffer, queue, commandPool);
}

void ClearImage(
    const TestVulkanContext& context,
    vk::Image image,
    vk::ClearColorValue color)
{
    vk::Device device = context.device;
    vk::Queue queue = context.queue;
    vk::CommandPool commandPool = context.commandPool;
    vk::Image target = image;

    vk::CommandBuffer commandBuffer =
        CommonFunction::BeginSingleTimeCommands(device, commandPool);
    commandBuffer.clearColorImage(
        target,
        vk::ImageLayout::eTransferDstOptimal,
        color,
        vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(0)
            .setLayerCount(1));
    CommonFunction::EndSingleTimeCommands(device, commandBuffer, queue, commandPool);
}
} // namespace

// 校验必须在任何 Vulkan 调用之前完成：空 region 列表即使拿到空 handle 也必须抛错，
// 不能先 Begin command buffer 再失败，否则会漏掉未提交的命令缓冲。
TEST(BufferToImageRegions, RejectsEmptyRegionList)
{
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;
    vk::Buffer buffer;
    vk::Image image;

    EXPECT_THROW(
        CommonFunction::CopyBufferToImageRegions(
            device, queue, commandPool, buffer, image, {}),
        std::invalid_argument);
}

TEST(BufferToImageRegions, RejectsZeroExtentRegion)
{
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;
    vk::Buffer buffer;
    vk::Image image;

    vk::BufferImageCopy region;
    region
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{ 0, 0, 0 })
        .setImageExtent(vk::Extent3D{ 0, 2, 1 });

    EXPECT_THROW(
        CommonFunction::CopyBufferToImageRegions(
            device, queue, commandPool, buffer, image, { region }),
        std::invalid_argument);
}

// 两个 region 分两次写入同一张图的不同位置，用来证明批量 API 在一次 submit 内
// 完成了全部 copy：两个 tile 都生效，且未被覆盖的像素保持 clear 值。
TEST(BufferToImageRegions, CopiesEveryRegionInOneSubmit)
{
    TestVulkanContext context;
    if (!context.Create())
    {
        GTEST_SKIP() << "No Vulkan device with graphics+transfer queue available";
    }

    constexpr uint32_t ImageSize = 4;
    constexpr uint32_t TileSize = 2;
    constexpr vk::DeviceSize TileBytes =
        static_cast<vk::DeviceSize>(TileSize) * TileSize * 4;

    TestImage image;
    CreateColorImage(context, ImageSize, ImageSize, image);
    TestBuffer staging;
    CreateHostBuffer(context, TileBytes * 2, staging);
    TestBuffer readback;
    CreateHostBuffer(
        context,
        static_cast<vk::DeviceSize>(ImageSize) * ImageSize * 4,
        readback);

    // 两个 tile 在 staging buffer 里首尾相接，靠 BufferImageCopy::bufferOffset 区分。
    void* mapped = context.device.mapMemory(staging.memory, 0, TileBytes * 2);
    std::memset(mapped, 0, static_cast<size_t>(TileBytes * 2));
    std::memset(mapped, 0xFF, static_cast<size_t>(TileBytes)); // tile 0: white
    auto* secondTile =
        static_cast<uint8_t*>(mapped) + static_cast<size_t>(TileBytes);
    for (uint32_t pixel = 0; pixel < TileSize * TileSize; ++pixel)
    {
        uint8_t* rgba = secondTile + pixel * 4;
        rgba[0] = 0;   // tile 1: R=0, G=255
        rgba[1] = 255;
        rgba[2] = 0;
        rgba[3] = 255;
    }
    context.device.unmapMemory(staging.memory);

    TransitionImageLayout(
        context,
        image.image,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal,
        vk::AccessFlags(),
        vk::AccessFlagBits::eTransferWrite,
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer);

    // 先清成蓝色，未覆盖区域应保留该值。
    vk::ClearColorValue clearColor;
    clearColor.setFloat32({ 0.0f, 0.0f, 1.0f, 1.0f });
    ClearImage(context, image.image, clearColor);

    std::vector<vk::BufferImageCopy> regions(2);
    regions[0]
        .setBufferOffset(0)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{ 0, 0, 0 })
        .setImageExtent(vk::Extent3D{ TileSize, TileSize, 1 });
    regions[1]
        .setBufferOffset(TileBytes)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{
            static_cast<int32_t>(ImageSize - TileSize),
            static_cast<int32_t>(ImageSize - TileSize),
            0 })
        .setImageExtent(vk::Extent3D{ TileSize, TileSize, 1 });

    vk::Device device = context.device;
    vk::Queue queue = context.queue;
    vk::CommandPool commandPool = context.commandPool;
    vk::Buffer stagingBuffer = staging.buffer;
    vk::Image targetImage = image.image;
    CommonFunction::CopyBufferToImageRegions(
        device, queue, commandPool, stagingBuffer, targetImage, regions);

    TransitionImageLayout(
        context,
        image.image,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eTransferRead,
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer);

    vk::Image sourceImage = image.image;
    vk::Buffer readbackBuffer = readback.buffer;
    CommonFunction::CopyImageToBuffer(
        device,
        queue,
        commandPool,
        sourceImage,
        readbackBuffer,
        ImageSize,
        ImageSize);

    const auto* pixels = static_cast<const uint8_t*>(
        context.device.mapMemory(
            readback.memory,
            0,
            static_cast<vk::DeviceSize>(ImageSize) * ImageSize * 4));
    const auto pixelAt = [&](uint32_t x, uint32_t y)
    {
        return pixels + (static_cast<size_t>(y) * ImageSize + x) * 4;
    };

    // tile 0 (0,0)：白色。
    EXPECT_EQ(pixelAt(0, 0)[0], 255);
    EXPECT_EQ(pixelAt(1, 1)[1], 255);
    // tile 1 (2,2)：绿色。
    EXPECT_EQ(pixelAt(2, 2)[0], 0);
    EXPECT_EQ(pixelAt(3, 3)[1], 255);
    EXPECT_EQ(pixelAt(3, 3)[2], 0);
    // 未覆盖的 (3,0) 应仍是 clear 的蓝色。
    EXPECT_EQ(pixelAt(3, 0)[0], 0);
    EXPECT_EQ(pixelAt(3, 0)[2], 255);

    context.device.unmapMemory(readback.memory);
}

// record-only 版本的校验也必须在任何 Vulkan 调用之前完成；空 handle 只用于
// 证明函数先抛 invalid_argument，而不是先解引用 command buffer。
TEST(BufferToImageRegions, RecordOnlyRejectsEmptyRegionList)
{
    vk::CommandBuffer commandBuffer;
    vk::Buffer buffer;
    vk::Image image;

    EXPECT_THROW(
        CommonFunction::RecordCopyBufferToImageRegions(
            commandBuffer, buffer, image, {}),
        std::invalid_argument);
}

TEST(BufferToImageRegions, RecordOnlyRejectsZeroArrayLayers)
{
    vk::CommandBuffer commandBuffer;
    vk::Image image;

    EXPECT_THROW(
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            image,
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            0,
            0),
        std::invalid_argument);
}

// 三种 record-only API 在同一个 command buffer 内完成“布局转换 + 两段 copy +
// 回读”，证明它们只录制、不各自 submit，且顺序与 offset 正确。
TEST(BufferToImageRegions, RecordApisShareOneCommandBuffer)
{
    TestVulkanContext context;
    if (!context.Create())
    {
        GTEST_SKIP() << "No Vulkan device with graphics+transfer queue available";
    }

    constexpr uint32_t ImageSize = 4;
    constexpr uint32_t TileSize = 2;
    constexpr vk::DeviceSize TileBytes =
        static_cast<vk::DeviceSize>(TileSize) * TileSize * 4;

    TestImage image;
    CreateColorImage(context, ImageSize, ImageSize, image);
    TestBuffer staging;
    CreateHostBuffer(context, TileBytes * 2, staging);
    TestBuffer readback;
    CreateHostBuffer(
        context,
        static_cast<vk::DeviceSize>(ImageSize) * ImageSize * 4,
        readback);

    void* mapped = context.device.mapMemory(staging.memory, 0, TileBytes * 2);
    std::memset(mapped, 0xFF, static_cast<size_t>(TileBytes));
    auto* secondTile = static_cast<uint8_t*>(mapped) + static_cast<size_t>(TileBytes);
    for (uint32_t pixel = 0; pixel < TileSize * TileSize; ++pixel)
    {
        uint8_t* rgba = secondTile + pixel * 4;
        rgba[0] = 0;
        rgba[1] = 255;
        rgba[2] = 0;
        rgba[3] = 255;
    }
    context.device.unmapMemory(staging.memory);

    std::vector<vk::BufferImageCopy> regions(2);
    regions[0]
        .setBufferOffset(0)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{ 0, 0, 0 })
        .setImageExtent(vk::Extent3D{ TileSize, TileSize, 1 });
    regions[1]
        .setBufferOffset(TileBytes)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(0)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{ 2, 2, 0 })
        .setImageExtent(vk::Extent3D{ TileSize, TileSize, 1 });

    vk::Device device = context.device;
    vk::Queue queue = context.queue;
    vk::CommandPool commandPool = context.commandPool;
    vk::CommandBuffer commandBuffer =
        CommonFunction::BeginSingleTimeCommands(device, commandPool);

    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal);
    // 先清蓝，未覆盖像素应保留该值。
    vk::ClearColorValue clearColor;
    clearColor.setFloat32({ 0.0f, 0.0f, 1.0f, 1.0f });
    commandBuffer.clearColorImage(
        image.image,
        vk::ImageLayout::eTransferDstOptimal,
        clearColor,
        vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(0)
            .setLayerCount(1));
    CommonFunction::RecordCopyBufferToImageRegions(
        commandBuffer,
        staging.buffer,
        image.image,
        regions);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal);
    CommonFunction::RecordCopyImageToBuffer(
        commandBuffer,
        image.image,
        readback.buffer,
        ImageSize,
        ImageSize);

    CommonFunction::EndSingleTimeCommands(device, commandBuffer, queue, commandPool);

    const auto* pixels = static_cast<const uint8_t*>(
        context.device.mapMemory(
            readback.memory,
            0,
            static_cast<vk::DeviceSize>(ImageSize) * ImageSize * 4));
    const auto pixelAt = [&](uint32_t x, uint32_t y)
    {
        return pixels + (static_cast<size_t>(y) * ImageSize + x) * 4;
    };

    EXPECT_EQ(pixelAt(0, 0)[0], 255);
    EXPECT_EQ(pixelAt(1, 1)[2], 255);
    EXPECT_EQ(pixelAt(2, 2)[1], 255);
    EXPECT_EQ(pixelAt(3, 3)[0], 0);
    EXPECT_EQ(pixelAt(3, 0)[2], 255);

    context.device.unmapMemory(readback.memory);
}
