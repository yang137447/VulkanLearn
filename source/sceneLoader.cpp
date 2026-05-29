#include "sceneLoader.h"
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include "commonFunction.h"
#include "material.h"
#include "material/loader/materialInstanceResolver.h"
#include "materialInstance.h"
#include "materialInstanceValidator.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/brdfLutGenerator.h"
#include "pipeline/environmentCubemapGenerator.h"
#include "pipeline/environmentPrefilterGenerator.h"
#include "pipeline/environmentSHGenerator.h"
#include "pipeline/graphicsPipeline.h"
#include "pipeline/pipelineFactory.h"
#include "renderGraph.h"
#include "renderableObject.h"
#include "scene/validation/sceneAssetValidator.h"
#include "sceneObject.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "vulkanManager.h"

namespace
{
    nlohmann::json LoadJsonFile(const std::string& absolutePath, const std::string& logicalPath, std::string_view assetLabel)
    {
        std::ifstream file(absolutePath);
        if (!file.is_open())
        {
            throw std::runtime_error(std::string(assetLabel) + " file not found: " + logicalPath);
        }

        try
        {
            return nlohmann::json::parse(file);
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                std::string(assetLabel) + " JSON parse failed: " + logicalPath +
                " error=" + exception.what());
        }
    }

    vk::CompareOp ParseDepthCompareOp(const std::string& compareOp)
    {
        if (compareOp == "lessOrEqual")
        {
            return vk::CompareOp::eLessOrEqual;
        }
        if (compareOp == "equal")
        {
            return vk::CompareOp::eEqual;
        }
        if (compareOp == "greater")
        {
            return vk::CompareOp::eGreater;
        }
        if (compareOp == "greaterOrEqual")
        {
            return vk::CompareOp::eGreaterOrEqual;
        }
        if (compareOp == "always")
        {
            return vk::CompareOp::eAlways;
        }
        return vk::CompareOp::eLess;
    }

    nlohmann::json LoadSceneJson(const std::string& scenePath)
    {
        if (!std::filesystem::exists(scenePath))
        {
            throw std::runtime_error("Scene file not found: " + scenePath);
        }
        return LoadJsonFile(scenePath, scenePath, "Scene");
    }
}

SceneLoader::SceneLoader()
{
}

void SceneLoader::SetPipelineFactory(PipelineFactory* pipelineFactory)
{
    this->pipelineFactory = pipelineFactory;
}

const std::shared_ptr<Texture>* SceneLoader::GetGlobalTextureByBindingName(std::string_view bindingName) const
{
    if (bindingName == "environmentCube")
    {
        return &environmentCube;
    }
    if (bindingName == "prefilteredEnvironmentCube")
    {
        return &environmentPrefilteredCube;
    }
    if (bindingName == "brdfLut")
    {
        return &brdfLut;
    }
    return nullptr;
}

void SceneLoader::ValidateSceneFile(const std::string& filename) const
{
    nlohmann::json sceneJson = LoadSceneJson(filename);
    SceneAssetBuildPlan sceneBuildPlan = SceneAssetValidator::BuildLoadPlan(filename, sceneJson);
    SceneAssetValidator::Validate(sceneBuildPlan);
}

void SceneLoader::LoadScene(const std::string& filename)
{
    nlohmann::json scnJson = LoadSceneJson(filename);
    SceneAssetBuildPlan sceneBuildPlan = SceneAssetValidator::BuildLoadPlan(filename, scnJson);
    SceneAssetValidator::Validate(sceneBuildPlan);

    //brdfLut生成
    brdfLut = BrdfLutGenerator::Generate(*pipelineFactory);

    // 加载后处理材质
    LoadPassMaterial();

    // 加载场景中的物体
    for (const SceneObjectBuildPlan& objectPlan : sceneBuildPlan.objectPlans)
    {
        const auto& obj = scnJson["objects"][objectPlan.objectIndex];
        const std::string& type = objectPlan.objectType;

        if (type == "mesh")
        {
            if (!objectPlan.meshLoadRequest.has_value())
            {
                throw std::runtime_error(
                    "Scene mesh object missing mesh load request: " + filename +
                    " object=" + objectPlan.objectName);
            }
            LoadMeshObject(obj, *objectPlan.meshLoadRequest);
        }
        else if(type == "directionalLight")
        {
            LoadDirectionalLightObject(obj);
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
            std::cout << "Unknown object type: " << objectPlan.objectName << std::endl;
        }
    }

    if (sceneCamera == nullptr)
    {
        throw std::runtime_error("Scene has no camera: " + filename);
    }
    currentScenePath = filename;
}

void SceneLoader::LoadMeshObject(const nlohmann::basic_json<>& node, const MeshAssetLoadRequest& meshLoadRequest)
{
    MeshAssetImportResult importResult = MeshAssetLoader::ImportSections(meshLoadRequest);
    const MeshAssetBuildPlan& buildPlan = importResult.buildPlan;
    const ModelResource& modelResource = importResult.modelResource;
    const std::vector<MeshSectionLoadPlan>& sectionPlans = importResult.sectionPlans;

    auto& instance = VulkanManager::GetInstance();
    auto& renderGraph = RenderGraph::GetInstance();

    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
    const std::string sceneObjectBaseName = node["name"];
    const auto& geometryPass = renderGraph.GetRenderpasses().at("geometry");

    for (size_t sectionIndex = 0; sectionIndex < modelResource.sections.size(); ++sectionIndex)
    {
        const MeshSection& section = modelResource.sections[sectionIndex];
        const MeshSectionLoadPlan& sectionPlan = sectionPlans[sectionIndex];
        std::string renderableObjectKey =
            buildPlan.modelCacheKey + "|" + std::to_string(sectionIndex) + "|" + section.sectionName;

        std::shared_ptr<RenderableObject> renderableObject;
        if(objects.find(renderableObjectKey) != objects.end())
        {
            renderableObject = objects[renderableObjectKey];
        }
        else
        {
            renderableObject = std::make_shared<RenderableObject>(
                section.vertices, section.indices,
                &instance.GetDevice(),
                &instance.GetGpuMemoryProperties(),
                &instance.GetCommandPool(),
                &instance.GetCommandBuffers()[0],
                &instance.GetGraphicQueue(),
                renderableObjectKey);
            objects.emplace(renderableObjectKey, renderableObject);
        }

        // 加载材质
        if (sectionPlan.bUsesUnsafeFallbackMaterial)
        {
            std::cout << "Unsafe mesh material slot fallback: "
                      << sceneObjectBaseName << "::" << section.sectionName
                      << " reason=" << sectionPlan.unsafeFallbackReason << std::endl;
        }
        std::shared_ptr<MaterialInstance> materialInstance =
            LoadMaterialInstance(sectionPlan.materialInstancePath, geometryPass.sampleCount);

        // 创建场景物体。多 section 模型拆成多个 SceneObject 参与现有材质分组渲染。
        std::string sceneObjectName = sceneObjectBaseName;
        if (modelResource.sections.size() > 1)
        {
            sceneObjectName += "::" + section.sectionName;
        }
        if(sceneObjects.find(sceneObjectName) != sceneObjects.end())
        {
            sceneObjectName += "_" + std::to_string(sectionIndex);
        }

        std::shared_ptr<SceneObject> sceneObject = std::make_shared<SceneObject>(renderableObject, materialInstance);
        sceneObject->SetName(sceneObjectName);
        sceneObject->SetPosition(position);
        sceneObject->SetRotation(rotation);
        sceneObject->SetScale(scale);
        sceneObject->UpdateModelMatrix();

        //储存到场景
        sceneObjects.emplace(sceneObjectName, std::move(sceneObject));
    }
}

void SceneLoader::LoadDirectionalLightObject(const nlohmann::basic_json<>& node)
{
    std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    float intensity = JsonParser::ParseValue<float>(node["intensity"]);

    std::shared_ptr<DirectionalLight> directionalLight = std::make_shared<DirectionalLight>();
    directionalLight->SetName(name);
    directionalLight->SetColor(color);
    directionalLight->SetIntensity(intensity);
    directionalLight->SetPosition(position);
    directionalLight->SetRotation(rotation);

    directionalLights.emplace(name, std::move(directionalLight));
}
void SceneLoader::LoadPointLightObject(const nlohmann::basic_json<>& node)
{
    std::string name = node["name"];
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    float intensity = JsonParser::ParseValue<float>(node["intensity"]);
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);

    std::shared_ptr<PointLight> pointLight = std::make_shared<PointLight>();
    pointLight->SetName(name);
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
    spotLight->SetName(name);
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
    std::string name = node["name"];
    float fov = node["fov"].get<float>();
    float near = node["near_clip"].get<float>();
    float far = node["far_clip"].get<float>();
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f scale = JsonParser::ParseValue<Eigen::Vector3f>(node["scale"]);
    std::shared_ptr<Camera> camera = std::make_shared<Camera>();
    camera->SetName(name);
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
    // environment 节点只负责声明输入 HDR/EXR 路径和 cubemap 输出尺寸。
    environmentHdrPath = node.value("hdrPath", std::string());
    environmentCubeSize = node.value("cubeSize", 512u);
    if (!environmentHdrPath.empty())
    {
        environmentSH = EnvironmentSHGenerator::Generate(environmentHdrPath);
        hasEnvironmentSH = true;
    }

    if (pipelineFactory != nullptr && !environmentHdrPath.empty())
    {
        // 环境贴图预处理跟场景资源绑定，读取到 environment 后立即生成 cubemap。
        environmentCube = EnvironmentCubemapGenerator::Generate(environmentHdrPath, environmentCubeSize, *pipelineFactory);
        if (environmentCube != nullptr)
        {
            environmentPrefilteredCube = EnvironmentPrefilterGenerator::Generate(*environmentCube, environmentCubeSize, *pipelineFactory);
        }
    }
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
        
        GraphicsPipelineStateDesc pipelineStateDesc;
        if (passJson.contains("pipelineState"))
        {
            const auto& pipelineStateJson = passJson["pipelineState"];
            pipelineStateDesc.bUseVertexInput = pipelineStateJson.value("useVertexInput", pipelineStateDesc.bUseVertexInput);
            pipelineStateDesc.bDepthTestEnable = pipelineStateJson.value("depthTestEnable", pipelineStateDesc.bDepthTestEnable);
            pipelineStateDesc.bDepthWriteEnable = pipelineStateJson.value("depthWriteEnable", pipelineStateDesc.bDepthWriteEnable);
            pipelineStateDesc.depthCompareOp = ParseDepthCompareOp(pipelineStateJson.value("depthCompareOp", std::string("less")));
        }

        // 如果当前 shaderName 对应的材质尚未加载，则新建并缓存
        const auto& renderPass = renderGraph.GetRenderpasses().at(passName);
        std::shared_ptr<MaterialInstance> materialInstance = LoadMaterialInstance(materialInstancePath, renderPass.sampleCount, passName, pipelineStateDesc);
        
        renderGraph.GetRenderpasses()[passName.data()].materialInstance = materialInstance;
    }
}

std::shared_ptr<MaterialInstance> SceneLoader::LoadMaterialInstance(const std::string_view materialInstancePath, vk::SampleCountFlagBits sampleCount, std::string_view passName, const GraphicsPipelineStateDesc& pipelineStateDesc)
{
    VulkanManager& instance = VulkanManager::GetInstance();
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    std::ifstream materialInstanceFile(CommonFunction::Path(materialInstancePath.data()));
    nlohmann::json materialInstanceJson;
    materialInstanceFile >> materialInstanceJson;
    MaterialInstanceResolveResult materialInstanceResolveResult =
        MaterialInstanceResolver::Resolve(materialInstancePath, materialInstanceJson);
    const nlohmann::json& effectiveMaterialInstanceJson = materialInstanceResolveResult.effectiveMaterialInstanceJson;
    MaterialInstanceBuildPlan loadPlan = MaterialInstanceValidator::BuildLoadPlan(materialInstancePath, passName, sampleCount, pipelineStateDesc, effectiveMaterialInstanceJson);
    std::shared_ptr<Material> material;
    if(materials.find(loadPlan.materialKey) != materials.end())
    {
        material = materials[loadPlan.materialKey];
    }
    else
    {
        if (pipelineFactory == nullptr)
        {
            throw std::runtime_error("PipelineFactory is not set in SceneLoader");
        }
        material = std::make_shared<Material>(
            *pipelineFactory, 
            &instance.GetGpuMemoryProperties(),
            &renderGraph.GetRenderpasses()[passName.data()].renderPass,
            loadPlan.shaderVariantKey,
            loadPlan.materialKey,
            sampleCount,
            renderGraph.GetRenderpasses()[passName.data()].colorAttachmentCount,
            loadPlan.pipelineStateDesc,
            loadPlan.bIsShadowPass
        );
    }
    const auto& shaderParameters = effectiveMaterialInstanceJson["parameters"];
    const auto& shaderTextures = effectiveMaterialInstanceJson.contains("textures")
        ? effectiveMaterialInstanceJson["textures"]
        : nlohmann::json::object();
    MaterialInstanceValidator::Validate(materialInstancePath, effectiveMaterialInstanceJson, material->GetRenderPipeline()->GetShaderBindings());

    //创建材质实例
    std::shared_ptr<MaterialInstance> materialInstance;
    if(materialInstances.find(loadPlan.materialInstanceKey) != materialInstances.end())
    {
        materialInstance = materialInstances[loadPlan.materialInstanceKey];
    }
    else
    {
        materialInstance = material->CreateInstance();
        materialInstance->SetName(loadPlan.materialInstanceKey);
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
        if (!value.is_string())
        {
            throw std::runtime_error(
                "Texture binding must be a string in material instance: " + std::string(materialInstancePath));
        }

        const std::string textureAssetPath = value.get<std::string>();
        ValidateTextureAssetReference(textureName, textureAssetPath, materialInstancePath);
        const TextureBindingLoadDesc textureLoadDesc = LoadTextureAssetDesc(textureAssetPath);
        const std::string textureCacheKey = BuildTextureCacheKey(textureLoadDesc);
        std::shared_ptr<Texture> texture;
        if(textures.find(textureCacheKey) != textures.end())
        {
            texture = textures[textureCacheKey];
        }
        else
        {
            texture = std::make_shared<Texture>(textureLoadDesc.source, ToTextureCreateDesc(textureLoadDesc));
            //缓存贴图
            textures[textureCacheKey] = texture;
        }
        materialInstance->SetTexture(textureName, texture);
    }

    //缓存材质和材质实例
    materials.emplace(loadPlan.materialKey, material);
    if (passName != "geometry")
    {
        materials[loadPlan.shaderName] = material;
    }
    materialInstances.emplace(loadPlan.materialInstanceKey, materialInstance);

    return materialInstance;
}
