#include "particles/particleRendererInternal.h"

namespace VL
{

namespace
{

bool SameBufferInfo(
    const vk::DescriptorBufferInfo& left,
    const vk::DescriptorBufferInfo& right)
{
    return left.buffer == right.buffer &&
        left.offset == right.offset &&
        left.range == right.range;
}

bool SameImageInfo(
    const vk::DescriptorImageInfo& left,
    const vk::DescriptorImageInfo& right)
{
    return left.sampler == right.sampler &&
        left.imageView == right.imageView &&
        left.imageLayout == right.imageLayout;
}

} // namespace

void ParticleRenderer::SetParticleLightBuffers(
    std::vector<vk::Buffer> buffers)
{
    if (particleLightBuffers != buffers)
    {
        ++particleLightBufferGeneration;
    }
    particleLightBuffers = std::move(buffers);
}

void ParticleRenderer::SetSceneLightingResources(
    std::vector<vk::DescriptorBufferInfo> lightInfos,
    std::vector<vk::DescriptorBufferInfo> particleLightInfos,
    vk::DescriptorImageInfo environmentCube,
    vk::DescriptorImageInfo brdfLut)
{
    bool changed = sceneLightInfos.size() != lightInfos.size() ||
        sceneParticleLightInfos.size() != particleLightInfos.size() ||
        !SameImageInfo(environmentCubeDescriptor, environmentCube) ||
        !SameImageInfo(brdfLutDescriptor, brdfLut);
    for (size_t index = 0; !changed && index < lightInfos.size(); ++index)
    {
        changed = !SameBufferInfo(sceneLightInfos[index], lightInfos[index]);
    }
    for (size_t index = 0;
         !changed && index < particleLightInfos.size();
         ++index)
    {
        changed = !SameBufferInfo(
            sceneParticleLightInfos[index],
            particleLightInfos[index]);
    }
    if (changed)
    {
        ++sceneLightingGeneration;
    }
    sceneLightInfos = std::move(lightInfos);
    sceneParticleLightInfos = std::move(particleLightInfos);
    environmentCubeDescriptor = environmentCube;
    brdfLutDescriptor = brdfLut;
}

void ParticleRenderer::SetLightCullingResources(
    std::vector<vk::DescriptorBufferInfo> gridInfos,
    std::vector<vk::DescriptorBufferInfo> indexInfos,
    std::vector<vk::DescriptorBufferInfo> paramInfos)
{
    bool changed =
        lightGridInfos.size() != gridInfos.size() ||
        lightIndexInfos.size() != indexInfos.size() ||
        lightGridParamInfos.size() != paramInfos.size();
    for (size_t index = 0;
         !changed && index < gridInfos.size();
         ++index)
    {
        changed = !SameBufferInfo(lightGridInfos[index], gridInfos[index]);
    }
    for (size_t index = 0;
         !changed && index < indexInfos.size();
         ++index)
    {
        changed = !SameBufferInfo(lightIndexInfos[index], indexInfos[index]);
    }
    for (size_t index = 0;
         !changed && index < paramInfos.size();
         ++index)
    {
        changed = !SameBufferInfo(
            lightGridParamInfos[index],
            paramInfos[index]);
    }
    if (changed)
    {
        ++sceneLightingGeneration;
    }
    lightGridInfos = std::move(gridInfos);
    lightIndexInfos = std::move(indexInfos);
    lightGridParamInfos = std::move(paramInfos);
}

const vk::DescriptorBufferInfo&
ParticleRenderer::SceneLightInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < sceneLightInfos.size()
        ? sceneLightInfos[imageIndex]
        : fallbackSceneLightInfo;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::ParticleLightInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < sceneParticleLightInfos.size()
        ? sceneParticleLightInfos[imageIndex]
        : fallbackParticleLightInfo;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::LightGridInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < lightGridInfos.size()
        ? lightGridInfos[imageIndex]
        : fallbackLightGridInfo;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::LightIndexInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < lightIndexInfos.size()
        ? lightIndexInfos[imageIndex]
        : fallbackLightIndexInfo;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::LightGridParamInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < lightGridParamInfos.size()
        ? lightGridParamInfos[imageIndex]
        : fallbackLightGridParamInfo;
}

const vk::DescriptorImageInfo&
ParticleRenderer::EnvironmentCubeDescriptor() const
{
    return environmentCubeDescriptor.imageView != vk::ImageView{}
        ? environmentCubeDescriptor
        : fallbackEnvironmentCube->GetDescriptorInfo();
}

const vk::DescriptorImageInfo&
ParticleRenderer::BrdfLutDescriptor() const
{
    return brdfLutDescriptor.imageView != vk::ImageView{}
        ? brdfLutDescriptor
        : defaultBlackTexture->GetDescriptorInfo();
}

void ParticleRenderer::DestroyTranslucencyVolume()
{
    if (backend == nullptr)
    {
        return;
    }
    DestroyBuffer(*backend, translucencyVolumeStaging);
    if (translucencyVolumeSampler)
    {
        backend->DestroySampler(translucencyVolumeSampler);
        translucencyVolumeSampler = nullptr;
    }
    if (translucencyVolumeView)
    {
        backend->DestroyImageView(translucencyVolumeView);
        translucencyVolumeView = nullptr;
    }
    if (translucencyVolumeImage)
    {
        backend->DestroyImageResource(
            translucencyVolumeImage,
            translucencyVolumeMemory,
            translucencyVolumeView,
            translucencyVolumeSampler);
        translucencyVolumeImage = nullptr;
        translucencyVolumeMemory = nullptr;
        translucencyVolumeView = nullptr;
        translucencyVolumeSampler = nullptr;
    }
    translucencyVolumeDescriptor = vk::DescriptorImageInfo{};
    translucencyVolumeReady = false;
    translucencyVolumeUploaded = false;
    translucencyVolumeSignature = 0;
}

void ParticleRenderer::EnsureTranslucencyVolume()
{
    if (translucencyVolumeReady || backend == nullptr)
    {
        return;
    }
    constexpr uint32_t kVolumeSize = 16;
    vk::ImageCreateInfo info;
    info
        .setImageType(vk::ImageType::e3D)
        .setFormat(vk::Format::eR32G32B32A32Sfloat)
        .setExtent(vk::Extent3D(kVolumeSize, kVolumeSize, kVolumeSize))
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    const auto imageAllocation = backend->CreateImage(
        info,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleTranslucencyVolume");
    translucencyVolumeImage = imageAllocation.first;
    translucencyVolumeMemory = imageAllocation.second;
    translucencyVolumeView = backend->CreateImageView(
        translucencyVolumeImage,
        vk::ImageViewType::e3D,
        vk::Format::eR32G32B32A32Sfloat,
        vk::ImageAspectFlagBits::eColor,
        0,
        1,
        0,
        1,
        "ParticleTranslucencyVolumeView");
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo
        .setMagFilter(vk::Filter::eLinear)
        .setMinFilter(vk::Filter::eLinear)
        .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setMipmapMode(vk::SamplerMipmapMode::eNearest);
    translucencyVolumeSampler = backend->CreateSampler(
        samplerInfo, "ParticleTranslucencyVolumeSampler");
    const vk::DeviceSize bytes =
        kVolumeSize * kVolumeSize * kVolumeSize *
        sizeof(float) * 4u;
    const auto staging = backend->CreateBuffer(
        bytes,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleTranslucencyVolumeStaging");
    translucencyVolumeStaging.buffer = staging.first;
    translucencyVolumeStaging.memory = staging.second;
    translucencyVolumeDescriptor = vk::DescriptorImageInfo(
        translucencyVolumeSampler,
        translucencyVolumeView,
        vk::ImageLayout::eShaderReadOnlyOptimal);
    translucencyVolumeReady = true;
}

void ParticleRenderer::UpdateTranslucencyVolume(
    const RenderScene& renderScene)
{
    EnsureTranslucencyVolume();
    if (!translucencyVolumeReady)
    {
        return;
    }
    uint64_t signature = 14695981039346656037ull;
    const auto mix = [&](uint64_t value)
    {
        signature ^= value;
        signature *= 1099511628211ull;
    };
    for (const LightSnapshot& light : renderScene.lights)
    {
        mix(static_cast<uint64_t>(light.type));
        for (int axis = 0; axis < 3; ++axis)
        {
            mix(static_cast<uint64_t>(
                std::llround(light.position[axis] * 4.0f)));
            mix(static_cast<uint64_t>(
                std::llround(light.color[axis] * 1024.0f)));
        }
        mix(static_cast<uint64_t>(
            std::llround(light.intensity * 64.0f)));
        mix(static_cast<uint64_t>(
            std::llround(light.radius * 4.0f)));
    }
    if (signature == translucencyVolumeSignature)
    {
        return;
    }
    translucencyVolumeSignature = signature;
    constexpr uint32_t kVolumeSize = 16;
    const Eigen::Vector3f volumeMin(-4000.0f, -1500.0f, -4000.0f);
    const Eigen::Vector3f volumeExtent(8000.0f, 8000.0f, 8000.0f);
    std::vector<float> voxels(
        kVolumeSize * kVolumeSize * kVolumeSize * 4u,
        0.0f);
    for (uint32_t z = 0; z < kVolumeSize; ++z)
    {
        for (uint32_t y = 0; y < kVolumeSize; ++y)
        {
            for (uint32_t x = 0; x < kVolumeSize; ++x)
            {
                const Eigen::Vector3f position =
                    volumeMin +
                    Eigen::Vector3f(
                        (x + 0.5f) / kVolumeSize,
                        (y + 0.5f) / kVolumeSize,
                        (z + 0.5f) / kVolumeSize)
                        .cwiseProduct(volumeExtent);
                Eigen::Vector3f irradiance = Eigen::Vector3f::Zero();
                for (const LightSnapshot& light : renderScene.lights)
                {
                    if (light.type == LightSnapshotType::Directional)
                    {
                        irradiance += light.color *
                            (light.intensity * 0.3f);
                        continue;
                    }
                    const Eigen::Vector3f offset =
                        light.position - position;
                    const float distanceSquared = offset.squaredNorm();
                    if (light.radius > 0.0f &&
                        distanceSquared > light.radius * light.radius)
                    {
                        continue;
                    }
                    const float normalizedSquared =
                        light.radius > 0.0f
                            ? distanceSquared /
                                (light.radius * light.radius)
                            : 0.0f;
                    const float window = light.radius > 0.0f
                        ? std::pow(std::max(
                            1.0f - normalizedSquared *
                                normalizedSquared,
                            0.0f), 2.0f)
                        : 1.0f;
                    irradiance += light.color *
                        (light.intensity * window /
                            (distanceSquared + 1.0f));
                }
                const size_t index =
                    ((z * kVolumeSize + y) * kVolumeSize + x) * 4u;
                voxels[index + 0] = irradiance.x();
                voxels[index + 1] = irradiance.y();
                voxels[index + 2] = irradiance.z();
                voxels[index + 3] = 1.0f;
            }
        }
    }
    void* mapped = backend->MapMemory(
        translucencyVolumeStaging.memory,
        voxels.size() * sizeof(float));
    if (mapped == nullptr)
    {
        return;
    }
    std::memcpy(
        mapped,
        voxels.data(),
        voxels.size() * sizeof(float));
    backend->UnmapMemory(translucencyVolumeStaging.memory);
    // 布局转换必须和 copyBufferToImage 录制在同一个 command buffer 里：
    // backend->TransitionImageLayout 会各自提交一个一次性 command buffer，
    // 等 copy 提交时图已经被转回 SHADER_READ_ONLY，校验会报
    // VUID-vkCmdDraw-None-09600（期望 TRANSFER_DST，实际 SHADER_READ_ONLY）。
    // 这里用一条独立提交承载 读->TRANSFER -> copy -> SHADER_READ 的完整链。
    const vk::ImageLayout oldLayout = translucencyVolumeUploaded
        ? vk::ImageLayout::eShaderReadOnlyOptimal
        : vk::ImageLayout::eUndefined;
    vk::CommandBuffer command = backend->BeginSingleTimeCommands();
    vk::ImageMemoryBarrier toTransfer;
    toTransfer
        .setOldLayout(oldLayout)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(translucencyVolumeImage)
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))
        .setSrcAccessMask(
            oldLayout == vk::ImageLayout::eUndefined
                ? vk::AccessFlags{}
                : vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    command.pipelineBarrier(
        oldLayout == vk::ImageLayout::eUndefined
            ? vk::PipelineStageFlagBits::eTopOfPipe
            : vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        nullptr,
        nullptr,
        toTransfer);
    vk::BufferImageCopy copy;
    copy
        .setBufferOffset(0)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(
            vk::ImageSubresourceLayers(
                vk::ImageAspectFlagBits::eColor, 0, 0, 1))
        .setImageOffset(vk::Offset3D(0, 0, 0))
        .setImageExtent(vk::Extent3D(
            kVolumeSize, kVolumeSize, kVolumeSize));
    command.copyBufferToImage(
        translucencyVolumeStaging.buffer,
        translucencyVolumeImage,
        vk::ImageLayout::eTransferDstOptimal,
        1,
        &copy);
    vk::ImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    command.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eAllCommands,
        {},
        nullptr,
        nullptr,
        toShaderRead);
    backend->EndSingleTimeCommands(command);
    translucencyVolumeUploaded = true;
}

void ParticleRenderer::CreateFallbackLightingResources()
{
    if (backend == nullptr)
    {
        return;
    }

    // 32 字节 = LightSSBOHeader（6 个有效 int + 2 个 padding）。全零表示
    // 没有任何场景灯，材质仍可安全读取 binding 6。
    constexpr vk::DeviceSize kSceneLightHeaderBytes = 32;
    const auto sceneLightAllocation = backend->CreateBuffer(
        kSceneLightHeaderBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackSceneLights");
    fallbackSceneLightBuffer.buffer = sceneLightAllocation.first;
    fallbackSceneLightBuffer.memory = sceneLightAllocation.second;
    fallbackSceneLightBuffer.mapped = backend->MapMemory(
        fallbackSceneLightBuffer.memory,
        kSceneLightHeaderBytes);
    if (fallbackSceneLightBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackSceneLightBuffer.mapped,
            0,
            static_cast<size_t>(kSceneLightHeaderBytes));
    }
    fallbackSceneLightInfo = vk::DescriptorBufferInfo(
        fallbackSceneLightBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    // GPU 粒子灯兜底：header 的 count/capacity 都是 0，任何读取循环立即结束。
    const auto particleLightAllocation = backend->CreateBuffer(
        ParticleLightSSBOSize(),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackLights");
    fallbackParticleLightBuffer.buffer = particleLightAllocation.first;
    fallbackParticleLightBuffer.memory = particleLightAllocation.second;
    fallbackParticleLightBuffer.mapped = backend->MapMemory(
        fallbackParticleLightBuffer.memory,
        ParticleLightSSBOSize());
    if (fallbackParticleLightBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackParticleLightBuffer.mapped,
            0,
            ParticleLightSSBOSize());
    }
    fallbackParticleLightInfo = vk::DescriptorBufferInfo(
        fallbackParticleLightBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    // tile 光照兜底：1 个 tile、可容纳 64 个索引，params.enabled=0。
    // 这样没有聚类 pass 的会话仍能绑定完整 set 0，shader 走连续灯循环。
    const auto lightGridAllocation = backend->CreateBuffer(
        LightGridBytes(1),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackLightGrid");
    fallbackLightGridBuffer.buffer = lightGridAllocation.first;
    fallbackLightGridBuffer.memory = lightGridAllocation.second;
    fallbackLightGridBuffer.mapped = backend->MapMemory(
        fallbackLightGridBuffer.memory,
        LightGridBytes(1));
    if (fallbackLightGridBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackLightGridBuffer.mapped,
            0,
            LightGridBytes(1));
    }
    fallbackLightGridInfo = vk::DescriptorBufferInfo(
        fallbackLightGridBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    const auto lightIndexAllocation = backend->CreateBuffer(
        LightIndexListBytes(1),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackLightIndex");
    fallbackLightIndexBuffer.buffer = lightIndexAllocation.first;
    fallbackLightIndexBuffer.memory = lightIndexAllocation.second;
    fallbackLightIndexBuffer.mapped = backend->MapMemory(
        fallbackLightIndexBuffer.memory,
        LightIndexListBytes(1));
    if (fallbackLightIndexBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackLightIndexBuffer.mapped,
            0,
            LightIndexListBytes(1));
    }
    fallbackLightIndexInfo = vk::DescriptorBufferInfo(
        fallbackLightIndexBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    const auto lightGridParamAllocation = backend->CreateBuffer(
        sizeof(LightGridParams),
        vk::BufferUsageFlagBits::eUniformBuffer |
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackLightGridParams");
    fallbackLightGridParamBuffer.buffer =
        lightGridParamAllocation.first;
    fallbackLightGridParamBuffer.memory =
        lightGridParamAllocation.second;
    fallbackLightGridParamBuffer.mapped = backend->MapMemory(
        fallbackLightGridParamBuffer.memory,
        sizeof(LightGridParams));
    if (fallbackLightGridParamBuffer.mapped != nullptr)
    {
        LightGridParams params{};
        params.tilesX = 1;
        params.tilesY = 1;
        params.renderWidth = 1;
        params.renderHeight = 1;
        params.enabled = 0;
        std::memcpy(
            fallbackLightGridParamBuffer.mapped,
            &params,
            sizeof(params));
    }
    fallbackLightGridParamInfo = vk::DescriptorBufferInfo(
        fallbackLightGridParamBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    // BRDF LUT 兜底用全黑图：环境高光乘 0，不会伪造能量。漫反射 IBL 仍
    // 可独立工作，因为它的近似只采样 prefiltered cubemap。
    HostImage blackImage;
    blackImage.width = 1;
    blackImage.height = 1;
    blackImage.channels = 4;
    blackImage.format = HostImage::PixelFormat::RGBA8_UNORM;
    blackImage.semantic = HostImage::TextureSemantic::Data;
    blackImage.rowStrideBytes = 4;
    blackImage.data = {0, 0, 0, 0};
    Texture::CreateDesc blackDesc;
    blackDesc.semantic = HostImage::TextureSemantic::Data;
    blackDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    blackDesc.generateMipmaps = false;
    blackDesc.filter = vk::Filter::eLinear;
    blackDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    blackDesc.debugName = "ParticleDefaultBlack";
    defaultBlackTexture = std::make_shared<Texture>(
        *backend,
        blackImage,
        blackDesc);

    // 1x1x6 黑色 cubemap：cube 采样要求视图类型匹配，不能拿 2D 白图顶替。
    // 图像创建后必须把全部 6 个 layer 从 UNDEFINED 转到 ShaderReadOnly；
    // 通用 TransitionImageLayout 只处理 1 个 layer，这里显式写 barrier。
    const vk::Format cubeFormat = vk::Format::eR8G8B8A8Unorm;
    vk::ImageCreateInfo cubeInfo;
    cubeInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{1, 1, 1})
        .setMipLevels(1)
        .setArrayLayers(6)
        .setFormat(cubeFormat)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);
    vk::Image cubeImage;
    vk::DeviceMemory cubeMemory;
    std::tie(cubeImage, cubeMemory) = backend->CreateImage(
        cubeInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleFallbackEnvironmentCube");
    vk::CommandBuffer command = backend->BeginSingleTimeCommands();
    try
    {
        vk::ImageMemoryBarrier barrier;
        barrier
            .setOldLayout(vk::ImageLayout::eUndefined)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(cubeImage)
            .setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(6));
        command.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            nullptr,
            nullptr,
            barrier);
        backend->EndSingleTimeCommands(command);
    }
    catch (...)
    {
        if (command)
        {
            backend->AbortSingleTimeCommands(command);
        }
        throw;
    }
    vk::ImageView cubeView = backend->CreateCubeImageView(
        cubeImage,
        1,
        cubeFormat,
        "ParticleFallbackEnvironmentCubeView");
    vk::Sampler cubeSampler = backend->CreateCubeSampler(
        0.0f,
        "ParticleFallbackEnvironmentCubeSampler");
    fallbackEnvironmentCube = std::make_shared<Texture>(
        *backend,
        cubeImage,
        cubeMemory,
        cubeView,
        cubeSampler,
        1,
        cubeFormat);
}

void ParticleRenderer::DestroyFallbackLightingResources()
{
    if (backend != nullptr)
    {
        DestroyBuffer(*backend, fallbackSceneLightBuffer);
        DestroyBuffer(*backend, fallbackParticleLightBuffer);
        DestroyBuffer(*backend, fallbackLightGridBuffer);
        DestroyBuffer(*backend, fallbackLightIndexBuffer);
        DestroyBuffer(*backend, fallbackLightGridParamBuffer);
    }
    else
    {
        fallbackSceneLightBuffer = {};
        fallbackParticleLightBuffer = {};
        fallbackLightGridBuffer = {};
        fallbackLightIndexBuffer = {};
        fallbackLightGridParamBuffer = {};
    }
    fallbackEnvironmentCube.reset();
    defaultBlackTexture.reset();
}

} // namespace VL
