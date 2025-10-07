#include "Eigen/Dense"
#include <memory>
#include <vulkan/vulkan.hpp>

class SceneNode
{
public:
    void SetPosition(Eigen::Vector3f& pos);
    void SetRotation(Eigen::Vector3f& rot);
    void SetScale(Eigen::Vector3f& scale);
    inline const Eigen::Vector3f& GetPosition() const { return position; }
    inline const Eigen::Vector3f& GetRotation() const { return rotation; }
    inline const Eigen::Vector3f& GetScale() const { return scale; }

protected:
    Eigen::Vector3f position;
    Eigen::Vector3f rotation;
    Eigen::Vector3f scale;
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

class Camera: public SceneNode {
public:
  void SetHFOV(float fov);
  void SetClip(float near, float far);
  inline float GetHFOV() const { return hFov; }
  inline float GetClipNear() const { return clipNear; }
  inline float GetClipFar() const { return clipFar;}

private:
  float hFov;
  float clipNear;
  float clipFar;
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
    inline std::vector<void*>& GetUniformBuffersMapped() { return uniformBuffersMapped; }
private:
    SceneObject();

    void SetRenderableObject(std::shared_ptr<RenderableObject> renderableObject);
    void SetMaterialInstance(std::shared_ptr<MaterialInstance> materialInstance);

    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void CreateDescriptorSets();
    void DestroyDescriptorSets();
    void UpdateDescriptorSet();

    uint32_t uniformBufferSize;
    std::vector<vk::Buffer> uniformBuffers;
    std::vector<vk::DeviceMemory> uniformBufferMemories;
    std::vector<void*> uniformBuffersMapped;
    std::vector<vk::DescriptorBufferInfo> uniformBufferInfos;
    vk::DescriptorImageInfo imageInfo;

    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    std::shared_ptr<RenderableObject> renderableObject;
    std::shared_ptr<MaterialInstance> materialInstance;
};