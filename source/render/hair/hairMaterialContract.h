#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace VL
{

class HairResourceSet;
struct HairAzimuthalLutMetadata;

// 纯 CPU authoring 合同；它只校验 JSON 与已冻结的 LUT metadata，不触碰 Vulkan
// 句柄，便于在资产工具和单元测试中复用同一套参数边界。
void ValidateHairMaterialAuthoringContract(
    const nlohmann::json& effectiveMaterialInstanceJson,
    const HairAzimuthalLutMetadata& lutMetadata,
    std::string_view materialInstancePath);

// Hair 材质合同只在 World-local 资源和有效 MI 合并完成后执行。这里拒绝
// IOR/fiberRadius 与 LUT 不一致的材质，避免 shader 运行时消费错误坐标或旧 kernel。
void ValidateHairMaterialContract(
    const nlohmann::json& effectiveMaterialInstanceJson,
    const HairResourceSet* hairResources,
    std::string_view materialInstancePath);

} // namespace VL
