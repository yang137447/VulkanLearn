#pragma once
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

class Texture;
class RenderableObject;
class Material;
class MaterialInstance;
class DirectinalLight;
class PointLight;
class SpotLight;
class Camera;
class SceneObject;

class SceneLoader{
    public:
    static SceneLoader& GetInstance()
    {
        static SceneLoader instance;
        return instance;
    }
    
    //加载场景
    void LoadScence(const std::string& filename);

    //获取场景数据
    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>& GetSceneObjects() const { return sceneObjects;}
    const std::unordered_map<std::string, std::shared_ptr<Material>>& GetMaterials() const { return materials;}
    const std::unordered_map<std::string, std::shared_ptr<MaterialInstance>>& GetMaterialInstances() const { return materialInstances;}
    const std::shared_ptr<Camera>& GetCamera() const { return sceneCamera;}
    const std::unordered_map<std::string, std::shared_ptr<DirectinalLight>>& GetDirectinalLight() const { return directinalLights;}
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLight() const { return pointLights;}
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLight() const { return spotLights;}
    
    const Eigen::Vector3f& GetAmbient() const { return ambient; }
private:
    SceneLoader();
    void LoadMeshObject(const nlohmann::basic_json<>& node);
    void LoadDirectinalLightObject(const nlohmann::basic_json<>& node);
    void LoadPointLightObject(const nlohmann::basic_json<>& node);
    void LoadSpotLightObject(const nlohmann::basic_json<>& node);
    void LoadCameraObject(const nlohmann::basic_json<>& node);
    void LoadEnvironmentObject(const nlohmann::basic_json<>& node);
    void LoadPassMaterial();

    //场景数据
    std::unordered_map<std::string, std::shared_ptr<RenderableObject>> objects; //模型相对路径和模型对象
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> sceneObjects; //模型名字和场景对象
    std::unordered_map<std::string, std::shared_ptr<Material>> materials; //shader名和材质对象
    std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> materialInstances; //材质实例相对路径和材质实例对象
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures; //贴图相对路径和贴图对象

    std::unordered_map<std::string, std::shared_ptr<DirectinalLight>> directinalLights;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
    std::shared_ptr<Camera> sceneCamera;

    Eigen::Vector3f ambient;
};