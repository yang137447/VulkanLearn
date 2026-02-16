#pragma once

#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <nlohmann/json.hpp>

//定义渲染资源
// render resource需要考虑msaa采样，pass之间传递时需要resolve, 作为 vksubpass 的 attachment传递无需resolve
// TODO: 先统一按msaa采样处理，都进行resolve，后续根据需要再优化
struct RenderResource
{
    std::string name;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView imageView;
    vk::Format format;
    vk::Sampler sampler;
    uint32_t width;
    uint32_t height;
};

struct Renderpass
{
    void Draw(vk::CommandBuffer& commandBuffer) const;

    void CreateUniformBuffers();
    void SetupDescriptors(class RenderGraph& renderGraph);
    void CreatePassDescriptorSetLayout();
    void CreateDescriptorSets();
    void UpdateDescriptorSets();

    const std::vector<std::vector<vk::DescriptorSet>>& GetDescriptorSets() const { return descriptorSets; }

    std::string name;
    vk::RenderPass renderPass;
    std::vector<vk::ClearValue> clearValues;
    std::vector<vk::Framebuffer> framebuffers;
    uint32_t width;
    uint32_t height;
    // pass 输入输出资源
    std::vector<std::string> inputResources;
    std::vector<std::string> outputResources;
    // pass 输入描述符集, 统一使用Set3
    vk::DescriptorPool descriptorPool;
    std::vector<std::vector<vk::DescriptorImageInfo>> inputDescriptorImageInfos;
    vk::DescriptorSetLayout emptyDescriptorSetLayout;
    vk::DescriptorSetLayout descriptorSetLayout;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;
    // materialInstance 相关
    std::weak_ptr<class MaterialInstance> materialInstance;
};

/**
 * @class RenderGraph
 * @brief 管理渲染管线的相关资源的创建、销毁和管理
 */
class RenderGraph
{
public:
    static RenderGraph& GetInstance()
    {
        static RenderGraph instance;
        return instance;
    }
    ~RenderGraph();
    void LoadRenderGraph(const nlohmann::json& renderGraphJson);
    std::vector<std::string>& GetRenderpassesOrdered() { return renderpassesOrdered; }
    std::unordered_map<std::string, Renderpass>& GetRenderpasses() { return renderpasses; }
    std::vector<RenderResource>& GetDepthResourceMsaa() { return resourcesMsaa["sceneDepth"]; }
    std::vector<RenderResource>& GetDepthResourceResolve() { return resourcesResolve["sceneDepth"]; }
    std::vector<RenderResource>& GetShadowMap() { return resourcesResolve["shadowMap"]; }
    std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesMsaa() { return resourcesMsaa; }
    std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesResolve() { return resourcesResolve; }
    // 渲染初始调用
    void RenderInitialize();
    
private:
    RenderResource CreateRenderResource(const nlohmann::json& resourceNode, bool bIsMsaaSource = false);
    void DestroyRenderResource(RenderResource& resource);
    
    Renderpass CreateRenderpass(const nlohmann::json& passNode);
    void DestroyRenderpass(Renderpass& renderpass);

    vk::RenderPass CreateVkRenderPass(std::vector<std::string>& inputResources, std::vector<std::string>& outputResources, bool bUseMsaa);
    void DestroyVkRenderPass(vk::RenderPass renderPass);

    std::vector<vk::Framebuffer> CreateVkFrameBuffers(Renderpass renderPass, std::vector<std::string>& inputResources, std::vector<std::string>& outputResources, bool bUseMsaa);
    void DestroyVkFrameBuffers(std::vector<vk::Framebuffer>& framebuffers);

    vk::Format GetFormat(const std::string& formatStr);
    vk::ImageUsageFlags GetImageUsage(const std::vector<std::string>& usageStr);

    std::vector<std::string> GetRenderpassInputResources(const nlohmann::json& inputNode);
    std::vector<std::string> GetRenderpassOutputResources(const nlohmann::json& outputNode);
    
    std::vector<vk::ClearValue> GetClearValues(std::vector<std::string>& outputResources, bool bUseMsaa);
private:
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesMsaa;
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesResolve;

    std::vector<std::string> renderpassesOrdered;
    std::unordered_map<std::string, Renderpass> renderpasses;
};
