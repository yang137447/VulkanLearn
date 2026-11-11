#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "commonFunction.h"
#include "terrain/terrainFeedback.h"

namespace
{

// 最小 headless Vulkan 上下文：只验证 feedback 的 attachment -> transferSrc
// 往返与 image -> buffer 回读；没有设备时 Skip。
struct FeedbackTestContext
{
    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;
    uint32_t queueFamilyIndex = 0;

    ~FeedbackTestContext()
    {
        if (device)
        {
            if (commandPool)
                device.destroyCommandPool(commandPool);
            device.destroy();
        }
        if (instance)
            instance.destroy();
    }

    bool Create()
    {
        try
        {
            vk::ApplicationInfo applicationInfo;
            applicationInfo
                .setPApplicationName("vl_renderer_backend_tests")
                .setApiVersion(VK_API_VERSION_1_1);
            instance = vk::createInstance(
                vk::InstanceCreateInfo().setPApplicationInfo(
                    &applicationInfo));

            for (const vk::PhysicalDevice candidate :
                 instance.enumeratePhysicalDevices())
            {
                const std::vector<vk::QueueFamilyProperties> families =
                    candidate.getQueueFamilyProperties();
                for (uint32_t index = 0;
                     index < static_cast<uint32_t>(families.size());
                     ++index)
                {
                    const vk::QueueFlags flags =
                        families[index].queueFlags;
                    if ((flags & vk::QueueFlagBits::eGraphics) &&
                        (flags & vk::QueueFlagBits::eTransfer))
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

            const float queuePriority = 1.0f;
            vk::DeviceQueueCreateInfo queueInfo;
            queueInfo
                .setQueueFamilyIndex(queueFamilyIndex)
                .setQueueCount(1)
                .setPQueuePriorities(&queuePriority);
            device = physicalDevice.createDevice(
                vk::DeviceCreateInfo().setQueueCreateInfos(queueInfo));
            if (!device)
                return false;
            queue = device.getQueue(queueFamilyIndex, 0);
            commandPool = device.createCommandPool(
                vk::CommandPoolCreateInfo()
                    .setQueueFamilyIndex(queueFamilyIndex)
                    .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer));
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
};

struct TestImage
{
    vk::Device device;
    vk::Image image;
    vk::DeviceMemory memory;

    ~TestImage()
    {
        if (device && image)
            device.destroyImage(image);
        if (device && memory)
            device.freeMemory(memory);
    }
};

struct TestBuffer
{
    vk::Device device;
    vk::Buffer buffer;
    vk::DeviceMemory memory;

    ~TestBuffer()
    {
        if (device && buffer)
            device.destroyBuffer(buffer);
        if (device && memory)
            device.freeMemory(memory);
    }
};

uint32_t FindMemoryType(
    vk::PhysicalDevice physicalDevice,
    uint32_t typeBits,
    vk::MemoryPropertyFlags required)
{
    const vk::PhysicalDeviceMemoryProperties properties =
        physicalDevice.getMemoryProperties();
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((typeBits & (1u << index)) &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
        {
            return index;
        }
    }
    throw std::runtime_error("No matching Vulkan memory type");
}

TestImage CreateColorImage(
    FeedbackTestContext& context,
    uint32_t width,
    uint32_t height)
{
    TestImage image;
    image.device = context.device;
    vk::ImageCreateInfo createInfo;
    createInfo
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D(width, height, 1))
        .setMipLevels(1)
        .setArrayLayers(1)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(
            vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setSharingMode(vk::SharingMode::eExclusive);
    image.image = context.device.createImage(createInfo);
    const vk::MemoryRequirements requirements =
        context.device.getImageMemoryRequirements(image.image);
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(requirements.size)
        .setMemoryTypeIndex(FindMemoryType(
            context.physicalDevice,
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eDeviceLocal));
    image.memory = context.device.allocateMemory(allocateInfo);
    context.device.bindImageMemory(image.image, image.memory, 0);
    return image;
}

TestBuffer CreateHostBuffer(
    FeedbackTestContext& context,
    vk::DeviceSize size)
{
    TestBuffer buffer;
    buffer.device = context.device;
    vk::BufferCreateInfo createInfo;
    createInfo
        .setSize(size)
        .setUsage(vk::BufferUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive);
    buffer.buffer = context.device.createBuffer(createInfo);
    const vk::MemoryRequirements requirements =
        context.device.getBufferMemoryRequirements(buffer.buffer);
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(requirements.size)
        .setMemoryTypeIndex(FindMemoryType(
            context.physicalDevice,
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
    buffer.memory = context.device.allocateMemory(allocateInfo);
    context.device.bindBufferMemory(buffer.buffer, buffer.memory, 0);
    return buffer;
}

} // namespace

TEST(TerrainFeedbackReadback, CopiesColorAttachmentAfterLayoutRoundTrip)
{
    FeedbackTestContext context;
    if (!context.Create())
        GTEST_SKIP() << "No Vulkan device with graphics+transfer queue available";

    constexpr uint32_t Size = 4;
    TestImage image = CreateColorImage(context, Size, Size);
    TestBuffer readback = CreateHostBuffer(
        context,
        static_cast<vk::DeviceSize>(Size) * Size *
            VL::TerrainFeedbackTexelBytes);

    vk::CommandBuffer commandBuffer =
        CommonFunction::BeginSingleTimeCommands(
            context.device,
            context.commandPool);

    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal);
    // 0.2 UNORM 量为 51；bit6(valid)=0，CPU 统计应全部忽略。
    vk::ClearColorValue clearColor;
    clearColor.setFloat32({ 0.2f, 0.2f, 0.2f, 1.0f });
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

    // 先走已存在的 TransferDst -> TransferSrc，再验证 P2 新增的
    // TransferSrc <-> ColorAttachment 往返；数据必须保持不变。
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eColorAttachmentOptimal);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eTransferSrcOptimal);
    CommonFunction::RecordCopyImageToBuffer(
        commandBuffer,
        image.image,
        readback.buffer,
        Size,
        Size);
    CommonFunction::EndSingleTimeCommands(
        context.device,
        commandBuffer,
        context.queue,
        context.commandPool);

    const vk::DeviceSize bytes =
        static_cast<vk::DeviceSize>(Size) * Size *
        VL::TerrainFeedbackTexelBytes;
    const auto* pixels = static_cast<const uint8_t*>(
        context.device.mapMemory(readback.memory, 0, bytes));
    for (uint32_t index = 0; index < Size * Size; ++index)
    {
        const uint8_t* texel =
            pixels + index * VL::TerrainFeedbackTexelBytes;
        EXPECT_EQ(texel[0], 51u);
        EXPECT_EQ(texel[3], 255u);
    }

    const VL::TerrainFeedbackFrameSummary summary =
        VL::SummarizeTerrainFeedback(
            pixels,
            static_cast<size_t>(bytes),
            1);
    EXPECT_EQ(summary.validTexelCount, 0u);
    EXPECT_EQ(
        summary.invalidTexelCount,
        static_cast<uint32_t>(Size * Size));
    context.device.unmapMemory(readback.memory);
}
