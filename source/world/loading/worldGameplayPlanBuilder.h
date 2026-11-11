#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "world/worldBuildPlan.h"

namespace VL
{

// Pure authoring-plan builder for gameplay runtime objects. Reads only the
// validated scene asset plan and the resolved scene JSON; never touches
// renderer resources, material keys, or GPU caches.
class WorldGameplayPlanBuilder
{
public:
    RuntimeResult<WorldGameplayPlans> Build(
        const SceneAssetBuildPlan& sceneAssetPlan,
        const nlohmann::json& resolvedSceneJson,
        std::string_view scenePath) const;
};

} // namespace VL
