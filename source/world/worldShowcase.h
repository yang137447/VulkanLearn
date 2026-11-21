#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{

// 关卡级展示配置：场景 JSON 顶层 "showcase" 块的运行时形态。
// 只有声明了该块的关卡才启用展示 UI 与交互；World 只保存数据，
// 状态机在 source/showcase/showcaseController.*。
struct ShowcaseMaterialVariant
{
    std::string id;         // 稳定 id，例如 "satin"
    std::string label;      // "Satin 缎面"
    std::string tag;        // "CLOTH · ASHIKHMIN 2000"
    std::string objectName; // 场景里已存在的 mesh 对象名
    std::string sheenHex;   // "#FFF8EB"
};

struct WorldShowcase
{
    bool enabled = false;
    std::string kind;                       // "cloth"
    std::string rotateObject;               // 可选：额外的旋转锚点对象
    std::array<float, 3> rotatePivot{};     // cm
    std::string cameraObject;               // 空表示取场景第一台相机
    std::size_t defaultIndex = 0;
    std::vector<ShowcaseMaterialVariant> materials;
};

// C# 蓝图提交给展示控制器的状态请求；由 GT 在脚本同步后消费一次。
// materialId 为空表示只改面板开关；id 由关卡 showcase.materials 解析，
// 这样蓝图不需要知道数组顺序。
struct ShowcaseStateRequest
{
    std::string materialId;
    bool panelOpen = false;
};

// 场景 JSON 顶层 "showcase" 块 → 运行时数据。结构已由
// ValidateShowcaseBlock 校验，这里只做取值。
inline WorldShowcase ReadSceneShowcase(const nlohmann::json& scene)
{
    WorldShowcase result;
    if (!scene.contains("showcase"))
    {
        return result;
    }
    const auto& block = scene.at("showcase");
    result.enabled = true;
    result.kind = block.value("kind", std::string());
    result.rotateObject = block.value("rotateObject", std::string());
    result.cameraObject = block.value("cameraObject", std::string());
    if (block.contains("rotatePivot"))
    {
        const auto& pivot = block.at("rotatePivot");
        for (std::size_t index = 0; index < result.rotatePivot.size(); ++index)
        {
            result.rotatePivot[index] = pivot[index].get<float>();
        }
    }
    for (const auto& material : block.at("materials"))
    {
        ShowcaseMaterialVariant variant;
        variant.id = material.at("id").get<std::string>();
        variant.label = material.at("label").get<std::string>();
        variant.tag = material.value("tag", std::string());
        variant.objectName = material.at("object").get<std::string>();
        variant.sheenHex = material.value("sheenHex", std::string());
        result.materials.push_back(std::move(variant));
    }
    if (block.contains("defaultMaterial"))
    {
        const std::string defaultId =
            block.at("defaultMaterial").get<std::string>();
        for (std::size_t index = 0; index < result.materials.size(); ++index)
        {
            if (result.materials[index].id == defaultId)
            {
                result.defaultIndex = index;
                break;
            }
        }
    }
    return result;
}

} // namespace VL
