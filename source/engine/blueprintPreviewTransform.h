#pragma once

#include <filesystem>
#include "engine/blueprintPreviewRequest.h"
#include "script/scriptScene.h"
#include "ui/uiAction.h"
#include "world/world.h"

namespace VL
{
// GT 消费 UI 冻结的变换值与文档 owner 的权威草稿；只更新已导入模型矩阵/灯光快照。
// 作者声明保持不动，取消可直接恢复；World generation/预览身份隔离真实关卡。
inline bool ApplyBlueprintPreviewTransform(World& world, const BlueprintPreviewTransformInput& input,
    uint64_t acceptedRevision, const nlohmann::json& acceptedDraft)
{
    if (world.GetGeneration() != input.worldGeneration || input.documentRevision != acceptedRevision ||
        std::filesystem::path(world.GetScenePath()).filename() != "SC_model_preview.json" ||
        world.GetScriptContext()) return false;
    const auto identity = BlueprintPreviewIdentity(input.assetPath, acceptedRevision);
    nlohmann::json candidate;
    for (const auto& actor : world.GetScriptBlueprints())
        if (actor.at("id") == identity) { candidate = actor; break; }
    if (candidate.is_null()) return false;
    try
    {
        const auto expected = BuildBlueprintPreviewObjects(input.assetPath, acceptedRevision,
            acceptedRevision, acceptedDraft).at(0);
        if (candidate != expected) return false;
        if (!input.reset)
        {
            if (input.field != "position" && input.field != "rotation" && input.field != "scale") return false;
            const nlohmann::json value = input.value;
            ValidateScriptVector(value, input.field == "scale");
            if (input.componentId.empty())
            {
                if (input.field == "position") return false;
                candidate[input.field] = value;
            }
            else
            {
                bool found = false;
                for (auto& component : candidate["components"])
                    if (ScriptComponentId(component) == input.componentId)
                    {
                        component[input.field] = value;
                        found = true;
                        break;
                    }
                if (!found) return false;
            }
            ValidateScriptBlueprint(candidate);
            // 矩阵溢出也在生产边界拒绝，避免非有限 model 进入不可变渲染快照。
            for (size_t index = 0; index < candidate["components"].size(); ++index)
                if (!ScriptComponentMatrix(candidate, index).allFinite()) return false;
        }
        world.ApplyScriptBlueprintVisuals(candidate);
        return true;
    }
    catch (const std::exception&) { return false; }
}
} // namespace VL
