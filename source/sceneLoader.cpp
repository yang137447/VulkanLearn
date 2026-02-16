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
#include "renderPipline.h"
#include "shaderReflect.h"
#include "renderGraph.h"

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

    // 加载后处理材质
    LoadPassMaterial();

    // 加载场景中的物体
    for (const auto& obj : scnJson["objects"])
    {
        std::string type = obj["type"];

        if (type == "mesh")
        {
            LoadMeshObject(obj);
        }
        else if(type == "directionalLight")
        {
            LoadDirectinalLightObject(obj);
        }
        else if(type == "pointLight")
        {
            LoadPointLightObject(obj);
        }
        else if(type == "spotLight")
        {
            LoadSpotLightObject(obj);
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
    auto& renderGraph = RenderGraph::GetInstance();

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
    std::shared_ptr<MaterialInstance> materialInstance = LoadMaterialInstance(materialInstancePath, CommonFunction::GetMsaaSampleCount());
    
    // 创建场景物体
    std::string sceneObjectName = node["name"];
    std::shared_ptr<SceneObject> sceneObject;
    if(sceneObjects.find(sceneObjectName) != sceneObjects.end())
    {
        sceneObject = sceneObjects[sceneObjectName];
    }
    else
    {
        Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
        Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
        Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
        sceneObject = std::make_shared<SceneObject>(renderableObject, materialInstance);
            sceneObject->SetPosition(position);
            sceneObject->SetRotation(rotation);
            sceneObject->SetScale(scale);
        sceneObject->UpdateModelMatrix();
    }
    //储存到场景
    objects.emplace(modelDataPath, std::move(renderableObject));
    sceneObjects.emplace(sceneObjectName, std::move(sceneObject));  
}

void SceneLoader::LoadDirectinalLightObject(const nlohmann::basic_json<>& node)
{
    std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    float intensity = JsonParser::ParseValue<float>(node["intensity"]);

    std::shared_ptr<DirectinalLight> directinalLight = std::make_shared<DirectinalLight>();
    directinalLight->SetColor(color);
    directinalLight->SetIntensity(intensity);
    directinalLight->SetPosition(position);
    directinalLight->SetRotation(rotation);

    directinalLights.emplace(name, std::move(directinalLight));
}
void SceneLoader::LoadPointLightObject(const nlohmann::basic_json<>& node)
{
    std::string name = node["name"];
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    float intensity = JsonParser::ParseValue<float>(node["intensity"]);
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);

    std::shared_ptr<PointLight> pointLight = std::make_shared<PointLight>();
    pointLight->SetColor(color);
    pointLight->SetIntensity(intensity);
    pointLight->SetPosition(position);
    pointLight->SetRotation(rotation);

    pointLights.emplace(name, std::move(pointLight));
}

void SceneLoader::LoadSpotLightObject(const nlohmann::basic_json<>& node)
{
    std::string name = node["name"];
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    float intensity = JsonParser::ParseValue<float>(node["intensity"]);
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    float cutAngleOuter = JsonParser::ParseValue<float>(node["cone_angle_outer"]);
    float cutAngleInner = JsonParser::ParseValue<float>(node["cone_angle_inner"]);

    std::shared_ptr<SpotLight> spotLight = std::make_shared<SpotLight>();
    spotLight->SetColor(color);
    spotLight->SetIntensity(intensity);
    spotLight->SetPosition(position);
    spotLight->SetRotation(rotation);
    spotLight->SetConeAngleOuter(cutAngleOuter);
    spotLight->SetConeAngleInner(cutAngleInner);

    spotLights.emplace(name, std::move(spotLight));
}

void SceneLoader::LoadCameraObject(const nlohmann::basic_json<>& node)
{
    float fov = node["fov"].get<float>();
    float near = node["near_clip"].get<float>();
    float far = node["far_clip"].get<float>();
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
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
    Eigen::Vector3f ambient = JsonParser::ParseValue<Eigen::Vector3f>(node["ambient"]);
    this->ambient = ambient;
}

void SceneLoader::LoadPassMaterial()
{
    VulkanManager& instance = VulkanManager::GetInstance();
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    auto& renderGraphJson = CommonFunction::InitRenderGraphJson();
    for(auto& passJson : renderGraphJson["passes"])
    {
        bool bNeedCreateMaterial = passJson.value("needCreateMaterial", false);
        if(!bNeedCreateMaterial)
        {
            continue;
        }
        std::string passName = passJson["name"];
        std::string materialInstancePath = passJson["materialInstancePath"];
        
        bool bNeedMsaa = passJson.value("needMsaa", false);
        bool bIsPostProcess = passJson.value("bIsPostProcess", false);

        // 如果当前 shaderName 对应的材质尚未加载，则新建并缓存
        std::shared_ptr<MaterialInstance> materialInstance = LoadMaterialInstance(materialInstancePath, bNeedMsaa ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1, passName);
        
        renderGraph.GetRenderpasses()[passName.data()].materialInstance = materialInstance;
    }
}

std::shared_ptr<MaterialInstance> SceneLoader::LoadMaterialInstance(const std::string_view materialInstancePath, vk::SampleCountFlagBits sampleCount, std::string_view passName)
{
    VulkanManager& instance = VulkanManager::GetInstance();
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    std::ifstream materialInstanceFile(CommonFunction::Path(materialInstancePath.data()));
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
        bool bIsShadowPass = passName == "shadow";
        material = std::make_shared<Material>(
            &instance.GetDevice(), 
            &instance.GetGpuMemoryProperties(),
            &renderGraph.GetRenderpasses()[passName.data()].renderPass,
            shaderName,
            sampleCount,
            false,
            bIsShadowPass
        );
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
        if(binding.set == 1 && binding.type == vk::DescriptorType::eCombinedImageSampler)
        {
            expectedTextureCount++;
        }
    }
    if(expectedTextureCount != textureCount)
    {
        throw std::runtime_error(std::string("Texture count mismatch in material instance: ") + materialInstancePath.data());
    }
        // 检查参数类型是否匹配set1,binding0
    bool validParemeters = false;
    bool hasMaterialUbo = false;
    for(const auto& binding : shaderBindings)
    {
        if(binding.set != 1 || binding.binding != 0)
        {
            continue;
        }
        if(binding.type != vk::DescriptorType::eUniformBuffer)
        {
            continue;
        }
        hasMaterialUbo = true;
        if(binding.memberCount != parameterCount)
        {
            break;
        }
        bool allMatch = true;
        uint32_t i = 0;
        for(auto it = shaderParameters.begin(); it != shaderParameters.end(); ++it, ++i)
        {
            const auto& paramValue = it.value();
            size_t paramSize = JsonParser::ParseValueSize(paramValue);
            if(i >= binding.members.size() || paramSize != binding.members[i])
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
        if(!(parameterCount == 0 && !hasMaterialUbo))
        {
            throw std::runtime_error(std::string("Parameter types or sizes mismatch in material instance: ") + materialInstancePath.data());
        }
    }

    //创建材质实例
    std::shared_ptr<MaterialInstance> materialInstance;
    if(materialInstances.find(materialInstancePath.data()) != materialInstances.end())
    {
        materialInstance = materialInstances[materialInstancePath.data()];
    }
    else
    {
        materialInstance = material->CreateInstance();
        materialInstance->SetName(materialInstancePath.data());
    }
    //设置材质实例参数
    //TODO:按shaderbinding设置参数
    for(const auto& [name, value]  : shaderParameters.items())
    {
        const std::string& paramName = name;
        uint32_t paramSize = JsonParser::ParseValueSize(value);
        if(paramSize == sizeof(float))
        {
            auto paramValue = JsonParser::ParseValue<float>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector2f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector2f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector3f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector3f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector4f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector4f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else 
        {
            throw std::runtime_error(std::string("Unsupported parameter type or size in material instance: ") + materialInstancePath.data());
        }
    }
    //设置材质实例贴图
    //TODO:按shaderbinding设置贴图
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

    //缓存材质和材质实例
    materials.emplace(shaderName, material);
    materialInstances.emplace(materialInstancePath, materialInstance);

    return materialInstance;
}
