#pragma once
#include "Eigen/Dense"
#include <memory>
#include <iomanip>
#include <vulkan/vulkan.hpp>
#include"baseStructs.h"

class SceneNode
{
public:
    void SetPosition(Eigen::Vector3f& position);
    void SetRotation(Eigen::Vector3f& rotation);
    void SetRotation(Eigen::Quaternionf& quaternion);
    void SetRotation(Eigen::Quaternionf quaternion);
    void SetDeltaRotation(Eigen::Vector3f& deltaRotation);
    void SetScale(Eigen::Vector3f& scale);
    void SetName(const std::string& name) { this->name = name; }
    inline const std::string& GetName() const { return name; }
    inline const Eigen::Vector3f& GetPosition() const { return position; }
    inline const Eigen::Vector3f& GetRotation() const { return rotation;}
    inline const Eigen::Vector3f& GetScale() const { return scale; }
    Eigen::Vector3f GetForwardVector() const;
    Eigen::Vector3f GetRightVector() const;
    Eigen::Vector3f GetUpVector() const;

    void SetTransform(Eigen::Vector3f& position, Eigen::Vector3f& rotation, Eigen::Vector3f& scale);
    void UpdateModelMatrix();
    inline const Eigen::Matrix4f& GetModelMatrix() const { return modelMatrix; } 

protected:
    Eigen::Vector3f position;
    Eigen::Vector3f rotation;
    Eigen::Quaternionf quaternion;
    Eigen::Vector3f scale;
    std::string name;

    Eigen::Matrix4f modelMatrix;
};

class DirectinalLight: public SceneNode {
public:
  void SetColor(Eigen::Vector3f& color);
  void SetIntensity(float intensity);
  inline const Eigen::Vector3f GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }

private:
  Eigen::Vector3f color;
  float intensity;
};

class PointLight: public SceneNode {
public:
  void SetColor(const Eigen::Vector3f& color) { this->color = color; }
  void SetIntensity(float intensity) { this->intensity = intensity; }
  void SetRadius(float radius) { this->radius = radius; }
  //void SetAttenuation(float constant, float linear, float quadratic);
  inline const Eigen::Vector3f& GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }
  inline float GetRadius() const { return radius; }
private:
  Eigen::Vector3f color;
  float intensity;
  float radius;
};

class SpotLight: public SceneNode {
public:
  void SetColor(const Eigen::Vector3f& color) { this->color = color; }
  void SetIntensity(float intensity) { this->intensity = intensity; }
  void SetRadius(float radius) { this->radius = radius; }
  void SetConeAngleOuter(float coneAngleOuter) { this->coneAngleOuter = coneAngleOuter; }
  void SetConeAngleInner(float coneAngleInner) { this->coneAngleInner = coneAngleInner; }
  //void SetAttenuation(float constant, float linear, float quadratic);
  inline const Eigen::Vector3f& GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }
  inline float GetRadius() const { return radius; }
  inline float GetConeAngleOuter() const { return coneAngleOuter; }
  inline float GetConeAngleInner() const { return coneAngleInner; }
private:
  Eigen::Vector3f color;
  float intensity;
  float radius;
  float coneAngleOuter;
  float coneAngleInner;
};

class Camera: public SceneNode {
public:
  Camera();
  void SetSize(float size);
  void SetHFOV(float fov);
  void SetClip(float near, float far);
  inline float GetHFOV() const { return hFov; }
  inline float GetClipNear() const { return clipNear; }
  inline float GetClipFar() const { return clipFar;}
  void EnableOrthographic(bool enable);
  void SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up);
  void SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f cameraRotation);
  void SetProjection(float fov, float aspect, float near, float far);
  void SetOrthographic(float size, float aspect, float near, float far);
  Eigen::Matrix4f& GetModelMatrix();
  Eigen::Matrix4f& GetViewMatrix();
  Eigen::Matrix4f& GetProjectionMatrix();
  void updateViewMatrix();

private:
  float size;
  float hFov;
  float clipNear;
  float clipFar;

  bool isOrthographic;

  Eigen::Matrix4f modelMatrix;
  Eigen::Matrix4f viewMatrix;
  Eigen::Matrix4f projectionMatrix;
  Eigen::Matrix4f ndcMatrix;
};

class RenderableObject;
class MaterialInstance;
class SceneObject: public SceneNode{
public:
    SceneObject(std::shared_ptr<RenderableObject> renderableObject, std::shared_ptr<MaterialInstance> materialInstance);
    ~SceneObject();
    inline std::shared_ptr<RenderableObject> GetRenderableObject() const { return renderableObject; }
    inline std::shared_ptr<MaterialInstance> GetMaterialInstance() const { return materialInstance; }
    inline const std::vector<vk::DescriptorSet>& GetDescriptorSets(uint32_t swapChainImageIndex) const { return descriptorSets[swapChainImageIndex]; }
    inline const std::vector<vk::DescriptorSet>& GetDescriptorSetsForShadow(uint32_t swapChainImageIndex) const { return descriptorSetsShadow[swapChainImageIndex]; }
    inline std::vector<void*>& GetUboModelMapped() { return uboModel.buffersMapped; }
    //相关的场景和材质实例需要先行就绪
    void RenderInitialize();
private:
    SceneObject();

    void SetRenderableObject(std::shared_ptr<RenderableObject> renderableObject);
    void SetMaterialInstance(std::shared_ptr<MaterialInstance> materialInstance);

    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void CreateDescriptorSets();
    void DestroyDescriptorSets();
    void SetupDescriptors();
    void UpdateDescriptorSet();

    // shadow
    void CreateDescriptorSetsForShadow();
    void DestroyDescriptorSetsForShadow();
    void SetupDescriptorsForShadow();
    void UpdateDescriptorSetForShadow();

    Buffer uboModel;

    vk::DescriptorPool descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    vk::DescriptorPool descriptorPoolShadow;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSetsShadow;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSetsShadow;

    std::shared_ptr<RenderableObject> renderableObject;
    std::shared_ptr<MaterialInstance> materialInstance;
};
