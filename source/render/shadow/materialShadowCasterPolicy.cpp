#include "render/shadow/materialShadowCasterPolicy.h"

#include <stdexcept>

#include "material.h"

namespace VL
{

MaterialShadowCasterDecision ResolveMaterialShadowCaster(const Material& material)
{
    if (material.GetShadowPipeline())
    {
        return {MaterialShadowCasterKind::MaterialPass};
    }

    switch (material.GetShaderVariantKey().renderMode)
    {
    case RenderMode::Opaque:
        return {MaterialShadowCasterKind::CommonOpaque};
    case RenderMode::OpaqueClip:
        throw std::runtime_error(
            "OpaqueClip material reached ShadowCaster routing without a ShadowDepth pipeline");
    case RenderMode::TransparentAlphaBlend:
    case RenderMode::TransparentAdditive:
        return {MaterialShadowCasterKind::None};
    }

    throw std::runtime_error("Unsupported material render mode while resolving ShadowCaster");
}

} // namespace VL
