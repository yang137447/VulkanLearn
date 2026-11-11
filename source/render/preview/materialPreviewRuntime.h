#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "materialInstance.h"
#include "render/editorBridge/materialInstancePreviewAdapter.h"

class RenderSystem;

namespace VL
{

using PreviewRenderStateValues = std::map<std::string, std::string>;

std::shared_ptr<IRendererMaterialInstancePreviewSession>
MakeMaterialInstancePreviewSession(
    RenderSystem& owner,
    MaterialInstancePreviewWorldIdentity world,
    NormalizedMaterialInstancePath path,
    uint64_t ownerGeneration,
    std::shared_ptr<MaterialInstance> materialInstance,
    MaterialInstanceNumericParameterValues baselineParameters,
    MaterialInstanceStateSnapshot::TextureMap baselineTextures,
    PreviewRenderStateValues baselineRenderStates,
    MaterialInstancePreviewDraft baselineDraft);

std::shared_ptr<IRendererMaterialInstancePreviewSession>
MakeOfflineMaterialInstancePreviewSession(
    RenderSystem& owner,
    MaterialInstancePreviewWorldIdentity world,
    NormalizedMaterialInstancePath path,
    MaterialInstancePreviewDraft baseline);

} // namespace VL
