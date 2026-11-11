#pragma once

#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "materialInstance.h"
#include "render/editorBridge/materialInstancePreviewTypes.h"

namespace VL
{

using PreviewRenderStateValues = std::map<std::string, std::string>;

struct NumericPreviewDraftParseResult
{
    std::optional<MaterialInstanceNumericParameterValues> values;
    std::optional<nlohmann::json> serializedJson;
    PreviewRenderStateValues renderStates;
    std::string errorMessage;
};

PreviewRenderStateValues BuildPreviewRenderStateValues(
    const nlohmann::json& effectiveMaterialInstanceJson);

bool ReadFinitePreviewNumber(const nlohmann::json& value, float& outValue);

NumericPreviewDraftParseResult ParseNumericPreviewDraft(
    const MaterialInstancePreviewDraft& draft,
    const NormalizedMaterialInstancePath& expectedPath,
    const MaterialInstanceStateSnapshot::TextureMap& activeTextures,
    const nlohmann::json& frozenDefinition);

} // namespace VL
