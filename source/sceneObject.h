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
    inline const Eigen::Vector3f& GetPosition() const { return position; }
    inline const Eigen::Vector3f& GetRotation() const { return rotation;}
    inline const Eigen::Vector3f& GetScale() const { return scale; }
    Eigen::Vector3f GetForwordVector() const;
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

    Eigen::Matrix4f modelMatrix;
};

class SunLight: public SceneNode {
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
  void SetColor(const Eigen::Vector4f& color) { this->color = color; }
  void SetSpecular(const Eigen::Vector4f& specular) { this->specular = specular; }
  void SetIntensity(float intensity) { this->intensity = intensity; }
  //void SetAttenuation(float constant, float linear, float quadratic);
  inline const Eigen::Vector4f& GetColor() const { return color; }
  inline const Eigen::Vector4f& GetSpecular() const { return specular; }
  inline float GetIntensity() const { return intensity; }
private:
  Eigen::Vector4f color;
  Eigen::Vector4f specular;
  float intensity;
};

class Camera: public SceneNode {
public:
  Camera();
  void SetHFOV(float fov);
  void SetClip(float near, float far);
  inline float GetHFOV() const { return hFov; }
  inline float GetClipNear() const { return clipNear; }
  inline float GetClipFar() const { return clipFar;}
  void SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up);
  void SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f cameraRotation);
  void SetProjection(float fov, float aspect, float near, float far);
  Eigen::Matrix4f& GetModelMatrix();
  Eigen::Matrix4f& GetViewMatrix();
  Eigen::Matrix4f& GetProjectionMatrix();
  void updateViewMatrix();

private:
  float hFov;
  float clipNear;
  float clipFar;

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
    inline const std::vector<vk::DescriptorSet>& GetDescriptorSets() const { return descriptorSets; }
    inline std::vector<void*>& GetUboModelMapped() { return uboModel.uniformBuffersMapped; }
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

    UBO uboModel;

    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    std::shared_ptr<RenderableObject> renderableObject;
    std::shared_ptr<MaterialInstance> materialInstance;
};