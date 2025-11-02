#include "materialInstance.h"
#include "texture.h"
#include "material.h"
#include "renderPipline.h"
#include "settings.h"
#include "vulkanManager.h"
#include "commonFunction.h"

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
    for(auto& ubo: {&uboMaterialInstance})
    {
        vk::DeviceSize uniformBufferSize = bufferSize;
        ubo->uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBufferMemories.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBufferSize = bufferSize;
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::tie(ubo->uniformBuffers[i], ubo->uniformBufferMemories[i]) = CommonFunction::CreateBuffer(
                device,
                uniformBufferSize, 
                usage, 
                VulkanManager::GetInstance().GetGpuMemoryProperties(), 
                memoryPropertyFlags
            );
            ubo->uniformBuffersMapped[i] = device.mapMemory(ubo->uniformBufferMemories[i], 0, uniformBufferSize);
        }
    }
}
void MaterialInstance::DestroyUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    for(auto& ubo: {&uboMaterialInstance})
    {
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            device.unmapMemory(ubo->uniformBufferMemories[i]);
            device.destroyBuffer(ubo->uniformBuffers[i]);
            device.freeMemory(ubo->uniformBufferMemories[i]);
        }
    }
}

void MaterialInstance::SetupDescriptors()
{
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboMaterialInstance})
    {
        ubo->uniformBufferInfos.resize(MAX_FRAMES_IN_FLIGHT);
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            ubo->uniformBufferInfos[i]
                .setBuffer(ubo->uniformBuffers[i])
                .setOffset(0)
                .setRange(ubo->uniformBufferSize);
        }
    }
    // 设置image信息
    imageInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(GetTexture("albedoMap")->getImageView())
        .setSampler(GetTexture("albedoMap")->getSampler());
}