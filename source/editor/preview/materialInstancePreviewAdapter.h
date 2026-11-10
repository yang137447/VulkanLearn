#pragma once

#include "editor/preview/materialInstancePreviewTypes.h"
#include "render/editorBridge/materialInstancePreviewAdapter.h"

namespace VL::Editor::Preview
{

// P1 forwarder: the adapter contract is runtime-owned; RenderSystem
// implements the owner interface without an editor include.
using ::VL::IMaterialInstancePreviewAdapter;
using ::VL::IRendererMaterialInstancePreviewOwner;
using ::VL::IRendererMaterialInstancePreviewSession;
using ::VL::RendererOwnedMaterialInstancePreviewAdapter;
using ::VL::UnavailableMaterialInstancePreviewAdapter;

} // namespace VL::Editor::Preview
