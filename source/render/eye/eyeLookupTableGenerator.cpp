#include "render/eye/eyeLookupTableGenerator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"

namespace VL
{
namespace
{

struct GpuVec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct GenerationParameters
{
    GpuVec4 profiles[EyeCausticLutMaximumProfileId + 1]{};
};

static_assert(sizeof(GpuVec4) == sizeof(float) * 4);
static_assert(sizeof(GenerationParameters) % 16 == 0);

struct GeneratedImage
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
};

class GeneratedImageOwner
{
public:
    explicit GeneratedImageOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~GeneratedImageOwner()
    {
        Reset();
    }

    GeneratedImageOwner(const GeneratedImageOwner&) = delete;
    GeneratedImageOwner& operator=(const GeneratedImageOwner&) = delete;

    void Adopt(GeneratedImage value) noexcept
    {
        image = value;
    }

    GeneratedImage& Get() noexcept
    {
        return image;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        image = {};
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend == nullptr)
        {
            return;
        }
        vk::Sampler sampler;
        rendererBackend->DestroyImageResource(
            image.image,
            image.memory,
            image.view,
            sampler);
        rendererBackend = nullptr;
        image = {};
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    GeneratedImage image;
};

class GpuBufferOwner
{
public:
    explicit GpuBufferOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~GpuBufferOwner()
    {
        Reset();
    }

    GpuBufferOwner(const GpuBufferOwner&) = delete;
    GpuBufferOwner& operator=(const GpuBufferOwner&) = delete;

    void Adopt(vk::Buffer buffer, vk::DeviceMemory memory) noexcept
    {
        this->buffer = buffer;
        this->memory = memory;
    }

    vk::Buffer GetBuffer() const noexcept
    {
        return buffer;
    }

    vk::DeviceMemory GetMemory() const noexcept
    {
        return memory;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        buffer = nullptr;
        memory = nullptr;
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroyBuffer(buffer, memory);
        }
        rendererBackend = nullptr;
        buffer = nullptr;
        memory = nullptr;
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::Buffer buffer;
    vk::DeviceMemory memory;
};

class DescriptorPoolOwner
{
public:
    explicit DescriptorPoolOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~DescriptorPoolOwner()
    {
        Reset();
    }

    void Adopt(vk::DescriptorPool pool) noexcept
    {
        this->pool = pool;
    }

    vk::DescriptorPool Get() const noexcept
    {
        return pool;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        pool = nullptr;
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroyDescriptorPool(pool);
        }
        rendererBackend = nullptr;
        pool = nullptr;
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::DescriptorPool pool;
};

class SamplerOwner
{
public:
    explicit SamplerOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~SamplerOwner()
    {
        Reset();
    }

    void Adopt(vk::Sampler sampler) noexcept
    {
        this->sampler = sampler;
    }

    vk::Sampler Get() const noexcept
    {
        return sampler;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        sampler = nullptr;
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroySampler(sampler);
        }
        rendererBackend = nullptr;
        sampler = nullptr;
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::Sampler sampler;
};

GeneratedImage CreateEyeLutImage(
    RendererBackendVulkan& rendererBackend,
    std::string_view sourceIdentity)
{
    constexpr vk::Format format = vk::Format::eR16G16B16A16Sfloat;
    vk::ImageCreateInfo createInfo;
    createInfo
        .setImageType(vk::ImageType::e2D)
        .setFormat(format)
        .setExtent(vk::Extent3D{EyeCausticLutWidth, EyeCausticLutHeight, 1})
        .setMipLevels(1)
        .setArrayLayers(EyeCausticLutLayerCount)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    const std::string debugName =
        "EyeCausticLut:" + std::string(sourceIdentity);
    GeneratedImageOwner owner(rendererBackend);
    auto [image, memory] = rendererBackend.CreateImage(
        createInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        debugName);
    owner.Adopt({image, memory, nullptr});
    owner.Get().view = rendererBackend.CreateImageView(
        image,
        vk::ImageViewType::e2DArray,
        format,
        vk::ImageAspectFlagBits::eColor,
        0,
        1,
        0,
        EyeCausticLutLayerCount,
        debugName + ":View");
    GeneratedImage result = owner.Get();
    owner.Release();
    return result;
}

vk::ImageMemoryBarrier BuildImageBarrier(
    vk::Image image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::AccessFlags sourceAccess,
    vk::AccessFlags destinationAccess)
{
    vk::ImageMemoryBarrier barrier;
    barrier
        .setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(image)
        .setSubresourceRange(
            vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(EyeCausticLutLayerCount))
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
    return barrier;
}

float HalfToFloat(uint16_t value) noexcept
{
    const uint32_t sign = (static_cast<uint32_t>(value) & 0x8000u) << 16u;
    const uint32_t exponent = (static_cast<uint32_t>(value) >> 10u) & 0x1fu;
    const uint32_t mantissa = static_cast<uint32_t>(value) & 0x03ffu;
    uint32_t bits = 0;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            bits = sign;
        }
        else
        {
            uint32_t normalizedMantissa = mantissa;
            uint32_t normalizedExponent = 127u - 14u;
            while ((normalizedMantissa & 0x0400u) == 0u)
            {
                normalizedMantissa <<= 1u;
                --normalizedExponent;
            }
            normalizedMantissa &= 0x03ffu;
            bits = sign |
                (normalizedExponent << 23u) |
                (normalizedMantissa << 13u);
        }
    }
    else if (exponent == 0x1fu)
    {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    }
    else
    {
        bits = sign |
            ((exponent + (127u - 15u)) << 23u) |
            (mantissa << 13u);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void ValidateGeneratedEyeLutReadback(
    RendererBackendVulkan& rendererBackend,
    const GeneratedImage& image,
    const std::vector<EyeProfileAsset>& profiles,
    EyeLutReadbackReport& report)
{
    constexpr vk::DeviceSize bytesPerPixel = sizeof(uint16_t) * 4;
    const vk::DeviceSize layerBytes =
        static_cast<vk::DeviceSize>(EyeCausticLutWidth) *
        EyeCausticLutHeight * bytesPerPixel;
    const vk::DeviceSize readbackSize =
        layerBytes * EyeCausticLutLayerCount;
    auto [stagingBuffer, stagingMemory] = rendererBackend.CreateBuffer(
        readbackSize,
        vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "EyeCausticLutValidationReadback");
    try
    {
        vk::CommandBuffer commandBuffer =
            rendererBackend.BeginSingleTimeCommands();
        const vk::ImageMemoryBarrier toTransfer = BuildImageBarrier(
            image.image,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits::eShaderRead,
            vk::AccessFlagBits::eTransferRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            toTransfer);
        std::vector<vk::BufferImageCopy> regions;
        regions.reserve(EyeCausticLutLayerCount);
        for (uint32_t layer = 0; layer < EyeCausticLutLayerCount; ++layer)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(layerBytes * layer)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(
                    vk::ImageSubresourceLayers()
                        .setAspectMask(vk::ImageAspectFlagBits::eColor)
                        .setMipLevel(0)
                        .setBaseArrayLayer(layer)
                        .setLayerCount(1))
                .setImageOffset(vk::Offset3D{0, 0, 0})
                .setImageExtent(
                    vk::Extent3D{EyeCausticLutWidth, EyeCausticLutHeight, 1});
            regions.push_back(region);
        }
        commandBuffer.copyImageToBuffer(
            image.image,
            vk::ImageLayout::eTransferSrcOptimal,
            stagingBuffer,
            regions);
        const vk::ImageMemoryBarrier backToSample = BuildImageBarrier(
            image.image,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferRead,
            vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            backToSample);
        rendererBackend.EndSingleTimeCommands(commandBuffer);

        const auto* mapped = static_cast<const uint16_t*>(
            rendererBackend.MapMemory(stagingMemory, readbackSize));
        double gainErrorSum = 0.0;
        double gainSum = 0.0;
        const std::vector<EyeProfileAsset> profileById = profiles;
        report.executed = true;
        report.sampleCount = static_cast<size_t>(
            EyeCausticLutWidth * EyeCausticLutHeight *
            EyeCausticLutLayerCount);
        for (uint32_t layer = 0; layer < EyeCausticLutLayerCount; ++layer)
        {
            const uint32_t profileId =
                layer / EyeCausticLutElevationSliceCount;
            const uint32_t elevationIndex =
                layer % EyeCausticLutElevationSliceCount;
            float strength = 0.0f;
            // shader 中 profileId>0 但未加载 profile 时，storage buffer 保持零值；
            // CPU reference 必须复现这个“无 profile layer 不可采样”的合同。
            float ior = profileId == 0 ? 1.376f : 0.0f;
            for (const EyeProfileAsset& profile : profileById)
            {
                if (profile.profileId == profileId)
                {
                    strength = profile.causticStrength;
                    ior = profile.ior;
                    break;
                }
            }
            const float elevation =
                (static_cast<float>(elevationIndex) + 0.5f) /
                static_cast<float>(EyeCausticLutElevationSliceCount);
            const float frontLightWeight = 0.35f + 0.65f * elevation;
            const float f0 = std::pow(
                (1.0f - ior) / (1.0f + ior),
                2.0f);
            for (uint32_t y = 0; y < EyeCausticLutHeight; ++y)
            {
                for (uint32_t x = 0; x < EyeCausticLutWidth; ++x)
                {
                    const float u =
                        (static_cast<float>(x) + 0.5f) /
                        static_cast<float>(EyeCausticLutWidth) * 2.0f - 1.0f;
                    const float v =
                        (static_cast<float>(y) + 0.5f) /
                        static_cast<float>(EyeCausticLutHeight) * 2.0f - 1.0f;
                    const float radialSquared = u * u + v * v;
                    const bool valid = radialSquared <= 1.0f;
                    const float hasProfile = profileId >= 1 ? 1.0f : 0.0f;
                    const float expectedGain = valid
                        ? (hasProfile > 0.5f
                            ? 1.0f + strength *
                                (1.0f - 2.0f * radialSquared) *
                                frontLightWeight
                            : 1.0f)
                        : 0.0f;
                    const float expectedTransmission = valid
                        ? (hasProfile > 0.5f
                            ? (1.0f - f0) * frontLightWeight
                            : 1.0f)
                        : 0.0f;
                    const float expectedCoverage = valid ? 1.0f : 0.0f;
                    const float expectedJacobian = valid
                        ? (hasProfile > 0.5f
                            ? 1.0f + strength *
                                (1.0f - radialSquared) *
                                frontLightWeight
                            : 1.0f)
                        : 0.0f;
                    const size_t index = static_cast<size_t>(
                        layer * EyeCausticLutWidth * EyeCausticLutHeight * 4u +
                        y * EyeCausticLutWidth * 4u + x * 4u);
                    const float actualGain = HalfToFloat(mapped[index]);
                    const float actualTransmission = HalfToFloat(mapped[index + 1]);
                    const float actualCoverage = HalfToFloat(mapped[index + 2]);
                    const float actualJacobian = HalfToFloat(mapped[index + 3]);
                    report.allFinite = report.allFinite &&
                        std::isfinite(actualGain) &&
                        std::isfinite(actualTransmission) &&
                        std::isfinite(actualCoverage) &&
                        std::isfinite(actualJacobian);
                    const float gainError = std::abs(actualGain - expectedGain);
                    const float transmissionError =
                        std::abs(actualTransmission - expectedTransmission);
                    const float coverageError =
                        std::abs(actualCoverage - expectedCoverage);
                    const float jacobianError =
                        std::abs(actualJacobian - expectedJacobian);
                    report.maxAbsoluteGainError = std::max(
                        report.maxAbsoluteGainError,
                        gainError);
                    report.maxAbsoluteTransmissionError = std::max(
                        report.maxAbsoluteTransmissionError,
                        transmissionError);
                    report.maxAbsoluteCoverageError = std::max(
                        report.maxAbsoluteCoverageError,
                        coverageError);
                    report.maxAbsoluteJacobianError = std::max(
                        report.maxAbsoluteJacobianError,
                        jacobianError);
                    if (valid)
                    {
                        ++report.validDomainSampleCount;
                        gainErrorSum += gainError;
                        gainSum += actualGain;
                    }
                }
            }
        }
        rendererBackend.UnmapMemory(stagingMemory);
        if (report.validDomainSampleCount != 0)
        {
            report.meanAbsoluteGainError = static_cast<float>(
                gainErrorSum / report.validDomainSampleCount);
            report.validDomainGainAverage = static_cast<float>(
                gainSum / report.validDomainSampleCount);
            report.normalizationError =
                std::abs(report.validDomainGainAverage - 1.0f);
        }
        rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
    }
    catch (...)
    {
        rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
        throw;
    }
}

} // namespace

EyeComputePipelineCandidate CreateEyeComputePipelineCandidate(
    PipelineFactory& pipelineFactory)
{
    ComputeShaderArtifact artifact;
    EyeComputePipelineCandidate candidate;
    candidate.pipeline = pipelineFactory.CreateComputePipelineCandidate(
        "generator/eyeCausticLut",
        &artifact);
    candidate.artifactGenerationKey = artifact.artifactGenerationKey;
    return candidate;
}

std::shared_ptr<Texture> GenerateEyeCausticLutTexture(
    RendererBackendVulkan& rendererBackend,
    const EyeComputePipelineCandidate& candidate,
    const std::vector<EyeProfileAsset>& profiles,
    std::string_view sourceIdentity,
    EyeLutReadbackReport* readbackReport)
{
    if (!candidate.pipeline || sourceIdentity.empty())
    {
        throw std::runtime_error(
            "Eye LUT generation requires a Compute pipeline and source identity");
    }

    GenerationParameters parameters;
    parameters.profiles[0] = {1.376f, 0.012f, 0.003f, 0.0f};
    for (const EyeProfileAsset& profile : profiles)
    {
        ValidateEyeProfileAsset(profile, profile.assetPath);
        parameters.profiles[profile.profileId] = {
            profile.ior,
            profile.corneaRadius,
            profile.irisDistance,
            profile.causticStrength};
    }

    GeneratedImageOwner imageOwner(rendererBackend);
    imageOwner.Adopt(CreateEyeLutImage(rendererBackend, sourceIdentity));

    GpuBufferOwner parameterBufferOwner(rendererBackend);
    auto [parameterBuffer, parameterMemory] = rendererBackend.CreateBuffer(
        sizeof(parameters),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "EyeCausticLutGenerationParameters");
    parameterBufferOwner.Adopt(parameterBuffer, parameterMemory);
    void* mapped = rendererBackend.MapMemory(
        parameterBufferOwner.GetMemory(),
        sizeof(parameters));
    std::memcpy(mapped, &parameters, sizeof(parameters));
    rendererBackend.UnmapMemory(parameterBufferOwner.GetMemory());

    const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1}};
    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo.setPoolSizes(poolSizes).setMaxSets(1);
    DescriptorPoolOwner descriptorPoolOwner(rendererBackend);
    descriptorPoolOwner.Adopt(rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: EyeCausticLutGeneration"));

    const vk::DescriptorSetLayout setLayout =
        candidate.pipeline->GetDescriptorSetLayouts().at(0);
    vk::DescriptorSetAllocateInfo allocateInfo;
    allocateInfo
        .setDescriptorPool(descriptorPoolOwner.Get())
        .setSetLayouts(setLayout);
    std::vector<vk::DescriptorSet> descriptorSets(1);
    rendererBackend.AllocateDescriptorSets(allocateInfo, descriptorSets);
    const vk::DescriptorSet descriptorSet = descriptorSets[0];

    const vk::DescriptorImageInfo imageInfo{
        vk::Sampler(),
        imageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo parameterInfo{
        parameterBufferOwner.GetBuffer(),
        0,
        sizeof(parameters)};
    std::array<vk::WriteDescriptorSet, 2> writes;
    writes[0]
        .setDstSet(descriptorSet)
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(imageInfo);
    writes[1]
        .setDstSet(descriptorSet)
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageBuffer)
        .setBufferInfo(parameterInfo);
    rendererBackend.UpdateDescriptorSets(
        std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));

    vk::CommandBuffer commandBuffer =
        rendererBackend.BeginSingleTimeCommands();
    try
    {
        const vk::ImageMemoryBarrier toGeneral = BuildImageBarrier(
            imageOwner.Get().image,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eNone,
            vk::AccessFlagBits::eShaderWrite);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            toGeneral);

        candidate.pipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            candidate.pipeline->GetPipelineLayout(),
            0,
            descriptorSet,
            nullptr);
        candidate.pipeline->Dispatch(
            commandBuffer,
            (EyeCausticLutWidth + 7) / 8,
            (EyeCausticLutHeight + 7) / 8,
            EyeCausticLutLayerCount);

        const vk::ImageMemoryBarrier toSample = BuildImageBarrier(
            imageOwner.Get().image,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            toSample);
        rendererBackend.EndSingleTimeCommands(commandBuffer);
    }
    catch (...)
    {
        rendererBackend.AbortSingleTimeCommands(commandBuffer);
        throw;
    }

    if (readbackReport != nullptr)
    {
        ValidateGeneratedEyeLutReadback(
            rendererBackend,
            imageOwner.Get(),
            profiles,
            *readbackReport);
    }

    SamplerOwner samplerOwner(rendererBackend);
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo
        .setMagFilter(vk::Filter::eLinear)
        .setMinFilter(vk::Filter::eLinear)
        .setMipmapMode(vk::SamplerMipmapMode::eNearest)
        .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setAnisotropyEnable(VK_FALSE)
        .setMaxAnisotropy(1.0f)
        .setBorderColor(vk::BorderColor::eFloatOpaqueBlack)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMinLod(0.0f)
        .setMaxLod(0.0f);
    samplerOwner.Adopt(rendererBackend.CreateSampler(
        samplerInfo,
        "EyeCausticLutSampler"));

    std::shared_ptr<Texture> texture = std::make_shared<Texture>(
        rendererBackend,
        imageOwner.Get().image,
        imageOwner.Get().memory,
        imageOwner.Get().view,
        samplerOwner.Get(),
        1,
        vk::Format::eR16G16B16A16Sfloat);
    imageOwner.Release();
    samplerOwner.Release();
    return texture;
}

} // namespace VL
