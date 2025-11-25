#include "sceneLoader.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include "commonFunction.h"
#include "modelLoader.h"
#include "renderableObject.h"
#include "vulkanManager.h"
#include "material.h"
#include "texture.h"
#include "materialInstance.h"
#include "sceneObject.h"
#include "settings.h"
#include "renderPipline.h"
#include "shaderReflect.h"

SceneLoader::SceneLoader()
{
}

void SceneLoader::LoadScence(const std::string& filename)
{
    // 读取json场景文件
    //检查文件是否存在
    if (!std::filesystem::exists(filename))
    {
        throw std::runtime_error("Scene file not found: " + filename);
    }
    std::ifstream file(filename);
    nlohmann::json scnJson = nlohmann::json::parse(file);

    // 加载场景中的物体
    for (const auto& obj : scnJson["objects"])
    {
        std::string type = obj["type"];

        if (type == "mesh")
        {
            LoadMeshObject(obj);
        }
        else if(type == "sunLight")
        {
            LoadSunLightObject(obj);
        }
        else if(type == "pointLight")
        {
            LoadPointLightObject(obj);
        }
        else if(type == "camera")
        {
            LoadCameraObject(obj);
        }
        else if(type == "environment")
        {
            LoadEnvironmentObject(obj);
        }
        else {
            std::cout << "Unknown object type: " << obj["name"] << std::endl;
        }
    }
}

void SceneLoader::LoadMeshObject(const nlohmann::basic_json<>& node)
{
    const std::string meshPath = node["modelPath"];

    std::ifstream file(CommonFunction::Path(meshPath));
    nlohmann::json meshObjectJson;
    file >> meshObjectJson;

    auto& instance = VulkanManager::GetInstance();

    // 加载模型
    std::string modelDataPath = meshObjectJson["modelDataPath"];
    std::shared_ptr<RenderableObject> renderableObject;
    if(objects.find(modelDataPath) != objects.end())
    {
        renderableObject = objects[modelDataPath];
    }
    else
    {
        ModelLoader& modelLoader = ModelLoader::GetInstance();
        modelLoader.LoadModel(CommonFunction::Path(modelDataPath));
        renderableObject = std::make_shared<RenderableObject>(
            modelLoader.GetVertexData(), modelLoader.GetIndexData(),
            &instance.GetDevice(), 
            &instance.GetGpuMemoryProperties(), 
            &instance.GetCommandPool(), 
            &instance.GetCommandBuffers()[0], 
            &instance.GetGraphicQueue());
    }

    // 加载材质
    std::string materialInstancePath = meshObjectJson["materialInstancePath"];
    std::ifstream materialInstanceFile(CommonFunction::Path(materialInstancePath));
    nlohmann::json materialInstanceJson;
    materialInstanceFile >> materialInstanceJson;
    std::string shaderName = materialInstanceJson["shader"];
    std::shared_ptr<Material> material;
    if(materials.find(shaderName) != materials.end())
    {
        material = materials[shaderName];
    }
    else
    {
        material = std::make_shared<Material>(
            &instance.GetDevice(), 
            &instance.GetGpuMemoryProperties(),
            &instance.GetRenderPass(),
            shaderName,
            instance.GetSampleCount());
    }

    // 加载材质参数
    const auto& shaderTextures = materialInstanceJson["textures"];
    uint32_t textureCount = shaderTextures.size();
    const auto& shaderParameters = materialInstanceJson["parameters"];
    uint32_t parameterCount = shaderParameters.size();
    // 根据shaderbinding校验参数和贴图
    auto& shaderBindings = material->GetRenderPipline()->GetShaderBindings();
        // 检查贴图数量是否匹配
    uint32_t expectedTextureCount = 0;
    for(const auto& binding : shaderBindings)
    {
        if(binding.type == vk::DescriptorType::eCombinedImageSampler)
        {
            expectedTextureCount++;
        }
    }
    if(expectedTextureCount != textureCount)
    {
        throw std::runtime_error("Texture count mismatch in material instance: " + materialInstancePath);
    }
        // 检查参数类型是否匹配set0,binding1
    bool validParemeters = false;
    for(const auto& binding : shaderBindings)
    {
        if(binding.set != 0 || binding.binding != 1)
        {
            continue;
        }
        if(binding.type != vk::DescriptorType::eUniformBuffer)
        {
            continue;
        }
        if(binding.memberCount != parameterCount)
        {
            break;
        }
        bool allMatch = true;
        for(uint32_t i = 0; i < binding.memberCount; i++)
        {
            const std::string& paramName = shaderParameters.begin().key();
            const auto& paramValue = shaderParameters[paramName];
            size_t paramSize = ParseValueSize(paramValue);
            if(paramSize != binding.members[i])
            {
                allMatch = false;
                break;
            }
        }
        if(allMatch)
        {
            validParemeters = true;
            break;
        }
    }
    if(!validParemeters)
    {
        throw std::runtime_error("Parameter types or sizes mismatch in material instance: " + materialInstancePath);
    }

    //创建材质实例
    std::shared_ptr<MaterialInstance> materialInstance;
    if(materialInstances.find(materialInstancePath) != materialInstances.end())
    {
        materialInstance = materialInstances[materialInstancePath];
    }
    else
    {
        materialInstance = material->CreateInstance();
        materialInstance->SetName(materialInstancePath);
    }
    //设置材质实例参数
    for(const auto& [name, value]  : shaderParameters.items())
    {
        const std::string& paramName = name;
        uint32_t paramSize = ParseValueSize(value);
        if(paramSize == sizeof(float))
        {
            auto paramValue = ParseValue<float>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector2f))
        {
            auto paramValue = ParseValue<Eigen::Vector2f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector3f))
        {
            auto paramValue = ParseValue<Eigen::Vector3f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector4f))
        {
            auto paramValue = ParseValue<Eigen::Vector4f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else 
        {
            throw std::runtime_error("Unsupported parameter type or size in material instance: " + materialInstancePath);
        }
    }
    //设置材质实例贴图
    for(const auto& [name, value] : shaderTextures.items())
    {
        const std::string& textureName = name;
        std::string texturePath = value;
        std::shared_ptr<Texture> texture;
        if(textures.find(texturePath) != textures.end())
        {
            texture = textures[texturePath];
        }
        else
        {
            texture = std::make_shared<Texture>(texturePath);
            //缓存贴图
            textures[texturePath] = texture;
        }
        materialInstance->SetTexture(textureName, texture);
    }
    
    // 创建场景物体
    std::string sceneObjectName = node["name"];
    std::shared_ptr<SceneObject> sceneObject;
    if(sceneObjects.find(sceneObjectName) != sceneObjects.end())
    {
        sceneObject = sceneObjects[sceneObjectName];
    }
    else
    {
        Eigen::Vector3f position = ParseVector3(node["position"]);
        Eigen::Vector3f rotation = ParseVector3(node["rotation"]);
        Eigen::Vector3f scale = ParseVector3(node["scale"]);
        sceneObject = std::make_shared<SceneObject>(renderableObject, materialInstance);
            sceneObject->SetPosition(position);
            sceneObject->SetRotation(rotation);
            sceneObject->SetScale(scale);
        sceneObject->UpdateModelMatrix();
    }
    //储存到场景
    objects[modelDataPath] = renderableObject;
    materials[shaderName] = material;
    materialInstances[materialInstancePath] = materialInstance;
    sceneObjects[sceneObjectName] = sceneObject;
}

void SceneLoader::LoadSunLightObject(const nlohmann::basic_json<>& node)
{
    Eigen::Vector3f color = ParseVector3(node["color"]);
    float intensity = node["intensity"].get<float>();
    Eigen::Vector3f position = ParseVector3(node["position"]);
    Eigen::Vector3f rotation = ParseVector3(node["rotation"]);
    //Eigen::Vector3f scale = ParseVector3(node["scale"]);
    std::shared_ptr<SunLight> sunLight = std::make_shared<SunLight>();
    sunLight->SetColor(color);
    sunLight->SetIntensity(intensity);
    sunLight->SetPosition(position);
    sunLight->SetRotation(rotation);
    //sunLight->SetScale(scale);

    this->sunLight = sunLight;
}
void SceneLoader::LoadPointLightObject(const nlohmann::basic_json<>& node)
{
    Eigen::Vector4f color = ParseVector4(node["color"]);
    Eigen::Vector4f specular = ParseVector4(node["specular"]);
    float intensity = node["intensity"].get<float>();
    Eigen::Vector3f position = ParseVector3(node["position"]);
    Eigen::Vector3f rotation = ParseVector3(node["rotation"]);
    //Eigen::Vector3f scale = ParseVector3(node["scale"]);
    std::shared_ptr<PointLight> pointLight = std::make_shared<PointLight>();
    pointLight->SetColor(color);
    pointLight->SetSpecular(specular);
    pointLight->SetIntensity(intensity);
    pointLight->SetPosition(position);
    pointLight->SetRotation(rotation);
    //pointLight->SetScale(scale);

    scenePointLight = std::move(pointLight);

}

void SceneLoader::LoadCameraObject(const nlohmann::basic_json<>& node)
{
    float fov = node["fov"].get<float>();
    float near = node["near_clip"].get<float>();
    float far = node["far_clip"].get<float>();
    Eigen::Vector3f position = ParseVector3(node["position"]);
    Eigen::Vector3f rotation = ParseVector3(node["rotation"]);
    Eigen::Vector3f scale = ParseVector3(node["scale"]);
    std::shared_ptr<Camera> camera = std::make_shared<Camera>();
    camera->SetHFOV(fov);
    camera->SetClip(near, far);
    //camera->SetProjection(fov, float(width)/float(height), near, far);
    camera->SetPosition(position);
    camera->SetRotation(rotation);
    //camera->SetCamera(position, rotation);

    sceneCamera = std::move(camera);
}

void SceneLoader::LoadEnvironmentObject(const nlohmann::basic_json<>& node)
{
    Eigen::Vector3f ambient = ParseVector3(node["ambient"]);
    this->ambient = ambient;
}

Eigen::Vector2f SceneLoader::ParseVector2(const nlohmann::basic_json<>& Value)
{
    if(Value.is_array() && Value.size() == 2)
    {
        return Eigen::Vector2f(Value[0].get<float>(), Value[1].get<float>());
    }
    else {
        throw std::runtime_error("Invalid vector2 format");
    }
}

Eigen::Vector3f SceneLoader::ParseVector3(const nlohmann::basic_json<>& Value)
{
    if(Value.is_array() && Value.size() == 3)
    {
        return Eigen::Vector3f(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
    }
    else {
        throw std::runtime_error("Invalid vector3 format");
    }
}

Eigen::Vector4f SceneLoader::ParseVector4(const nlohmann::basic_json<>& Value)
{
    if(Value.is_array() && Value.size() == 4)
    {
        return Eigen::Vector4f(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>(), Value[3].get<float>());
    }
    else {
        throw std::runtime_error("Invalid vector4 format");
    }
}

uint32_t SceneLoader::ParseValueSize(const nlohmann::basic_json<>& Value)
{
    if(Value.is_number_float())
    {
        return sizeof(float);
    }
    else if(Value.is_array())
    {
        if(Value.size() == 2)
        {
            return sizeof(Eigen::Vector2f);
        }
        else if(Value.size() == 3)
        {
            return sizeof(Eigen::Vector3f);
        }
        else if(Value.size() == 4)
        {
            return sizeof(Eigen::Vector4f);
        }
    }
    throw std::runtime_error("Unsupported parameter type or size");
}

template<typename T>
T SceneLoader::ParseValue(const nlohmann::basic_json<>& Value)
{
    if constexpr(std::is_same_v<T, float>)
    {
        return Value.get<float>();
    }
    else if constexpr(std::is_same_v<T, Eigen::Vector2f>)
    {
        return ParseVector2(Value);
    }
    else if constexpr(std::is_same_v<T, Eigen::Vector3f>)
    {
        return ParseVector3(Value);
    }
    else if constexpr(std::is_same_v<T, Eigen::Vector4f>)
    {
        return ParseVector4(Value);    }
    throw std::runtime_error("Unsupported parameter type or size");
}