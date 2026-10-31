#include "hair/tressfxGroupSimulation.h"

#include <algorithm>
#include <stdexcept>

namespace VL::Hair
{

bool IsTressfxGroupSimulationOverrideField(std::string_view field)
{
    for (std::string_view allowed : TressfxGroupSimulationOverrideFields)
        if (field == allowed)
            return true;
    return false;
}

uint32_t SelectTressfxGroupSimulationIndex(
    uint32_t guideGroupId, size_t configCount)
{
    if (configCount == 0 || guideGroupId >= configCount)
        return 0;
    return guideGroupId;
}

std::vector<TressfxSimulationConfig> ResolveTressfxGroupSimulationConfigs(
    const TressfxSimulationConfig& base,
    const std::vector<TressfxGuideGroupPlan>& groups,
    const TressfxPlacementBehavior& placement,
    const std::string& context)
{
    // 组 0 只有资产默认值与放置覆盖；命名组在此之上叠自己的稀疏覆盖，
    // 最后仍由共享入口应用 placement，保证关卡放置永远压过组。
    std::vector<TressfxSimulationConfig> configs;
    configs.reserve(groups.size() + 1);
    configs.push_back(ResolveTressfxBehaviorLayers(
        base, nlohmann::json::object(), placement, context));
    for (const TressfxGuideGroupPlan& group : groups)
    {
        for (auto field = group.simulationOverrides.begin();
             field != group.simulationOverrides.end(); ++field)
            if (!IsTressfxGroupSimulationOverrideField(field.key()))
                throw std::runtime_error(
                    "TressFX group \"" + group.name +
                    "\" simulationOverrides field \"" + field.key() +
                    "\" cannot be applied per group" +
                    (context.empty() ? std::string() : ": " + context));
        configs.push_back(ResolveTressfxBehaviorLayers(
            base, group.simulationOverrides, placement, context));
    }
    return configs;
}

TressfxGroupSimulationGpu MakeTressfxGroupSimulationGpu(
    const TressfxSimulationConfig& config)
{
    TressfxGroupSimulationGpu gpu;
    gpu.values[0] = config.damping;
    gpu.values[1] = config.bendStiffness;
    gpu.values[2] = config.globalShapeStiffness;
    gpu.values[3] = config.globalShapeEffectiveRange;
    gpu.values[4] = config.vspCoefficient;
    gpu.values[5] = config.vspAccelThresholdMax;
    return gpu;
}

std::vector<TressfxGroupSimulationGpu> BuildTressfxGroupSimulationGpuData(
    const std::vector<TressfxSimulationConfig>& configs, size_t capacity)
{
    if (configs.empty())
        return {};
    std::vector<TressfxGroupSimulationGpu> data;
    const size_t tableSize = std::max(configs.size(), capacity);
    data.reserve(tableSize);
    for (const TressfxSimulationConfig& config : configs)
        data.push_back(MakeTressfxGroupSimulationGpu(config));
    // 固定容量 descriptor 的长度决定 shader 看到的组数；尾部复制定位后的
    // base 配置，使越界/尚未重映射的旧组号回退到安全参数而不是全零。
    while (data.size() < tableSize)
        data.push_back(data.front());
    return data;
}

std::string MakeTressfxGroupSimulationKey(
    const std::vector<TressfxSimulationConfig>& configs)
{
    if (configs.size() <= 1)
        return {};
    nlohmann::json identity = nlohmann::json::array();
    for (const TressfxSimulationConfig& config : configs)
        identity.push_back({
            {"damping", config.damping},
            {"bendStiffness", config.bendStiffness},
            {"globalShapeStiffness", config.globalShapeStiffness},
            {"globalShapeEffectiveRange", config.globalShapeEffectiveRange},
            {"vspCoefficient", config.vspCoefficient},
            {"vspAccelThresholdMax", config.vspAccelThresholdMax}});
    return identity.dump();
}

} // namespace VL::Hair
