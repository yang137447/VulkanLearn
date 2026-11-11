#ifndef VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_MATH_GLSL
#define VL_MATERIAL_FUNCTION_TERRAIN_LAYER_VT_MATH_GLSL

// L3/L4b 共享的层 VT 地址数学：层 UV -> mip -> 页坐标。
// M_terrain 的采样路径（mf_terrainLayerVT.glsl）与层页 feedback
// （pass/terrainFeedback.frag）必须用同一份实现，否则 feedback 报告的
// 页/mip 会和真正采样的页错位。CPU 参考实现是 terrain/terrainLayerVT.h。
const int TERRAIN_VT_MAX_LAYERS = 256;
const int TERRAIN_VT_MAX_MIP = 6;

struct TerrainLayerVTAddress
{
    int mip;
    int side;
    int pageX;
    int pageY;
    // pageY * side + pageX；feedback 的 G 通道就是它（0..255）。
    int pageIndex;
    vec2 localUv;
};

// catalog 的 pagesPerSide 是 mip0 的每边页数（512 -> 4、1K -> 8、2K -> 16）。
// 尾页是 128²，因此最大 mip = log2(pagesPerSide)，例如 1K 为 mip0..3。
int TerrainLayerMaxMipForPagesPerSide(int pagesPerSide)
{
    int maxMip = 0;
    int side = pagesPerSide;
    while (side > 1 && maxMip < TERRAIN_VT_MAX_MIP)
    {
        side >>= 1;
        ++maxMip;
    }
    return maxMip;
}

int TerrainLayerPagesPerSideAtMip(int pagesPerSide, int mip)
{
    return max(1, pagesPerSide >> mip);
}

// 层 UV 与 mode 0 完全一致：uvScale/uvRotation 来自 catalog row1。
vec2 TerrainLayerUvForSlot(
    vec2 meshUv,
    float globalUvScale,
    vec4 catalogUvRow)
{
    const float angle = catalogUvRow.w;
    const mat2 rotation =
        mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    return rotation * (meshUv * globalUvScale * catalogUvRow.z);
}

// 按层自身的纹素密度选 mip。导数必须在 fract 之前取；调用点必须在
// uniform control flow（固定 4 次的 slot 循环）内，否则导数未定义。
// derivativeScale 把“当前渲染目标的每像素变化”换算成主视图的每像素变化：
// M_terrain 传 (1,1)；128×128 的 feedback pass 传 (screen/128)，否则它的
// 导数比主视图大一个屏幕/feedback 比例，mip 会被系统性选粗。
int TerrainLayerMipForUv(
    vec2 layerUv,
    int pagesPerSide,
    int maxMip,
    float pageResolution,
    vec2 derivativeScale)
{
    const vec2 layerDdx = dFdx(layerUv) / max(derivativeScale.x, 1.0e-6);
    const vec2 layerDdy = dFdy(layerUv) / max(derivativeScale.y, 1.0e-6);
    const float sourceResolution =
        float(pagesPerSide) * pageResolution;
    const float texelsPerPixel =
        max(length(layerDdx), length(layerDdy)) * sourceResolution;
    const float lod = log2(max(texelsPerPixel, 1.0e-8));
    return clamp(int(floor(lod + 0.5)), 0, maxMip);
}

// 按全局 page UV（texCoord1）选 mip：一个像素覆盖多少个 mip0 页 texel。
// 与层的 uvScale、源图分辨率无关，feedback（128x128，乘 screen/128）和
// M_terrain（全分辨率）用同一套公式，驻留与实际采样因此严格一致。
int TerrainLayerMipForPageUv(
    vec2 pageUv,
    int pagesPerSide,
    int maxMip,
    float pageTableSize,
    float pageResolution,
    vec2 derivativeScale)
{
    const vec2 pageDdx = dFdx(pageUv) / max(derivativeScale.x, 1.0e-6);
    const vec2 pageDdy = dFdy(pageUv) / max(derivativeScale.y, 1.0e-6);
    // 一个 mip0 覆盖页 = 1/pageTableSize 的 page UV。
    const float coverTexelsPerPixel =
        max(length(pageDdx), length(pageDdy)) * pageTableSize * pageResolution;
    const float coverLod = log2(max(coverTexelsPerPixel, 1.0e-8));
    // 层页比覆盖页大 pageTableSize/pagesPerSide 倍，mip0 texel 也大同样的倍数。
    const float layerBias =
        log2(max(pageTableSize, 1.0) / float(max(pagesPerSide, 1)));
    return clamp(int(floor(coverLod - layerBias + 0.5)), 0, maxMip);
}

// L4e：调试用强制目标 mip。forceMip < 0 表示按导数自动选择；0..6 直接
// 覆盖自动结果，再按该层实际 mip 链 clamp。M_terrain 采样与 feedback pass
// 必须用同一份覆盖规则，否则 feedback 报告的页和真正采样的页会错位。
int TerrainLayerTargetMip(int autoMip, float forceMip, int maxMip)
{
    if (forceMip < 0.0)
        return autoMip;
    return clamp(int(forceMip + 0.5), 0, maxMip);
}

// L4e：目标 mip 的页没有驻留、shader 向更粗 mip 回退时的混合系数。
// 每差一级混入 20%，上限 60%：差 0 级为 0（完整驻留，行为不变），
// 差 1/2/3+ 级为 0.2/0.4/0.6。CPU 参考实现是
// terrain/terrainLayerVT.h 的 FallbackBlendForMipGap，测试锁定同一公式。
float TerrainLayerFallbackBlend(int targetMip, int usedMip)
{
    const float gap = float(max(0, usedMip - targetMip));
    return clamp(0.2 * gap, 0.0, 0.6);
}

// 层纹理是 repeat 平铺：先 fract 到 [0,1)，再按该 mip 的页数切页。
// side 与 CPU 的 TerrainLayerPagesPerSide(sourceResolution, mip) 一致。
TerrainLayerVTAddress TerrainLayerAddressAtMip(
    vec2 layerUv,
    int pagesPerSide,
    int mip)
{
    TerrainLayerVTAddress address;
    address.mip = mip;
    address.side = TerrainLayerPagesPerSideAtMip(pagesPerSide, mip);
    const vec2 scaledUv = fract(layerUv) * float(address.side);
    const ivec2 pageXY = clamp(
        ivec2(floor(scaledUv)),
        ivec2(0),
        ivec2(address.side - 1));
    address.pageX = pageXY.x;
    address.pageY = pageXY.y;
    address.pageIndex = address.pageY * address.side + address.pageX;
    address.localUv = fract(scaledUv);
    return address;
}

#endif
