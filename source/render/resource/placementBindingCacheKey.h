#pragma once

#include "hair/tressfxBehavior.h"
#include "scene/placementBinding.h"

namespace VL
{

// 只缓存不依赖实例身份的绑定载荷；源组件名在应用时重写。行为、外部文件与源
// 身体/SK 的内容身份全部参与，避免同一路径换代后复用旧关节下标或旧转移权重。
inline std::string MakePlacementBindingCacheKey(
    const std::string& templateModelKey, const PlacementBinding& binding,
    const TressfxSimulationConfig& config, const std::string& resourceIdentity,
    const std::string& sourceAssetIdentity)
{
    TressfxSimulationConfig sharedConfig = config;
    if (sharedConfig.skinning)
        sharedConfig.skinning->source.clear();
    return nlohmann::json::array({templateModelKey, static_cast<int>(binding.kind),
        binding.rootBone, binding.weights, Hair::MakeTressfxBehaviorKey(sharedConfig),
        resourceIdentity, sourceAssetIdentity}).dump();
}

} // namespace VL
