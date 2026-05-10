#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>
#include "pipeline/graphicsPipelineBuilder.h"

class Texture;
class RenderableObject;
class Material;
class MaterialInstance;
class DirectionalLight;
class PointLight;
class SpotLight;
class Camera;
class SceneObject;
class PipelineFactory;

class SceneLoader{
    public:
    static SceneLoader& GetInstance()
    {
        static SceneLoader instance;
        return instance;
    }
    
    //加载场景
    void SetPipelineFactory(PipelineFactory* pipelineFactory) { this->pipelineFactory = pipelineFactory; }
    void LoadScene(const std::string& filename);

    //获取场景数据
    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>& GetSceneObjects() const { return sceneObjects;}
    const std::unordered_map<std::string, std::shared_ptr<Material>>& GetMaterials() const { return materials;}
    const std::unordered_map<std::string, std::shared_ptr<MaterialInstance>>& GetMaterialInstances() const { return materialInstances;}
    const std::shared_ptr<Camera>& GetCamera() const { return sceneCamera;}
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights;}
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLight() const { return pointLights;}
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLight() const { return spotLights;}
    // environment 节点解析出的运行时环境资源配置，供后续 IBL 预处理链继续复用。
    const std::string& GetEnvironmentHdrPath() const { return environmentHdrPath; }
    uint32_t GetEnvironmentCubeSize() const { return environmentCubeSize; }
    const std::shared_ptr<Texture>& GetEnvironmentCube() const { return environmentCube; }
private:
    SceneLoader();
    void LoadMeshObject(const nlohmann::basic_json<>& node);
    void LoadDirectionalLightObject(const nlohmann::basic_json<>& node);
    void LoadPointLightObject(const nlohmann::basic_json<>& node);
    void LoadSpotLightObject(const nlohmann::basic_json<>& node);
    void LoadCameraObject(const nlohmann::basic_json<>& node);
    void LoadEnvironmentObject(const nlohmann::basic_json<>& node);
    void LoadPassMaterial();
    std::shared_ptr<MaterialInstance> LoadMaterialInstance(const std::string_view materialInstancePath, vk::SampleCountFlagBits sampleCount, std::string_view passName = "geometry", const GraphicsPipelineStateDesc& pipelineStateDesc = {});

    //场景数据
    std::unordered_map<std::string, std::shared_ptr<RenderableObject>> objects; //模型相对路径和模型对象
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> sceneObjects; //模型名字和场景对象
    std::unordered_map<std::string, std::shared_ptr<Material>> materials; //shader名和材质对象
    std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> materialInstances; //材质实例相对路径和材质实例对象
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures; //贴图相对路径和贴图对象

    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
    std::shared_ptr<Camera> sceneCamera;

    // 当前场景声明的环境贴图输入及 cubemap 目标分辨率。
    std::string environmentHdrPath;
    uint32_t environmentCubeSize = 512;
    std::shared_ptr<Texture> environmentCube;
    PipelineFactory* pipelineFactory = nullptr;
};
