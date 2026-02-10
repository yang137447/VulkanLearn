#include "renderGraph.h"
#include "commonFunction.h"
#include "vulkanManager.h"

void Renderpass::Draw(vk::CommandBuffer& commandBuffer) const
{
        vk::Viewport viewport;
        viewport
            .setX(0.0f)
            .setY(0.0f)
            .setWidth(static_cast<float>(width))
            .setHeight(static_cast<float>(height))
            .setMinDepth(0.0f)
            .setMaxDepth(1.0f);
        vk::Rect2D scissor;
        scissor
            .setOffset({ 0, 0 })
            .setExtent({ 
                static_cast<uint32_t>(width), 
                static_cast<uint32_t>(height) });
        commandBuffer.setViewport(0, 1, &viewport);
        commandBuffer.setScissor(0, 1, &scissor);
        commandBuffer.draw(3, 1, 0,0);
}

void Renderpass::CreateUniformBuffers()
{
    //对于pass,目前暂时没有uniform buffer
}
void Renderpass::SetupDescriptors(RenderGraph& renderGraph)
{
    // 设置image信息,根据pass的输入资源创建
    for(auto& inputResource : inputResources)
    {
        const RenderResource* resource = nullptr;
        vk::ImageLayout imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        if(inputResource == "depthMap")
        {
            resource = &renderGraph.GetDepthResourceResolve();
        }
        else if(inputResource == "shadowMap")
        {
            resource = &renderGraph.GetShadowMap();
            imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        }
        else
        {
            resource = &renderGraph.GetColorResourcesResolve().at(inputResource);
        }

        vk::DescriptorImageInfo imageInfo = vk::DescriptorImageInfo();
        imageInfo
            .setImageLayout(imageLayout)
            .setImageView(resource->imageView)
            .setSampler(resource->sampler);
        inputDescriptorImageInfos.push_back(imageInfo);
    }
}

void Renderpass::CreatePassDescriptorSetLayout()
{
    vk::Device& device = VulkanManager::GetInstance().GetDevice();

    std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    for(uint32_t i = 0; i < inputDescriptorImageInfos.size(); ++i)
    {
        vk::DescriptorSetLayoutBinding layoutBinding;
        layoutBinding
            .setBinding(i)
            .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eFragment)
            .setPImmutableSamplers(nullptr);
        descriptorSetLayoutBindings.push_back(layoutBinding);
    }

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindings(descriptorSetLayoutBindings);

    vk::Result result = device.createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayout);
    assert(result == vk::Result::eSuccess);
}

void Renderpass::CreateDescriptorSets()
{
    // 暂时没有max frame in flight相关的数据，所以descriptor set数量为1
    VulkanManager& vulkanManager = VulkanManager::GetInstance();

    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    for(const auto& imageInfo : inputDescriptorImageInfos)
    {
        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(vk::DescriptorType::eCombinedImageSampler)
            .setDescriptorCount(1);
        descriptorPoolSizes.push_back(poolSize);
    }
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(1)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayout> setLayouts(1, descriptorSetLayout);
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    
    inputDescriptorSets.resize(1);
    result = vulkanManager.GetDevice().allocateDescriptorSets(&descriptorSetAllocateInfo, inputDescriptorSets.data());
    assert(result == vk::Result::eSuccess);
}

void Renderpass::UpdateDescriptorSets()
{
    vk::Device& device = VulkanManager::GetInstance().GetDevice();
    std::vector<vk::WriteDescriptorSet> writes;
    for(uint32_t i = 0; i < inputDescriptorImageInfos.size(); ++i)
    {
        vk::WriteDescriptorSet write;
        write
            .setDstSet(inputDescriptorSets[0])
            .setDstBinding(i)
            .setDstArrayElement(0)
            .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
            .setDescriptorCount(1)
            .setPImageInfo(&inputDescriptorImageInfos[i]);
        writes.push_back(write);
    }
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

RenderGraph::~RenderGraph()
{
    // 销毁msaa,resolve资源
    for(auto& [name, resource] : colorResourcesMsaa)
    {
        DestroyRenderResource(resource);
    }
    for(auto& [name, resource] : colorResourcesResolve)
    {
        DestroyRenderResource(resource);
    }
    DestroyRenderResource(depthResourceMsaa);
    DestroyRenderResource(depthResourceResolve);
    DestroyRenderResource(shadowMap);
    // 销毁renderpass
    for(auto& [name, renderpass] : renderpasses)
    {
        DestroyRenderpass(renderpass);
    }
}

void RenderGraph::LoadRenderGraph(const nlohmann::json& renderGraphJson)
{

    // 解析渲染资源
    for (const auto& resourceNode : renderGraphJson["resources"])
    {
        // 先建立msaa资源
        if (resourceNode["name"] == "sceneDepth")
        {
            // 创建深度缓冲区
            depthResourceMsaa = CreateRenderResource(resourceNode, true);
        }
        else if (resourceNode["name"] == "shadowMap")
        {
            // 创建阴影贴图, 不需要msaa
        }
        else {
            RenderResource resource = CreateRenderResource(resourceNode, true);
            colorResourcesMsaa.emplace(resource.name, std::move(resource));
        }

        // 再建立resolve资源
        if (resourceNode["name"] == "swapChain")
        {
            // swapchain 的资源属于交换链，这里不再创建
        }
        else if (resourceNode["name"] == "sceneDepth")
        {
            depthResourceResolve = CreateRenderResource(resourceNode);
        }
        else if (resourceNode["name"] == "shadowMap")
        {
            shadowMap = CreateRenderResource(resourceNode);
        }
        else {
            RenderResource resourceResolve = CreateRenderResource(resourceNode);
            colorResourcesResolve.emplace(resourceResolve.name, std::move(resourceResolve));
        }
    }
    // 解析渲染pass
    for (const auto& passNode : renderGraphJson["passes"])
    {
        std::string passName = passNode["name"];
        renderpassOrdered.push_back(passName);
        Renderpass renderpass = CreateRenderpass(passNode);
        renderpasses[passName] = renderpass;
    }
}

void RenderGraph::RenderInitialize()
{
    for(auto& [passName, renderpass] : renderpasses)
    {
        renderpass.CreateUniformBuffers();
        renderpass.SetupDescriptors(*this);
        renderpass.CreatePassDescriptorSetLayout();
        renderpass.CreateDescriptorSets();
        renderpass.UpdateDescriptorSets();
    }
}

RenderResource RenderGraph::CreateRenderResource(const nlohmann::json& resourceNode, bool bIsMsaaSource)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    vk::CommandPool& commandPool = vulkanManager.GetCommandPool();
    vk::Queue& graphicQueue = vulkanManager.GetGraphicQueue();
    auto& physicalDevice = vulkanManager.GetPhysicalDevice();
    auto& gpuMemoryProperties = vulkanManager.GetGpuMemoryProperties();

    RenderResource resource;

    resource.name = resourceNode["name"];

    resource.format = GetFormat(resourceNode["format"]);

    bool bIsDepthFormat = CommonFunction::IsDepthFormat(resource.format);

    resource.width = CommonFunction::ParserRenderResourceSize(resourceNode).x();
    resource.height = CommonFunction::ParserRenderResourceSize(resourceNode).y();

    vk::ImageUsageFlags usage = GetImageUsage(resourceNode["usage"]);

    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    std::tie(resource.image, resource.memory) = CommonFunction::CreateImage(
        device, resource.width, resource.height,
        1, 
        // 这里根据是否是msaa源图，来确定是否需要msaa采样
        bIsMsaaSource ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1,
        resource.format, tiling, usage,
        gpuMemoryProperties, memoryPropertyFlags);
    if(bIsDepthFormat)
    {
        CommonFunction::TransitionImageLayout(
            resource.image, 
            1, 
            resource.format, 
            device, 
            commandPool, 
            graphicQueue, 
            vk::ImageLayout::eUndefined, 
            vk::ImageLayout::eDepthStencilAttachmentOptimal);
    }
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
    if(bIsDepthFormat)
    {
        aspect = vk::ImageAspectFlagBits::eDepth;
        if(CommonFunction::HasStencilComponent(resource.format))
        {
            // TODO:这里有问题，看后续怎么修复stencil
            // aspect |= vk::ImageAspectFlagBits::eStencil;
        }
    }
    resource.imageView = CommonFunction::CreateImageView(
        device, 
        resource.image, 
        1, 
        resource.format,
        aspect);

    resource.sampler = CommonFunction::CreateSampler(device, physicalDevice, bIsDepthFormat);

    return resource;
}

void RenderGraph::DestroyRenderResource(RenderResource& resource)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();

    device.destroyImageView(resource.imageView);
    device.freeMemory(resource.memory);
    device.destroyImage(resource.image);
    device.destroySampler(resource.sampler);
}

Renderpass RenderGraph::CreateRenderpass(const nlohmann::json& passNode)
{
    Renderpass renderpass;
    renderpass.name = passNode["name"];
    bool bIsShadowPass = renderpass.name == "shadow";

    std::vector<std::string> inputResources = GetRenderpassInputResources(passNode["input"]);
    std::vector<std::string> outputResources = GetRenderpassOutputResources(passNode["output"]);
    renderpass.inputResources = inputResources;
    renderpass.outputResources = outputResources;

    vk::RenderPass vkRenderPass = CreateVkRenderPass(inputResources, outputResources, bIsShadowPass);
    renderpass.renderPass = vkRenderPass;
    
    renderpass.width = bIsShadowPass ? shadowMap.width : colorResourcesMsaa[outputResources[0]].width;
    renderpass.height = bIsShadowPass ? shadowMap.height : colorResourcesMsaa[outputResources[0]].height;
    
    renderpass.clearValues = GetClearValues(outputResources);
    renderpass.framebuffers = CreateVkFrameBuffers(renderpass, inputResources, outputResources);
    
    return renderpass;
}

void RenderGraph::DestroyRenderpass(Renderpass& renderpass)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();

    DestroyVkRenderPass(renderpass.renderPass);
    DestroyVkFrameBuffers(renderpass.framebuffers);
    device.destroyDescriptorSetLayout(renderpass.descriptorSetLayout);
    device.destroyDescriptorPool(renderpass.descriptorPool);
}

vk::RenderPass RenderGraph::CreateVkRenderPass(std::vector<std::string>& inputResources, std::vector<std::string>& outputResources, bool bIsShadowPass)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    vk::SurfaceFormatKHR& surfaceFormat = vulkanManager.GetSurfaceFormat();
    vk::SampleCountFlagBits sampleCount = bIsShadowPass ? vk::SampleCountFlagBits::e1 : CommonFunction::GetMsaaSampleCount();

    //创建渲染通道
        // attachment，msaa附件、解析附件、深度附件
    std::vector<vk::AttachmentDescription> attachmentDescriptions;
    for (const auto& resourceName : outputResources)
    {
        if (resourceName == "sceneDepth" || resourceName == "shadowMap")
        {
            continue;
        }

        vk::AttachmentDescription colorAttachmentDescription;
        colorAttachmentDescription
            .setFormat(colorResourcesMsaa.at(resourceName).format)
            .setSamples(sampleCount)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setFlags(vk::AttachmentDescriptionFlags(0));

        attachmentDescriptions.push_back(colorAttachmentDescription);
    }
    for (const auto& resourceName : outputResources)
    {
        if (resourceName == "sceneDepth" || resourceName == "shadowMap")
        {
            continue;
        }

        vk::AttachmentDescription colorAttachmentDescription;
        colorAttachmentDescription
            .setFormat(surfaceFormat.format)
            .setSamples(vk::SampleCountFlagBits::e1)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setFlags(vk::AttachmentDescriptionFlags(0));
        if (resourceName == "swapChain")
        {
            colorAttachmentDescription
                .setFormat(surfaceFormat.format)
                .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        }
        attachmentDescriptions.push_back(colorAttachmentDescription);
    }
    vk::AttachmentDescription depthStencilAttachmentDescription;
    depthStencilAttachmentDescription
        .setFormat(depthResourceMsaa.format)
        .setSamples(sampleCount)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
        .setFlags(vk::AttachmentDescriptionFlags(0));
    attachmentDescriptions.push_back(depthStencilAttachmentDescription);
        // attachment reference，msaa附件
    std::vector<vk::AttachmentReference> colorAttachmentReferences;
    for (auto& resourceName : outputResources)
    {
        if (resourceName == "sceneDepth" || resourceName == "shadowMap")
        {
            continue;
        }
        vk::AttachmentReference attachmentReference;
        attachmentReference.setAttachment(colorAttachmentReferences.size());
        attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
        colorAttachmentReferences.push_back(attachmentReference);
    }
        // attachment reference，解析附件
    std::vector<vk::AttachmentReference> resolveAttachmentReferences;
    for (auto& resourceName : outputResources)
    {
        if (resourceName == "sceneDepth" || resourceName == "shadowMap")
        {
            continue;
        }
        vk::AttachmentReference attachmentReference;
        attachmentReference.setAttachment(colorAttachmentReferences.size() + resolveAttachmentReferences.size());
        attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
        resolveAttachmentReferences.push_back(attachmentReference);
    }
        // attachment reference，深度附件
    vk::AttachmentReference depthAttachmentReference;
    depthAttachmentReference.setAttachment(colorAttachmentReferences.size() + resolveAttachmentReferences.size());
    depthAttachmentReference.setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);

        // subpass
    vk::SubpassDescription subpassDescription;
    subpassDescription.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics);
    if(!colorAttachmentReferences.empty())
    {
        subpassDescription.setColorAttachments(colorAttachmentReferences);
    }
    if(!resolveAttachmentReferences.empty())
    {
        subpassDescription.setResolveAttachments(resolveAttachmentReferences);
    }
    subpassDescription.setPDepthStencilAttachment(&depthAttachmentReference);

        // subpass dependency
    vk::SubpassDependency subpassDependency;
    subpassDependency
        .setSrcSubpass(0)
        .setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDependencyFlags(vk::DependencyFlagBits::eByRegion);

    vk::RenderPassCreateInfo renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(attachmentDescriptions)
        .setSubpasses(subpassDescription)
        .setDependencies(subpassDependency);

    vk::RenderPass renderPass = device.createRenderPass(renderPassCreateInfo);
    assert(renderPass);

    return renderPass;
}

void RenderGraph::DestroyVkRenderPass(vk::RenderPass renderPass)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();

    device.destroyRenderPass(renderPass);
    std::cout << "Destroy VkRenderPass" << std::endl;
}

std::vector<vk::Framebuffer> RenderGraph::CreateVkFrameBuffers(Renderpass renderPass, std::vector<std::string>& inputResources, std::vector<std::string>& outputResources)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();

    bool bHasShadowMap = std::find(outputResources.begin(), outputResources.end(), "shadowMap") != outputResources.end();

    // 确定framebuffer数量，含swapchain，数量需要等于swapChainImageCount，否者为1
    uint32_t framebufferSize = 1;
    if (std::find(outputResources.begin(), outputResources.end(), std::string("swapChain")) != outputResources.end())
    {
        framebufferSize = swapChainImageCount;
    }

    std::vector<vk::Framebuffer> framebuffers;
    // attachments.resize(3);
    // attachments[0] = colorImageView;
    // attachments[1] = depthImageView;
    // attachments[2] = swapChainImageViews[0]; //占位，后面会被替换

    framebuffers.resize(framebufferSize);
    for (uint32_t i = 0; i < framebufferSize; i++)
    {
        std::vector<vk::ImageView> attachments;
        // msaa view
        for (const auto& outputResource : outputResources)
        {
            if (outputResource == "sceneDepth" || outputResource == "shadowMap")
            {
                continue;
            }
            attachments.push_back(colorResourcesMsaa[outputResource].imageView);
        }
        // resolve view
        for (const auto& outputResource : outputResources)
        {
            if (outputResource == "sceneDepth" || outputResource == "shadowMap")
            {
                continue;
            }
            else if (outputResource == "swapChain")
            {
                attachments.push_back(vulkanManager.GetSwapChainImageViews()[i]);
            }

            else{
                attachments.push_back(colorResourcesResolve[outputResource].imageView);
            }
        }
        // depth view
        if(bHasShadowMap)
        {
            // shadowPass中，深度输出到shadowMap, 后续pass中需要采样shadowMap
            attachments.push_back(shadowMap.imageView);
        }
        else {
            attachments.push_back(depthResourceMsaa.imageView);
        }

        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            .setRenderPass(renderPass.renderPass)
            .setAttachments(attachments)
            // 用输出资源的width和height，目前是为了适配shadowMap
            .setWidth(renderPass.width)
            .setHeight(renderPass.height)
            .setLayers(1);

        framebuffers[i] = device.createFramebuffer(framebufferCreateInfo);
        assert(framebuffers[i]);
    }

    return framebuffers;
}

void RenderGraph::DestroyVkFrameBuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    for (uint32_t i = 0; i < framebuffers.size(); i++)
    {
        device.destroyFramebuffer(framebuffers[i]);
    }
    std::cout << "Destroy VkFrameBuffers" << std::endl;
}

vk::Format RenderGraph::GetFormat(const std::string& formatStr)
{
    if (formatStr == "R8G8B8A8_SRGB")
    {
        return vk::Format::eR8G8B8A8Srgb;
    }
    else if (formatStr == "D32_SFLOAT")
    {
        return vk::Format::eD32Sfloat;
    }
    else if (formatStr == "R8G8B8A8_UNORM")
    {
        return vk::Format::eR8G8B8A8Unorm;
    }
    else
    {
        throw std::runtime_error("Unsupported format string");
    }
}

vk::ImageUsageFlags RenderGraph::GetImageUsage(const std::vector<std::string>& usageStr)
{
    vk::ImageUsageFlags imageUsage;
    for(const auto& usage : usageStr)
    {
        if (usage == "colorAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eColorAttachment;
        }
        else if (usage == "depthStencilAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }
        else if (usage == "sampled")
        {
            imageUsage |= vk::ImageUsageFlagBits::eSampled;
        }
        else if (usage == "transferSrc")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
        }
        else if (usage == "transferDst")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferDst;
        }
    }
    return imageUsage;
}

std::vector<std::string> RenderGraph::GetRenderpassInputResources(const nlohmann::json& inputNode)
{
    std::vector<std::string> inputResources;
    for(const auto& input : inputNode)
    {
        inputResources.push_back(input["resource"]);
    }
    return inputResources;
}

std::vector<std::string> RenderGraph::GetRenderpassOutputResources(const nlohmann::json& outputNode)
{
    std::vector<std::string> outputResources;
    for(const auto& output : outputNode)
    {
        outputResources.push_back(output["resource"]);
    }
    return outputResources;
}

std::vector<vk::ClearValue> RenderGraph::GetClearValues(std::vector<std::string>& outputResources)
{
    std::vector<vk::ClearValue> clearValues;
    // MSAA
    for(const auto& resource : outputResources)
    {
        if (resource == "sceneDepth" || resource == "shadowMap") continue;
        vk::ClearValue clearValue;
        clearValue.setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f}));
        clearValues.push_back(clearValue);
    }
    // Resolve
    for(const auto& resource: outputResources)
    {
        if (resource == "sceneDepth" || resource == "shadowMap") continue;
        vk::ClearValue clearValue;
        clearValue.setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f}));
        clearValues.push_back(clearValue);
    }
    // Depth or shadowMap
    vk::ClearValue clearValue;
    clearValue.setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));
    clearValues.push_back(clearValue);

    return clearValues;
}
