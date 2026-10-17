#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace VL
{
class World;
class ScriptRuntimeSubsystem;
struct UiAction;
namespace Editor { class SceneDocumentService; }

// GT 脚本作者适配器；场景修改暂存到 Scene owner，新资产经该 owner 显式创建，不操作 GPU。
bool StageScriptBlueprintAction(World& world, ScriptRuntimeSubsystem& runtime,
    Editor::SceneDocumentService& documents, const UiAction& action,
    std::string& actorId, std::string& error);
} // namespace VL
