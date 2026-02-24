#include "materialInstance.h"
#include "texture.h"
#include "material.h"
#include "renderPipline.h"
#include "vulkanManager.h"
#include "commonFunction.h"
#include "shaderReflect.h"

MaterialInstance::MaterialInstance()
{
}
MaterialInstance::~MaterialInstance()
{
    DestroyUniformBuffers();
}
void MaterialInstance::SetParameter(const std::string& parameterName, const float& value)
{
    uint32_t size = parameters.size();
    parameters[parameterName] = ParamMap(ParamType::Float, size);
    floatParameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector2f& value)
{
    uint32_t size = parameters.size();
    parameters[parameterName] = ParamMap(ParamType::Vec2, size);
    vec2Parameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector3f& value)
{
    uint32_t size = parameters.size();
    parameters[parameterName] = ParamMap(ParamType::Vec3, size);
    vec3Parameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector4f& value)
{
    uint32_t size = parameters.size();
    parameters[parameterName] = ParamMap(ParamType::Vec4, size);
    vec4Parameters[parameterName] = value;
}

bool MaterialInstance::HasParameter(const std::string& parameterName) const
{
    return parameters.find(parameterName) != parameters.end();
}

void MaterialInstance::RemoveParameter(const std::string& parameterName)
{
    parameters.erase(parameterName);
}

void MaterialInstance::SetTexture(const std::string& textureName, const std::shared_ptr<Texture>& texture)
{
    textures[textureName] = texture;
}

const std::shared_ptr<Texture> MaterialInstance::GetTexture(const std::string& textureName) const
{
    auto it = textures.find(textureName);
    if (it != textures.end())
    {
        return it->second;
    }
    return nullptr;
}

bool MaterialInstance::HasTexture(const std::string& textureName) const
{
    return textures.find(textureName) != textures.end();
}

void MaterialInstance::RemoveTexture(const std::string& textureName)
{
    textures.erase(textureName);
}

void MaterialInstance::RenderInitialize()
{
    CreateUniformBuffers();
    SetupDescriptors();
}

void MaterialInstance::CreateUniformBuffers()
{
    uint32_t bufferSize = 0;
    for(auto& parameter : parameters)
    {
        if(parameter.second.type == ParamType::Float)
        {
            bufferSize += parameter.second.size;
        }
        else if(parameter.second.type == ParamType::Vec2)
        {
            bufferSize += parameter.second.size;
        }
        else if(parameter.second.type == ParamType::Vec3)
        {
            bufferSize += parameter.second.size;
        }
        else if(parameter.second.type == ParamType::Vec4)
        {
            bufferSize += parameter.second.size;
        }
    }
    auto& device = VulkanManager::GetInstance().GetDevice();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    for(auto& ubo: {&uboMaterialInstance})
    {
        vk::DeviceSize size = bufferSize;
        ubo->buffers.resize(swapChainImageCount);
        ubo->bufferMemories.resize(swapChainImageCount);
        ubo->buffersMapped.resize(swapChainImageCount);
        ubo->bufferSize = size;
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < swapChainImageCount; i++)
        {
            std::tie(ubo->buffers[i], ubo->bufferMemories[i]) = CommonFunction::CreateBuffer(
                device,
                size, 
                usage, 
                VulkanManager::GetInstance().GetGpuMemoryProperties(), 
                memoryPropertyFlags,
                "UBO_Material: " + materialInstanceName + " (SwapchainIndex " + std::to_string(i) + ")"
            );
            ubo->buffersMapped[i] = device.mapMemory(ubo->bufferMemories[i], 0, bufferSize);
        }
    }
}
void MaterialInstance::DestroyUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    for(auto& ubo: {&uboMaterialInstance})
    {
        for(int i = 0; i < ubo->buffers.size(); i++)
        {
            device.unmapMemory(ubo->bufferMemories[i]);
            device.destroyBuffer(ubo->buffers[i]);
            device.freeMemory(ubo->bufferMemories[i]);
        }
    }
}

void MaterialInstance::SetupDescriptors()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboMaterialInstance})
    {
        ubo->bufferInfos.resize(swapChainImageCount);
        for(int i = 0; i < swapChainImageCount; i++)
        {
            ubo->bufferInfos[i]
                .setBuffer(ubo->buffers[i])
                .setOffset(0)
                .setRange(ubo->bufferSize);
        }
    }
    // 设置image信息
    for(const auto& binding: baseMaterial.lock()->GetRenderPipline()->GetShaderBindings())
    {
        if(binding.set != MaterialSetIndex && binding.type != vk::DescriptorType::eCombinedImageSampler)
        {
            continue;
        }
        const auto tex = GetTexture(binding.name);
        if(tex == nullptr)
        {
            continue;
            std::cerr << "Texture not found: " << binding.name << std::endl;
        }
        imageInfos[binding.name]
            .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setImageView(tex->getImageView())
            .setSampler(tex->getSampler());
    }
}
