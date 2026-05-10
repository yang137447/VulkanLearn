#include "environmentCubemapGenerator.h"

#include "../commonFunction.h"
#include "../resource/device/deviceTextureFactory.h"
#include "../resource/image/textureIO.h"
#include "../texture.h"
#include "../vulkanManager.h"
#include "computePipeline.h"
#include "pipelineFactory.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{
#if !defined(NDEBUG)
    constexpr bool kEnableDebugCubemapDump = true;
#else
    constexpr bool kEnableDebugCubemapDump = false;
#endif

    // 调试输出按标准 cubemap 朝向命名，便于肉眼检查各面的方向是否正确。
    constexpr std::array<const char*, 6> kFaceNames = { "px", "nx", "py", "ny", "pz", "nz" };

    void FlipImageX(HostImage& image)
    {
        if (image.width <= 1)
        {
            return;
        }

        const size_t pixelSize = image.GetPixelSize();
        const size_t rowBytes = image.rowStrideBytes > 0 ? image.rowStrideBytes : image.width * pixelSize;
        uint8_t* raw = image.data.data();
        std::vector<uint8_t> pixelBuffer(pixelSize);

        for (uint32_t y = 0; y < image.height; ++y)
        {
            uint8_t* row = raw + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < image.width / 2; ++x)
            {
                uint8_t* leftPixel = row + static_cast<size_t>(x) * pixelSize;
                uint8_t* rightPixel = row + static_cast<size_t>(image.width - 1 - x) * pixelSize;
                std::memcpy(pixelBuffer.data(), leftPixel, pixelSize);
                std::memcpy(leftPixel, rightPixel, pixelSize);
                std::memcpy(rightPixel, pixelBuffer.data(), pixelSize);
            }
        }
    }

    vk::Sampler CreateHdrSampler(vk::Device& device)
    {
        // 经纬度环境图只做线性采样，不需要各向异性和 mip 选择。
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eRepeat)
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
        return CommonFunction::CreateSamplerBase(device, samplerInfo);
    }

    void CopyFaceToBuffer(
        vk::Device& device,
        vk::Queue& graphicsQueue,
        vk::CommandPool& commandPool,
        vk::Image image,
        vk::Buffer buffer,
        uint32_t cubeSize,
        uint32_t faceIndex)
    {
        vk::CommandBuffer commandBuffer = CommonFunction::BeginSingleTimeCommands(device, commandPool);

        // 一次只把一个 face 拷到 staging buffer，便于逐面保存调试结果。
        vk::BufferImageCopy region;
        region
            .setBufferOffset(0)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(faceIndex)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{ 0, 0, 0 })
            .setImageExtent(vk::Extent3D{ cubeSize, cubeSize, 1 });

        commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, 1, &region);

        CommonFunction::EndSingleTimeCommands(device, commandBuffer, graphicsQueue, commandPool);
    }
}

std::shared_ptr<Texture> EnvironmentCubemapGenerator::Generate(const std::string& hdrPath, uint32_t cubeSize, PipelineFactory& pipelineFactory)
{
    if (hdrPath.empty())
    {
        return nullptr;
    }
    if (cubeSize == 0)
    {
        throw std::runtime_error("Environment cubemap size must be greater than zero.");
    }

    auto& vulkanManager = VulkanManager::GetInstance();
    auto& device = vulkanManager.GetDevice();
    auto& gpuMemoryProperties = vulkanManager.GetGpuMemoryProperties();
    auto& commandPool = vulkanManager.GetCommandPool();
    auto& graphicsQueue = vulkanManager.GetGraphicQueue();

    // 环境贴图按线性 HDR 纹理读入，不做 sRGB 转换，也不翻转 Y。
    TextureIO::LoadOptions loadOptions;
    loadOptions.semantic = HostImage::TextureSemantic::EnvHdr;
    loadOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    loadOptions.transfer = TextureIO::LoadOptions::Transfer::Linear;
    loadOptions.forceChannels = 4;

    const std::filesystem::path hdrFullPath = CommonFunction::Path(hdrPath);
    auto cpuHdrImage = TextureIO::Load(hdrFullPath, loadOptions);
    if (!cpuHdrImage.has_value())
    {
        throw std::runtime_error("Failed to load environment image: " + hdrFullPath.string());
    }

    DeviceTextureCreateOptions hdrCreateOptions;
    hdrCreateOptions.semantic = cpuHdrImage->semantic;
    hdrCreateOptions.generateMipmapsOnDevice = false;
    hdrCreateOptions.createSampler = false;

    vk::Image hdrImage;
    vk::DeviceMemory hdrImageMemory;
    uint32_t hdrMipLevels = 1;
    vk::Format hdrFormat = vk::Format::eUndefined;
    std::tie(hdrImage, hdrImageMemory, hdrMipLevels, hdrFormat) =
        DeviceTextureFactory::CreateFromHostImage(*cpuHdrImage, hdrFullPath.string(), hdrCreateOptions);

    vk::ImageView hdrImageView = CommonFunction::Create2DImageView(
        device,
        hdrImage,
        hdrMipLevels,
        hdrFormat,
        vk::ImageAspectFlagBits::eColor,
        "EnvironmentHdrView");
    vk::Sampler hdrSampler = CreateHdrSampler(device);

    // 输出图像实际就是 6 layer 的 2D image，设置 cube-compatible 方便后续直接当 cubemap 继续使用。
    vk::Format cubeFormat = vk::Format::eR16G16B16A16Sfloat;
    vk::ImageUsageFlags cubeUsage =
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eSampled;
    vk::ImageTiling cubeTiling = vk::ImageTiling::eOptimal;
    vk::MemoryPropertyFlags cubeMemoryFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;

    vk::ImageCreateInfo cubeImageInfo;
    cubeImageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ cubeSize, cubeSize, 1 })
        .setMipLevels(1)
        .setArrayLayers(6)
        .setFormat(cubeFormat)
        .setTiling(cubeTiling)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(cubeUsage)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);

    vk::Image cubeImage = device.createImage(cubeImageInfo);
    vk::MemoryRequirements cubeMemoryRequirements = device.getImageMemoryRequirements(cubeImage);
    vk::MemoryAllocateInfo cubeAllocInfo;
    cubeAllocInfo
        .setAllocationSize(cubeMemoryRequirements.size)
        .setMemoryTypeIndex(CommonFunction::FindMemoryType(
            gpuMemoryProperties,
            cubeMemoryRequirements.memoryTypeBits,
            cubeMemoryFlags));

    vk::DeviceMemory cubeImageMemory = device.allocateMemory(cubeAllocInfo);
    device.bindImageMemory(cubeImage, cubeImageMemory, 0);

    vk::ImageView cubeStorageView = CommonFunction::CreateCubeStorageImageView(device, cubeImage, cubeFormat, "EnvironmentCubeStorageView");

    auto computePipeline = pipelineFactory.CreateComputePipeline("generator/equirectToCubemap");

    // set 0 只需要两类资源：输入 HDR 采样器和输出 cubemap storage image。
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 1)
    };

    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(1);
    vk::DescriptorPool descriptorPool = device.createDescriptorPool(poolInfo);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(computePipeline->GetDescriptorSetLayouts()[0]);
    vk::DescriptorSet descriptorSet = device.allocateDescriptorSets(descriptorSetAllocateInfo)[0];

    vk::DescriptorImageInfo hdrDescriptorImageInfo;
    hdrDescriptorImageInfo
        .setSampler(hdrSampler)
        .setImageView(hdrImageView)
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::DescriptorImageInfo cubeDescriptorImageInfo;
    cubeDescriptorImageInfo
        .setImageView(cubeStorageView)
        .setImageLayout(vk::ImageLayout::eGeneral);

    std::array<vk::WriteDescriptorSet, 2> writeDescriptorSets;
    writeDescriptorSets[0]
        .setDstSet(descriptorSet)
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(hdrDescriptorImageInfo);
    writeDescriptorSets[1]
        .setDstSet(descriptorSet)
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(cubeDescriptorImageInfo);
    device.updateDescriptorSets(writeDescriptorSets, nullptr);

    vk::CommandBuffer commandBuffer = CommonFunction::BeginSingleTimeCommands(device, commandPool);

    // 输出 cubemap 先切到 General，允许 compute shader 直接 imageStore 写入。
    vk::ImageMemoryBarrier cubeToGeneralBarrier;
    cubeToGeneralBarrier
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(cubeImage)
        .setSubresourceRange(vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(0)
            .setLayerCount(6))
        .setSrcAccessMask(vk::AccessFlagBits::eNone)
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        cubeToGeneralBarrier);

    computePipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        computePipeline->GetPipelineLayout(),
        0,
        descriptorSet,
        nullptr);
    // z 维度固定为 6，对应 cubemap 的 6 个面。
    computePipeline->Dispatch(commandBuffer, (cubeSize + 7) / 8, (cubeSize + 7) / 8, 6);

    if constexpr (kEnableDebugCubemapDump)
    {
        // Debug 模式下切到 TransferSrc，便于逐面导出 EXR 做朝向校验。
        vk::ImageMemoryBarrier cubeToTransferBarrier;
        cubeToTransferBarrier
            .setOldLayout(vk::ImageLayout::eGeneral)
            .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(cubeImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(6))
            .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            cubeToTransferBarrier);
    }
    else
    {
        vk::ImageMemoryBarrier cubeToSampleBarrier;
        cubeToSampleBarrier
            .setOldLayout(vk::ImageLayout::eGeneral)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(cubeImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(6))
            .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            cubeToSampleBarrier);
    }

    CommonFunction::EndSingleTimeCommands(device, commandBuffer, graphicsQueue, commandPool);

    if constexpr (kEnableDebugCubemapDump)
    {
        const vk::DeviceSize bytesPerPixel = sizeof(uint16_t) * 4;
        vk::DeviceSize readbackSize = static_cast<vk::DeviceSize>(cubeSize) * cubeSize * bytesPerPixel;
        vk::BufferUsageFlags stagingUsage = vk::BufferUsageFlagBits::eTransferDst;
        vk::MemoryPropertyFlags stagingMemoryFlags =
            vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent;
        auto [stagingBuffer, stagingBufferMemory] = CommonFunction::CreateBuffer(
            device,
            readbackSize,
            stagingUsage,
            gpuMemoryProperties,
            stagingMemoryFlags,
            "EnvironmentCubemapReadback");

        const std::filesystem::path outputDir =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "resources" / "generated" / "cubemap" / hdrFullPath.stem();
        std::filesystem::create_directories(outputDir);

        TextureIO::SaveOptions saveOptions;
        saveOptions.semantic = HostImage::TextureSemantic::EnvHdr;
        saveOptions.format = TextureIO::FileFormat::Exr;

        for (uint32_t faceIndex = 0; faceIndex < 6; ++faceIndex)
        {
            // Debug 模式下把每个 face 单独落盘，便于检查朝向与接缝。
            CopyFaceToBuffer(device, graphicsQueue, commandPool, cubeImage, stagingBuffer, cubeSize, faceIndex);

            void* mapped = device.mapMemory(stagingBufferMemory, 0, readbackSize);
            HostImage faceImage;
            faceImage.width = cubeSize;
            faceImage.height = cubeSize;
            faceImage.channels = 4;
            faceImage.mipLevels = 1;
            faceImage.arrayLayers = 1;
            faceImage.format = HostImage::PixelFormat::RGBA16_FLOAT;
            faceImage.semantic = HostImage::TextureSemantic::EnvHdr;
            faceImage.rowStrideBytes = static_cast<uint32_t>(cubeSize * bytesPerPixel);
            faceImage.data.resize(static_cast<size_t>(readbackSize));
            std::memcpy(faceImage.data.data(), mapped, static_cast<size_t>(readbackSize));
            device.unmapMemory(stagingBufferMemory);

            // 运行时 cubemap 方向是正确的，这里只修正 debug 落盘图的人眼观察朝向。
            FlipImageX(faceImage);

            TextureIO::Save(outputDir / (std::string(kFaceNames[faceIndex]) + ".exr"), faceImage, saveOptions);
        }

        device.destroyBuffer(stagingBuffer);
        device.freeMemory(stagingBufferMemory);

        vk::CommandBuffer layoutCommandBuffer = CommonFunction::BeginSingleTimeCommands(device, commandPool);
        vk::ImageMemoryBarrier cubeToSampleBarrier;
        cubeToSampleBarrier
            .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(cubeImage)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(6))
            .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        layoutCommandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            cubeToSampleBarrier);
        CommonFunction::EndSingleTimeCommands(device, layoutCommandBuffer, graphicsQueue, commandPool);
    }

    device.destroyDescriptorPool(descriptorPool);
    device.destroyImageView(cubeStorageView);
    device.destroySampler(hdrSampler);
    device.destroyImageView(hdrImageView);
    device.destroyImage(hdrImage);
    device.freeMemory(hdrImageMemory);

    vk::ImageView cubeSampleView = CommonFunction::CreateCubeImageView(device, cubeImage, 1, cubeFormat, "EnvironmentCubeView");
    vk::Sampler cubeSampler = CommonFunction::CreateCubeSampler(device, "EnvironmentCubeSampler");

    return std::make_shared<Texture>(cubeImage, cubeImageMemory, cubeSampleView, cubeSampler, 1, cubeFormat);
}
