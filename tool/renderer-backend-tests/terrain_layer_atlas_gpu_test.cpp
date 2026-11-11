#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "commonFunction.h"

namespace
{

// 最小 headless Vulkan 上下文：只验证 layer atlas 的 clear / 页表 texel /
// GPU->GPU 子区域 copy。没有可用设备时 Skip。
struct LayerAtlasContext
{
    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;
    uint32_t queueFamilyIndex = 0;

    ~LayerAtlasContext()
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

struct LayerAtlasImage
{
    vk::Device device;
    vk::Image image;
    vk::DeviceMemory memory;

    ~LayerAtlasImage()
    {
        if (device && image)
            device.destroyImage(image);
        if (device && memory)
            device.freeMemory(memory);
    }
};

struct LayerAtlasBuffer
{
    vk::Device device;
    vk::Buffer buffer;
    vk::DeviceMemory memory;

    ~LayerAtlasBuffer()
    {
        if (device && buffer)
            device.destroyBuffer(buffer);
        if (device && memory)
            device.freeMemory(memory);
    }
};

uint32_t FindMemoryTypeIndex(
    const vk::PhysicalDeviceMemoryProperties& properties,
    uint32_t typeBits,
    vk::MemoryPropertyFlags required)
{
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        const bool allowed = (typeBits & (1u << index)) != 0;
        const vk::MemoryPropertyFlags flags =
            properties.memoryTypes[index].propertyFlags;
        if (allowed && (flags & required) == required)
            return index;
    }
    throw std::runtime_error("No matching Vulkan memory type");
}

LayerAtlasImage CreateImage(
    const LayerAtlasContext& context,
    uint32_t width,
    uint32_t height,
    uint32_t layers,
    vk::Format format)
{
    vk::ImageCreateInfo info;
    info
        .setImageType(vk::ImageType::e2D)
        .setFormat(format)
        .setExtent(vk::Extent3D{ width, height, 1 })
        .setMipLevels(1)
        .setArrayLayers(layers)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(
            vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    LayerAtlasImage result;
    result.device = context.device;
    result.image = context.device.createImage(info);
    const vk::MemoryRequirements requirements =
        context.device.getImageMemoryRequirements(result.image);
    result.memory = context.device.allocateMemory(
        vk::MemoryAllocateInfo()
            .setAllocationSize(requirements.size)
            .setMemoryTypeIndex(FindMemoryTypeIndex(
                context.physicalDevice.getMemoryProperties(),
                requirements.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eDeviceLocal)));
    context.device.bindImageMemory(result.image, result.memory, 0);
    return result;
}

LayerAtlasBuffer CreateHostBuffer(
    const LayerAtlasContext& context,
    vk::DeviceSize size)
{
    LayerAtlasBuffer result;
    result.device = context.device;
    result.buffer = context.device.createBuffer(
        vk::BufferCreateInfo()
            .setSize(size)
            .setUsage(
                vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst)
            .setSharingMode(vk::SharingMode::eExclusive));
    const vk::MemoryRequirements requirements =
        context.device.getBufferMemoryRequirements(result.buffer);
    result.memory = context.device.allocateMemory(
        vk::MemoryAllocateInfo()
            .setAllocationSize(requirements.size)
            .setMemoryTypeIndex(FindMemoryTypeIndex(
                context.physicalDevice.getMemoryProperties(),
                requirements.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent)));
    context.device.bindBufferMemory(result.buffer, result.memory, 0);
    return result;
}

std::vector<uint8_t> ReadImage(
    LayerAtlasContext& context,
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t layer,
    vk::DeviceSize bytesPerTexel)
{
    const vk::DeviceSize bytes =
        static_cast<vk::DeviceSize>(width) * height * bytesPerTexel;
    LayerAtlasBuffer readback = CreateHostBuffer(context, bytes);
    vk::CommandBuffer commandBuffer =
        CommonFunction::BeginSingleTimeCommands(context.device, context.commandPool);
    vk::BufferImageCopy region;
    region
        .setBufferOffset(0)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(0)
            .setBaseArrayLayer(layer)
            .setLayerCount(1))
        .setImageOffset(vk::Offset3D{ 0, 0, 0 })
        .setImageExtent(vk::Extent3D{ width, height, 1 });
    commandBuffer.copyImageToBuffer(
        image,
        vk::ImageLayout::eTransferSrcOptimal,
        readback.buffer,
        1,
        &region);
    CommonFunction::EndSingleTimeCommands(
        context.device,
        commandBuffer,
        context.queue,
        context.commandPool);

    std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
    void* mapped = context.device.mapMemory(readback.memory, 0, bytes);
    std::memcpy(pixels.data(), mapped, static_cast<size_t>(bytes));
    context.device.unmapMemory(readback.memory);
    return pixels;
}

} // namespace

// L2 的上传形状：一次 command buffer 同时完成
//   1) 空 atlas clear -> 0；
//   2) RGBA32F 页表 texel 写入；
//   3) 源数组两个 layer 分别 copy 到 atlas 的两个 128×128 页。
// 之后各回读一次，证明三条路径在同一批提交内生效。
TEST(TerrainLayerAtlasGpu, ClearsAtlasWritesPageTableAndCopiesLayersInOneSubmit)
{
    LayerAtlasContext context;
    if (!context.Create())
    {
        GTEST_SKIP() << "No Vulkan device with graphics+transfer queue available";
    }

    constexpr uint32_t PageSize = 128;
    constexpr uint32_t AtlasWidth = PageSize * 2;
    constexpr uint32_t AtlasHeight = PageSize * 2;
    constexpr vk::DeviceSize PageBytes =
        static_cast<vk::DeviceSize>(PageSize) * PageSize * 4;

    LayerAtlasImage source =
        CreateImage(context, PageSize, PageSize, 2, vk::Format::eR8G8B8A8Unorm);
    LayerAtlasImage atlas =
        CreateImage(
            context,
            AtlasWidth,
            AtlasHeight,
            1,
            vk::Format::eR8G8B8A8Unorm);
    LayerAtlasImage pageTable =
        CreateImage(context, 2, 2, 1, vk::Format::eR32G32B32A32Sfloat);

    LayerAtlasBuffer sourceStaging = CreateHostBuffer(context, PageBytes * 2);
    LayerAtlasBuffer pageTableStaging =
        CreateHostBuffer(context, 4 * 16);

    // 源 layer 0 全红、layer 1 全绿；页表 4 个 texel 写入可区分的值。
    void* sourcePixels =
        context.device.mapMemory(sourceStaging.memory, 0, PageBytes * 2);
    std::memset(sourcePixels, 0, static_cast<size_t>(PageBytes));
    auto* red = static_cast<uint8_t*>(sourcePixels);
    auto* green = red + PageBytes;
    for (uint32_t pixel = 0; pixel < PageSize * PageSize; ++pixel)
    {
        red[pixel * 4 + 0] = 255;
        red[pixel * 4 + 3] = 255;
        green[pixel * 4 + 1] = 255;
        green[pixel * 4 + 3] = 255;
    }
    context.device.unmapMemory(sourceStaging.memory);

    void* pagePixels =
        context.device.mapMemory(pageTableStaging.memory, 0, 4 * 16);
    const float entries[4][4] = {
        {3.0f, 1.0f, 1.0f, 7.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {11.0f, 2.0f, 1.0f, 5.0f},
        {0.0f, 0.0f, 0.0f, 0.0f}};
    std::memcpy(pagePixels, entries, 4 * 16);
    context.device.unmapMemory(pageTableStaging.memory);

    // 源层上传与切到 TransferSrc 单独一批；随后 clear/页表/切页共用同一批。
    {
        vk::CommandBuffer commandBuffer =
            CommonFunction::BeginSingleTimeCommands(
                context.device,
                context.commandPool);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            source.image,
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            0,
            2);
        std::vector<vk::BufferImageCopy> regions(2);
        for (uint32_t layer = 0; layer < 2; ++layer)
        {
            regions[layer]
                .setBufferOffset(PageBytes * layer)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(layer)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{ 0, 0, 0 })
                .setImageExtent(vk::Extent3D{ PageSize, PageSize, 1 });
        }
        CommonFunction::RecordCopyBufferToImageRegions(
            commandBuffer,
            sourceStaging.buffer,
            source.image,
            regions);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            source.image,
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            0,
            2);
        CommonFunction::EndSingleTimeCommands(
            context.device,
            commandBuffer,
            context.queue,
            context.commandPool);
    }

    {
        vk::CommandBuffer commandBuffer =
            CommonFunction::BeginSingleTimeCommands(
                context.device,
                context.commandPool);

        // 页表：2×2 RGBA32F，四个 texel 一次 buffer->image 批量写。
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            pageTable.image,
            1,
            vk::Format::eR32G32B32A32Sfloat,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal);
        std::vector<vk::BufferImageCopy> pageRegions;
        for (uint32_t index = 0; index < 4; ++index)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(index * 16)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{
                    static_cast<int32_t>(index % 2),
                    static_cast<int32_t>(index / 2),
                    0 })
                .setImageExtent(vk::Extent3D{ 1, 1, 1 });
            pageRegions.push_back(region);
        }
        CommonFunction::RecordCopyBufferToImageRegions(
            commandBuffer,
            pageTableStaging.buffer,
            pageTable.image,
            pageRegions);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            pageTable.image,
            1,
            vk::Format::eR32G32B32A32Sfloat,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal);

        // atlas：clear 后把源 layer0/1 分别写到 (0,0) 与 (128,0)；
        // 下半张保持 clear 的零值，用来证明 clear 真的执行过。
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            atlas.image,
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal);
        vk::ClearColorValue clearValue;
        clearValue.setFloat32({ 0.0f, 0.0f, 0.0f, 0.0f });
        CommonFunction::RecordClearColorImage(
            commandBuffer,
            atlas.image,
            vk::ImageLayout::eTransferDstOptimal,
            clearValue,
            0,
            1);
        std::vector<vk::ImageCopy> imageCopies(2);
        for (uint32_t layer = 0; layer < 2; ++layer)
        {
            imageCopies[layer]
                .setSrcSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(layer)
                    .setLayerCount(1))
                .setDstSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setSrcOffset(vk::Offset3D{ 0, 0, 0 })
                .setDstOffset(vk::Offset3D{
                    static_cast<int32_t>(layer * PageSize),
                    0,
                    0 })
                .setExtent(vk::Extent3D{ PageSize, PageSize, 1 });
        }
        CommonFunction::RecordCopyImageRegions(
            commandBuffer,
            source.image,
            atlas.image,
            imageCopies);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            atlas.image,
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal);

        CommonFunction::EndSingleTimeCommands(
            context.device,
            commandBuffer,
            context.queue,
            context.commandPool);
    }

    const std::vector<uint8_t> pageData = ReadImage(
        context,
        pageTable.image,
        2,
        2,
        0,
        16);
    const float* pageFloats =
        reinterpret_cast<const float*>(pageData.data());
    EXPECT_FLOAT_EQ(pageFloats[0], 3.0f);
    EXPECT_FLOAT_EQ(pageFloats[1], 1.0f);
    EXPECT_FLOAT_EQ(pageFloats[2], 1.0f);
    EXPECT_FLOAT_EQ(pageFloats[3], 7.0f);
    for (size_t channel = 4; channel < 8; ++channel)
        EXPECT_FLOAT_EQ(pageFloats[channel], 0.0f);
    EXPECT_FLOAT_EQ(pageFloats[8], 11.0f);
    EXPECT_FLOAT_EQ(pageFloats[11], 5.0f);

    const std::vector<uint8_t> atlasData = ReadImage(
        context,
        atlas.image,
        AtlasWidth,
        AtlasHeight,
        0,
        4);
    const auto pixelAt = [&](uint32_t x, uint32_t y)
    {
        return atlasData.data() +
            (static_cast<size_t>(y) * AtlasWidth + x) * 4;
    };
    // 左页来自 layer0（红），右页来自 layer1（绿）。
    EXPECT_EQ(pixelAt(0, 0)[0], 255);
    EXPECT_EQ(pixelAt(0, 0)[1], 0);
    EXPECT_EQ(pixelAt(PageSize, 0)[0], 0);
    EXPECT_EQ(pixelAt(PageSize, 0)[1], 255);
    // 下半张没有被任何 copy 覆盖，必须是 clear 的 0。
    EXPECT_EQ(pixelAt(0, PageSize)[0], 0);
    EXPECT_EQ(pixelAt(0, PageSize)[1], 0);
    EXPECT_EQ(pixelAt(PageSize, PageSize)[2], 0);
    EXPECT_EQ(pixelAt(PageSize, PageSize)[3], 0);
}
