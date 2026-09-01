#pragma once

#include <string>

#include <Eigen/Dense>

class SceneNode
{
public:
    void SetPosition(const Eigen::Vector3f& position);
    void SetRotation(const Eigen::Vector3f& rotation);
    void SetRotation(Eigen::Quaternionf quaternion);
    void SetDeltaRotation(const Eigen::Vector3f& deltaRotation);
    void SetScale(const Eigen::Vector3f& scale);
    void SetName(const std::string& name) { this->name = name; }
    inline const std::string& GetName() const { return name; }
    inline const Eigen::Vector3f& GetPosition() const { return position; }
    inline const Eigen::Vector3f& GetRotation() const { return rotation;}
    inline const Eigen::Vector3f& GetScale() const { return scale; }
    Eigen::Vector3f GetForwardVector() const;
    Eigen::Vector3f GetRightVector() const;
    Eigen::Vector3f GetUpVector() const;

    void SetTransform(
        const Eigen::Vector3f& position,
        const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);
    void UpdateModelMatrix();
    inline const Eigen::Matrix4f& GetModelMatrix() const { return modelMatrix; }

protected:
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    Eigen::Quaternionf quaternion = Eigen::Quaternionf::Identity();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
    std::string name;

    Eigen::Matrix4f modelMatrix = Eigen::Matrix4f::Identity();
};

class DirectionalLight: public SceneNode {
public:
  void SetColor(const Eigen::Vector3f& color);
  void SetIntensity(float intensity);
  inline const Eigen::Vector3f& GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }

private:
  Eigen::Vector3f color = Eigen::Vector3f::Ones();
  float intensity = 1.0f;
};

class PointLight: public SceneNode {
public:
  void SetColor(const Eigen::Vector3f& color) { this->color = color; }
  void SetIntensity(float intensity) { this->intensity = intensity; }
  void SetRadius(float radius) { this->radius = radius; }
  inline const Eigen::Vector3f& GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }
  inline float GetRadius() const { return radius; }
private:
  Eigen::Vector3f color = Eigen::Vector3f::Ones();
  float intensity = 1.0f;
  float radius = 1.0f;
};

class SpotLight: public SceneNode {
public:
  void SetColor(const Eigen::Vector3f& color) { this->color = color; }
  void SetIntensity(float intensity) { this->intensity = intensity; }
  void SetRadius(float radius) { this->radius = radius; }
  void SetConeAngleOuter(float coneAngleOuter) { this->coneAngleOuter = coneAngleOuter; }
  void SetConeAngleInner(float coneAngleInner) { this->coneAngleInner = coneAngleInner; }
  inline const Eigen::Vector3f& GetColor() const { return color; }
  inline float GetIntensity() const { return intensity; }
  inline float GetRadius() const { return radius; }
  inline float GetConeAngleOuter() const { return coneAngleOuter; }
  inline float GetConeAngleInner() const { return coneAngleInner; }
private:
  Eigen::Vector3f color = Eigen::Vector3f::Ones();
  float intensity = 1.0f;
  float radius = 1.0f;
  float coneAngleOuter = 0.0f;
  float coneAngleInner = 0.0f;
};

class Camera: public SceneNode {
public:
  Camera();
  void SetHFOV(float fov);
  void SetClip(float near, float far);
  inline float GetHFOV() const { return hFov; }
  inline float GetClipNear() const { return clipNear; }
  inline float GetClipFar() const { return clipFar;}
  void SetCamera(const Eigen::Vector3f& cameraPosition, const Eigen::Vector3f& lookAtPosition, const Eigen::Vector3f& up);
  void SetInitialLookAt(const Eigen::Vector3f& cameraPosition, const Eigen::Vector3f& lookAtPosition);
  void SetCamera(const Eigen::Vector3f& cameraPosition, const Eigen::Vector3f& cameraRotation);
  inline bool HasInitialLookAt() const { return hasInitialLookAt; }
  void SetProjection(float fov, float aspect, float near, float far);
  void SetOrthographic(float size, float aspect, float near, float far);
  const Eigen::Matrix4f& GetViewMatrix();
  Eigen::Matrix4f GetProjectionMatrix() const;

private:
  void UpdateViewMatrix();

  float hFov = 90.0f;
  float clipNear = 0.1f;
  float clipFar = 10.0f;

  Eigen::Matrix4f viewMatrix = Eigen::Matrix4f::Identity();
  Eigen::Matrix4f projectionMatrix = Eigen::Matrix4f::Identity();
  Eigen::Matrix4f ndcMatrix = Eigen::Matrix4f::Identity();
  bool hasInitialLookAt = false;
};
