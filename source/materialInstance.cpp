#include "materialInstance.h"
#include "texture.h"
#include "material.h"
#include "render/backend/rendererBackendVulkan.h"

MaterialInstance::~MaterialInstance()
{
    DestroyUniformBuffers();
}
void MaterialInstance::SetParameter(const std::string& parameterName, const float& value)
{
    ClearParameterValues(parameterName);
    parameters.insert_or_assign(parameterName, ParamMap(ParamType::Float));
    floatParameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector2f& value)
{
    ClearParameterValues(parameterName);
    parameters.insert_or_assign(parameterName, ParamMap(ParamType::Vec2));
    vec2Parameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector3f& value)
{
    ClearParameterValues(parameterName);
    parameters.insert_or_assign(parameterName, ParamMap(ParamType::Vec3));
    vec3Parameters[parameterName] = value;
}

void MaterialInstance::SetParameter(const std::string& parameterName, const Eigen::Vector4f& value)
{
    ClearParameterValues(parameterName);
    parameters.insert_or_assign(parameterName, ParamMap(ParamType::Vec4));
    vec4Parameters[parameterName] = value;
}

bool MaterialInstance::HasParameter(const std::string& parameterName) const
{
    return parameters.find(parameterName) != parameters.end();
}

void MaterialInstance::ClearParameterValues(const std::string& parameterName)
{
    floatParameters.erase(parameterName);
    vec2Parameters.erase(parameterName);
    vec3Parameters.erase(parameterName);
    vec4Parameters.erase(parameterName);
}

void MaterialInstance::SetTexture(const std::string& textureName, const std::shared_ptr<Texture>& texture)
{
    textures[textureName] = texture;
}

std::shared_ptr<Texture> MaterialInstance::GetTexture(const std::string& textureName) const
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

const vk::DescriptorImageInfo& MaterialInstance::GetTextureDescriptorInfo(const std::string& textureName) const
{
    auto it = textures.find(textureName);
    if (it == textures.end() || it->second == nullptr)
    {
        throw std::runtime_error(
            "Texture binding not found in material instance '" +
            materialInstanceName +
            "': " +
            textureName);
    }

    return it->second->GetDescriptorInfo();
}

void MaterialInstance::RenderInitialize(VL::RendererBackendVulkan& rendererBackend)
{
    if (renderInitialized)
    {
        return;
    }

    this->rendererBackend = &rendererBackend;
    CreateUniformBuffers();
    SetupDescriptors();
    renderInitialized = true;
}

void MaterialInstance::ShutdownRenderResources()
{
    DestroyUniformBuffers();
}

void MaterialInstance::CreateUniformBuffers()
{
    uint32_t bufferSize = 0;
    for (const auto& parameter : parameters)
    {
        bufferSize += parameter.second.size;
    }

    if (bufferSize == 0)
    {
        // Materials without a material UBO have no per-instance parameter
        // buffer to create. Descriptor planning decides whether a UBO binding
        // is actually required for the shader.
        if (uboMaterialInstance.HasResources())
        {
            rendererBackend->DestroyBufferSet(uboMaterialInstance);
        }
        return;
    }

    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;
    rendererBackend->CreatePerSwapchainBufferSet(
        uboMaterialInstance,
        bufferSize,
        usage,
        memoryPropertyFlags,
        "UBO_Material: " + materialInstanceName);
}
void MaterialInstance::DestroyUniformBuffers()
{
    if (!renderInitialized && !uboMaterialInstance.HasResources())
    {
        return;
    }

    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyBufferSet(uboMaterialInstance);
    }
    renderInitialized = false;
    rendererBackend = nullptr;
}

void MaterialInstance::SetupDescriptors()
{
    if (!uboMaterialInstance.HasResources())
    {
        return;
    }

    rendererBackend->SetupDescriptorBufferInfos(uboMaterialInstance);
}
