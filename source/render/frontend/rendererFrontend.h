#pragma once

#include "core/runtimeResult.h"
#include "render/frontend/renderScene.h"
#include "world/worldSnapshot.h"

namespace VL
{

// Translates immutable WorldSnapshot data into render-domain grouping. This is
// the boundary where gameplay-shaped data becomes renderer-shaped data.
class RendererFrontend
{
public:
    RuntimeResult<RenderScene> BuildRenderScene(const WorldSnapshot& snapshot) const;
};

} // namespace VL
