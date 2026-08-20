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
    case RenderMode::ThinTranslucent:
        // 当前 Shadow Map 不表达彩色透射或透明覆盖率；透明材质默认不投影，
        // 避免把薄灯罩错误地当作不透明遮挡物。
        return {MaterialShadowCasterKind::None};
    }

    throw std::runtime_error("Unsupported material render mode while resolving ShadowCaster");
}

} // namespace VL
