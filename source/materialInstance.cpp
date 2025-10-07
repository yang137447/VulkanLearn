#include "materialInstance.h"
#include "texture.h"
#include "material.h"
#include "renderPipline.h"
#include "settings.h"
#include "vulkanManager.h"

template<typename T>
void MaterialInstance::SetParameter(const std::string& parameterName, const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        parameters[parameterName] = ParameterValue(value);
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        parameters[parameterName] = ParameterValue(value);
    }
    else if constexpr (std::is_same_v<T, Eigen::Vector2f>)
    {
        parameters[parameterName] = ParameterValue(value);
    }
    else if constexpr (std::is_same_v<T, Eigen::Vector3f>)
    {
        parameters[parameterName] = ParameterValue(value);
    }
    else if constexpr (std::is_same_v<T, Eigen::Vector4f>)
    {
        parameters[parameterName] = ParameterValue(value);
    }

    //TODO: 更新UniformBuffer
}

template<typename T>
T MaterialInstance::GetParameter(const std::string& parameterName) const
{
    auto it = parameters.find(parameterName);
    if (it != parameters.end())
    {
        const ParameterValue& value = it->second;
        if constexpr (std::is_same_v<T, bool>)
        {
            return value.boolValue;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            return value.floatValue;
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector2f>)
        {
            return value.vec2Value;
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector3f>)
        {
            return value.vec3Value;
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector4f>)
        {
            return value.vec4Value;
        }
    }
    return T();
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

void MaterialInstance::RemoveTexture(const std::string& textureName)
{
    textures.erase(textureName);
}