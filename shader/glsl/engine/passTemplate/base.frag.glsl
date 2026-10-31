#ifndef VL_PASS_TEMPLATE_BASE_FRAG_GLSL
#define VL_PASS_TEMPLATE_BASE_FRAG_GLSL

#include "../../common/commonUbo.glsl"
#include "../../materialFunction/mf_alphaClip.glsl"
#include "../materialPass.glsl"

#if VL_MATERIAL_OUTPUT_GBUFFER
    #include "../materialGBufferOutput.glsl"
#else
    #define VL_FORWARD_DECLARE_SHADOWMAP_INPUT
    #include "../materialForwardOutput.glsl"
#endif

layout(location = 0) in MaterialVaryings v2f;
#if defined(VL_TRESSFX_STRANDS) && VL_TRESSFX_STRANDS
#include "../tressfxCoverage.glsl"
layout(location = 8) flat in uint strandCoverageSeed;
layout(location = 9) flat in uint tressfxGroupId;
layout(location = 10) flat in float tressfxBrushWeight;
layout(location = 11) flat in uint tressfxBrushTargetGroup;
// 与 vertex factory 的 binding 5 同布局；片元只需颜色表读取，
// 不重复声明 group id buffer，避免把顶点函数带进 fragment stage。
layout(set = 2, binding = 6, std430) readonly buffer TressfxGuideGroupColors
{
    vec4 groupColors[];
} tressfxGuideGroupColors;
#endif

#if VL_MATERIAL_OUTPUT_GBUFFER
layout(location = 0) out vec4 outGBufferA;
layout(location = 1) out vec4 outGBufferB;
layout(location = 2) out vec4 outGBufferC;
layout(location = 3) out vec4 outGBufferD;
layout(location = 4) out vec4 outGBufferE;
layout(location = 5) out vec4 outGBufferVelocity;
layout(location = 6) out vec4 outGBufferF;
layout(location = 7) out vec4 outSceneColorBase;
#elif VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT
// ThinTranslucent 的输出路径随旧实现于 2026-09-12 清理：双源 Add/Mul 输出、混合状态与
// 对应的 build 函数都已删除。这里显式报错而不是留一条空分支，避免 macro 被重新打开后
// 静默产出错误画面；按论文重建该模型时，要连 renderMode 与混合状态一起重新设计。
#error "VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT was removed with the ThinTranslucent implementation (2026-09-12)"
#else
layout(location = 0) out vec4 outSceneColor;
layout(location = 1) out vec4 outSelectionMask;
#endif

void main()
{
    MaterialFunctionContext context = CreateMaterialFunctionContext(v2f);
    MaterialInputs inputs = EvaluateMaterialInputs(context);
#if defined(VL_TRESSFX_STRANDS) && VL_TRESSFX_STRANDS
    // 分组色直接顶掉材质基色（等价于改 M_strandPbr 的 u_tintColor），
    // 让 roughness/specular/AO/阴影继续参与 PBR；默认组保持原材质基色。
    // 调色板按 sRGB 存储，进入线性光照前在这里转线性。
    const uint basePreviewFlags = uint(max(uboM.selectionData.w, 0.0));
    if ((basePreviewFlags & 1u) != 0u && tressfxGroupId != 0u &&
        tressfxGuideGroupColors.groupColors.length() != 0u)
    {
        const uint baseGroupId = min(
            tressfxGroupId, tressfxGuideGroupColors.groupColors.length() - 1u);
        inputs.baseColor = pow(
            tressfxGuideGroupColors.groupColors[baseGroupId].rgb, vec3(2.2));
    }
    // 裁除在 Pass 中统一执行；失败的样本不写深度或选中标记。
    // 保留样本是全覆盖，不能再 alpha 混合一次，否则期望颜色会被重复乘覆盖率。
    if (inputs.opacity <= TressfxCoverageThreshold(uvec2(gl_FragCoord.xy), strandCoverageSeed))
        discard;
    inputs.opacity = 1.0;
#endif
    MaterialSurface surface = ResolveMaterialSurface(inputs, context);

    // Coverage 是 Pass 行為：所有需要 Alpha Clip 的 pass 都在消費 Surface 後統一執行。
#if MATERIAL_USES_OPACITY_MASK
    // clip 用的是作者的原始 opacity mask，不是 closure 的 coverage。若模型把 coverage
    // 再乘进几何裁剪，同一张卡会被稀释两次（Hair 的 Core/Fringe 分层就踩过这个坑）。
    ApplyAlphaClip(inputs.opacityMask, u_alphaClipThreshold);
#endif

#if VL_MATERIAL_OUTPUT_GBUFFER
    GBufferData gbuffer = BuildMaterialGBufferOutput(surface, context);
    outGBufferA = gbuffer.gbufferA;
    outGBufferB = gbuffer.gbufferB;
    outGBufferC = gbuffer.gbufferC;
    outGBufferD = gbuffer.gbufferD;
    outGBufferE = gbuffer.gbufferE;
    outGBufferVelocity = gbuffer.gbufferVelocity;
    // Geometry 已占满设备允许的 8 个颜色附件，选中标记复用 Velocity 的保留 z 通道；
    // Eye 的 z/w 有既定语义，因此只对普通 GBuffer 表面写入标记。
    if (surface.shadingModel != SHADING_MODEL_EYE)
        outGBufferVelocity.z = uboM.selectionData.x;
    outGBufferF = gbuffer.gbufferF;
    outSceneColorBase = gbuffer.sceneColorBase;
#else
    outSceneColor = BuildMaterialForwardOutput(surface);
    outSelectionMask = vec4(uboM.selectionData.x * step(0.001, inputs.opacity));
#if defined(VL_TRESSFX_STRANDS) && VL_TRESSFX_STRANDS
    // 分组预览只改 forward RGB：strand 的 coverage/opacity 已在上方统一为 1，
    // 这里不能碰 alpha，否则会破坏 coverage discard 的保留语义。
    const uint previewFlags = uint(max(uboM.selectionData.w, 0.0));
    const int selectedGroup = int(uboM.selectionData.z);
    uint groupId = tressfxGroupId;
    if (tressfxGuideGroupColors.groupColors.length() == 0u)
    {
        // 无颜色表时不改变材质外观；正常情况下默认组 0 始终存在。
    }
    else
    {
        if (groupId >= tressfxGuideGroupColors.groupColors.length())
            groupId = 0u;
        if ((previewFlags & 2u) != 0u && selectedGroup >= 0 && int(groupId) != selectedGroup)
            discard;
        // Ghost 只改 RGB；alpha/coverage 已在上面统一为 1，不能被这里重新解释。
        if ((previewFlags & 4u) != 0u && tressfxBrushWeight > 0.0 &&
            tressfxBrushTargetGroup < tressfxGuideGroupColors.groupColors.length())
        {
            const bool removeMode = (previewFlags & 8u) != 0u;
            const vec3 brushColor = removeMode
                ? vec3(1.0, 0.32, 0.14)
                : pow(tressfxGuideGroupColors.groupColors[tressfxBrushTargetGroup].rgb,
                    vec3(2.2));
            if (removeMode)
            {
                if (groupId == tressfxBrushTargetGroup)
                    outSceneColor.rgb = mix(outSceneColor.rgb, brushColor,
                        tressfxBrushWeight * 0.22);
            }
            else if (groupId == tressfxBrushTargetGroup)
            {
                // 已在目标组的 guide 只做目标色 tint；Group Colors 关闭时也保持材质层次。
                const vec3 tint = mix(vec3(1.0), brushColor, 0.35);
                outSceneColor.rgb *= tint;
            }
            else
            {
                outSceneColor.rgb = mix(outSceneColor.rgb, brushColor,
                    tressfxBrushWeight * 0.22);
            }
        }
    }
#endif
#endif
}

#endif
