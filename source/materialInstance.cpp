#include "materialInstance.h"

#include <cmath>

#include "texture.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "render/backend/rendererBackendVulkan.h"

namespace
{

ParamType GetParameterValueType(
    const MaterialInstanceParameterValue& value)
{
    if (std::holds_alternative<float>(value))
    {
        return ParamType::Float;
    }
    if (std::holds_alternative<Eigen::Vector2f>(value))
    {
        return ParamType::Vec2;
    }
    if (std::holds_alternative<Eigen::Vector3f>(value))
    {
        return ParamType::Vec3;
    }
    return ParamType::Vec4;
}

const char* GetParameterTypeName(ParamType type)
{
    switch (type)
    {
    case ParamType::Float:
        return "float";
    case ParamType::Vec2:
        return "vec2";
    case ParamType::Vec3:
        return "vec3";
    case ParamType::Vec4:
        return "vec4";
    }
    return "unknown";
}

bool IsFiniteParameterValue(
    const MaterialInstanceParameterValue& value)
{
    if (const float* scalar = std::get_if<float>(&value))
    {
        return std::isfinite(*scalar);
    }
    if (const Eigen::Vector2f* vector =
            std::get_if<Eigen::Vector2f>(&value))
    {
        return vector->allFinite();
    }
    if (const Eigen::Vector3f* vector =
            std::get_if<Eigen::Vector3f>(&value))
    {
        return vector->allFinite();
    }
    return std::get<Eigen::Vector4f>(value).allFinite();
}

}

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

void MaterialInstance::CommitNumericParameterValues(
    const MaterialInstanceNumericParameterValues& parameterValues)
{
    for (const auto& [parameterName, value] : parameterValues)
    {
        const auto parameterIt = parameters.find(parameterName);
        if (parameterIt == parameters.end())
        {
            throw std::invalid_argument(
                "Material instance numeric parameter not found: " +
                parameterName);
        }

        const ParamType valueType = GetParameterValueType(value);
        if (parameterIt->second.type != valueType)
        {
            throw std::invalid_argument(
                "Material instance numeric parameter type mismatch: " +
                parameterName + " expected " +
                GetParameterTypeName(parameterIt->second.type) +
                ", got " + GetParameterTypeName(valueType));
        }
        if (!IsFiniteParameterValue(value))
        {
            throw std::invalid_argument(
                "Material instance numeric parameter is not finite: " +
                parameterName);
        }
    }

    auto stagedFloatParameters = floatParameters;
    auto stagedVec2Parameters = vec2Parameters;
    auto stagedVec3Parameters = vec3Parameters;
    auto stagedVec4Parameters = vec4Parameters;
    for (const auto& [parameterName, value] : parameterValues)
    {
        switch (parameters.at(parameterName).type)
        {
        case ParamType::Float:
            stagedFloatParameters.at(parameterName) = std::get<float>(value);
            break;
        case ParamType::Vec2:
            stagedVec2Parameters.at(parameterName) =
                std::get<Eigen::Vector2f>(value);
            break;
        case ParamType::Vec3:
            stagedVec3Parameters.at(parameterName) =
                std::get<Eigen::Vector3f>(value);
            break;
        case ParamType::Vec4:
            stagedVec4Parameters.at(parameterName) =
                std::get<Eigen::Vector4f>(value);
            break;
        }
    }

    // 四份暂存表均准备成功后才交换，输入错误或分配失败不会留下部分更新。
    floatParameters.swap(stagedFloatParameters);
    vec2Parameters.swap(stagedVec2Parameters);
    vec3Parameters.swap(stagedVec3Parameters);
    vec4Parameters.swap(stagedVec4Parameters);
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
    textureBindingIdentities.erase(textureName);
}

void MaterialInstance::SetTexture(
    const std::string& textureName,
    const std::shared_ptr<Texture>& texture,
    std::optional<std::string> textureAssetIdentity,
    std::optional<std::string> textureCacheIdentity)
{
    textures[textureName] = texture;
    if (textureAssetIdentity)
    {
        *textureAssetIdentity =
            MaterialAssetUtils::NormalizeAssetPath(*textureAssetIdentity);
    }
    textureBindingIdentities[textureName] = {
        std::move(textureAssetIdentity),
        std::move(textureCacheIdentity)};
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

MaterialInstanceStateSnapshot MaterialInstance::CaptureStateSnapshot() const
{
    MaterialInstanceStateSnapshot::ParameterMap parameterValues;
    for (const auto& [parameterName, parameter] : parameters)
    {
        switch (parameter.type)
        {
        case ParamType::Float:
            parameterValues.emplace(parameterName, floatParameters.at(parameterName));
            break;
        case ParamType::Vec2:
            parameterValues.emplace(parameterName, vec2Parameters.at(parameterName));
            break;
        case ParamType::Vec3:
            parameterValues.emplace(parameterName, vec3Parameters.at(parameterName));
            break;
        case ParamType::Vec4:
            parameterValues.emplace(parameterName, vec4Parameters.at(parameterName));
            break;
        }
    }

    MaterialInstanceStateSnapshot::TextureMap textureBindings;
    for (const auto& [textureName, texture] : textures)
    {
        std::optional<std::string> textureAssetIdentity;
        std::optional<std::string> textureCacheIdentity;
        const auto identityIt = textureBindingIdentities.find(textureName);
        if (identityIt != textureBindingIdentities.end())
        {
            textureAssetIdentity =
                identityIt->second.textureAssetIdentity;
            textureCacheIdentity =
                identityIt->second.textureCacheIdentity;
        }

        textureBindings.emplace(
            textureName,
            MaterialInstanceTextureBindingSnapshot(
                texture,
                std::move(textureAssetIdentity),
                std::move(textureCacheIdentity)));
    }

    return MaterialInstanceStateSnapshot(
        MaterialAssetUtils::NormalizeAssetPath(materialInstanceName),
        std::move(parameterValues),
        std::move(textureBindings));
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
    const std::shared_ptr<Material> material = baseMaterial.lock();
    if (material)
    {
        const ShaderBinding* materialUbo =
            material->GetMaterialDescriptorSchema().FindBinding(0);
        if (materialUbo != nullptr &&
            materialUbo->type == vk::DescriptorType::eUniformBuffer)
        {
            bufferSize = materialUbo->size;
        }
    }

    if (bufferSize == 0)
    {
        // M_ schema沒有材質 UBO 時不建立空 buffer。Shader reflection
        // 不參與大小計算，避免不同 pass 對同一 MI 得到不同結果。
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
