#version 450

#include "../common/commonUbo.glsl"
#include "../materialFunction/terrainLayerVTMath.glsl"
#include "generate/M_terrainFeedbackParamter.glsl"

layout(location = 0) in vec2 inPageUv;
layout(location = 1) in vec2 inMaterialUv;
layout(location = 0) out vec4 outFeedback;
// L4b：top-4 slot 各一张 RGBA8，编码见 terrain/terrainLayerFeedback.h：
// R=layerId, G=pageIndex, B=mip|valid(bit7), A=frameTag。
layout(location = 1) out vec4 outLayerSlot0;
layout(location = 2) out vec4 outLayerSlot1;
layout(location = 3) out vec4 outLayerSlot2;
layout(location = 4) out vec4 outLayerSlot3;

// Set 3 来自 RenderGraph worldTexture 输入，与 M_terrain 采样的是同一张页表。
layout(set = 3, binding = 0) uniform sampler2D pageTable;
// L4b 输入：层 catalog（256×4）与 top-4 ID/权重 atlas，全部与 M_terrain 共享。
layout(set = 3, binding = 1) uniform sampler2D layerCatalog;
layout(set = 3, binding = 2) uniform sampler2D layerIds;
layout(set = 3, binding = 3) uniform sampler2D layerWeights;

vec4 TerrainFeedbackCatalogRow(int layer, int row)
{
    // nearest + 无 mip：texel 中心严格落在指定 catalog 行内。
    return texture(
        layerCatalog,
        vec2(
            (float(layer) + 0.5) / float(TERRAIN_VT_MAX_LAYERS),
            (float(row) + 0.5) / 4.0));
}

vec4 EncodeLayerSlot(int layerId, int pageIndex, int mip, uint frameTag)
{
    // layerId/pageIndex 各 8 bit，mip 3 bit + valid(bit7)，tag 8 bit。
    const uint packedMip = uint(mip) | 128u;
    return vec4(
        float(layerId),
        float(pageIndex),
        float(packedMip),
        float(frameTag)) / 255.0;
}

// 编码格式版本 1（与 terrain/terrainFeedback.h 保持一致）：
// R[0..4]=pageX, bit5=miss, bit6=valid, bit7=0
// G[0..4]=pageY, B=priority, A=frameTag（0 保留）
// 本 pass 挂 128×128 depth 且 depthCompareOp=less：深度只用于近页优先，
// 让最近的地形页赢得 feedback texel，避免后绘制的远页覆盖近页 miss。
// 同一 pass 另外输出 4 张层页 attachment；层页 mip 报告的是 shader 按导数
// 选出的目标 mip，不是缺页回退后的粗 mip。
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
    const uint frameTag = uint(round(clamp(u_frameTag, 1.0, 255.0)));
    const uint packedA = frameTag;
    outFeedback = vec4(
        float(packedX),
        float(packedY),
        float(packedB),
        float(packedA)) / 255.0;

    // ---- L4b 层页 feedback ----
    //
    // 导数必须在 uniform control flow 里：`u_layerFeedbackEnabled` 是 uniform、
    // 但 `miss` 是逐片元值，`if (weight <= 0) continue` 也是逐片元发散。
    // 因此这里**无条件**算完 4 个 slot 的 layerUv / mip / 页号，最后只用
    // 权重与 enabled/miss 掩码选择写出的 slot——不能把 dFdx/dFdy 放进分支。
    //
    // 覆盖页命中后，用与 mf_terrainLayerBlend 完全相同的 atlas UV 取 top-4 层。
    // rawPageUv 不 clamp：全局 UV 恰为 1.0 时要选该页最后一列。
    const vec2 rawPageUv = inPageUv * float(pageTableSize);
    vec2 localUv = fract(rawPageUv);
    localUv = clamp(
        localUv,
        vec2(0.5 / u_pageResolution),
        vec2(1.0 - 0.5 / u_pageResolution));
    localUv = mix(
        localUv,
        vec2(1.0 - 0.5 / u_pageResolution),
        step(vec2(float(pageTableSize)), rawPageUv));
    const float atlasResolution = u_pageTableSize * u_pageResolution;
    const vec2 atlasUv = (vec2(page) * u_pageResolution +
        localUv * u_pageResolution) / atlasResolution;

    // atlas 只有 1 级 mip：显式 LOD 0，避免隐式导数再引入一次发散风险。
    const ivec4 ids =
        ivec4(round(textureLod(layerIds, atlasUv, 0.0).rgba * 255.0));
    const vec4 weights = textureLod(layerWeights, atlasUv, 0.0);

    // 固定 4 次迭代、无 break/continue：动态 uniform 的循环，导数有定义。
    // 每层 catalog/页计算都是无条件的，slot 只影响最终是否写出。
    vec4 encodedSlots[4];
    for (int slot = 0; slot < 4; ++slot)
    {
        const int layer = ids[slot];
        const vec4 uvRow = TerrainFeedbackCatalogRow(layer, 1);
        const vec4 pageRow = TerrainFeedbackCatalogRow(layer, 2);
        const int pagesPerSide = max(1, int(pageRow.x + 0.5));
        const int maxMip = min(
            TerrainLayerMaxMipForPagesPerSide(pagesPerSide),
            int(u_layerMaxMip + 0.5));
        const vec2 layerUv =
            TerrainLayerUvForSlot(inMaterialUv, u_uvScale, uvRow);
        // L4e：与 M_terrain 共用同一份强制 mip 覆盖。feedback 必须报告
        // shader 真正想采的目标 mip，而不是自动导数结果，否则 console
        // `terrainlayer forcemip` 后 CPU 驻留与 GPU 采样会错位。
        // mip 由全局 page UV 决定，和 M_terrain 的全分辨率采样同一套公式。
        const int autoMip = TerrainLayerMipForPageUv(
            inPageUv,
            pagesPerSide,
            maxMip,
            u_pageTableSize,
            u_layerPageResolution,
            max(vec2(1.0), vec2(u_feedbackScaleX, u_feedbackScaleY)));
        const int mip =
            TerrainLayerTargetMip(autoMip, u_layerForceMip, maxMip);
        const TerrainLayerVTAddress address =
            TerrainLayerAddressAtMip(layerUv, pagesPerSide, mip);
        encodedSlots[slot] =
            EncodeLayerSlot(layer, address.pageIndex, mip, frameTag);
    }

    // 关闭时（mode 0 / 未请求 VT）或覆盖页 miss 时四张附件全部写 invalid；
    // 权重为 0 的 slot 也不写出。
    const bool layerFeedbackValid =
        (u_layerFeedbackEnabled > 0.5) && !miss;
    outLayerSlot0 = (layerFeedbackValid && weights.x > 0.0)
        ? encodedSlots[0] : vec4(0.0);
    outLayerSlot1 = (layerFeedbackValid && weights.y > 0.0)
        ? encodedSlots[1] : vec4(0.0);
    outLayerSlot2 = (layerFeedbackValid && weights.z > 0.0)
        ? encodedSlots[2] : vec4(0.0);
    outLayerSlot3 = (layerFeedbackValid && weights.w > 0.0)
        ? encodedSlots[3] : vec4(0.0);
}
