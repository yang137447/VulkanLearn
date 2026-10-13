#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "scene/sceneAssetTypes.h"

namespace VL
{

struct WorldBuildPlan
{
    std::string scenePath;
    nlohmann::json sceneJson;
    SceneAssetBuildPlan sceneAssetPlan;
    std::vector<MeshObjectBuildPlan> meshObjectPlans;
    std::vector<SpeedTreeWindProfile> speedTreeWindProfiles;
};

// IO and validation stage for loading a World. It produces a pure build plan;
// runtime object creation belongs to WorldBuilder or renderer resource loaders.
class WorldLoader
{
public:
    RuntimeResult<WorldBuildPlan> Load(const std::string& scenePath) const;

    // 加载场景，并把**附加场景对象**并入 `objects` 后再校验与规划。
    //
    // 用途：模型编辑器要用一个固定的中性预览环境观察任意模型，而"把模型放进那个环境"
    // 不该靠临时生成场景文件 —— 那会在磁盘上留下第二份真相，并且在异常退出时残留。
    // 附加对象在这里**与场景作者写的对象走完全相同的路径**：同样的 schema 校验、
    // 同样的网格规划、同样的材质解析。因此注入的模型不是一个特殊通道，
    // 而是"这一次加载里多了一个对象"。
    //
    // `extraObjects` 必须是 JSON 对象数组，且每个对象都要像场景对象那样自带
    // `name` 与 `type`（校验器按这两个字段分派）；格式错误会在校验阶段报错，
    // 不会被静默忽略。
    //
    // 注意 mesh 路径一律是**资源根相对**（`Maps/SC_x/Meshes/SM_y.json`），
    // 与场景文件所在目录无关，因此注入别的目录下的模型无需改写路径。
    RuntimeResult<WorldBuildPlan> Load(
        const std::string& scenePath,
        const nlohmann::json& extraObjects) const;
};

} // namespace VL
