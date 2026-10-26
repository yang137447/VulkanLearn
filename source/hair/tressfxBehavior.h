#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{

enum class TressfxSimulationMode { Inherit, Off, On };

// 放置保留稀疏作者覆盖；只在资源创建前与 TF 默认值合成，不修改共享几何。
struct TressfxPlacementBehavior
{
    TressfxSimulationMode mode = TressfxSimulationMode::Inherit;
    nlohmann::json overrides = nlohmann::json::object();
    std::optional<TressfxSimulationSkinning> skinning;
    nlohmann::json hairCollision = nlohmann::json::object();
};

nlohmann::json WriteTressfxSimulationValues(const TressfxSimulationConfig& config);
nlohmann::json WriteTressfxSkinning(const TressfxSimulationSkinning& skinning);
TressfxSimulationConfig ReadTressfxSimulationDefaults(
    const nlohmann::json& asset, const std::string& context);
TressfxPlacementBehavior ReadTressfxPlacementBehavior(
    const nlohmann::json& node, const std::string& context);
TressfxSimulationConfig ResolveTressfxBehavior(
    const TressfxSimulationConfig& defaults,
    const TressfxPlacementBehavior& placement, const std::string& context = {});
std::string MakeTressfxBehaviorKey(const TressfxSimulationConfig& config);

} // namespace VL::Hair
