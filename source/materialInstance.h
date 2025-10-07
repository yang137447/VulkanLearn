#include "textureLoader.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

enum class ParameterType
{
    Bool,
    Float,
    Vec2,
    Vec3,
    Vec4,
};
struct ParameterValue
{
    ParameterType type;
    union
    {
        bool boolValue;
        float floatValue;
        Eigen::Vector2f vec2Value;
        Eigen::Vector3f vec3Value;
        Eigen::Vector4f vec4Value;
    };
    //构造函数
    ParameterValue(bool value) : type(ParameterType::Bool), boolValue(value) {}
    ParameterValue(float value) : type(ParameterType::Float), floatValue(value) {}
    ParameterValue(const Eigen::Vector2f& value) : type(ParameterType::Vec2), vec2Value(value) {}
    ParameterValue(const Eigen::Vector3f& value) : type(ParameterType::Vec3), vec3Value(value) {}
    ParameterValue(const Eigen::Vector4f& value) : type(ParameterType::Vec4), vec4Value(value) {}
    //拷贝构造函数
    ParameterValue(const ParameterValue& other) : type(other.type)
    {
        switch (type)
        {
        case ParameterType::Bool:
            boolValue = other.boolValue;
            break;
        case ParameterType::Float:
            floatValue = other.floatValue;
            break;
        case ParameterType::Vec2:
            vec2Value = other.vec2Value;
            break;
        case ParameterType::Vec3:
            vec3Value = other.vec3Value;
            break;
        case ParameterType::Vec4:
            vec4Value = other.vec4Value;
            break;
        }
    }
    //重载赋值运算符
    ParameterValue& operator=(const ParameterValue& other)
    {
        if (this != &other)
        {
            type = other.type;
            switch (type)
            {
            case ParameterType::Bool:
                boolValue = other.boolValue;
                break;
            case ParameterType::Float:
                floatValue = other.floatValue;
                break;
            case ParameterType::Vec2:
                vec2Value = other.vec2Value;
                break;
            case ParameterType::Vec3:
                vec3Value = other.vec3Value;
                break;
            case ParameterType::Vec4:
                vec4Value = other.vec4Value;
                break;
            }
        }
        return *this;
    }
};

class Material;
class Texture;

class MaterialInstance
{
public:
    //设置参数的模板函数
    template<typename T>
    void SetParameter(const std::string& parameterName, const T& value);
    //获取参数的模板函数
    template<typename T>
    T GetParameter(const std::string& parameterName) const;
    // 检查参数是否存在
    bool HasParameter(const std::string& parameterName) const;
    //移除参数
    void RemoveParameter(const std::string& parameterName);
    //设置纹理
    void SetTexture(const std::string& textureName, const std::shared_ptr<Texture>& texture);
    //获取纹理
    std::shared_ptr<Texture> GetTexture(const std::string& textureName) const;
    //检查纹理是否存在
    bool HasTexture(const std::string& textureName) const;
    //移除纹理
    void RemoveTexture(const std::string& textureName);
    
    void SetBaseMaterial(std::shared_ptr<Material> baseMaterial) { this->baseMaterial = baseMaterial; }
    const std::shared_ptr<Material>& GetBaseMaterial() const { return baseMaterial; }

    void SetName(const std::string& name) { materialInstanceName = name; }
    const std::string& GetName() const { return materialInstanceName; }
private:

    vk::Device device;
    std::string materialInstanceName;
    std::shared_ptr<Material> baseMaterial;
    std::unordered_map<std::string, ParameterValue> parameters;
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
};