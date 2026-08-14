#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vulkan/vulkan.hpp>
#include "baseStructs.h"
#include <Eigen/Dense>

class Material;
class Texture;

namespace VL
{
class RendererBackendVulkan;
}

enum class ParamType
{
    Float,
    Vec2,
    Vec3,
    Vec4
};

struct ParamMap
{
    ParamType type;
    uint32_t size;

    ParamMap() : type(ParamType::Float), size(sizeof(float)) {}
    explicit ParamMap(ParamType type) : type(type) {
        switch (type)
        {
        case ParamType::Float:
            size = sizeof(float);
            break;
        case ParamType::Vec2:
            size = sizeof(Eigen::Vector2f);
            break;
        case ParamType::Vec3:
            size = sizeof(Eigen::Vector3f);
            break;
        case ParamType::Vec4:
            size = sizeof(Eigen::Vector4f);
            break;
        }
    }
};

using MaterialInstanceParameterValue =
    std::variant<float, Eigen::Vector2f, Eigen::Vector3f, Eigen::Vector4f>;

struct MaterialInstanceTextureBindingSnapshot
{
    MaterialInstanceTextureBindingSnapshot(
        std::shared_ptr<Texture> textureValue,
        std::optional<std::string> textureAssetIdentityValue,
        std::optional<std::string> textureCacheIdentityValue)
        : texture(std::move(textureValue))
        , textureAssetIdentity(std::move(textureAssetIdentityValue))
        , textureCacheIdentity(std::move(textureCacheIdentityValue))
    {
    }

    const std::shared_ptr<Texture> texture;
    const std::optional<std::string> textureAssetIdentity;
    const std::optional<std::string> textureCacheIdentity;
};

struct MaterialInstanceStateSnapshot
{
    using ParameterMap =
        std::map<std::string, MaterialInstanceParameterValue>;
    using TextureMap =
        std::map<std::string, MaterialInstanceTextureBindingSnapshot>;

    MaterialInstanceStateSnapshot(
        std::string normalizedIdentity,
        ParameterMap parameterValues,
        TextureMap textureBindings)
        : normalizedMaterialInstanceIdentity(std::move(normalizedIdentity))
        , parameters(std::move(parameterValues))
        , textures(std::move(textureBindings))
    {
    }

    const std::string normalizedMaterialInstanceIdentity;
    const ParameterMap parameters;
    const TextureMap textures;
};

class MaterialInstance
{
public:
    MaterialInstance() = default;
    ~MaterialInstance();
    //设置参数的模板函数
    void SetParameter(const std::string& parameterName, const float& value);
    void SetParameter(const std::string& parameterName, const Eigen::Vector2f& value);
    void SetParameter(const std::string& parameterName, const Eigen::Vector3f& value);
    void SetParameter(const std::string& parameterName, const Eigen::Vector4f& value);
    //获取参数的模板函数
    template<typename T>
    T GetParameter(const std::string& parameterName) const
    {
        auto it = parameters.find(parameterName);
        if (it == parameters.end())
            throw std::runtime_error("Parameter not found: " + parameterName);
        const ParamMap& paramMap = it->second;

        if constexpr (std::is_same_v<T, float>)
        {
            if (paramMap.type != ParamType::Float) throw std::runtime_error("Parameter type mismatch: " + parameterName);
            return floatParameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector2f>)
        {
            if (paramMap.type != ParamType::Vec2) throw std::runtime_error("Parameter type mismatch: " + parameterName);
            return vec2Parameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector3f>)
        {
            if (paramMap.type != ParamType::Vec3) throw std::runtime_error("Parameter type mismatch: " + parameterName);
            return vec3Parameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector4f>)
        {
            if (paramMap.type != ParamType::Vec4) throw std::runtime_error("Parameter type mismatch: " + parameterName);
            return vec4Parameters.at(parameterName);
        }
        else
        {
            static_assert(!sizeof(T), "Unsupported MaterialInstance parameter type");
        }
    }
    const std::unordered_map<std::string, ParamMap>& GetParameters() const { return parameters; }
    // 检查参数是否存在
    bool HasParameter(const std::string& parameterName) const;
    //设置纹理
    void SetTexture(const std::string& textureName, const std::shared_ptr<Texture>& texture);
    void SetTexture(
        const std::string& textureName,
        const std::shared_ptr<Texture>& texture,
        std::optional<std::string> textureAssetIdentity,
        std::optional<std::string> textureCacheIdentity);
    //获取纹理
    std::shared_ptr<Texture> GetTexture(const std::string& textureName) const;
    //检查纹理是否存在
    bool HasTexture(const std::string& textureName) const;
    
    void SetBaseMaterial(const std::shared_ptr<Material>& baseMaterial) { this->baseMaterial = baseMaterial; }
    const std::weak_ptr<Material>& GetBaseMaterial() const { return baseMaterial; }

    void SetName(const std::string& name) { materialInstanceName = name; }
    const std::string& GetName() const { return materialInstanceName; }
    MaterialInstanceStateSnapshot CaptureStateSnapshot() const;

    inline std::vector<void*>& GetUboMaterialInstanceMapped(){ return uboMaterialInstance.buffersMapped; }
    std::vector<vk::DescriptorBufferInfo>& GetUboMaterialInstanceInfo(){ return uboMaterialInstance.bufferInfos; }
    const std::vector<vk::DescriptorBufferInfo>& GetUboMaterialInstanceInfo() const { return uboMaterialInstance.bufferInfos; }
    const vk::DescriptorImageInfo& GetTextureDescriptorInfo(const std::string& textureName) const;

    void RenderInitialize(VL::RendererBackendVulkan& rendererBackend);
    // Resize and graph-reload paths must recreate per-swapchain UBOs while the
    // material instance and its CPU parameters stay alive.
    void ShutdownRenderResources();
private:
    void ClearParameterValues(const std::string& parameterName);
    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void SetupDescriptors();

    std::string materialInstanceName;
    std::weak_ptr<Material> baseMaterial;
    std::unordered_map<std::string, ParamMap> parameters;
    std::unordered_map<std::string, float> floatParameters;
    std::unordered_map<std::string, Eigen::Vector2f> vec2Parameters;
    std::unordered_map<std::string, Eigen::Vector3f> vec3Parameters;
    std::unordered_map<std::string, Eigen::Vector4f> vec4Parameters;
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
    struct TextureBindingIdentity
    {
        std::optional<std::string> textureAssetIdentity;
        std::optional<std::string> textureCacheIdentity;
    };
    std::unordered_map<std::string, TextureBindingIdentity> textureBindingIdentities;
    Buffer uboMaterialInstance; 
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    bool renderInitialized = false;
};
