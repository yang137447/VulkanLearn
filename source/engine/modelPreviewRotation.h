#pragma once

#include <filesystem>
#include <optional>

#include "engine/modelPreviewRequest.h"
#include "engine/runtimeCommand.h"

namespace VL
{

// UI actor 地址含类型前缀，World placement 地址却是原始 scene identity；转换只发生在 GT。
// 旧标签手势不能落到换代后恰好同名的对象，普通关卡也不能借同名对象接收预览操作。
inline std::optional<RuntimeCommand> BuildModelPreviewRotationCommand(
    const ModelPreviewRotationInput& input, uint64_t worldGeneration, const std::string& scenePath)
{
    if (input.worldGeneration == 0 || input.worldGeneration != worldGeneration ||
        std::filesystem::path(scenePath).filename() != "SC_model_preview.json")
        return std::nullopt;
    RuntimeCommand command;
    command.type = RuntimeCommandType::SetMeshPlacement;
    command.sourceText = "ui";
    command.actorProperty.worldGeneration = input.worldGeneration;
    command.meshPlacementMode = MeshPlacementMode::Absolute;
    command.meshPlacement.identity = "preview_model";
    command.meshPlacement.rotation = {input.pitch, input.yaw, 0.0f};
    return command;
}

} // namespace VL
