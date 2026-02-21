#include "renderGraph.h"
#include "commonFunction.h"
#include "vulkanManager.h"
#include "sceneLoader.h"
#include "shaderReflect.h"
#include "material.h"
#include "materialInstance.h"
#include "renderPipline.h"
#include "renderSystem.h"
#include "lightManager.h"
#include <stdint.h>
#include "profiler.h"

void Renderpass::Draw(vk::CommandBuffer& commandBuffer) const
{
        PROFILE_FUNCTION();
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
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    inputDescriptorImageInfos.clear();
    inputDescriptorImageInfos.resize(swapChainImageCount);
    // 总览：为每个输入资源建立 DescriptorImageInfo
    // 1) 先从 resolve 里找，找不到再从 msaa 里找
    // 2) shadowMap 使用深度只读 layout
    // 3) 写入 inputDescriptorImageInfos
    // 设置image信息,根据pass的输入资源创建
    for(uint32_t imageIndex = 0; imageIndex < swapChainImageCount; ++imageIndex)
    {
        for(auto& inputResource : inputResources)
        {
            const RenderResource* resource = nullptr;
            
            auto& resolveMap = renderGraph.GetResourcesResolve();
            auto resolveIt = resolveMap.find(inputResource);
            if(resolveIt != resolveMap.end() && imageIndex < resolveIt->second.size())
            {
                resource = &resolveIt->second[imageIndex];
            }
            else
            {
                auto& msaaMap = renderGraph.GetResourcesMsaa();
                auto msaaIt = msaaMap.find(inputResource);
                if(msaaIt != msaaMap.end() && imageIndex < msaaIt->second.size())
                {
                    resource = &msaaIt->second[imageIndex];
                }
            }

            if(resource == nullptr)
            {
                continue;
            }

            vk::ImageLayout imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            if(CommonFunction::IsDepthFormat(resource->format))
            {
                imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            }

            vk::DescriptorImageInfo imageInfo = vk::DescriptorImageInfo();
            imageInfo
                .setImageLayout(imageLayout)
                .setImageView(resource->imageView)
                .setSampler(resource->sampler);
            inputDescriptorImageInfos[imageIndex].push_back(imageInfo);
        }
    }
}

void Renderpass::CreatePassDescriptorSetLayout()
{
    vk::Device& device = VulkanManager::GetInstance().GetDevice();

    // 创建空的 DescriptorSetLayout，用于绑定空的 DescriptorSet, 主要用于geometrypass对齐使用，渲染时能用PassSetIndex调用到passSet
    vk::DescriptorSetLayoutCreateInfo emptyLayoutCreateInfo;
    vk::Result result = device.createDescriptorSetLayout(&emptyLayoutCreateInfo, nullptr, &emptyDescriptorSetLayout);
    assert(result == vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    if(!materialInstance.expired())
    {
        auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
        const auto& shaderBindings = baseMaterial->GetRenderPipline()->GetShaderBindings();
        for(const auto& binding : shaderBindings)
        {
            if(binding.set != PassSetIndex)
            {
                continue;
            }
            vk::DescriptorSetLayoutBinding layoutBinding;
            layoutBinding
                .setBinding(binding.binding)
                .setDescriptorType(binding.type)
                .setDescriptorCount(1)
                .setStageFlags(binding.stageFlags)
                .setPImmutableSamplers(nullptr);
            descriptorSetLayoutBindings.push_back(layoutBinding);
        }
    }
    else
    {
        uint32_t inputCount = 0;
        if(!inputDescriptorImageInfos.empty())
        {
            inputCount = static_cast<uint32_t>(inputDescriptorImageInfos[0].size());
        }
        for(uint32_t i = 0; i < inputCount; ++i)
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
    }

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindings(descriptorSetLayoutBindings);

    result = device.createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayout);
    assert(result == vk::Result::eSuccess);
}

void Renderpass::CreateDescriptorSets()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    std::vector<vk::DescriptorSetLayout> allocateLayouts;
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    if (!materialInstance.expired()) //geometryPass没有MaterialInstance,需要额外处理
    {
        auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
        const std::vector<ShaderBinding>& shaderBindings = baseMaterial->GetRenderPipline()->GetShaderBindings();
        // std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
        for(const auto& binding : shaderBindings)
        {
            vk::DescriptorPoolSize poolSize;
            poolSize
                .setType(binding.type)
                .setDescriptorCount(swapChainImageCount);
            descriptorPoolSizes.push_back(poolSize);
        }
        const auto& pipelineSetLayouts = baseMaterial->GetRenderPipline()->GetDescriptorSetLayouts();
        
        // 只分配前4个Set (0:Global, 1:Material, 2:Object)，Set 3 (Pass)
        // std::vector<vk::DescriptorSetLayout> allocateLayouts;
        for(size_t i = 0; i < pipelineSetLayouts.size(); ++i)
        {
            // if(i != PassSetIndex)
            {
                allocateLayouts.push_back(pipelineSetLayouts[i]);
            }
        }
    }
    else {
        uint32_t inputCount = 0;
        if(!inputDescriptorImageInfos.empty())
        {
            inputCount = static_cast<uint32_t>(inputDescriptorImageInfos[0].size());
        }
        uint32_t descriptorCount = swapChainImageCount * MAX_DESCRIPTOR_SETS * inputCount;
        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(vk::DescriptorType::eCombinedImageSampler)
            .setDescriptorCount(descriptorCount);
        descriptorPoolSizes.push_back(poolSize);
        
        // 分配空的 DescriptorSetLayout
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(descriptorSetLayout); //保持passDescriptorSetLayout在最后
    }
    uint32_t SetLayoutCount = allocateLayouts.size();

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount * SetLayoutCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(allocateLayouts);
    
    descriptorSets.resize(swapChainImageCount);
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        descriptorSets[i].resize(SetLayoutCount);
        result = vulkanManager.GetDevice().allocateDescriptorSets(&descriptorSetAllocateInfo, descriptorSets[i].data());
        assert(result == vk::Result::eSuccess);
    }
}

void Renderpass::UpdateDescriptorSets()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置descriptor set信息
    writeDescriptorSets.resize(swapChainImageCount);
    
    if(!materialInstance.expired())
    {
        auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
        const auto& shaderBindings = baseMaterial->GetRenderPipline()->GetShaderBindings();
        auto& renderSystem = RenderSystem::GetInstance();
        auto& lightManager = LightManager::GetInstance();
        for(uint32_t i = 0; i < swapChainImageCount; i++)
        {
            for(const auto& binding : shaderBindings)
            {
                // if(binding.set == PassSetIndex)
                // {
                //     continue;
                // }

                vk::WriteDescriptorSet write;
                write
                    .setDstSet(descriptorSets[i][binding.set])
                    .setDstBinding(binding.binding)
                    .setDescriptorCount(1)
                    .setDescriptorType(binding.type);

                if (binding.type == vk::DescriptorType::eUniformBuffer)
                {
                    if (binding.set == GlobalSetIndex)
                    {
                        write.setBufferInfo(renderSystem.GetUBOGlobalBufferInfo()[i]);
                    }
                    else if (binding.set == MaterialSetIndex)
                    {
                        // write.setBufferInfo(materialInstance.lock()->GetUboMaterialInstanceInfo()[i]);
                        // TODO: 待后续后处理传参再看
                        continue;
                    }
                    else if (binding.set == ObjectSetIndex)
                    {
                        // 这里数据用sceneObject
                        // write.setBufferInfo(uboModel.bufferInfos[i]);
                        continue;
                    }
                }
                else if (binding.type == vk::DescriptorType::eStorageBuffer)
                {
                    write.setBufferInfo(lightManager.GetLightBufferInfo()[i]);
                }
                else if (binding.type == vk::DescriptorType::eCombinedImageSampler)
                {
                    if (binding.set != PassSetIndex)
                    {
                        write.setImageInfo(materialInstance.lock()->GetUboMaterialInstanceImageInfo(binding.name));
                    }
                    else {
                        // pass需要根据binding index来找到PassInputResource
                        uint32_t inputIndex = binding.binding;
                        if(i >= inputDescriptorImageInfos.size() || inputIndex >= inputDescriptorImageInfos[i].size())
                        {
                            continue;
                        }
                        write.setPImageInfo(&inputDescriptorImageInfos[i][inputIndex]);
                    }
                }

                writeDescriptorSets[i].push_back(write);
            }
            
            VulkanManager::GetInstance().GetDevice().updateDescriptorSets(writeDescriptorSets[i], nullptr);
        }
    }
    else {
        vk::Device& device = VulkanManager::GetInstance().GetDevice();
        std::vector<vk::WriteDescriptorSet> writes;
        for(uint32_t i = 0; i < swapChainImageCount; i++)
        {
            for(uint32_t j = 0; j < inputDescriptorImageInfos[i].size(); j++)
            {
                vk::WriteDescriptorSet write;
                write
                    .setDstSet(descriptorSets[i][PassSetIndex])
                    .setDstBinding(j)
                    .setDstArrayElement(0)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1)
                    .setPImageInfo(&inputDescriptorImageInfos[i][j]);
                writes.push_back(write);
            }
            device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

RenderGraph::~RenderGraph()
{
    // 销毁msaa,resolve资源
    for(auto& [name, resources] : resourcesMsaa)
    {
        for(auto& resource : resources)
        {
            DestroyRenderResource(resource);
        }
    }
    for(auto& [name, resources] : resourcesResolve)
    {
        for(auto& resource : resources)
        {
            DestroyRenderResource(resource);
        }
    }

    // 销毁renderpass
    for(auto& [name, renderpass] : renderpasses)
    {
        DestroyRenderpass(renderpass);
    }
}

void RenderGraph::LoadRenderGraph(const nlohmann::json& renderGraphJson)
{

    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 解析渲染资源
    for (const auto& resourceNode : renderGraphJson["resources"])
    {
        std::string name = resourceNode["name"];
        if(name == "swapChain") continue;

        // 先建立msaa资源
        if (name != "shadowMap")
        {
            std::vector<RenderResource> msaaResources;
            msaaResources.reserve(swapChainImageCount);
            for(uint32_t i = 0; i < swapChainImageCount; ++i)
            {
                RenderResource resource = CreateRenderResource(resourceNode, true);
                msaaResources.push_back(std::move(resource));
            }
            resourcesMsaa.emplace(name, std::move(msaaResources));
        }

        // 再建立resolve资源
        std::vector<RenderResource> resolveResources;
        resolveResources.reserve(swapChainImageCount);
        for(uint32_t i = 0; i < swapChainImageCount; ++i)
        {
            RenderResource resourceResolve = CreateRenderResource(resourceNode);
            resolveResources.push_back(std::move(resourceResolve));
        }
        resourcesResolve.emplace(name, std::move(resolveResources));
    }
    // 解析渲染pass
    for (const auto& passNode : renderGraphJson["passes"])
    {
        std::string passName = passNode["name"];
        renderpassesOrdered.push_back(passName);
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
    bool bUseMsaa = passNode.value("needMsaa", false);

    std::vector<std::string> inputResources = GetRenderpassInputResources(passNode["input"]);
    std::vector<std::string> outputResources = GetRenderpassOutputResources(passNode["output"]);
    renderpass.inputResources = inputResources;
    renderpass.outputResources = outputResources;

    vk::RenderPass vkRenderPass = CreateVkRenderPass(inputResources, outputResources, bUseMsaa);
    renderpass.renderPass = vkRenderPass;
    
    if (outputResources[0] == "swapChain")
    {
        auto extent = VulkanManager::GetInstance().GetSwapChainExtent();
        renderpass.width = extent.width;
        renderpass.height = extent.height;
    }
    else if (bIsShadowPass)
    {
        renderpass.width = resourcesResolve["shadowMap"][0].width;
        renderpass.height = resourcesResolve["shadowMap"][0].height;
    }
    else
    {
        renderpass.width = resourcesMsaa[outputResources[0]][0].width;
        renderpass.height = resourcesMsaa[outputResources[0]][0].height;
    }
    
    renderpass.clearValues = GetClearValues(outputResources, bUseMsaa);
    renderpass.framebuffers = CreateVkFrameBuffers(renderpass, inputResources, outputResources, bUseMsaa);
    
    return renderpass;
}

void RenderGraph::DestroyRenderpass(Renderpass& renderpass)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();

    DestroyVkRenderPass(renderpass.renderPass);
    DestroyVkFrameBuffers(renderpass.framebuffers);
    device.destroyDescriptorSetLayout(renderpass.descriptorSetLayout);
    device.destroyDescriptorSetLayout(renderpass.emptyDescriptorSetLayout);
    device.destroyDescriptorPool(renderpass.descriptorPool);
}

vk::RenderPass RenderGraph::CreateVkRenderPass(std::vector<std::string>& inputResources, std::vector<std::string>& outputResources, bool bUseMsaa)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    vk::SurfaceFormatKHR& surfaceFormat = vulkanManager.GetSurfaceFormat();
    vk::SampleCountFlagBits sampleCount = bUseMsaa ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1;

    // 总览：按输出资源顺序构建 RenderPass
    // 1) 颜色附件（MSAA/非MSAA）
    // 2) 如启用 MSAA，追加 resolve 附件
    // 3) 如包含深度资源，追加 depth 附件
    // 4) 构建 subpass 与依赖
    //创建渲染通道
    std::vector<vk::AttachmentDescription> attachmentDescriptions;
    std::vector<vk::AttachmentReference> colorAttachmentReferences;
    std::vector<vk::AttachmentReference> resolveAttachmentReferences;
    vk::AttachmentReference depthAttachmentReference;

    bool bHasDepth = false;
    auto isDepthResource = [&](const std::string& resourceName) -> bool
    {
        auto msaaIt = resourcesMsaa.find(resourceName);
        if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(msaaIt->second[0].format);
        }
        auto resolveIt = resourcesResolve.find(resourceName);
        if (resolveIt != resourcesResolve.end() && !resolveIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(resolveIt->second[0].format);
        }
        return false;
    };

    // 1. Color Attachments (MSAA & Resolve)
    for (const auto& resourceName : outputResources)
    { 
        if (isDepthResource(resourceName))
        {
            bHasDepth = true;
            continue;
        }

        vk::AttachmentDescription colorAttachmentDescription;
        vk::Format format = vk::Format::eUndefined;
        if (resourceName == "swapChain") 
        {
            format = surfaceFormat.format;
        }
        else{
            format = resourcesMsaa.at(resourceName)[0].format;
        }

        // MSAA / Main Attachment
        colorAttachmentDescription
            .setFormat(format)
            .setSamples(sampleCount)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);

        if (resourceName == "swapChain")
        {
            colorAttachmentDescription.setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        }
        
        attachmentDescriptions.push_back(colorAttachmentDescription);

        vk::AttachmentReference attachmentReference;
        attachmentReference.setAttachment(colorAttachmentReferences.size()); // This index is temporary, need global index
        attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
        colorAttachmentReferences.push_back(attachmentReference);
    }
    
    // Fix up attachment indices for Color Refs
    // Current attachmentDescriptions size is colorAttachmentReferences.size()

    // 2. Resolve Attachments (Only if MSAA is enabled)
    if (bUseMsaa)
    {
        for (const auto& resourceName : outputResources)
        {
            if (isDepthResource(resourceName)) continue;

            vk::AttachmentDescription resolveAttachmentDescription;
            vk::Format format = (resourceName == "swapChain") ? surfaceFormat.format : resourcesMsaa.at(resourceName)[0].format;
            
            resolveAttachmentDescription
                .setFormat(format)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setLoadOp(vk::AttachmentLoadOp::eClear)
                .setStoreOp(vk::AttachmentStoreOp::eStore)
                .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
                .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                .setInitialLayout(vk::ImageLayout::eUndefined)
                .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);

            if (resourceName == "swapChain") 
            {
                resolveAttachmentDescription.setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
            }

            attachmentDescriptions.push_back(resolveAttachmentDescription);

            vk::AttachmentReference attachmentReference;
            attachmentReference.setAttachment(attachmentDescriptions.size() - 1);
            attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
            resolveAttachmentReferences.push_back(attachmentReference);
        }
    }

    // 3. Depth Attachment
    if (bHasDepth)
    {
        // Find depth resource
        std::string depthName = "";
        for(const auto& resource : outputResources)
        {
            if (isDepthResource(resource))
            {
                depthName = resource;
                break;
            }
        }

        vk::AttachmentDescription depthStencilAttachmentDescription;
        vk::Format depthFormat = vk::Format::eUndefined;
        
        if(bUseMsaa) 
        {
            depthFormat = resourcesMsaa[depthName][0].format;
        }
        else {
            depthFormat = resourcesResolve[depthName][0].format;
        }

        depthStencilAttachmentDescription
            .setFormat(depthFormat)
            .setSamples(sampleCount)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        
        attachmentDescriptions.push_back(depthStencilAttachmentDescription);

        depthAttachmentReference.setAttachment(attachmentDescriptions.size() - 1);
        depthAttachmentReference.setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
    }

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
    if(bHasDepth)
    {
        subpassDescription.setPDepthStencilAttachment(&depthAttachmentReference);
    }

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

std::vector<vk::Framebuffer> RenderGraph::CreateVkFrameBuffers(Renderpass renderPass, std::vector<std::string>& inputResources, std::vector<std::string>& outputResources, bool bUseMsaa)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();
    
    // 总览：attachments 顺序需与 RenderPass 保持一致
    // 1) (可选) MSAA 颜色视图
    // 2) resolve/非MSAA 颜色视图
    // 3) 深度视图
    auto isDepthResource = [&](const std::string& resourceName) -> bool
    {
        auto msaaIt = resourcesMsaa.find(resourceName);
        if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(msaaIt->second[0].format);
        }
        auto resolveIt = resourcesResolve.find(resourceName);
        if (resolveIt != resourcesResolve.end() && !resolveIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(resolveIt->second[0].format);
        }
        return false;
    };

    // 确定framebuffer数量，含swapchain，数量需要等于swapChainImageCount，否者为1
    uint32_t framebufferSize = swapChainImageCount;

    std::vector<vk::Framebuffer> framebuffers;
    framebuffers.resize(framebufferSize);

    for (uint32_t i = 0; i < framebufferSize; i++)
    {
        std::vector<vk::ImageView> attachments;
        
        // 1. MSAA view (Only if MSAA enabled)
        if (bUseMsaa)
        {
            for (const auto& outputResource : outputResources)
            {
                if (isDepthResource(outputResource)) continue;
                attachments.push_back(resourcesMsaa[outputResource][i].imageView);
            }
        }

        // 2. Resolve view (or Color view if MSAA disabled)
        for (const auto& outputResource : outputResources)
        {
            if (isDepthResource(outputResource)) continue;
            
            if (outputResource == "swapChain")
            {
                attachments.push_back(vulkanManager.GetSwapChainImageViews()[i]);
            }
            else{
                attachments.push_back(resourcesResolve[outputResource][i].imageView);
            }
        }

        // 3. Depth view (Only if present in output)
        std::string depthName = "";
        for(const auto& r : outputResources)
        {
            if (isDepthResource(r))
            {
                depthName = r;
                break;
            }
        }

        if(!depthName.empty())
        {
            if(bUseMsaa) 
            {
                attachments.push_back(resourcesMsaa[depthName][i].imageView);
            }
            else {
                attachments.push_back(resourcesResolve[depthName][i].imageView);
            }
        }

        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            .setRenderPass(renderPass.renderPass)
            .setAttachments(attachments)
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

std::vector<vk::ClearValue> RenderGraph::GetClearValues(std::vector<std::string>& outputResources, bool bUseMsaa)
{
    std::vector<vk::ClearValue> clearValues;
    vk::ClearValue clearColor;
    clearColor.setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f}));
    vk::ClearValue clearDepth;
    clearDepth.setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));
    // 总览：清除值顺序需与 RenderPass attachments 顺序一致
    // 1) 颜色（MSAA/非MSAA）
    // 2) 仅在启用 MSAA 时追加 resolve 颜色
    // 3) 若包含深度资源，追加深度清除
    auto isDepthResource = [&](const std::string& resourceName) -> bool
    {
        auto msaaIt = resourcesMsaa.find(resourceName);
        if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(msaaIt->second[0].format);
        }
        auto resolveIt = resourcesResolve.find(resourceName);
        if (resolveIt != resourcesResolve.end() && !resolveIt->second.empty())
        {
            return CommonFunction::IsDepthFormat(resolveIt->second[0].format);
        }
        return false;
    };

    // 1. Color Attachments (MSAA & Resolve)
    for (const auto& resourceName : outputResources)
    {
        if (isDepthResource(resourceName)) continue;

        // MSAA / Main Attachment
        clearValues.push_back(clearColor);
    }
    
    // 2. Resolve Attachments
    if (bUseMsaa)
    {
        for (const auto& resourceName : outputResources)
        {
            if (isDepthResource(resourceName)) continue;
            
            clearValues.push_back(clearColor);
        }
    }

    // 3. Depth Attachment
    bool bHasDepth = false;
    for(const auto& r : outputResources)
    {
        if (isDepthResource(r))
        {
            bHasDepth = true;
            break;
        }
    }
    if (bHasDepth)
    {
        clearValues.push_back(clearDepth);
    }

    return clearValues;
}
