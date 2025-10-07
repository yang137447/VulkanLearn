#include "sceneLoader.h"
#include <fstream>
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
#include "matrix.h"
#include "settings.h"
#include <filesystem>
#include <iostream>

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
        else if(type == "camera")
        {
            LoadCameraObject(obj);
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
    const auto& shaderParameters = materialInstanceJson["parameters"];
    std::string albedoMap = shaderParameters["albedoMap"];
    std::shared_ptr<Texture> texture;
    if(textures.find(albedoMap) != textures.end())
    {
        texture = textures[albedoMap];
    }
    else
    {
        texture = std::make_shared<Texture>(albedoMap);
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
        materialInstance->SetTexture("albedoMap", texture);
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
    }
    //储存到场景
    objects[modelDataPath] = renderableObject;
    materials[shaderName] = material;
    materialInstances[materialInstancePath] = materialInstance;
    textures[albedoMap] = texture;
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

    Light = sunLight;
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
    camera->SetPosition(position);
    camera->SetRotation(rotation);

    SceneCamera = camera;
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