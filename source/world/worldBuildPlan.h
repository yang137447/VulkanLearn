#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/sceneAssetTypes.h"

namespace VL
{

// 场景作者声明的投射物配方。只保存作者路径与物理参数；渲染器负责把
// materialInstancePath 解析成实际 Material / MaterialInstance key。
struct ProjectileBuildPlan
{
    std::string materialInstancePath;
    float speedCmPerSecond = 3500.0f;
    float radiusCm = 1.5f;
    float massKg = 0.08f;
    float cooldownSeconds = 0.15f;
};

// WorldLoader 产出的纯 gameplay 作者计划。这里不出现任何渲染器资源 key、
// 缓存指针或 Vulkan 类型；WorldBuilder 在合并渲染器解析结果后才构造运行对象。
struct WorldGameplayPlans
{
    std::vector<SoftBodyObjectBuildPlan> softBodyObjectPlans;
    std::vector<GlassObjectBuildPlan> glassObjectPlans;
    std::optional<ProjectileBuildPlan> projectile;
};

// IO + 场景校验阶段的一次加载计划。renderer 可以包含这个纯头文件来消费
// 计划数据，但不能反过来让 WorldLoader 依赖渲染器。
struct WorldBuildPlan
{
    std::string scenePath;
    nlohmann::json sceneJson;
    nlohmann::json authoredSceneJson;
    SceneAssetBuildPlan sceneAssetPlan;
    WorldGameplayPlans gameplay;
};

// 渲染器对一个 gameplay 材质路径的解析结果。`materialKey` 由目标 pass、
// shader 变体与管线合同共同决定，只有渲染器材质加载器能给出。
struct ResolvedMaterialBinding
{
    // 规范化后的 MI 路径，作为 WorldBuilder 的查找键。
    std::string materialInstancePath;
    std::string materialKey;
    std::string materialInstanceKey;
};

// 渲染器资源加载的纯输出：网格/风计划与 gameplay 材质绑定。
// WorldBuilder 只消费这份数据和 WorldBuildPlan，不接触渲染缓存。
struct WorldAssemblyAssets
{
    std::vector<MeshObjectBuildPlan> meshObjectPlans;
    std::vector<WindResponseProfile> windResponseProfiles;
    std::unordered_map<std::string, ResolvedMaterialBinding> materialBindings;
};

// 渲染器只需要"要解析哪些材质路径"，不需要 gameplay 参数本身。
// required=false 表示缺失时允许退化（当前仅裂纹覆盖层）。
struct GameplayMaterialRequest
{
    std::string materialInstancePath;
    std::string objectName;
    bool required = true;
};

inline std::vector<GameplayMaterialRequest> CollectGameplayMaterialRequests(
    const WorldGameplayPlans& plans)
{
    std::vector<GameplayMaterialRequest> requests;
    for (const SoftBodyObjectBuildPlan& plan : plans.softBodyObjectPlans)
    {
        if (!plan.materialInstancePath.empty())
            requests.push_back(
                {plan.materialInstancePath, plan.objectName, true});
    }
    for (const GlassObjectBuildPlan& plan : plans.glassObjectPlans)
    {
        if (!plan.materialInstancePath.empty())
            requests.push_back(
                {plan.materialInstancePath, plan.objectName, true});
        if (!plan.crackMaterialInstancePath.empty())
            requests.push_back(
                {plan.crackMaterialInstancePath, plan.objectName, false});
    }
    if (plans.projectile.has_value() &&
        !plans.projectile->materialInstancePath.empty())
    {
        requests.push_back(
            {plans.projectile->materialInstancePath, "<projectile>", true});
    }
    return requests;
}

} // namespace VL
