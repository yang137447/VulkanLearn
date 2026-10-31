#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "hair/tressfxBehavior.h"

namespace VL
{

// 放置级绑定（placement binding）：场景对象 → WorldMeshObject → WorldSnapshot →
// RenderDrawPacket 共用的一份值语义记录。它只描述“这个放置如何解析自己的资源”，
// 具体含义由 kind 决定：
//   None            — 没有放置级绑定（普通共享对象）。
//   TressfxSkinning — GPU 毛发的根点骨骼绑定。sourceSceneObject 是父 mesh 组件的
//                     场景身份；rootBone / weights 属于放置，独立场景缺省使用根骨权重。
//
// 新增一种类型（例如以后别的 per-placement 网格）：
//   1. 在 Kind 里加一个枚举值；
//   2. 在 placementBindingResolver.h 的 ApplyPlacementBinding switch 里加一个
//      handler（模板 renderable → 新实例的状态复制/解析）；
//   3. 加载期解析在 rendererResourceLoadCoordinator.cpp 的
//      ResolvePlacementBindings switch 里加同样的 handler。
// 数据通路（scene/World/snapshot/draw packet/cache）不需要再改。
struct PlacementBinding
{
    enum class Kind
    {
        None,
        TressfxSkinning
    };

    Kind kind = Kind::None;
    std::string sourceSceneObject;
    std::string rootBone;
    std::string weights;
    // 已在加载边界合成的有效配置；快照与 spawn 沿用值语义，不再读 TF 文件。
    std::optional<TressfxSimulationConfig> tressfx;
    // 与 tressfx 同一次解析的 per-group 配置；组索引 0 是 base + placement，
    // 命名组 k 已按 base -> group -> placement 合成。空表示未携带（回退到
    // tressfx 的单份配置）。
    std::vector<TressfxSimulationConfig> tressfxGroupConfigs;

    bool IsEmpty() const { return kind == Kind::None; }
};

// 蓝图组件对象 → 绑定。优先读通用 `placementBinding` 对象；为了兼容 T4a/T4b 已经
// 生成的 `__tressfx*` 内部字段，缺失时回退到旧的四个键（新类型只写通用对象，
// 不再新增 `__xxx*` 专用字段）。未知 kind 按 None 返回，由调用方决定是否报错。
inline PlacementBinding ReadPlacementBindingFromComponent(const nlohmann::json& component)
{
    PlacementBinding binding;
    if (!component.is_object())
        return binding;
    if (component.contains("tressfx"))
    {
        const auto behavior = Hair::ReadTressfxPlacementBehavior(
            component.at("tressfx"), component.value("name", ""));
        if (behavior.skinning)
        {
            binding.kind = PlacementBinding::Kind::TressfxSkinning;
            binding.sourceSceneObject = behavior.skinning->source;
            binding.rootBone = behavior.skinning->rootBone;
            if (behavior.skinning->rootWeightsAuthored)
                binding.weights = behavior.skinning->rootWeights == TressfxRootWeightSource::Mesh
                    ? "mesh" : "rootBone";
        }
        return binding;
    }
    if (component.contains("placementBinding") && component.at("placementBinding").is_object())
    {
        const auto& node = component.at("placementBinding");
        const std::string kind = node.value("kind", "");
        if (kind == "tressfxSkinning")
            binding.kind = PlacementBinding::Kind::TressfxSkinning;
        binding.sourceSceneObject = node.value("source", "");
        binding.rootBone = node.value("rootBone", "");
        binding.weights = node.value("weights", "");
        return binding;
    }
    const std::string legacySource = component.value("__tressfxSkinningSource", "");
    if (!legacySource.empty())
    {
        binding.kind = PlacementBinding::Kind::TressfxSkinning;
        binding.sourceSceneObject = legacySource;
        binding.rootBone = component.value("__tressfxRootBone", "");
        binding.weights = component.value("__tressfxWeights", "");
    }
    return binding;
}

// 身份前缀重写：spawn 时把 `__blueprint/<template>/<comp>` 换成
// `__blueprint/<spawn>/<comp>`。只重写精确前缀，避免误改别的 actor。
inline void RebasePlacementBindingSource(PlacementBinding& binding,
    const std::string& previousPrefix, const std::string& newPrefix)
{
    if (binding.sourceSceneObject.rfind(previousPrefix, 0) == 0)
        binding.sourceSceneObject =
            newPrefix + binding.sourceSceneObject.substr(previousPrefix.size());
    if (binding.tressfx && binding.tressfx->skinning)
        binding.tressfx->skinning->source = binding.sourceSceneObject;
}

} // namespace VL
