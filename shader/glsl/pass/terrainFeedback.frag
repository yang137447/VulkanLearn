#version 450

#include "../common/commonUbo.glsl"
#include "generate/M_terrainFeedbackParamter.glsl"

layout(location = 0) in vec2 inPageUv;
layout(location = 0) out vec4 outFeedback;

// Set 3 来自 RenderGraph worldTexture 输入，与 M_terrain 采样的是同一张页表。
layout(set = 3, binding = 0) uniform sampler2D pageTable;

// 编码格式版本 1（与 terrain/terrainFeedback.h 保持一致）：
// R[0..4]=pageX, bit5=miss, bit6=valid, bit7=0
// G[0..4]=pageY, B=priority, A=frameTag（0 保留）
// 本 pass 挂 128×128 depth 且 depthCompareOp=less：深度只用于近页优先，
// 让最近的地形页赢得 feedback texel，避免后绘制的远页覆盖近页 miss。
void main()
{
    const int pageTableSize = int(u_pageTableSize + 0.5);
    // 与 mf_terrainLayerBlend 同一内缩规则：全局 UV 为 1.0 时不能落到 size。
    const float pageInset = 0.5 / (u_pageTableSize * u_pageResolution);
    const vec2 pageUvGlobal = clamp(
        inPageUv,
        vec2(pageInset),
        vec2(1.0 - pageInset));
    const vec2 pageUv = pageUvGlobal * float(pageTableSize);
    const ivec2 page = ivec2(floor(pageUv));

    // 用屏幕空间导数近似页覆盖率：近处/斜视角大页优先驻留，远处页优先淘汰。
    const float pageDerivative =
        max(fwidth(pageUv.x), fwidth(pageUv.y));
    const float pagePixels =
        1.0 / max(pageDerivative, 1.0e-4);
    const float coverage =
        clamp(log2(pagePixels + 1.0) / 8.0, 0.0, 1.0);
    uint priority = uint(round(255.0 * coverage));
    // terrain 的 Set 2 UBOModel 在 VS/FS 都被静态引用；feedback 也必须
    // 在 fragment 引用同一 UBO，否则两个 pipeline layout 的 stageFlags
    // 不一致，逐对象绑定 Set 2 会被 Vulkan 判为不兼容。
    // selectionData.x 当前为 0/1，step(2.0, x) 恒为 0，不改变优先级。
    priority = min(priority + uint(step(2.0, uboM.selectionData.x)), 255u);

    // page table A=0 表示该虚拟页尚未驻留；feedback 只记录 miss，
    // P2 不允许在这里 RequestPage，也不采样 ID/weight atlas。
    const bool miss = texelFetch(pageTable, page, 0).a < 0.5;
    if (miss)
        priority = max(priority, 192u);

    const uint packedX =
        uint(page.x) | (miss ? 32u : 0u) | 64u;
    const uint packedY = uint(page.y);
    const uint packedB = min(priority, 255u);
    const uint packedA = uint(round(clamp(u_frameTag, 1.0, 255.0)));
    outFeedback = vec4(
        float(packedX),
        float(packedY),
        float(packedB),
        float(packedA)) / 255.0;
}
