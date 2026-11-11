#pragma once

#include <string>

namespace VL
{

struct RenderScene;

// Material-instance preview commit shares these two RenderScene rewrite helpers
// with the RenderSystem override wrappers that remain in renderSystem.cpp.
void ApplyMaterialOverrideToRenderScene(
    RenderScene& renderScene,
    const std::string& materialInstancePath,
    const std::string& materialKey);

void RebuildRenderSceneMaterialGroupsImpl(RenderScene& renderScene);

} // namespace VL
