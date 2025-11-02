#pragma once
#include "textureLoader.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>
#include "baseStructs.h"
#include <Eigen/Dense>

class Material;
class Texture;

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
    uint32_t index;
    uint32_t size;

    ParamMap() : type(ParamType::Float), index(0) {}
    ParamMap(ParamType type, uint32_t index) : type(type), index(index) {
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

class MaterialInstance
{
public:
    MaterialInstance();
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
        const ParamMap& paraMap = it->second;

        if constexpr (std::is_same_v<T, float>)
        {
            if (paraMap.type != ParamType::Float) throw std::runtime_error("Parameter type mismatch");
            return floatParameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector2f>)
        {
            if (paraMap.type != ParamType::Vec2) throw std::runtime_error("Parameter type mismatch");
            return vec2Parameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector3f>)
        {
            if (paraMap.type != ParamType::Vec3) throw std::runtime_error("Parameter type mismatch");
            return vec3Parameters.at(parameterName);
        }
        else if constexpr (std::is_same_v<T, Eigen::Vector4f>)
        {
            if (paraMap.type != ParamType::Vec4) throw std::runtime_error("Parameter type mismatch");
            return vec4Parameters.at(parameterName);
        }
    }
    const std::unordered_map<std::string, ParamMap>& GetParameters() const { return parameters; }
    // 检查参数是否存在
    bool HasParameter(const std::string& parameterName) const;
    //移除参数
    void RemoveParameter(const std::string& parameterName);
    //设置纹理
    void SetTexture(const std::string& textureName, const std::shared_ptr<Texture>& texture);
    //获取纹理
    const std::shared_ptr<Texture> GetTexture(const std::string& textureName) const;
    //检查纹理是否存在
    bool HasTexture(const std::string& textureName) const;
    //移除纹理
    void RemoveTexture(const std::string& textureName);
    
    void SetBaseMaterial(std::shared_ptr<Material> baseMaterial) { this->baseMaterial = baseMaterial; }
    const std::shared_ptr<Material>& GetBaseMaterial() const { return baseMaterial; }

    void SetName(const std::string& name) { materialInstanceName = name; }
    const std::string& GetName() const { return materialInstanceName; }

    inline std::vector<void*>& GetUboMaterialInstanceMapped(){ return uboMaterialInstance.uniformBuffersMapped; }
    std::vector<vk::DescriptorBufferInfo>& GetUboMaterialInstanceInfo(){ return uboMaterialInstance.uniformBufferInfos; }
    vk::DescriptorImageInfo& GetUboMaterialInstanceImageInfo(){ return imageInfo; }

    void RenderInitialize();
private:
    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void SetupDescriptors();

    vk::Device device;
    std::string materialInstanceName;
    std::shared_ptr<Material> baseMaterial;
    std::unordered_map<std::string, ParamMap> parameters;
    std::unordered_map<std::string, float> floatParameters;
    std::unordered_map<std::string, Eigen::Vector2f> vec2Parameters;
    std::unordered_map<std::string, Eigen::Vector3f> vec3Parameters;
    std::unordered_map<std::string, Eigen::Vector4f> vec4Parameters;
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures;

    vk::DescriptorImageInfo imageInfo;
    UBO uboMaterialInstance; 
};