#include "ui/uiOverlayRendererVulkan.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "commonFunction.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/reload/computePipelineReloadParticipant.h"
#include "vulkanDebug.h"

namespace VL
{

namespace
{

constexpr vk::DeviceSize InitialVertexBufferCapacity = 256 * 1024;
constexpr vk::DeviceSize InitialIndexBufferCapacity = 128 * 1024;
constexpr uint32_t MaxUiTextures = 2048;

vk::DeviceSize NextBufferCapacity(vk::DeviceSize requiredSize, vk::DeviceSize initialCapacity)
{
    vk::DeviceSize capacity = initialCapacity;
    while (capacity < requiredSize)
    {
        capacity *= 2;
    }
    return capacity;
}

vk::ShaderModule CreateShaderModule(
    RendererBackendVulkan& backend,
    const std::string& path,
    const std::string& debugName)
{
    const std::string shaderCode = CommonFunction::ReadFile(path);
    if (shaderCode.empty())
    {
        throw std::runtime_error("UI shader is empty: " + path);
    }

    vk::ShaderModuleCreateInfo createInfo;
    createInfo
        .setCodeSize(shaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(shaderCode.data()));
    return backend.CreateShaderModule(createInfo, debugName);
}

vk::ShaderModule CreateShaderModuleFromSpirv(
    RendererBackendVulkan& backend,
    const std::vector<uint32_t>& spirv,
    const std::string& debugName)
{
    if (spirv.empty())
    {
        throw std::runtime_error("UI shader SPIR-V is empty: " + debugName);
    }

    vk::ShaderModuleCreateInfo createInfo;
    createInfo
        .setCodeSize(spirv.size() * sizeof(uint32_t))
        .setPCode(spirv.data());
    return backend.CreateShaderModule(createInfo, debugName);
}

class UiPipelineBuildGuard
{
public:
    explicit UiPipelineBuildGuard(RendererBackendVulkan& backend)
        : backend(backend)
    {
    }

    ~UiPipelineBuildGuard()
    {
        if (!armed)
        {
            return;
        }
        if (straightAlphaPipeline)
        {
            backend.DestroyPipeline(straightAlphaPipeline);
        }
        if (straightAlphaPipelineCache)
        {
            backend.DestroyPipelineCache(straightAlphaPipelineCache);
        }
        if (premultipliedAlphaPipeline)
        {
            backend.DestroyPipeline(premultipliedAlphaPipeline);
        }
        if (premultipliedAlphaPipelineCache)
        {
            backend.DestroyPipelineCache(
                premultipliedAlphaPipelineCache);
        }
        if (vertexShader)
        {
            backend.DestroyShaderModule(vertexShader);
        }
        if (fragmentShader)
        {
            backend.DestroyShaderModule(fragmentShader);
        }
    }

    void DestroyShaderModules() noexcept
    {
        if (vertexShader)
        {
            backend.DestroyShaderModule(vertexShader);
        }
        if (fragmentShader)
        {
            backend.DestroyShaderModule(fragmentShader);
        }
    }

    void Disarm() noexcept
    {
        armed = false;
    }

    vk::ShaderModule vertexShader;
    vk::ShaderModule fragmentShader;
    vk::Pipeline straightAlphaPipeline;
    vk::PipelineCache straightAlphaPipelineCache;
    vk::Pipeline premultipliedAlphaPipeline;
    vk::PipelineCache premultipliedAlphaPipelineCache;

private:
    RendererBackendVulkan& backend;
    bool armed = true;
};

class UiPreparedReplacementGuard
{
public:
    UiPreparedReplacementGuard(
        RendererBackendVulkan& backend,
        UiOverlayPipelineReplacement& replacement)
        : backend(backend),
          replacement(replacement)
    {
    }

    ~UiPreparedReplacementGuard()
    {
        if (!armed)
        {
            return;
        }
        if (replacement.straightAlphaPipeline)
        {
            backend.DestroyPipeline(
                replacement.straightAlphaPipeline);
        }
        if (replacement.straightAlphaPipelineCache)
        {
            backend.DestroyPipelineCache(
                replacement.straightAlphaPipelineCache);
        }
        if (replacement.premultipliedAlphaPipeline)
        {
            backend.DestroyPipeline(
                replacement.premultipliedAlphaPipeline);
        }
        if (replacement.premultipliedAlphaPipelineCache)
        {
            backend.DestroyPipelineCache(
                replacement.premultipliedAlphaPipelineCache);
        }
    }

    void Disarm() noexcept
    {
        armed = false;
    }

private:
    RendererBackendVulkan& backend;
    UiOverlayPipelineReplacement& replacement;
    bool armed = true;
};

} // namespace

void UiOverlayRendererVulkan::Initialize(
    RendererBackendVulkan& rendererBackend,
    std::string initializeVertexShaderPath,
    std::string initializeFragmentShaderPath)
{
    Shutdown();
    backend = &rendererBackend;
    vertexShaderPath = std::move(initializeVertexShaderPath);
    fragmentShaderPath = std::move(initializeFragmentShaderPath);
    CreateDescriptorResources();
    CreateSwapchainResources();
    EnsureWhiteTexture();
    initialized = true;
}

void UiOverlayRendererVulkan::Shutdown()
{
    if (backend == nullptr)
    {
        initialized = false;
        return;
    }

    DestroySwapchainResources();
    for (auto& textureEntry : textures)
    {
        DestroyTexture(textureEntry.second);
    }
    textures.clear();
    FlushRetiredTextures();
    DestroyDescriptorResources();
    backend = nullptr;
    vertexShaderPath.clear();
    fragmentShaderPath.clear();
    initialized = false;
}

void UiOverlayRendererVulkan::ReleaseSwapchainDependentResources()
{
    if (backend == nullptr)
    {
        return;
    }
    DestroySwapchainResources();
    FlushRetiredTextures();
}

void UiOverlayRendererVulkan::RebuildSwapchainDependentResources()
{
    if (backend == nullptr)
    {
        return;
    }
    CreateSwapchainResources();
}

void UiOverlayRendererVulkan::SynchronizeTextures(const UiRenderSnapshot& snapshot)
{
    if (backend == nullptr)
    {
        return;
    }

    EnsureWhiteTexture();
    std::unordered_set<UiTextureId> activeTextureIds;
    activeTextureIds.insert(0);
    for (const UiTextureSnapshot& textureSnapshot : snapshot.textures)
    {
        if (textureSnapshot.id == 0 || textureSnapshot.rgba8Pixels == nullptr)
        {
            continue;
        }
        activeTextureIds.insert(textureSnapshot.id);

        auto existingTexture = textures.find(textureSnapshot.id);
        if (existingTexture != textures.end() &&
            existingTexture->second.generation == textureSnapshot.generation)
        {
            continue;
        }

        if (existingTexture != textures.end())
        {
            RetireTexture(existingTexture->second);
            textures.erase(existingTexture);
        }
        textures.emplace(textureSnapshot.id, CreateTexture(textureSnapshot));
    }

    for (auto textureIt = textures.begin(); textureIt != textures.end();)
    {
        if (activeTextureIds.find(textureIt->first) != activeTextureIds.end())
        {
            ++textureIt;
            continue;
        }
        RetireTexture(textureIt->second);
        textureIt = textures.erase(textureIt);
    }
}

void UiOverlayRendererVulkan::CollectRetiredTextures()
{
    const uint64_t completedEpoch =
        ResourceRetireQueue::GetInstance().GetLastCompletedEpoch();
    size_t destinationIndex = 0;
    for (size_t sourceIndex = 0; sourceIndex < retiredTextures.size(); ++sourceIndex)
    {
        RetiredTexture& retiredTexture = retiredTextures[sourceIndex];
        if (retiredTexture.lastUsedEpoch <= completedEpoch)
        {
            DestroyTexture(retiredTexture.texture);
            continue;
        }

        if (destinationIndex != sourceIndex)
        {
            retiredTextures[destinationIndex] = std::move(retiredTexture);
        }
        ++destinationIndex;
    }
    retiredTextures.resize(destinationIndex);
}

void UiOverlayRendererVulkan::Record(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex,
    const UiRenderSnapshot& snapshot)
{
    if (!initialized || snapshot.drawCommands.empty())
    {
        return;
    }

    FrameBuffers& buffers = frameBuffers.at(swapchainImageIndex);
    const vk::DeviceSize vertexBytes = snapshot.vertices.size() * sizeof(UiVertex);
    const vk::DeviceSize indexBytes = snapshot.indices.size() * sizeof(uint32_t);
    EnsureBufferCapacity(
        buffers.vertices,
        vertexBytes,
        vk::BufferUsageFlagBits::eVertexBuffer,
        "UI Vertex Buffer " + std::to_string(swapchainImageIndex));
    EnsureBufferCapacity(
        buffers.indices,
        indexBytes,
        vk::BufferUsageFlagBits::eIndexBuffer,
        "UI Index Buffer " + std::to_string(swapchainImageIndex));

    UiVertex* destinationVertices = static_cast<UiVertex*>(buffers.vertices.mappedMemory);
    const float inverseWidth = 1.0f / static_cast<float>(snapshot.viewportWidth);
    const float inverseHeight = 1.0f / static_cast<float>(snapshot.viewportHeight);
    for (size_t vertexIndex = 0; vertexIndex < snapshot.vertices.size(); ++vertexIndex)
    {
        destinationVertices[vertexIndex] = snapshot.vertices[vertexIndex];
        destinationVertices[vertexIndex].position[0] =
            snapshot.vertices[vertexIndex].position[0] * inverseWidth * 2.0f - 1.0f;
        destinationVertices[vertexIndex].position[1] =
            snapshot.vertices[vertexIndex].position[1] * inverseHeight * 2.0f - 1.0f;
    }
    std::memcpy(buffers.indices.mappedMemory, snapshot.indices.data(), static_cast<size_t>(indexBytes));

    vk::RenderPassBeginInfo renderPassBeginInfo;
    renderPassBeginInfo
        .setRenderPass(renderPass)
        .setFramebuffer(framebuffers.at(swapchainImageIndex))
        .setRenderArea(vk::Rect2D({0, 0}, backend->GetSwapchainExtent()));
    commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

    const vk::Viewport viewport(
        0.0f,
        0.0f,
        static_cast<float>(snapshot.viewportWidth),
        static_cast<float>(snapshot.viewportHeight),
        0.0f,
        1.0f);
    commandBuffer.setViewport(0, viewport);
    commandBuffer.bindVertexBuffers(0, buffers.vertices.buffer, vk::DeviceSize{0});
    commandBuffer.bindIndexBuffer(buffers.indices.buffer, 0, vk::IndexType::eUint32);

    UiBlendMode boundBlendMode = UiBlendMode::StraightAlpha;
    bool pipelineBound = false;
    for (const UiDrawCommand& drawCommand : snapshot.drawCommands)
    {
        if (drawCommand.clipRect.width == 0 || drawCommand.clipRect.height == 0)
        {
            continue;
        }

        if (!pipelineBound || boundBlendMode != drawCommand.blendMode)
        {
            boundBlendMode = drawCommand.blendMode;
            const vk::Pipeline pipeline = boundBlendMode == UiBlendMode::PremultipliedAlpha ?
                premultipliedAlphaPipeline : straightAlphaPipeline;
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            pipelineBound = true;
        }

        const vk::Rect2D scissor(
            vk::Offset2D(drawCommand.clipRect.x, drawCommand.clipRect.y),
            vk::Extent2D(drawCommand.clipRect.width, drawCommand.clipRect.height));
        commandBuffer.setScissor(0, scissor);

        const vk::DescriptorSet descriptorSet = ResolveTextureDescriptor(drawCommand.textureId);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipelineLayout,
            0,
            descriptorSet,
            {});
        commandBuffer.drawIndexed(
            drawCommand.indexCount,
            1,
            drawCommand.firstIndex,
            0,
            0);
    }
    commandBuffer.endRenderPass();
}

void UiOverlayRendererVulkan::CreateDescriptorResources()
{
    vk::DescriptorSetLayoutBinding textureBinding;
    textureBinding
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eFragment);
    vk::DescriptorSetLayoutCreateInfo layoutCreateInfo;
    layoutCreateInfo.setBindings(textureBinding);
    descriptorSetLayout = backend->CreateDescriptorSetLayout(
        layoutCreateInfo,
        "UI Texture Descriptor Set Layout");

    vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, MaxUiTextures);
    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setMaxSets(MaxUiTextures)
        .setPoolSizes(poolSize);
    descriptorPool = backend->CreateDescriptorPool(poolCreateInfo, "UI Texture Descriptor Pool");

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo.setSetLayouts(descriptorSetLayout);
    pipelineLayout = backend->CreatePipelineLayout(
        pipelineLayoutCreateInfo,
        "UI Overlay Pipeline Layout");
}

void UiOverlayRendererVulkan::DestroyDescriptorResources()
{
    if (pipelineLayout)
    {
        backend->DestroyPipelineLayout(pipelineLayout);
    }
    if (descriptorPool)
    {
        backend->DestroyDescriptorPool(descriptorPool);
    }
    if (descriptorSetLayout)
    {
        backend->DestroyDescriptorSetLayout(descriptorSetLayout);
    }
}

void UiOverlayRendererVulkan::CreateSwapchainResources()
{
    CreateRenderPassAndFramebuffers();
    CreatePipelines();
    CreateFrameBuffers();
}

void UiOverlayRendererVulkan::DestroySwapchainResources()
{
    DestroyFrameBuffers();
    DestroyPipelines();
    DestroyRenderPassAndFramebuffers();
}

void UiOverlayRendererVulkan::CreateRenderPassAndFramebuffers()
{
    vk::AttachmentDescription2 colorAttachment;
    colorAttachment
        .setFormat(backend->GetSwapchainImageFormat())
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eLoad)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::ePresentSrcKHR)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);

    vk::AttachmentReference2 colorReference;
    colorReference
        .setAttachment(0)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setAspectMask(vk::ImageAspectFlagBits::eColor);

    vk::SubpassDescription2 subpass;
    subpass
        .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        .setColorAttachments(colorReference);

    vk::SubpassDependency2 dependency;
    dependency
        .setSrcSubpass(VK_SUBPASS_EXTERNAL)
        .setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite)
        .setDependencyFlags(vk::DependencyFlagBits::eByRegion);

    vk::RenderPassCreateInfo2 renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(colorAttachment)
        .setSubpasses(subpass)
        .setDependencies(dependency);
    renderPass = backend->CreateRenderPass(renderPassCreateInfo, "UI Overlay Render Pass");

    const vk::Extent2D extent = backend->GetSwapchainExtent();
    const std::vector<vk::ImageView>& imageViews = backend->GetSwapchainImageViews();
    framebuffers.reserve(imageViews.size());
    for (size_t imageIndex = 0; imageIndex < imageViews.size(); ++imageIndex)
    {
        vk::ImageView attachment = imageViews[imageIndex];
        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            .setRenderPass(renderPass)
            .setAttachments(attachment)
            .setWidth(extent.width)
            .setHeight(extent.height)
            .setLayers(1);
        framebuffers.push_back(backend->CreateFramebuffer(
            framebufferCreateInfo,
            "UI Overlay Framebuffer " + std::to_string(imageIndex)));
    }
}

void UiOverlayRendererVulkan::DestroyRenderPassAndFramebuffers()
{
    backend->DestroyFramebuffers(framebuffers);
    framebuffers.clear();
    if (renderPass)
    {
        backend->DestroyRenderPass(renderPass);
    }
}

void UiOverlayRendererVulkan::CreatePipelines()
{
    const std::string vertexCode = CommonFunction::ReadFile(vertexShaderPath);
    const std::string fragmentCode = CommonFunction::ReadFile(fragmentShaderPath);
    if (vertexCode.empty() || fragmentCode.empty())
    {
        throw std::runtime_error("UI overlay shader is empty");
    }
    std::vector<uint32_t> vertexSpirv(
        reinterpret_cast<const uint32_t*>(vertexCode.data()),
        reinterpret_cast<const uint32_t*>(
            vertexCode.data() + vertexCode.size()));
    std::vector<uint32_t> fragmentSpirv(
        reinterpret_cast<const uint32_t*>(fragmentCode.data()),
        reinterpret_cast<const uint32_t*>(
            fragmentCode.data() + fragmentCode.size()));
    UiOverlayPipelineReplacement replacement =
        BuildUiOverlayPipelinePair(vertexSpirv, fragmentSpirv);
    straightAlphaPipelineCache =
        replacement.straightAlphaPipelineCache;
    straightAlphaPipeline =
        replacement.straightAlphaPipeline;
    premultipliedAlphaPipelineCache =
        replacement.premultipliedAlphaPipelineCache;
    premultipliedAlphaPipeline =
        replacement.premultipliedAlphaPipeline;
}

UiOverlayPipelineReplacement
UiOverlayRendererVulkan::BuildUiOverlayPipelinePair(
    const std::vector<uint32_t>& vertexSpirv,
    const std::vector<uint32_t>& fragmentSpirv)
{
    UiPipelineBuildGuard buildGuard(*backend);
    buildGuard.vertexShader = CreateShaderModuleFromSpirv(
        *backend,
        vertexSpirv,
        "UI Overlay Vertex Shader");
    buildGuard.fragmentShader = CreateShaderModuleFromSpirv(
        *backend,
        fragmentSpirv,
        "UI Overlay Fragment Shader");

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages(2);
    shaderStages[0]
        .setStage(vk::ShaderStageFlagBits::eVertex)
        .setModule(buildGuard.vertexShader)
        .setPName("main");
    shaderStages[1]
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setModule(buildGuard.fragmentShader)
        .setPName("main");

    vk::VertexInputBindingDescription bindingDescription(
        0,
        sizeof(UiVertex),
        vk::VertexInputRate::eVertex);
    std::vector<vk::VertexInputAttributeDescription> attributes;
    attributes.emplace_back(0, 0, vk::Format::eR32G32Sfloat, offsetof(UiVertex, position));
    attributes.emplace_back(1, 0, vk::Format::eR32G32Sfloat, offsetof(UiVertex, texCoord));
    attributes.emplace_back(2, 0, vk::Format::eR8G8B8A8Unorm, offsetof(UiVertex, color));

    GraphicsPipelineStateDesc state;
    state.bUseVertexInput = true;
    state.bDepthTestEnable = false;
    state.bDepthWriteEnable = false;
    state.cullMode = vk::CullModeFlagBits::eNone;

    state.blendMode = GraphicsPipelineBlendMode::AlphaBlend;
    GraphicsPipelineBuildDesc straightDesc{
        renderPass,
        pipelineLayout,
        shaderStages,
        bindingDescription,
        attributes,
        "UI Overlay Straight Alpha",
        vk::SampleCountFlagBits::e1,
        1,
        state,
        false
    };
    GraphicsPipelineBuildResult straightResult =
        backend->BuildGraphicsPipeline(straightDesc);
    buildGuard.straightAlphaPipelineCache = straightResult.pipelineCache;
    buildGuard.straightAlphaPipeline = straightResult.graphicsPipeline;

    state.blendMode = GraphicsPipelineBlendMode::PremultipliedAlpha;
    GraphicsPipelineBuildDesc premultipliedDesc{
        renderPass,
        pipelineLayout,
        shaderStages,
        bindingDescription,
        attributes,
        "UI Overlay Premultiplied Alpha",
        vk::SampleCountFlagBits::e1,
        1,
        state,
        false
    };
    GraphicsPipelineBuildResult premultipliedResult =
        backend->BuildGraphicsPipeline(premultipliedDesc);
    buildGuard.premultipliedAlphaPipelineCache =
        premultipliedResult.pipelineCache;
    buildGuard.premultipliedAlphaPipeline =
        premultipliedResult.graphicsPipeline;

    UiOverlayPipelineReplacement replacement;
    replacement.straightAlphaPipelineCache =
        buildGuard.straightAlphaPipelineCache;
    replacement.straightAlphaPipeline =
        buildGuard.straightAlphaPipeline;
    replacement.premultipliedAlphaPipelineCache =
        buildGuard.premultipliedAlphaPipelineCache;
    replacement.premultipliedAlphaPipeline =
        buildGuard.premultipliedAlphaPipeline;

    buildGuard.DestroyShaderModules();
    buildGuard.Disarm();
    return replacement;
}

std::string UiOverlayRendererVulkan::GetShaderName() const
{
    return "uiOverlay";
}

UiOverlayPipelineReplacement
UiOverlayRendererVulkan::PrepareReplacementPipelines(
    const GraphicsShaderVariantArtifact& candidate)
{
    if (!initialized || backend == nullptr)
    {
        throw std::runtime_error(
            "Cannot prepare UI overlay replacement before initialization");
    }
    UiOverlayPipelineReplacement replacement =
        BuildUiOverlayPipelinePair(
        candidate.vertexSpirv,
        candidate.fragmentSpirv);
    UiPreparedReplacementGuard replacementGuard(
        *backend,
        replacement);
    replacement.release =
        [backend = backend,
         straight = replacement.straightAlphaPipeline,
         straightCache = replacement.straightAlphaPipelineCache,
         premultiplied = replacement.premultipliedAlphaPipeline,
         premultipliedCache = replacement.premultipliedAlphaPipelineCache]()
        {
            if (straight)
            {
                vk::Pipeline resource = straight;
                backend->DestroyPipeline(resource);
            }
            if (straightCache)
            {
                vk::PipelineCache resource = straightCache;
                backend->DestroyPipelineCache(resource);
            }
            if (premultiplied)
            {
                vk::Pipeline resource = premultiplied;
                backend->DestroyPipeline(resource);
            }
            if (premultipliedCache)
            {
                vk::PipelineCache resource = premultipliedCache;
                backend->DestroyPipelineCache(resource);
            }
        };
    replacement.retirement =
        MakePreparedRetiredResourcePackage(
            [backend = backend,
             straight = straightAlphaPipeline,
             straightCache = straightAlphaPipelineCache,
             premultiplied = premultipliedAlphaPipeline,
             premultipliedCache = premultipliedAlphaPipelineCache]()
            {
                if (straight)
                {
                    vk::Pipeline resource = straight;
                    backend->DestroyPipeline(resource);
                }
                if (straightCache)
                {
                    vk::PipelineCache resource =
                        straightCache;
                    backend->DestroyPipelineCache(resource);
                }
                if (premultiplied)
                {
                    vk::Pipeline resource =
                        premultiplied;
                    backend->DestroyPipeline(resource);
                }
                if (premultipliedCache)
                {
                    vk::PipelineCache resource =
                        premultipliedCache;
                    backend->DestroyPipelineCache(resource);
                }
            });
    replacementGuard.Disarm();
    return replacement;
}

void UiOverlayRendererVulkan::CommitReplacement(
    GraphicsShaderVariantArtifact committedArtifact,
    UiOverlayPipelineReplacement&& replacement) noexcept
{
    activeShaderArtifact = std::move(committedArtifact);
    straightAlphaPipeline =
        replacement.straightAlphaPipeline;
    straightAlphaPipelineCache =
        replacement.straightAlphaPipelineCache;
    premultipliedAlphaPipeline =
        replacement.premultipliedAlphaPipeline;
    premultipliedAlphaPipelineCache =
        replacement.premultipliedAlphaPipelineCache;
}

void UiOverlayRendererVulkan::DestroyPipelines()
{
    if (straightAlphaPipeline)
    {
        backend->DestroyPipeline(straightAlphaPipeline);
    }
    if (straightAlphaPipelineCache)
    {
        backend->DestroyPipelineCache(
            straightAlphaPipelineCache);
    }
    if (premultipliedAlphaPipeline)
    {
        backend->DestroyPipeline(
            premultipliedAlphaPipeline);
    }
    if (premultipliedAlphaPipelineCache)
    {
        backend->DestroyPipelineCache(
            premultipliedAlphaPipelineCache);
    }
}

void UiOverlayRendererVulkan::CreateFrameBuffers()
{
    frameBuffers.resize(backend->GetSwapchainImageCount());
    for (size_t imageIndex = 0; imageIndex < frameBuffers.size(); ++imageIndex)
    {
        EnsureBufferCapacity(
            frameBuffers[imageIndex].vertices,
            InitialVertexBufferCapacity,
            vk::BufferUsageFlagBits::eVertexBuffer,
            "UI Vertex Buffer " + std::to_string(imageIndex));
        EnsureBufferCapacity(
            frameBuffers[imageIndex].indices,
            InitialIndexBufferCapacity,
            vk::BufferUsageFlagBits::eIndexBuffer,
            "UI Index Buffer " + std::to_string(imageIndex));
    }
}

void UiOverlayRendererVulkan::DestroyFrameBuffers()
{
    for (FrameBuffers& buffers : frameBuffers)
    {
        DestroyDynamicBuffer(buffers.vertices);
        DestroyDynamicBuffer(buffers.indices);
    }
    frameBuffers.clear();
}

void UiOverlayRendererVulkan::EnsureBufferCapacity(
    DynamicBuffer& buffer,
    vk::DeviceSize requiredSize,
    vk::BufferUsageFlags usage,
    const std::string& debugName)
{
    if (requiredSize == 0 || buffer.capacity >= requiredSize)
    {
        return;
    }

    DestroyDynamicBuffer(buffer);
    const vk::DeviceSize initialCapacity = usage == vk::BufferUsageFlagBits::eVertexBuffer ?
        InitialVertexBufferCapacity : InitialIndexBufferCapacity;
    buffer.capacity = NextBufferCapacity(requiredSize, initialCapacity);
    auto bufferResource = backend->CreateBuffer(
        buffer.capacity,
        usage,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        debugName);
    buffer.buffer = bufferResource.first;
    buffer.memory = bufferResource.second;
    buffer.mappedMemory = backend->MapMemory(buffer.memory, buffer.capacity);
}

void UiOverlayRendererVulkan::DestroyDynamicBuffer(DynamicBuffer& buffer)
{
    if (buffer.mappedMemory != nullptr)
    {
        backend->UnmapMemory(buffer.memory);
        buffer.mappedMemory = nullptr;
    }
    if (buffer.buffer || buffer.memory)
    {
        backend->DestroyBuffer(buffer.buffer, buffer.memory);
    }
    buffer = DynamicBuffer{};
}

UiOverlayRendererVulkan::GpuTexture UiOverlayRendererVulkan::CreateTexture(
    const UiTextureSnapshot& snapshot)
{
    const vk::DeviceSize imageSize =
        static_cast<vk::DeviceSize>(snapshot.width) *
        static_cast<vk::DeviceSize>(snapshot.height) * 4;
    auto stagingResource = backend->CreateBuffer(
        imageSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "UI Texture Staging: " + snapshot.source);
    void* stagingMemory = backend->MapMemory(stagingResource.second, imageSize);
    std::memcpy(stagingMemory, snapshot.rgba8Pixels->data(), static_cast<size_t>(imageSize));
    backend->UnmapMemory(stagingResource.second);

    GpuTexture texture;
    texture.id = snapshot.id;
    texture.generation = snapshot.generation;
    auto imageResource = backend->CreateImage(
        snapshot.width,
        snapshot.height,
        1,
        vk::SampleCountFlagBits::e1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "UI Texture: " + snapshot.source);
    texture.image = imageResource.first;
    texture.memory = imageResource.second;
    backend->TransitionImageLayout(
        texture.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal);
    backend->CopyBufferToImage(
        stagingResource.first,
        texture.image,
        snapshot.width,
        snapshot.height);
    backend->TransitionImageLayout(
        texture.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal);
    backend->DestroyBuffer(stagingResource.first, stagingResource.second);

    texture.imageView = backend->Create2DImageView(
        texture.image,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageAspectFlagBits::eColor,
        "UI Texture View: " + snapshot.source);
    texture.sampler = backend->Create2DSampler(
        vk::Filter::eLinear,
        vk::SamplerAddressMode::eClampToEdge,
        false,
        "UI Texture Sampler: " + snapshot.source);

    vk::DescriptorSetAllocateInfo allocateInfo;
    allocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(descriptorSetLayout);
    std::vector<vk::DescriptorSet> allocatedSets(1);
    backend->AllocateDescriptorSets(allocateInfo, allocatedSets);
    texture.descriptorSet = allocatedSets[0];
    backend->SetDescriptorSetDebugName(
        texture.descriptorSet,
        "UI Texture Descriptor: " + snapshot.source);

    vk::DescriptorImageInfo imageInfo(
        texture.sampler,
        texture.imageView,
        vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::WriteDescriptorSet write;
    write
        .setDstSet(texture.descriptorSet)
        .setDstBinding(0)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(imageInfo);
    backend->UpdateDescriptorSets({write});
    return texture;
}

void UiOverlayRendererVulkan::DestroyTexture(GpuTexture& texture)
{
    if (texture.descriptorSet && descriptorPool)
    {
        backend->FreeDescriptorSet(descriptorPool, texture.descriptorSet);
    }
    if (texture.image || texture.memory || texture.imageView || texture.sampler)
    {
        backend->DestroyImageResource(
            texture.image,
            texture.memory,
            texture.imageView,
            texture.sampler);
    }
    texture = GpuTexture{};
}

void UiOverlayRendererVulkan::RetireTexture(GpuTexture& texture)
{
    RetiredTexture retiredTexture;
    retiredTexture.texture = texture;
    retiredTexture.lastUsedEpoch =
        ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch();
    retiredTextures.push_back(std::move(retiredTexture));
    texture = GpuTexture{};
}

void UiOverlayRendererVulkan::FlushRetiredTextures()
{
    for (RetiredTexture& retiredTexture : retiredTextures)
    {
        DestroyTexture(retiredTexture.texture);
    }
    retiredTextures.clear();
}

vk::DescriptorSet UiOverlayRendererVulkan::ResolveTextureDescriptor(UiTextureId textureId) const
{
    auto textureIt = textures.find(textureId);
    if (textureIt == textures.end())
    {
        textureIt = textures.find(0);
    }
    return textureIt->second.descriptorSet;
}

void UiOverlayRendererVulkan::EnsureWhiteTexture()
{
    if (textures.find(0) != textures.end())
    {
        return;
    }

    std::shared_ptr<const std::vector<uint8_t>> whitePixels =
        std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 255, 255, 255});
    UiTextureSnapshot whiteTexture;
    whiteTexture.id = 0;
    whiteTexture.generation = 1;
    whiteTexture.width = 1;
    whiteTexture.height = 1;
    whiteTexture.source = "builtin:white";
    whiteTexture.rgba8Pixels = std::move(whitePixels);
    textures.emplace(0, CreateTexture(whiteTexture));
}

} // namespace VL
