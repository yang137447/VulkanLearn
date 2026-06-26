#include "render/backend/rendererFrameResources.h"

#include <algorithm>
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
#include "render/frontend/renderScene.h"
#include "render/resource/resourceRetireQueue.h"
#include "shaderReflect.h"

namespace VL
{
namespace
{

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

void RendererFrameResources::Initialize(RendererBackendVulkan& rendererBackend)
{
    if (initialized)
    {
        return;
    }

    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;

    rendererBackend.CreatePerSwapchainBufferSet(
        globalUniformBuffer,
        sizeof(UBOGlobal),
        usage,
        memoryPropertyFlags,
        "UBO_Global");
    rendererBackend.CreatePerSwapchainBufferSet(
        skyParametersBuffer,
        sizeof(SkyParametersGPU),
        usage,
        memoryPropertyFlags,
        "UBO_SkyParameters");
    initialized = true;
}

void RendererFrameResources::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    DestroyLightBuffer(rendererBackend);
    rendererBackend.DestroyBufferSet(skyParametersBuffer);
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

void RendererFrameResources::UpdateGlobalUniformBuffer(
    vk::CommandBuffer& commandBuffer,
    uint32_t swapChainImageIndex,
    const UBOGlobal& uboGlobal)
{
    if (!initialized || swapChainImageIndex >= globalUniformBuffer.buffers.size())
    {
        throw std::runtime_error("RendererFrameResources global UBO is not initialized for this swapchain image.");
    }

    commandBuffer.updateBuffer(
        globalUniformBuffer.buffers[swapChainImageIndex],
        0,
        sizeof(UBOGlobal),
        &uboGlobal);

    vk::BufferMemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(globalUniformBuffer.buffers[swapChainImageIndex])
        .setOffset(0)
        .setSize(sizeof(UBOGlobal));

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);
}

void RendererFrameResources::UpdateSkyParametersBuffer(
    vk::CommandBuffer& commandBuffer,
    uint32_t swapChainImageIndex,
    const SkyParametersGPU& skyParameters)
{
    if (!initialized || swapChainImageIndex >= skyParametersBuffer.buffers.size())
    {
        throw std::runtime_error("RendererFrameResources sky parameters UBO is not initialized for this swapchain image.");
    }

    commandBuffer.updateBuffer(
        skyParametersBuffer.buffers[swapChainImageIndex],
        0,
        sizeof(SkyParametersGPU),
        &skyParameters);

    vk::BufferMemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(skyParametersBuffer.buffers[swapChainImageIndex])
        .setOffset(0)
        .setSize(sizeof(SkyParametersGPU));

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
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

    const std::vector<ShaderBinding>& bindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
    const ShaderBinding* uboBinding = nullptr;
    for (const ShaderBinding& binding : bindings)
    {
        if (binding.set == MaterialSetIndex &&
            binding.binding == 0 &&
            binding.type == vk::DescriptorType::eUniformBuffer)
        {
            uboBinding = &binding;
            break;
        }
    }

    if (uboBinding == nullptr)
    {
        return;
    }
    if (swapChainImageIndex >= materialInstance.GetUboMaterialInstanceMapped().size())
    {
        throw std::runtime_error("Material instance UBO is not initialized for this swapchain image.");
    }

    uint32_t offset = 0;
    for (size_t memberIndex = 0; memberIndex < uboBinding->memberNames.size(); ++memberIndex)
    {
        const std::string& memberName = uboBinding->memberNames[memberIndex];
        auto paramIt = parameters.find(memberName);
        if (paramIt == parameters.end())
        {
            // Reflection defines the shader layout even when a JSON material
            // instance omits a member. Advance by the reflected size so later
            // parameters still land at the correct byte offset.
            if (memberIndex < uboBinding->members.size())
            {
                offset += uboBinding->members[memberIndex];
            }
            continue;
        }

        const ParamMap& parameter = paramIt->second;
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

        offset += parameter.size;
    }
}

void RendererFrameResources::UpdateObjectUniformBuffer(
    uint32_t swapChainImageIndex,
    RendererObjectGpuResources& objectResources,
    const RenderDrawPacket& drawPacket)
{
    if (swapChainImageIndex >= objectResources.objectUniformBuffer.buffersMapped.size())
    {
        throw std::runtime_error("Object UBO is not initialized for this swapchain image.");
    }

    UBOModel ubo{};
    ubo.model = drawPacket.model;
    ubo.previousModel = drawPacket.previousModel;
    std::memcpy(
        objectResources.objectUniformBuffer.buffersMapped[swapChainImageIndex],
        &ubo,
        sizeof(ubo));
}

const std::vector<vk::DescriptorBufferInfo>& RendererFrameResources::GetGlobalUniformBufferInfos() const
{
    return globalUniformBuffer.bufferInfos;
}

const std::vector<vk::DescriptorBufferInfo>& RendererFrameResources::GetSkyParametersBufferInfos() const
{
    return skyParametersBuffer.bufferInfos;
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

    const vk::DeviceSize lightSSBOSize =
        sizeof(LightSSBOHeader) + sizeof(LightGPU) * maxLightCount;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;

    rendererBackend.CreatePerSwapchainBufferSet(
        lightBuffer,
        lightSSBOSize,
        usage,
        memoryPropertyFlags,
        "SSBO_Light");
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
