#include "render/backend/rendererFrameResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineBase.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectGpuResources.h"
#include "render/foliage/speedTreeWindTransform.h"
#include "render/frontend/renderScene.h"
#include "render/resource/resourceRetireQueue.h"
#include "shaderReflect.h"

namespace VL
{
namespace
{
struct CpuUploadSkipRange
{
    vk::DeviceSize offset = 0;
    vk::DeviceSize size = 0;
};

// 这里使用宏定义来计算UBO字段的偏移量和大小
#define VL_UBO_FIELD_RANGE(StructType, FieldName) \
    CpuUploadSkipRange{ \
        static_cast<vk::DeviceSize>(offsetof(StructType, FieldName)), \
        static_cast<vk::DeviceSize>(sizeof(((StructType*)nullptr)->FieldName)) \
    }

static constexpr std::array<CpuUploadSkipRange, 1> kUboGlobalGpuOwnedRanges = {
    VL_UBO_FIELD_RANGE(UBOGlobal, environmentSH)
};

template <size_t RangeCount>
static void UpdateBufferExcludingRanges(
    vk::CommandBuffer& commandBuffer,
    vk::Buffer buffer,
    const void* data,
    vk::DeviceSize totalSize,
    const std::array<CpuUploadSkipRange, RangeCount>& skipRanges)
{
    const char* bytes = reinterpret_cast<const char*>(data);
    vk::DeviceSize cursor = 0;

    for (const CpuUploadSkipRange& range : skipRanges)
    {
        if (cursor < range.offset)
        {
            commandBuffer.updateBuffer(
                buffer,
                cursor,
                range.offset - cursor,
                bytes + cursor);
        }

        cursor = std::max(cursor, range.offset + range.size);
    }

    if (cursor < totalSize)
    {
        commandBuffer.updateBuffer(
            buffer,
            cursor,
            totalSize - cursor,
            bytes + cursor);
    }
}

constexpr size_t kDefaultLightCapacity = 64;

bool HasBufferSetResources(const Buffer& bufferSet)
{
    return bufferSet.HasResources();
}

Buffer TakeBufferSet(Buffer& bufferSet)
{
    Buffer retiredBufferSet = std::move(bufferSet);
    bufferSet = Buffer{};
    return retiredBufferSet;
}

struct RetiredFrameBufferSet
{
    RendererBackendVulkan* rendererBackend = nullptr;
    Buffer bufferSet;

    ~RetiredFrameBufferSet()
    {
        if (rendererBackend == nullptr)
        {
            return;
        }

        rendererBackend->DestroyBufferSet(bufferSet);
    }
};

void DestroyPreparedBufferSet(
    RendererBackendVulkan* rendererBackend,
    Buffer& bufferSet)
{
    if (rendererBackend != nullptr && bufferSet.HasResources())
    {
        rendererBackend->DestroyBufferSet(bufferSet);
    }
}

void RetireFrameBufferSet(
    RendererBackendVulkan& rendererBackend,
    Buffer& bufferSet,
    const std::string& debugName)
{
    if (!HasBufferSetResources(bufferSet))
    {
        return;
    }

    auto retiredBufferSet = std::make_shared<RetiredFrameBufferSet>();
    retiredBufferSet->rendererBackend = &rendererBackend;
    retiredBufferSet->bufferSet = TakeBufferSet(bufferSet);

    ResourceRetireQueue::GetInstance().RetireShared(
        debugName,
        0,
        ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
        std::move(retiredBufferSet));
}

LightGPU BuildLightGPU(const LightSnapshot& light)
{
    LightGPU gpuLight{};
    gpuLight.colorIntensity = Eigen::Vector4f(
        light.color.x(),
        light.color.y(),
        light.color.z(),
        light.intensity);
    gpuLight.positionRadius = Eigen::Vector4f(
        light.position.x(),
        light.position.y(),
        light.position.z(),
        light.radius);
    gpuLight.directionPad = Eigen::Vector4f(
        light.direction.x(),
        light.direction.y(),
        light.direction.z(),
        0.0f);
    gpuLight.coneAngleOuterInnerPadPad = Eigen::Vector4f(
        light.coneAngleOuter,
        light.coneAngleInner,
        0.0f,
        0.0f);
    return gpuLight;
}

} // namespace

RendererFrameResources::PreparedLightCapacity::~PreparedLightCapacity()
{
    if (!adopted)
    {
        DestroyPreparedBufferSet(rendererBackend, replacement);
    }
}

RendererFrameResources::PreparedLightCapacity::PreparedLightCapacity(
    PreparedLightCapacity&& other) noexcept
    : replacement(std::move(other.replacement))
    , capacity(other.capacity)
    , rendererBackend(other.rendererBackend)
    , retiredResource(std::move(other.retiredResource))
    , adopted(other.adopted)
{
    other.rendererBackend = nullptr;
    other.adopted = true;
}

RendererFrameResources::PreparedLightCapacity&
RendererFrameResources::PreparedLightCapacity::operator=(
    PreparedLightCapacity&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    if (!adopted)
    {
        DestroyPreparedBufferSet(rendererBackend, replacement);
    }
    replacement = std::move(other.replacement);
    capacity = other.capacity;
    rendererBackend = other.rendererBackend;
    retiredResource = std::move(other.retiredResource);
    adopted = other.adopted;
    other.rendererBackend = nullptr;
    other.adopted = true;
    return *this;
}

const std::vector<vk::DescriptorBufferInfo>&
RendererFrameResources::PreparedLightCapacity::GetBufferInfos(
    const RendererFrameResources& active) const
{
    return HasReplacement()
        ? replacement.bufferInfos
        : active.GetLightBufferInfos();
}

void RendererFrameResources::Initialize(RendererBackendVulkan& rendererBackend)
{
    if (initialized)
    {
        return;
    }

    vk::BufferUsageFlags globalUniformUsage =
        vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eTransferDst;
    // 这里留给一般的ubo使用
    vk::BufferUsageFlags uniformUsage =
        vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;

    rendererBackend.CreatePerSwapchainBufferSet(
        globalUniformBuffer,
        sizeof(UBOGlobal),
        globalUniformUsage,
        memoryPropertyFlags,
        "UBO_Global");

    // environmentSH 由 GPU 在代际 commit 时写入，而常规 CPU 上传会跳过该区间。
    // 首个环境代际尚未完成时先保持零值，使 active 黑色 IBL 也具有确定结果。
    for (void* mappedBuffer : globalUniformBuffer.buffersMapped)
    {
        std::memset(mappedBuffer, 0, sizeof(UBOGlobal));
    }
    initialized = true;
}

void RendererFrameResources::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    DestroyLightBuffer(rendererBackend);
    rendererBackend.DestroyBufferSet(globalUniformBuffer);
    initialized = false;
}

bool RendererFrameResources::EnsureLightCapacity(
    size_t requestedLightCount,
    RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererFrameResources must be initialized before creating the light buffer.");
    }

    const size_t requiredLightCapacity = std::max(requestedLightCount, kDefaultLightCapacity);
    if (lightBuffer.HasResources() && requiredLightCapacity <= maxLightCount)
    {
        return false;
    }

    CreateLightBuffer(requiredLightCapacity, rendererBackend);
    return true;
}

RendererFrameResources::PreparedLightCapacity
RendererFrameResources::PrepareLightCapacity(
    size_t requestedLightCount,
    RendererBackendVulkan& rendererBackend) const
{
    if (!initialized)
    {
        throw std::runtime_error(
            "RendererFrameResources must be initialized before preparing light capacity.");
    }

    PreparedLightCapacity prepared;
    const size_t requiredLightCapacity =
        std::max(requestedLightCount, kDefaultLightCapacity);
    if (lightBuffer.HasResources() &&
        requiredLightCapacity <= maxLightCount)
    {
        return prepared;
    }

    prepared.replacement =
        CreateLightBufferSet(requiredLightCapacity, rendererBackend);
    prepared.capacity = requiredLightCapacity;
    prepared.rendererBackend = &rendererBackend;
    auto retiredBufferSet =
        std::make_shared<RetiredFrameBufferSet>();
    retiredBufferSet->rendererBackend = &rendererBackend;
    prepared.retiredResource =
        std::static_pointer_cast<void>(
            std::move(retiredBufferSet));
    return prepared;
}

std::shared_ptr<void>
RendererFrameResources::CommitPreparedLightCapacity(
    PreparedLightCapacity&& prepared) noexcept
{
    if (!prepared.HasReplacement())
    {
        prepared.adopted = true;
        return {};
    }

    auto retiredBufferSet =
        std::static_pointer_cast<RetiredFrameBufferSet>(
            prepared.retiredResource);
    retiredBufferSet->bufferSet = std::move(lightBuffer);
    lightBuffer = std::move(prepared.replacement);
    maxLightCount = prepared.capacity;
    prepared.adopted = true;
    return std::move(prepared.retiredResource);
}

void RendererFrameResources::UpdateGlobalUniformBuffer(
    vk::CommandBuffer& commandBuffer,
    uint32_t swapChainImageIndex,
    const UBOGlobal& uboGlobal)
{
    if (!initialized || swapChainImageIndex >= globalUniformBuffer.buffers.size())
    {
        throw std::runtime_error("RendererFrameResources global UBO is not initialized for this swapchain image.");
    }

    vk::Buffer buffer = globalUniformBuffer.buffers[swapChainImageIndex];

    // environmentSH 由 GPU 在代际 commit 时广播；常规 frame upload 永远跳过
    // GPU-owned 区间，避免任何 pass 意外把全部 swapchain 的新 SH 覆盖掉。
    UpdateBufferExcludingRanges(
        commandBuffer,
        buffer,
        &uboGlobal,
        sizeof(UBOGlobal),
        kUboGlobalGpuOwnedRanges);

    vk::BufferMemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(buffer)
        .setOffset(0)
        .setSize(sizeof(UBOGlobal));

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);
}

void RendererFrameResources::UpdateLightBuffer(
    uint32_t swapChainImageIndex,
    const std::vector<LightSnapshot>& lights)
{
    if (lights.size() > maxLightCount)
    {
        throw std::runtime_error("RendererFrameResources received more snapshot lights than its SSBO capacity.");
    }
    if (swapChainImageIndex >= lightBuffer.buffersMapped.size())
    {
        throw std::runtime_error("RendererFrameResources light SSBO is not initialized for this swapchain image.");
    }

    std::vector<LightGPU> directionalLightGPUs;
    std::vector<LightGPU> pointLightGPUs;
    std::vector<LightGPU> spotLightGPUs;
    directionalLightGPUs.reserve(lights.size());
    pointLightGPUs.reserve(lights.size());
    spotLightGPUs.reserve(lights.size());

    // The SSBO groups lights by type. RenderScene owns the frozen source data;
    // frame resources only pack that snapshot into the shader layout.
    for (const LightSnapshot& light : lights)
    {
        if (light.type == LightSnapshotType::Directional)
        {
            directionalLightGPUs.push_back(BuildLightGPU(light));
        }
        else if (light.type == LightSnapshotType::Point)
        {
            pointLightGPUs.push_back(BuildLightGPU(light));
        }
        else if (light.type == LightSnapshotType::Spot)
        {
            spotLightGPUs.push_back(BuildLightGPU(light));
        }
    }

    const uint32_t directionalLightCount = static_cast<uint32_t>(directionalLightGPUs.size());
    const uint32_t pointLightCount = static_cast<uint32_t>(pointLightGPUs.size());
    const uint32_t spotLightCount = static_cast<uint32_t>(spotLightGPUs.size());

    LightSSBOHeader lightSSBOHeader{};
    lightSSBOHeader.directionalLightOffset = 0;
    lightSSBOHeader.directionalLightCount = static_cast<int>(directionalLightCount);
    lightSSBOHeader.pointLightOffset = static_cast<int>(directionalLightCount);
    lightSSBOHeader.pointLightCount = static_cast<int>(pointLightCount);
    lightSSBOHeader.spotLightOffset = static_cast<int>(directionalLightCount + pointLightCount);
    lightSSBOHeader.spotLightCount = static_cast<int>(spotLightCount);

    uint32_t offset = 0;
    uint8_t* data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, &lightSSBOHeader, sizeof(LightSSBOHeader));

    offset += sizeof(LightSSBOHeader);
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, directionalLightGPUs.data(), sizeof(LightGPU) * directionalLightCount);

    offset += sizeof(LightGPU) * directionalLightCount;
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, pointLightGPUs.data(), sizeof(LightGPU) * pointLightCount);

    offset += sizeof(LightGPU) * pointLightCount;
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, spotLightGPUs.data(), sizeof(LightGPU) * spotLightCount);
}

void RendererFrameResources::UpdateMaterialInstanceUniformBuffer(
    uint32_t swapChainImageIndex,
    MaterialInstance& materialInstance)
{
    const auto& parameters = materialInstance.GetParameters();

    std::shared_ptr<Material> baseMaterial = materialInstance.GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        return;
    }

    const ShaderBinding* uboBinding =
        baseMaterial->GetMaterialDescriptorSchema().FindBinding(0);

    if (uboBinding == nullptr)
    {
        return;
    }
    if (swapChainImageIndex >= materialInstance.GetUboMaterialInstanceMapped().size())
    {
        throw std::runtime_error("Material instance UBO is not initialized for this swapchain image.");
    }

    for (size_t memberIndex = 0; memberIndex < uboBinding->memberNames.size(); ++memberIndex)
    {
        const std::string& memberName = uboBinding->memberNames[memberIndex];
        auto paramIt = parameters.find(memberName);
        if (paramIt == parameters.end())
        {
            continue;
        }

        const ParamMap& parameter = paramIt->second;
        if (memberIndex >= uboBinding->memberOffsets.size())
        {
            throw std::runtime_error("Material schema is missing a UBO member offset.");
        }
        // 使用 schema 的 std140 offset，而不是把 CPU 參數尺寸連續相加。
        // 這保證 Base 與 ShadowDepth 共用同一份 MI UBO layout。
        const uint32_t offset = uboBinding->memberOffsets[memberIndex];
        uint8_t* uboData =
            static_cast<uint8_t*>(materialInstance.GetUboMaterialInstanceMapped()[swapChainImageIndex]) +
            offset;
        if (parameter.type == ParamType::Float)
        {
            const float value = materialInstance.GetParameter<float>(memberName);
            std::memcpy(uboData, &value, parameter.size);
        }
        else if (parameter.type == ParamType::Vec2)
        {
            const Eigen::Vector2f value = materialInstance.GetParameter<Eigen::Vector2f>(memberName);
            std::memcpy(uboData, &value, parameter.size);
        }
        else if (parameter.type == ParamType::Vec3)
        {
            const Eigen::Vector3f value = materialInstance.GetParameter<Eigen::Vector3f>(memberName);
            std::memcpy(uboData, &value, parameter.size);
        }
        else if (parameter.type == ParamType::Vec4)
        {
            const Eigen::Vector4f value = materialInstance.GetParameter<Eigen::Vector4f>(memberName);
            std::memcpy(uboData, &value, parameter.size);
        }
    }
}

void RendererFrameResources::UpdateObjectUniformBuffer(
    uint32_t swapChainImageIndex,
    RendererObjectGpuResources& objectResources,
    const RenderDrawPacket& drawPacket,
    const SpeedTreeWindStateGPU* speedTreeWindState)
{
    if (swapChainImageIndex >= objectResources.objectUniformBuffer.buffersMapped.size())
    {
        throw std::runtime_error("Object UBO is not initialized for this swapchain image.");
    }

    UBOModel ubo{};
    ubo.model = drawPacket.model;
    ubo.previousModel = drawPacket.previousModel;
    if (speedTreeWindState != nullptr)
    {
        ubo.speedTreeWind = *speedTreeWindState;
        ubo.speedTreeWind.windVector.head<3>() =
            TransformSpeedTreeWindDirectionToLocal(
                drawPacket.speedTreeWorldToLocalDirection,
                speedTreeWindState->windVector.head<3>());
    }
    ubo.selectionData.x() = drawPacket.selected ? 1.0f : 0.0f;
    std::memcpy(
        objectResources.objectUniformBuffer.buffersMapped[swapChainImageIndex],
        &ubo,
        sizeof(ubo));
}

const std::vector<vk::DescriptorBufferInfo>& RendererFrameResources::GetGlobalUniformBufferInfos() const
{
    return globalUniformBuffer.bufferInfos;
}

const std::vector<vk::DescriptorBufferInfo>& RendererFrameResources::GetLightBufferInfos() const
{
    return lightBuffer.bufferInfos;
}

void RendererFrameResources::CreateLightBuffer(
    size_t requestedLightCount,
    RendererBackendVulkan& rendererBackend)
{
    // Descriptor refresh happens after EnsureLightCapacity returns. The old
    // SSBO can still be referenced by already submitted frames, so growth uses
    // the retire queue instead of a whole-device wait.
    RetireFrameBufferSet(rendererBackend, lightBuffer, "FrameLocalLightSSBO");
    maxLightCount = requestedLightCount;
    lightBuffer = CreateLightBufferSet(
        requestedLightCount,
        rendererBackend);
}

Buffer RendererFrameResources::CreateLightBufferSet(
    size_t requestedLightCount,
    RendererBackendVulkan& rendererBackend)
{
    Buffer bufferSet;
    const vk::DeviceSize lightSSBOSize =
        sizeof(LightSSBOHeader) +
        sizeof(LightGPU) * requestedLightCount;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;

    rendererBackend.CreatePerSwapchainBufferSet(
        bufferSet,
        lightSSBOSize,
        usage,
        memoryPropertyFlags,
        "SSBO_Light");
    return bufferSet;
}

void RendererFrameResources::DestroyLightBuffer(RendererBackendVulkan& rendererBackend)
{
    if (!lightBuffer.HasResources())
    {
        return;
    }

    rendererBackend.DestroyBufferSet(lightBuffer);
    maxLightCount = 0;
}

} // namespace VL
