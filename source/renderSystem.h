#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

class Material;
class SceneObject;
//用于按材质分类渲染
class RenderSystem
{
public:
    void InitRenderObject();
    void Render();
private:
    void UpdateUniformBuffer(const std::shared_ptr<SceneObject> object);

    uint32_t currentFrame = 0;
    uint32_t swapchainImageIndex = 0;
    // 按基础材质分组
    std::unordered_map<std::string, std::vector<std::shared_ptr<SceneObject>>> objectsByMaterial;  //shader名, 对应物体
};