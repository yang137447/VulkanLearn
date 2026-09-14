#ifndef VL_M_BRDF_PLOT_SURFACE_GLSL
#define VL_M_BRDF_PLOT_SURFACE_GLSL

#include "common/clothBrdf.glsl"
#include "common/hairPathScattering.glsl"
#include "common/microfacetDistribution.glsl"
#include "engine/materialInputs.glsl"
// 离线验证结果表（D-05 的方向反照率）：由 tool/validation/verify_brdf_integrals.py 生成。
// 搬进 shader 的只是**数值**——逐像素做半球积分在 shader 里做不到；公式本体仍在
// common/microfacetDistribution.glsl 与 common/lighting.glsl，生成物带自动生成标记。
#include "validationAlbedoTable.glsl"
// 离线验证结果表（D-09 的 split-sum 相对误差）：由 tool/validation/verify_split_sum.py 生成。
// 与上面同一套理由：shader 画的是**离线算好的数**，图与记录因此同源。
#include "validationSplitSumTable.glsl"

// 论文曲线探针材质（M_brdfPlot）。
//
// 目的：在 runtime 用引擎**真实的 BRDF 实现**复现论文里的分布曲线，而不是照论文
// 重抄一遍公式。曲线求值直接调用着色器库里已有的函数：
//   DistributionGGX(vec3 N, vec3 H, float roughness)  —— common/microfacetDistribution.glsl
//       注意参数是 perceptual roughness，函数内部自己做 a = roughness^2。
//       （该函数与 common/lighting.glsl 共用同一份定义，不在这里另抄一遍。）
//   ClothCharlieDistribution(float alpha, float NdotH) —— common/clothBrdf.glsl
//       注意参数是 alpha，需要用 ClothSheenRoughnessToAlpha() 转换。
//   EvaluateHairUeR / TT / TRT —— common/hairPathScattering.glsl
//   GeometrySchlickGGX / GeometrySchlickGGXIbl / SmithG1Ggx —— common/microfacetDistribution.glsl
//       两支 Schlick 分别对应论文允许的两条路径（analytic 直接光 / IBL），mode 4 专门画这个对照。
//
// 版式（数学规范）：面板内缩出一个**绘图区矩形**，矩形边框即坐标轴；刻度朝内；
// 刻度数值写在矩形外侧的留白里；轴名写在留白最外侧（y 轴名旋转 90°）。五张图的
// 坐标定义见下面 PLOT_MODE_* 常量前的注释清单。mode 3 是 UV 诊断图，**不画任何标注**：
// 它的用途就是让材质直接输出 uv，任何叠加都会破坏这份读数的可信度。
//
// 版式尺寸全部以**像素**给出，再由 `fwidth(uv)` 换算：uv 对屏幕是仿射映射，
// fwidth(uv) 精确等于"每像素的 uv 步长"，所以 `uv / fwidth(uv)` 就是以面板左下角
// 为原点、y 向上的像素坐标。于是同一套参数在任何"面板铺满画面"的探针场景里都给出
// 相同的像素版式——换面板大小、换 scale、换机位都不必重调，也不必写进每个 MI。
//
// 纵轴方向（M-07 实测，不要再翻转）：mesh texCoord.y **沿屏幕向上增大**——
// mode 3 UV 探针在同一会话里读出画面顶部 uv.y = 0.768、底部 = 0.232，与 plot_quad.obj
// 里 vt.y=1 落在 +Y 顶点一致。曾经的 `1.0 - uv.y` 基于相反的前提，把整张图竖直镜像了
// （实测同一批数据在镜像假设下差 ~700 px、直立假设下 ≤0.5 px），已删除。
//
// 与测量链路的关系（重要）：绘图区矩形是本材质公开的参数 `u_plotRectPixels`，
// 测量脚本必须用同一组值把像素反算成轴值（tool/validation/measure_plot_curve.py 的
// `--plot-rect`），并会用"从图上检出的边框位置"与声明值交叉核对。
//
// 颜色约定：坐标轴、刻度、文字、网格全部是**无彩色**（通道差 <= 0.08），只有曲线与
// UV 探针是强彩色。测量脚本据此用一个彩度门限把标注排除在曲线判定之外，所以这里
// 新增任何标注都必须继续遵守"标注无彩色"这条约束。

const int PLOT_SAMPLE_COUNT = 256;

// 坐标定义（同时是这六个 mode 的"轴语义"清单）：
//   mode 0  x = θh，0..90°，主刻度 15°     y = D(θh) 归一化后的 log10 轴，1e-3..1
//   mode 1  x = sinθl，-1..1，主刻度 0.5   y = Mp 归一化后的 log10 轴，1e-3..1
//   mode 2  x = cosθ，1→0（左端 1），主刻度 0.25      y = G1 线性轴，0..1，主刻度 0.25
//   mode 3  UV 诊断，无坐标轴
//   mode 4  同 mode 2 的坐标轴，画**两支 Schlick 变体**（analytic 与 IBL）与精确 Smith
//   mode 5  同 mode 0 的 x 轴，y 轴加深到 **6 个数量级**（1e-6..1）：GGX 与 Beckmann 各自按峰值归一化
//   mode 6  x = θv（入射/视线角），y = 线性 0..1：四条 G 臂的**方向反照率**（离线表驱动）
//   mode 7  x = roughness（0..1），y = log10 轴 1e-6..1：五个环境的 split-sum **相对误差**（离线表驱动）
const int PLOT_MODE_GGX_CHARLIE = 0;
const int PLOT_MODE_HAIR = 1;
const int PLOT_MODE_G1 = 2;
const int PLOT_MODE_UV = 3;
const int PLOT_MODE_G1_VARIANTS = 4;
const int PLOT_MODE_BECKMANN = 5;
const int PLOT_MODE_ALBEDO = 6;
const int PLOT_MODE_SPLIT_SUM = 7;

// mode 7 的四个环境臂，顺序与 validationSplitSumTable.glsl 的注释（arm N = ...）一致。
// 图例里写的名字必须与表头那四个 ASCII 短名逐字一致，否则"图上这条线是哪个环境"就失锚了。
// 离线判定表里还有 gradient（与 constant 同为低频、误差同量级），但两条线在 6 数量级 log 轴上
// 完全重合、逐列测量分不开，所以图上只留 constant 作低频代表。
const int PLOT_SPLITSUM_ARM_CONSTANT = 0;
const int PLOT_SPLITSUM_ARM_COSINE = 1;
const int PLOT_SPLITSUM_ARM_SUN = 2;
const int PLOT_SPLITSUM_ARM_BISTRO = 3;

// mode 6 的四条臂，顺序与 validationAlbedoTable.glsl 的 [arm] 维一致。
// 名字取自离线表头（analytic / ibl / smith / no G）；图上写的名字与颜色是另一套更长的
// 写法（"Schlick (direct) k=(r+1)^2/8" 等），两者指的是同一个函数，见 PlotDrawLegend。
const int PLOT_ALBEDO_ARM_ANALYTIC = 0;
const int PLOT_ALBEDO_ARM_IBL = 1;
const int PLOT_ALBEDO_ARM_SMITH = 2;
const int PLOT_ALBEDO_ARM_NO_G = 3;

// mode 6 说明行里报数用的角度（度）。75° 是各臂差异已经拉开、又还没到 90° 奇点的位置；
// 与离线判定（D-05）报的是同一个角度，图上数字可以直接和记录里的数字对。
const float PLOT_ALBEDO_CAPTION_ANGLE = 75.0;

// ---------------------------------------------------------------------------
// 曲线求值（与 M_pbr / M_cloth / M_hair 共用同一份实现）
// ---------------------------------------------------------------------------

// GGX 法线分布。构造 H 使 N·H = cos(θh)，从而直接得到 D(θh)。
float PlotGgxCurve(float thetaH, float roughness)
{
    vec3 normal = vec3(0.0, 0.0, 1.0);
    vec3 halfVector = normalize(vec3(sin(thetaH), 0.0, cos(thetaH)));
    return DistributionGGX(normal, halfVector, roughness);
}

// Charlie（丝绒）分布，与 M_cloth 使用的是同一个函数。
float PlotCharlieCurve(float thetaH, float sheenRoughness)
{
    float alpha = ClothSheenRoughnessToAlpha(sheenRoughness);
    return ClothCharlieDistribution(alpha, cos(thetaH));
}

// 采样定义域求峰值。两条曲线的绝对量级差很多（GGX 低粗糙度可以到 1/(π a^2)），
// 不做归一化就没法在同一张图里比较形状。
float PlotSamplePeak(bool charlie, float roughness)
{
    float peak = 0.0;
    for (int i = 0; i <= PLOT_SAMPLE_COUNT; ++i)
    {
        float thetaH = (float(i) / float(PLOT_SAMPLE_COUNT)) * (PI * 0.5);
        float value = charlie
            ? PlotCharlieCurve(thetaH, roughness)
            : PlotGgxCurve(thetaH, roughness);
        peak = max(peak, value);
    }
    return max(peak, 1.0e-6);
}

// 网格线相对轴 / 刻度线宽的缩放（半宽 = PLOT_GRID_STROKE_SCALE · u_plotTextPixels.z）。
const float PLOT_GRID_STROKE_SCALE = 0.6;

// 曲线描边宽度，以「网格线宽度」为单位。曲线半宽 = 该倍数 · 网格半宽。
// 想单独调粗细就改这一处：网格随之不动，两者的比例关系始终写在代码里。
const float PLOT_CURVE_WIDTH_IN_GRID_LINES = 2.0;

// 曲线描边的半宽（**像素**，垂直于曲线方向）。
float PlotCurveHalfWidthPixels()
{
    return PLOT_CURVE_WIDTH_IN_GRID_LINES * PLOT_GRID_STROKE_SCALE * u_plotTextPixels.z;
}

// 曲线描边：`axisDistance` 是**轴坐标下的纵向距离**（value - axisY）。
//
// 这里不能直接拿一个轴坐标常量去比 |距离|：那样得到的是"恒定纵向厚度"的带子，
// 它的**垂直宽度**在陡峭段会按 1/sqrt(1+slope²) 变细——实测 mode 2 的 Schlick
// 在平缓段垂直宽 16.5 px、在陡段只有 8.8 px，看起来就是"同一条曲线粗细不一致"。
//
// 改成把纵向距离除以它在屏幕上的梯度长度，得到近似的**像素距离**（SDF 的标准做法；
// 梯度由 dFdx/dFdy 免费给出），描边宽度就只由 halfWidthPixels 决定、与斜率无关：
// 陡峭段纵向自然变高，垂直宽度保持不变。
float PlotCurveLine(float axisDistance, float halfWidthPixels)
{
    float gradient = length(vec2(dFdx(axisDistance), dFdy(axisDistance)));
    float distancePixels = abs(axisDistance) / max(gradient, 1.0e-9);
    return 1.0 - smoothstep(halfWidthPixels, halfWidthPixels + 1.0, distancePixels);
}

// 纵轴按 log10 压缩到指定数量级（mode 0/1 用 3 个数量级，mode 5 用 6 个）。
// 下限取 `10^-decades` 的十分之一，与原来"3 个数量级时 max(value, 1e-4)"的行为逐位一致。
float PlotLogYRange(float normalizedValue, float decades)
{
    float floorValue = pow(10.0, -decades);
    float safeValue = max(normalizedValue, floorValue * 0.1);
    return clamp(
        (log2(safeValue) / log2(10.0) + decades) / decades,
        0.0,
        1.0);
}

// 纵轴按 log10 压缩，覆盖 3 个数量级（1e-3 .. 1.0）。
// 论文（Neubelt & Pettineo 2013 Fig 6 左）本身就画在对数轴上：GGX 低粗糙度的峰值
// 可以达到 1/(π a^2)，和 Charlie 差了十几倍，线性轴会把其中一条压成贴着底边的直线。
float PlotLogY(float normalizedValue)
{
    return PlotLogYRange(normalizedValue, 3.0);
}

// ---------------------------------------------------------------------------
// mode 1：头发 R / TT / TRT 纵向散射
// 复现 Marschner 2003 Fig 5 与 pbrt Fig 9：固定 θv，扫 θl，三条路径的纵向
// 高斯峰被各自的 Shift 错开。
// 求值直接调用引擎真实的 EvaluateHairUeR / EvaluateHairUeTT / EvaluateHairUeTRT，
// 上下文用 BuildHairUeScatteringContext 构造，和 M_hair 走的是同一套代码。
// ---------------------------------------------------------------------------

const vec3 HAIR_PLOT_TANGENT = vec3(1.0, 0.0, 0.0);
const vec3 HAIR_PLOT_BITANGENT = vec3(0.0, 1.0, 0.0);
// UE Legacy 路径里的 area 项；MVP 阶段取 0，只保留 roughness 造成的展宽。
const float HAIR_PLOT_AREA = 0.0;
const float HAIR_PLOT_SPECULAR = 1.0;

// 用沿发丝方向的 sinθ 构造单位方向（切向分量 + 垂直于发丝的残余分量）。
vec3 HairPlotDirection(float sinTheta)
{
    float clamped = clamp(sinTheta, -1.0, 1.0);
    return normalize(
        clamped * HAIR_PLOT_TANGENT +
        sqrt(max(1.0 - clamped * clamped, 0.0)) * HAIR_PLOT_BITANGENT);
}

// 返回 (R, TT, TRT) 三条路径的纵向散射值。baseColor 决定 TT/TRT 的透射色。
vec3 SampleHairPaths(float sinThetaL, float sinThetaV, float roughness)
{
    vec3 lightDirection = HairPlotDirection(sinThetaL);
    vec3 viewDirection = HairPlotDirection(sinThetaV);
    HairUeScatteringContext scattering = BuildHairUeScatteringContext(
        HAIR_PLOT_TANGENT,
        lightDirection,
        viewDirection);

    vec3 baseColor = vec3(0.35, 0.18, 0.10);
    float r = EvaluateHairUeR(
        scattering, roughness, HAIR_PLOT_SPECULAR, HAIR_PLOT_AREA, 1.0).r;
    float tt = EvaluateHairUeTT(
        scattering, roughness, baseColor, 1.0, HAIR_PLOT_AREA).r;
    float trt = EvaluateHairUeTRT(
        scattering, roughness, baseColor, HAIR_PLOT_AREA).r;
    return vec3(r, tt, trt);
}

// 三条路径的量级差别很大（R 是窄高峰、TT/TRT 是宽包），各自按自身峰值归一化。
vec3 SampleHairPeaks(float sinThetaV, float roughness)
{
    vec3 peak = vec3(1.0e-6);
    for (int i = 0; i <= PLOT_SAMPLE_COUNT; ++i)
    {
        float sinThetaL = -1.0 + 2.0 * float(i) / float(PLOT_SAMPLE_COUNT);
        peak = max(peak, SampleHairPaths(sinThetaL, sinThetaV, roughness));
    }
    return peak;
}

// ---------------------------------------------------------------------------
// 版式：绘图区矩形、像素坐标、轴坐标
// ---------------------------------------------------------------------------

struct PlotLayout
{
    vec2 pixelMin;    // 绘图区左下角（面板像素坐标，y 向上）
    vec2 pixelMax;    // 绘图区右上角
    vec2 panelSize;   // 面板像素尺寸 = uv 0..1 覆盖的像素数
};

PlotLayout BuildPlotLayout(vec2 uv)
{
    PlotLayout plot;
    // fwidth(uv) 是每像素的 uv 步长；uv 仿射映射到屏幕，所以这是精确值而不是近似。
    plot.panelSize = 1.0 / max(fwidth(uv), vec2(1.0e-9));
    plot.pixelMin = u_plotRectPixels.xy;
    // u_plotRectPixels.zw 是到面板右 / 上边缘的留白。
    plot.pixelMax = plot.panelSize - u_plotRectPixels.zw;
    return plot;
}

// 面板像素坐标 -> 绘图区归一化坐标 [0,1]（"轴坐标"）。
// 各 mode 再把这个 [0,1] 映射成物理量：角度 / sinθl / cosθ / 归一化分布。
float PlotAxisX(vec2 pixel, PlotLayout plot)
{
    return clamp(
        (pixel.x - plot.pixelMin.x) / max(plot.pixelMax.x - plot.pixelMin.x, 1.0),
        0.0,
        1.0);
}

float PlotAxisY(vec2 pixel, PlotLayout plot)
{
    return clamp(
        (pixel.y - plot.pixelMin.y) / max(plot.pixelMax.y - plot.pixelMin.y, 1.0),
        0.0,
        1.0);
}

float PlotAxisToPixelX(float axisX, PlotLayout plot)
{
    return mix(plot.pixelMin.x, plot.pixelMax.x, axisX);
}

float PlotAxisToPixelY(float axisY, PlotLayout plot)
{
    return mix(plot.pixelMin.y, plot.pixelMax.y, axisY);
}

// 曲线只在绘图区内绘制。
//
// 没有这个裁剪会出一类很难看出来的错：PlotAxisX / PlotAxisY 对绘图区之外的片元
// 会把轴坐标 clamp 到 0 / 1，于是"端点值刚好等于 0 或 1"的曲线（例如 mode 2 的
// G1 在 cosθ=1 处正好是 1）会在整片留白上被判成"曲线经过这里"。实测表现是左上角
// 与右下角被曲线色涂满，看起来像曲线跑出坐标框。
float PlotInsidePlot(vec2 pixel, PlotLayout plot)
{
    vec2 inside = step(plot.pixelMin, pixel) * step(pixel, plot.pixelMax);
    return inside.x * inside.y;
}

// ---------------------------------------------------------------------------
// 5x7 点阵字体
// ---------------------------------------------------------------------------
//
// 为什么要自带字体：探针必须在**引擎自己渲染的图**上就写明坐标轴与刻度值，
// 这样坐标定义不依赖外部脚本或文档；同时不引入字体贴图资产，字形数据就是源码。
//
// 编码：每个字形 7 行，行内低 5 位有效，bit4 = 该行最左像素，第 0 行在顶部。
// 需要增删字形时，同时改 PLOT_FONT_CODES 与 PLOT_FONT_ROWS 两处并保持顺序一致。

const int PLOT_TEXT_MAX_CHARS = 48;

const int PLOT_FONT_GLYPH_COUNT = 55;
const int PLOT_FONT_CODES[55] = int[55](
    32, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 61, 66, 67, 68,
    70, 71, 72, 77, 82, 83, 84, 88, 94, 95, 97, 98,
    99, 100, 101, 103, 104, 105, 107, 108, 109, 110, 111, 112,
    114, 115, 116, 117, 118, 120, 122
);

const uint PLOT_FONT_ROWS[385] = uint[385](
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, // 'space'
    0x02u, 0x04u, 0x08u, 0x08u, 0x08u, 0x04u, 0x02u, // '('
    0x08u, 0x04u, 0x02u, 0x02u, 0x02u, 0x04u, 0x08u, // ')'
    0x00u, 0x04u, 0x15u, 0x0Eu, 0x15u, 0x04u, 0x00u, // '*'
    0x00u, 0x04u, 0x04u, 0x1Fu, 0x04u, 0x04u, 0x00u, // '+'
    0x00u, 0x00u, 0x00u, 0x00u, 0x0Cu, 0x0Cu, 0x08u, // ','
    0x00u, 0x00u, 0x00u, 0x1Fu, 0x00u, 0x00u, 0x00u, // '-'
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0Cu, 0x0Cu, // '.'
    0x01u, 0x02u, 0x02u, 0x04u, 0x08u, 0x08u, 0x10u, // '/'
    0x0Eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0Eu, // '0'
    0x04u, 0x0Cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x0Eu, // '1'
    0x0Eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x08u, 0x1Fu, // '2'
    0x1Fu, 0x02u, 0x04u, 0x02u, 0x01u, 0x11u, 0x0Eu, // '3'
    0x02u, 0x06u, 0x0Au, 0x12u, 0x1Fu, 0x02u, 0x02u, // '4'
    0x1Fu, 0x10u, 0x1Eu, 0x01u, 0x01u, 0x11u, 0x0Eu, // '5'
    0x06u, 0x08u, 0x10u, 0x1Eu, 0x11u, 0x11u, 0x0Eu, // '6'
    0x1Fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x08u, 0x08u, // '7'
    0x0Eu, 0x11u, 0x11u, 0x0Eu, 0x11u, 0x11u, 0x0Eu, // '8'
    0x0Eu, 0x11u, 0x11u, 0x0Fu, 0x01u, 0x02u, 0x0Cu, // '9'
    0x00u, 0x00u, 0x0Cu, 0x0Cu, 0x00u, 0x0Cu, 0x0Cu, // ':'
    0x00u, 0x00u, 0x1Fu, 0x00u, 0x1Fu, 0x00u, 0x00u, // '='
    0x1Eu, 0x11u, 0x11u, 0x1Eu, 0x11u, 0x11u, 0x1Eu, // 'B'
    0x0Eu, 0x11u, 0x10u, 0x10u, 0x10u, 0x11u, 0x0Eu, // 'C'
    0x1Eu, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x1Eu, // 'D'
    0x1Fu, 0x10u, 0x10u, 0x1Eu, 0x10u, 0x10u, 0x10u, // 'F'
    0x0Eu, 0x11u, 0x10u, 0x17u, 0x11u, 0x11u, 0x0Eu, // 'G'
    0x11u, 0x11u, 0x11u, 0x1Fu, 0x11u, 0x11u, 0x11u, // 'H'
    0x11u, 0x1Bu, 0x15u, 0x15u, 0x11u, 0x11u, 0x11u, // 'M'
    0x1Eu, 0x11u, 0x11u, 0x1Eu, 0x14u, 0x12u, 0x11u, // 'R'
    0x0Fu, 0x10u, 0x10u, 0x0Eu, 0x01u, 0x01u, 0x1Eu, // 'S'
    0x1Fu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, // 'T'
    0x11u, 0x11u, 0x0Au, 0x04u, 0x0Au, 0x11u, 0x11u, // 'X'
    0x04u, 0x0Au, 0x11u, 0x00u, 0x00u, 0x00u, 0x00u, // '^'
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x1Fu, // '_'
    0x00u, 0x00u, 0x0Eu, 0x01u, 0x0Fu, 0x11u, 0x0Fu, // 'a'
    0x10u, 0x10u, 0x1Eu, 0x11u, 0x11u, 0x11u, 0x1Eu, // 'b'
    0x00u, 0x00u, 0x0Eu, 0x11u, 0x10u, 0x11u, 0x0Eu, // 'c'
    0x01u, 0x01u, 0x0Fu, 0x11u, 0x11u, 0x11u, 0x0Fu, // 'd'
    0x00u, 0x00u, 0x0Eu, 0x11u, 0x1Fu, 0x10u, 0x0Eu, // 'e'
    0x00u, 0x0Fu, 0x11u, 0x11u, 0x0Fu, 0x01u, 0x0Eu, // 'g'
    0x10u, 0x10u, 0x1Eu, 0x11u, 0x11u, 0x11u, 0x11u, // 'h'
    0x04u, 0x00u, 0x0Cu, 0x04u, 0x04u, 0x04u, 0x0Eu, // 'i'
    0x10u, 0x10u, 0x12u, 0x14u, 0x18u, 0x14u, 0x12u, // 'k'
    0x0Cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x0Eu, // 'l'
    0x00u, 0x00u, 0x1Au, 0x15u, 0x15u, 0x15u, 0x15u, // 'm'
    0x00u, 0x00u, 0x1Eu, 0x11u, 0x11u, 0x11u, 0x11u, // 'n'
    0x00u, 0x00u, 0x0Eu, 0x11u, 0x11u, 0x11u, 0x0Eu, // 'o'
    0x00u, 0x00u, 0x1Eu, 0x11u, 0x11u, 0x1Eu, 0x10u, // 'p'
    0x00u, 0x00u, 0x16u, 0x19u, 0x10u, 0x10u, 0x10u, // 'r'
    0x00u, 0x00u, 0x0Fu, 0x10u, 0x0Eu, 0x01u, 0x1Eu, // 's'
    0x08u, 0x08u, 0x1Eu, 0x08u, 0x08u, 0x09u, 0x06u, // 't'
    0x00u, 0x00u, 0x11u, 0x11u, 0x11u, 0x13u, 0x0Du, // 'u'
    0x00u, 0x00u, 0x11u, 0x11u, 0x11u, 0x0Au, 0x04u, // 'v'
    0x00u, 0x00u, 0x11u, 0x0Au, 0x04u, 0x0Au, 0x11u, // 'x'
    0x00u, 0x00u, 0x1Fu, 0x02u, 0x04u, 0x08u, 0x1Fu // 'z'
);

const int PLOT_CHAR_MINUS = 45;
const int PLOT_CHAR_DOT = 46;
const int PLOT_CHAR_DIGIT_ZERO = 48;

// 说明行文案（ASCII 码，按 mode 0/1/2/4/5/6/7 顺序，每组 48 字符、尾部补 0）。数字结论由 shader 现算后追加。
// 第 4 组（索引 3）是 mode 4：两支 Schlick 变体的偏差都以 Smith 为基准，各报一个数。
// 第 5 组（索引 4）是 mode 5：报"在 Beckmann 尾角处 GGX 高多少倍"（尾部对照的结论数字）。
// 第 6 组（索引 5）是 mode 6：报 75° 处 smith 与 no-G 两支的差（能量损失有多大一目了然）。
// 第 7 组（索引 6）是 mode 7：报全表最大相对误差（"误差有多大"这个 headline 数字）。
const int PLOT_CAPTION_CODES[336] = int[336](
    71, 71, 88, 32, 118, 115, 32, 67, 104, 97, 114, 108, 105, 101, 58, 32,
    110, 111, 114, 109, 97, 108, 105, 122, 101, 100, 32, 112, 101, 97, 107, 32,
    114, 97, 116, 105, 111, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    72, 97, 105, 114, 32, 82, 44, 32, 84, 84, 44, 32, 84, 82, 84, 58,
    32, 110, 111, 114, 109, 97, 108, 105, 122, 101, 100, 32, 108, 111, 103, 49,
    48, 32, 77, 112, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    83, 99, 104, 108, 105, 99, 107, 32, 118, 115, 32, 83, 109, 105, 116, 104,
    58, 32, 109, 97, 120, 32, 71, 49, 32, 100, 101, 118, 105, 97, 116, 105,
    111, 110, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    71, 49, 32, 109, 97, 120, 32, 100, 101, 118, 32, 118, 115, 32, 83, 109,
    105, 116, 104, 58, 32, 100, 105, 114, 101, 99, 116, 32, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    97, 116, 32, 98, 101, 99, 107, 109, 97, 110, 110, 32, 116, 97, 105, 108,
    32, 49, 101, 45, 51, 58, 32, 103, 103, 120, 47, 98, 101, 99, 107, 109,
    97, 110, 110, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    97, 108, 98, 101, 100, 111, 32, 97, 116, 32, 55, 53, 100, 101, 103, 58,
    32, 83, 109, 105, 116, 104, 32, 45, 32, 110, 111, 32, 71, 32, 61, 32,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    109, 97, 120, 32, 114, 101, 108, 32, 101, 114, 114, 32, 40, 70, 48, 61,
    49, 44, 32, 116, 118, 61, 54, 48, 100, 101, 103, 41, 58, 32, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
);
// 文本行缓冲。GLSL 没有字符串，所以一行文字就是 ASCII 码数组 + 字符个数。
// 成员名刻意避开 `length`：它与 GLSL 内建函数同名，glslang 会把成员访问
// 解析成对内建函数的调用而报错。
struct PlotText
{
    int codes[PLOT_TEXT_MAX_CHARS];
    int count;
};

void PlotTextClear(inout PlotText text)
{
    text.count = 0;
}

void PlotTextPush(inout PlotText text, int code)
{
    if (text.count < PLOT_TEXT_MAX_CHARS)
    {
        text.codes[text.count] = code;
        text.count = text.count + 1;
    }
}

void PlotTextPushDigit(inout PlotText text, int digit)
{
    PlotTextPush(text, PLOT_CHAR_DIGIT_ZERO + clamp(digit, 0, 9));
}

// 定点十进制格式化：整数部分最多 3 位，小数位 decimals ∈ [0,3]。
// 刻度标签全部由**刻度值**算出来，而不是手写字符串——这样"图上写的数"与
// "刻度位置"永远同源，不会出现标签与刻度错位的那种假图。
void PlotTextPushFixed(inout PlotText text, float value, int decimals)
{
    if (value < 0.0)
    {
        PlotTextPush(text, PLOT_CHAR_MINUS);
        value = -value;
    }

    float scale = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        if (i < decimals)
        {
            scale = scale * 10.0;
        }
    }

    int scaled = int(floor(value * scale + 0.5));
    int divisor = max(int(scale), 1);
    int integerPart = scaled / divisor;
    int fractionPart = scaled - integerPart * divisor;

    if (integerPart >= 100)
    {
        PlotTextPushDigit(text, (integerPart / 100) % 10);
    }
    if (integerPart >= 10)
    {
        PlotTextPushDigit(text, (integerPart / 10) % 10);
    }
    PlotTextPushDigit(text, integerPart % 10);

    if (decimals > 0)
    {
        PlotTextPush(text, PLOT_CHAR_DOT);
        for (int i = 0; i < 3; ++i)
        {
            if (i >= decimals)
            {
                break;
            }
            divisor = max(divisor / 10, 1);
            PlotTextPushDigit(text, (fractionPart / divisor) % 10);
        }
    }
}

int PlotGlyphIndex(int code)
{
    for (int i = 0; i < PLOT_FONT_GLYPH_COUNT; ++i)
    {
        if (PLOT_FONT_CODES[i] == code)
        {
            return i;
        }
    }
    return -1;
}

// 文本行宽度（像素）：每字符占 6 个单元（5 宽 + 1 间隔），末字符不计间隔。
float PlotTextWidth(PlotText text, float cell)
{
    return max(float(text.count) * 6.0 - 1.0, 0.0) * cell;
}

// 绘制一行文本，返回该片元处的覆盖率。
//   origin：首个字模单元的**左下角**（面板像素坐标）
//   vertical = true：整行沿 +y 排布、字形旋转 90°（用于 y 轴名，符合数学图的竖排轴名）
float PlotTextCoverage(vec2 pixel, PlotText text, vec2 origin, float cell, bool vertical)
{
    float advance = cell * 6.0;
    vec2 glyphSize = vec2(cell * 5.0, cell * 7.0);
    float runLength = advance * float(text.count);

    // 整行包围盒早退：绝大多数片元在这里就被挡掉，不必进字形循环。
    vec2 lineMin = origin;
    vec2 lineMax = origin;
    if (vertical)
    {
        lineMin = origin + vec2(-glyphSize.y, 0.0);
        lineMax = origin + vec2(0.0, runLength);
    }
    else
    {
        lineMin = origin;
        lineMax = origin + vec2(runLength, glyphSize.y);
    }
    if (pixel.x < lineMin.x || pixel.x > lineMax.x ||
        pixel.y < lineMin.y || pixel.y > lineMax.y)
    {
        return 0.0;
    }

    float coverage = 0.0;
    for (int i = 0; i < PLOT_TEXT_MAX_CHARS; ++i)
    {
        if (i >= text.count)
        {
            break;
        }

        vec2 glyphOrigin = vertical
            ? origin + vec2(0.0, advance * float(i))
            : origin + vec2(advance * float(i), 0.0);
        vec2 local = pixel - glyphOrigin;
        // 旋转 90°：字形的阅读方向(+x)变成 +y，字形上方(+y)变成 -x，反变换即 (y, -x)。
        vec2 cellPoint = vertical ? vec2(local.y, -local.x) : local;
        if (cellPoint.x < 0.0 || cellPoint.x >= glyphSize.x ||
            cellPoint.y < 0.0 || cellPoint.y >= glyphSize.y)
        {
            continue;
        }

        int glyph = PlotGlyphIndex(text.codes[i]);
        if (glyph < 0)
        {
            continue;
        }

        int column = int(floor(cellPoint.x / cell));
        int rowFromBottom = int(floor(cellPoint.y / cell));
        int rowFromTop = 6 - clamp(rowFromBottom, 0, 6);
        uint bits = PLOT_FONT_ROWS[glyph * 7 + rowFromTop];
        coverage = max(coverage, float((bits >> uint(4 - clamp(column, 0, 4))) & 1u));
    }
    return coverage;
}

// ---------------------------------------------------------------------------
// 坐标系绘制
// ---------------------------------------------------------------------------

const vec3 PLOT_AXIS_COLOR = vec3(0.45, 0.47, 0.52);
const vec3 PLOT_TEXT_COLOR = vec3(0.72, 0.74, 0.78);
const vec3 PLOT_GRID_COLOR = vec3(0.28, 0.30, 0.35);
// 底板色：既是绘图底色，也是图例的不透明底板。测量脚本按同一个值判定
// "四角是不是底板"，所以这里必须是唯一来源。
const vec3 PLOT_BASE_COLOR = vec3(0.13, 0.14, 0.17);

void PlotBlend(inout vec3 color, vec3 target, float coverage)
{
    color = mix(color, target, clamp(coverage, 0.0, 1.0));
}

// 轴对齐线段覆盖率：半宽以像素计，边缘做 1 px 过渡。
float PlotLineCoverage(vec2 pixel, vec2 from, vec2 to, float halfWidth)
{
    vec2 segment = to - from;
    vec2 offset = pixel - from;
    float projection = clamp(
        dot(offset, segment) / max(dot(segment, segment), 1.0e-6), 0.0, 1.0);
    float distance = length(offset - segment * projection);
    return 1.0 - smoothstep(halfWidth, halfWidth + 1.0, distance);
}

// 绘图区边框：四条边就是这张图的坐标轴（数学图的盒式坐标框）。
void PlotDrawFrame(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    float halfWidth = u_plotTextPixels.z;
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel, plot.pixelMin, vec2(plot.pixelMax.x, plot.pixelMin.y), halfWidth));
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel, plot.pixelMin, vec2(plot.pixelMin.x, plot.pixelMax.y), halfWidth));
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel, vec2(plot.pixelMax.x, plot.pixelMin.y), plot.pixelMax, halfWidth));
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel, vec2(plot.pixelMin.x, plot.pixelMax.y), plot.pixelMax, halfWidth));
}

// x 轴刻度：在底边上朝内画。主 / 次刻度长度不同，这是数学图的读数惯例。
void PlotDrawXTick(inout vec3 color, vec2 pixel, PlotLayout plot, float axisX, bool major)
{
    float length = major ? u_plotTextPixels.y : u_plotTextPixels.y * 0.6;
    float x = PlotAxisToPixelX(axisX, plot);
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel,
        vec2(x, plot.pixelMin.y),
        vec2(x, plot.pixelMin.y + length),
        u_plotTextPixels.z));
}

// y 轴刻度：在左边上朝内画。
void PlotDrawYTick(inout vec3 color, vec2 pixel, PlotLayout plot, float axisY, bool major)
{
    float length = major ? u_plotTextPixels.y : u_plotTextPixels.y * 0.6;
    float y = PlotAxisToPixelY(axisY, plot);
    PlotBlend(color, PLOT_AXIS_COLOR, PlotLineCoverage(
        pixel,
        vec2(plot.pixelMin.x, y),
        vec2(plot.pixelMin.x + length, y),
        u_plotTextPixels.z));
}

// 网格线：与**主刻度**对齐（不是随手取周期），这样网格、刻度、标签三者同源。
void PlotDrawGridX(inout vec3 color, vec2 pixel, PlotLayout plot, float axisX)
{
    float x = PlotAxisToPixelX(axisX, plot);
    PlotBlend(color, PLOT_GRID_COLOR, PlotLineCoverage(
        pixel,
        vec2(x, plot.pixelMin.y),
        vec2(x, plot.pixelMax.y),
        u_plotTextPixels.z * PLOT_GRID_STROKE_SCALE));
}

void PlotDrawGridY(inout vec3 color, vec2 pixel, PlotLayout plot, float axisY)
{
    float y = PlotAxisToPixelY(axisY, plot);
    PlotBlend(color, PLOT_GRID_COLOR, PlotLineCoverage(
        pixel,
        vec2(plot.pixelMin.x, y),
        vec2(plot.pixelMax.x, y),
        u_plotTextPixels.z * PLOT_GRID_STROKE_SCALE));
}

// x 轴刻度值：写在轴下方留白里，按刻度居中。
void PlotDrawXLabel(inout vec3 color, vec2 pixel, PlotLayout plot, float axisX, PlotText text)
{
    float cell = u_plotTextPixels.x;
    float width = PlotTextWidth(text, cell);
    vec2 origin = vec2(
        PlotAxisToPixelX(axisX, plot) - width * 0.5,
        plot.pixelMin.y - u_plotTextPixels.w - cell * 7.0);
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(pixel, text, origin, cell, false));
}

// y 轴刻度值：写在轴左侧留白里，右对齐、按刻度垂直居中。
void PlotDrawYLabel(inout vec3 color, vec2 pixel, PlotLayout plot, float axisY, PlotText text)
{
    float cell = u_plotTextPixels.x;
    float width = PlotTextWidth(text, cell);
    vec2 origin = vec2(
        plot.pixelMin.x - u_plotTextPixels.w - width,
        PlotAxisToPixelY(axisY, plot) - cell * 3.5);
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(pixel, text, origin, cell, false));
}

// x 轴名：横排，居中在刻度值下方一行。
void PlotDrawXTitle(inout vec3 color, vec2 pixel, PlotLayout plot, PlotText text)
{
    float cell = u_plotTextPixels.x;
    float width = PlotTextWidth(text, cell);
    vec2 origin = vec2(
        0.5 * (plot.pixelMin.x + plot.pixelMax.x) - width * 0.5,
        plot.pixelMin.y - u_plotTextPixels.w - cell * 14.0 - u_plotTextPixels.w);
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(pixel, text, origin, cell, false));
}

// y 轴名：竖排（旋转 90°），居中在刻度值左侧一列。
void PlotDrawYTitle(inout vec3 color, vec2 pixel, PlotLayout plot, PlotText text, float labelWidth)
{
    float cell = u_plotTextPixels.x;
    float runLength = PlotTextWidth(text, cell);
    vec2 origin = vec2(
        plot.pixelMin.x - u_plotTextPixels.w - labelWidth - u_plotTextPixels.w,
        0.5 * (plot.pixelMin.y + plot.pixelMax.y) - runLength * 0.5);
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(pixel, text, origin, cell, true));
}

// 说明行 / 图例行的基线（像素，y 向上）：版式（自上而下）是
// 说明行 → 图例第 1..3 条 → 轴框，因此 u_plotRectPixels.w 必须容纳「1 行说明 + 最多 3 行图例」。
float PlotCaptionBaselineY(PlotLayout plot)
{
    return plot.pixelMax.y + u_plotRectPixels.w
        - u_plotTextPixels.w - u_plotTextPixels.x * 7.0;
}

// 参数注记：右对齐在**说明行**同一行上，说明这张曲线是在哪个参数下画的。
void PlotDrawAnnotation(inout vec3 color, vec2 pixel, PlotLayout plot, PlotText text)
{
    float cell = u_plotTextPixels.x;
    float width = PlotTextWidth(text, cell);
    vec2 origin = vec2(
        plot.pixelMax.x - width,
        PlotCaptionBaselineY(plot));
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(pixel, text, origin, cell, false));
}

// ---------------------------------------------------------------------------
// 图头：说明行 + 图例（都在绘图区外面、上方）
// ---------------------------------------------------------------------------
//
// 合成一个"图头"的理由：读者先读到"这是什么、结论是什么"再进入坐标区；放下面则要先读完
// 坐标轴才看到说明。放绘图区**外面**还有两条硬理由：
//   ① 绘图区内任何位置都会被曲线穿过，压上去要么挡曲线、要么被曲线染色；
//   ② 图例色线"与曲线同色同粗细"（这正是它的价值），留在绘图区内会被逐列测量误认成曲线
//      ——实测 mode 0 的平均误差会从 0.153 px 涨到 4.990 px。
//
// 版式（自上而下）：说明行 → 图例第 1..3 条 → 轴框，基线由 PlotCaptionBaselineY 给出。

// 说明行：这张图在画什么 + 在当前参数下能得出的结论（数字由 Shader 现算）。
void PlotDrawCaption(inout vec3 color, vec2 pixel, PlotLayout plot, PlotText text)
{
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(
        pixel, text, vec2(plot.pixelMin.x, PlotCaptionBaselineY(plot)),
        u_plotTextPixels.x, false));
}

// Schlick 近似与精确 Smith 在定义域上的最大偏差（mode 2 / mode 4 的结论数字）。
// 逐样本扫一遍 cosθ ∈ (0,1]（θ=90° 处两者都是 0，从 1/N 起步即可）。
//
// iblVariant 选的是 Schlick 的 k 变体，两支都来自 Karis 2013 §Specular G：
//   false -> GeometrySchlickGGX     k=(r+1)²/8，论文只用于 analytic light source（直接光）
//   true  -> GeometrySchlickGGXIbl  k=α/2，IBL 域（也是 brfdLut.comp 生成 LUT 时用的那支）
// 两个变体必须分别报数：它们不是同一条曲线的两种写法（mode 4 就是为此存在的）。
float PlotMaxG1Deviation(bool iblVariant, float roughness)
{
    float worst = 0.0;
    for (int i = 1; i <= PLOT_SAMPLE_COUNT; ++i)
    {
        float cosTheta = float(i) / float(PLOT_SAMPLE_COUNT);
        float schlick = iblVariant
            ? GeometrySchlickGGXIbl(cosTheta, roughness)
            : GeometrySchlickGGX(cosTheta, roughness);
        float smith = SmithG1Ggx(cosTheta, roughness);
        worst = max(worst, abs(schlick - smith));
    }
    return worst;
}

// 两条曲线的共同峰值参照：各自在 θh = 0 处的取值（相互之间差 α⁴，见 case 记录）。
float PlotPeakValue(bool beckmann, float roughness)
{
    return beckmann
        ? DistributionBeckmann(1.0, roughness)
        : PlotGgxCurve(0.0, roughness);
}

// mode 5 的结论数字：分布衰减到峰值某个比例时的角度（度）。
//
// 为什么用"尾角"而不是"某角度上的比值"：两条曲线都按各自峰值归一化后掉得极快
// （α = 0.5 时 Beckmann 到 60° 已经 1e-21），比值在大部分角度上要么溢出要么无意义；
// 而"峰值的 1e-3 落在哪个角度"是一个有界、可复核、且直接编码了
// PBRT Fig 9.23 那句 "Trowbridge–Reitz has higher tails" 的量。
//
// 顺序注意：GLSL 没有函数提升，PlotPeakValue 必须定义在本函数之前。
float PlotTailAngle(bool beckmann, float roughness, float ratio)
{
    float peak = PlotPeakValue(beckmann, roughness);
    for (int i = 1; i <= PLOT_SAMPLE_COUNT; ++i)
    {
        float thetaH = (float(i) / float(PLOT_SAMPLE_COUNT)) * (PI * 0.5);
        float value = beckmann
            ? DistributionBeckmann(cos(thetaH), roughness)
            : PlotGgxCurve(thetaH, roughness);
        if (value <= peak * ratio)
        {
            return degrees(thetaH);
        }
    }
    return 90.0;
}

// mode 5 真正报出的结论：在 Beckmann 掉到峰值 `level` 的角度上，GGX 还剩峰值的多少倍。
//
// 为什么不直接报"GGX 的尾角"（第一版就是这么写的，被图上的读数否掉了）：
// **GGX 在掠射极限是非零的**——D(90°) = α²/π，归一化后是 α⁴（α = 0.5 时为 6.25%），
// 所以"掉到峰值 1e-3 的角度"对 GGX 根本不存在，函数只能返回 90 这个哨兵值，
// 图上的说明行会写成 "ggx 90.0"，看起来像数据而其实是"没找到"。
// 换成"Beckmann 的尾角处两者之比"之后：数字有界、有物理含义（同样是"尾部谁更高"），
// 而且直接对应 PBRT Fig 9.23 的 *"Trowbridge–Reitz has higher tails at larger values of θ"*。
float PlotBeckmannTailRatio(float roughness, float level)
{
    float tailTheta = radians(PlotTailAngle(true, roughness, level));
    float ggxNormalized = PlotGgxCurve(tailTheta, roughness) /
        max(PlotPeakValue(false, roughness), 1.0e-30);
    float beckmannNormalized = DistributionBeckmann(cos(tailTheta), roughness) /
        max(PlotPeakValue(true, roughness), 1.0e-30);
    return ggxNormalized / max(beckmannNormalized, 1.0e-30);
}

// ---------------------------------------------------------------------------
// mode 6：方向反照率（离线表驱动）
// ---------------------------------------------------------------------------

// 表在 α 方向上的插值下标：找最后一个 <= α 的档位。
int PlotAlbedoAlphaIndex(float alpha)
{
    int index = 0;
    for (int i = 1; i < PLOT_ALBEDO_ALPHA_COUNT; ++i)
    {
        if (alpha >= PLOT_ALBEDO_ALPHAS[i])
        {
            index = i;
        }
    }
    return index;
}

// 表在角度方向上的插值下标：找最后一个 <= θ 的档位。
int PlotAlbedoAngleIndex(float thetaDeg)
{
    int index = 0;
    for (int i = 1; i < PLOT_ALBEDO_ANGLE_COUNT; ++i)
    {
        if (thetaDeg >= PLOT_ALBEDO_ANGLES[i])
        {
            index = i;
        }
    }
    return index;
}

// 从生成表里双线性插值（α × 角度）。表由 tool/validation/verify_brdf_integrals.py 生成，
// 所以"图上这条曲线"与"离线积分那个结论"同源；shader 只负责画，不负责算积分。
float PlotAlbedoTable(int arm, float alpha, float thetaDeg)
{
    float alphaLow = PLOT_ALBEDO_ALPHAS[0];
    float alphaHigh = PLOT_ALBEDO_ALPHAS[PLOT_ALBEDO_ALPHA_COUNT - 1];
    float thetaMax = PLOT_ALBEDO_ANGLES[PLOT_ALBEDO_ANGLE_COUNT - 1];
    float clampedAlpha = clamp(alpha, alphaLow, alphaHigh);
    float clampedTheta = clamp(thetaDeg, 0.0, thetaMax);

    int alphaIndex = PlotAlbedoAlphaIndex(clampedAlpha);
    int alphaNext = min(alphaIndex + 1, PLOT_ALBEDO_ALPHA_COUNT - 1);
    float alphaSpan = max(
        PLOT_ALBEDO_ALPHAS[alphaNext] - PLOT_ALBEDO_ALPHAS[alphaIndex], 1.0e-6);
    float alphaBlend = clamp(
        (clampedAlpha - PLOT_ALBEDO_ALPHAS[alphaIndex]) / alphaSpan, 0.0, 1.0);

    int angleIndex = PlotAlbedoAngleIndex(clampedTheta);
    int angleNext = min(angleIndex + 1, PLOT_ALBEDO_ANGLE_COUNT - 1);
    float angleSpan = max(
        PLOT_ALBEDO_ANGLES[angleNext] - PLOT_ALBEDO_ANGLES[angleIndex], 1.0e-6);
    float angleBlend = clamp(
        (clampedTheta - PLOT_ALBEDO_ANGLES[angleIndex]) / angleSpan, 0.0, 1.0);

    int stride = PLOT_ALBEDO_ARM_COUNT * PLOT_ALBEDO_ANGLE_COUNT;
    float lowLow = PLOT_ALBEDO_TABLE[
        alphaIndex * stride + arm * PLOT_ALBEDO_ANGLE_COUNT + angleIndex];
    float lowHigh = PLOT_ALBEDO_TABLE[
        alphaIndex * stride + arm * PLOT_ALBEDO_ANGLE_COUNT + angleNext];
    float highLow = PLOT_ALBEDO_TABLE[
        alphaNext * stride + arm * PLOT_ALBEDO_ANGLE_COUNT + angleIndex];
    float highHigh = PLOT_ALBEDO_TABLE[
        alphaNext * stride + arm * PLOT_ALBEDO_ANGLE_COUNT + angleNext];

    float low = mix(lowLow, lowHigh, angleBlend);
    float high = mix(highLow, highHigh, angleBlend);
    return mix(low, high, alphaBlend);
}

// ---------------------------------------------------------------------------
// mode 7：split-sum 相对误差（离线表驱动）
// ---------------------------------------------------------------------------

// 表在 roughness 方向线性插值。任一端是 -1（该配置不可判定）就返回 -1：
// 曲线在那里断开。"不画"比画一个假的 0 诚实——那样看起来像"这个环境没有误差"。
float PlotSplitSumTable(int arm, float roughness)
{
    float low = PLOT_SPLITSUM_ROUGHNESS[0];
    float high = PLOT_SPLITSUM_ROUGHNESS[PLOT_SPLITSUM_ROUGHNESS_COUNT - 1];
    float clamped = clamp(roughness, low, high);

    int index = 0;
    for (int i = 1; i < PLOT_SPLITSUM_ROUGHNESS_COUNT; ++i)
    {
        if (clamped >= PLOT_SPLITSUM_ROUGHNESS[i])
        {
            index = i;
        }
    }
    int next = min(index + 1, PLOT_SPLITSUM_ROUGHNESS_COUNT - 1);
    float span = max(
        PLOT_SPLITSUM_ROUGHNESS[next] - PLOT_SPLITSUM_ROUGHNESS[index], 1.0e-6);
    float blend = clamp((clamped - PLOT_SPLITSUM_ROUGHNESS[index]) / span, 0.0, 1.0);

    // 索引布局：[env][roughness]，与生成脚本**逐行**写出的顺序一致（一行一个环境）。
    float lowValue = PLOT_SPLITSUM_TABLE[arm * PLOT_SPLITSUM_ROUGHNESS_COUNT + index];
    float highValue = PLOT_SPLITSUM_TABLE[arm * PLOT_SPLITSUM_ROUGHNESS_COUNT + next];
    if (lowValue < 0.0 || highValue < 0.0)
    {
        return -1.0;
    }
    return mix(lowValue, highValue, blend);
}

// 全表最大相对误差：说明行里报的那个 headline 数字。现算而不是写死——
// 换采样数重新生成表以后，图上的数字会跟着变，不会留下一句过期的结论。
float PlotSplitSumMaxError()
{
    float worst = 0.0;
    for (int i = 0; i < PLOT_SPLITSUM_ENV_COUNT * PLOT_SPLITSUM_ROUGHNESS_COUNT; ++i)
    {
        worst = max(worst, PLOT_SPLITSUM_TABLE[i]);
    }
    return worst;
}

// 说明行分组：mode 3 是 UV 诊断图（不画图头），所以 mode 4 / 5 / 6 / 7 依次复用第 4 / 5 / 6 / 7 组文案。
// 写成函数而不是 `clamp(mode, 0, 6)`：后者会把"mode 3 用第 4 组"这个巧合写死成隐式约定。
// 必须定义在 PlotBuildCaption 之前：GLSL 没有函数提升，前向调用不会编译通过。
int PlotCaptionGroup(int mode)
{
    if (mode == PLOT_MODE_G1_VARIANTS)
    {
        return 3;
    }
    if (mode == PLOT_MODE_BECKMANN)
    {
        return 4;
    }
    if (mode == PLOT_MODE_ALBEDO)
    {
        return 5;
    }
    if (mode == PLOT_MODE_SPLIT_SUM)
    {
        return 6;
    }
    return clamp(mode, 0, 2);
}

// 说明行文案：文字部分取自 PLOT_CAPTION_CODES（按 mode 索引），数字结论现算后追加。
// 数字**必须**现算：图上写的数要与曲线同源，否则改一个 roughness 就会留下过期的结论。
void PlotBuildCaption(inout PlotText text, int mode, float roughness)
{
    int base = PlotCaptionGroup(mode) * PLOT_TEXT_MAX_CHARS;
    for (int i = 0; i < PLOT_TEXT_MAX_CHARS; ++i)
    {
        int code = PLOT_CAPTION_CODES[base + i];
        if (code == 0)
        {
            break;
        }
        PlotTextPush(text, code);
    }

    if (mode == PLOT_MODE_GGX_CHARLIE)
    {
        float ggxPeak = PlotSamplePeak(false, roughness);
        float charliePeak = PlotSamplePeak(true, roughness);
        PlotTextPushFixed(text, ggxPeak / max(charliePeak, 1.0e-6), 1);
    }
    else if (mode == PLOT_MODE_G1)
    {
        PlotTextPushFixed(text, PlotMaxG1Deviation(false, roughness), 3);
    }
    else if (mode == PLOT_MODE_G1_VARIANTS)
    {
        // 两个结论数字各自跟着自己的标签：先 analytic（k=(r+1)²/8），再 ibl（k=α/2）。
        PlotTextPushFixed(text, PlotMaxG1Deviation(false, roughness), 3);
        PlotTextPush(text, 32);       // ' '
        PlotTextPush(text, 105);      // 'i'
        PlotTextPush(text, 98);       // 'b'
        PlotTextPush(text, 108);      // 'l'
        PlotTextPush(text, 32);
        PlotTextPushFixed(text, PlotMaxG1Deviation(true, roughness), 3);
    }
    else if (mode == PLOT_MODE_BECKMANN)
    {
        // 结论：在 Beckmann 的尾角（峰值 1e-3）处，GGX 还高多少倍——尾部谁更长一目了然。
        PlotTextPushFixed(text, PlotBeckmannTailRatio(roughness, 1.0e-3), 1);
    }
    else if (mode == PLOT_MODE_ALBEDO)
    {
        // 结论：75° 处 smith 与 no-G 两支的反照率之差，即"丢掉 G 会损失多少能量"。
        // α = r²，与曲线同源；数字从同一张表取，不另抄一份公式。
        float alpha = roughness * roughness;
        PlotTextPushFixed(
            text,
            PlotAlbedoTable(PLOT_ALBEDO_ARM_SMITH, alpha, PLOT_ALBEDO_CAPTION_ANGLE)
                - PlotAlbedoTable(PLOT_ALBEDO_ARM_NO_G, alpha, PLOT_ALBEDO_CAPTION_ANGLE),
            3);
    }
    else if (mode == PLOT_MODE_SPLIT_SUM)
    {
        // 结论：全表最大相对误差（哪个环境、哪个 roughness 最大，图上一眼能看出来）。
        PlotTextPushFixed(text, PlotSplitSumMaxError(), 3);
    }
}



// ---------------------------------------------------------------------------
// 图例（曲线身份）
// ---------------------------------------------------------------------------
//
// 名字就是这张图的"曲线身份"，与论文里的叫法一致；mode 2 额外标出哪条是引擎实际用的
// 近似、哪条是精确参考（这正是 Karis Fig 2 的对照点）。
//
// 位置：绘图区**左下角**内部（数学图的常规读法），带不透明底板与 1 px 边框。
// 两个必须写清楚的取舍：
// 位置：绘图区**外面**的左下角——与 x 轴名同一行、从轴框左边缘起排，落在原点刻度值下方。
// 放外面有两个硬理由：① 绘图区内任何位置都会被曲线穿过，图例压上去要么挡曲线、
// 要么被曲线染色；② 图例色线"与曲线同色同粗细"（这正是它的价值），留在绘图区内会被
// 逐列测量误认成曲线——实测 mode 0 的平均误差会从 0.153 px 涨到 4.990 px。
// 放外面两个问题都不存在，也就不需要保留区、底板与边框。
//
//   1. 图例画在**曲线之前**：即使版面以后变化，曲线仍压在最上层，
//      "曲线最亮处就是曲线颜色"这条测量前提不受影响；
//   2. 色线用 PlotCurveHalfWidthPixels() 画，图例同时说明"这条曲线有多粗"。

struct PlotLegendItem
{
    PlotText text;
    vec3 color;
};

void PlotLegendPush(inout PlotLegendItem item, vec3 color, int code)
{
    PlotTextClear(item.text);
    PlotTextPush(item.text, code);
    item.color = color;
}

// 图例项：一段与曲线**同粗细、同颜色**的色线 + 名称。
// 色线用 PlotCurveHalfWidthPixels() 画，所以图例本身就在说明"这条曲线有多粗"。
void PlotDrawLegendEntry(
    inout vec3 color,
    vec2 pixel,
    float startX,
    float baselineY,
    vec3 curveColor,
    PlotText text)
{
    float cell = u_plotTextPixels.x;
    float lineY = baselineY + cell * 3.5;

    PlotBlend(color, curveColor, PlotLineCoverage(
        pixel,
        vec2(startX, lineY),
        vec2(startX + cell * 5.0, lineY),
        PlotCurveHalfWidthPixels()));

    float textX = startX + cell * 5.0 + cell * 2.0;
    PlotBlend(color, PLOT_TEXT_COLOR, PlotTextCoverage(
        pixel, text, vec2(textX, baselineY), cell, false));
}

// 图例（曲线身份）。名字与论文里的叫法一致；mode 2 额外标出哪条是引擎实际用的近似、
// 哪条是精确参考（这正是 Karis Fig 2 的对照点）。
void PlotDrawLegend(inout vec3 color, vec2 pixel, PlotLayout plot, int mode)
{
    float cell = u_plotTextPixels.x;
    PlotLegendItem items[4];
    int count = 0;

    if (mode == PLOT_MODE_GGX_CHARLIE)
    {
        PlotLegendPush(items[0], vec3(0.28, 0.85, 0.36), 71);          // "G"
        PlotTextPush(items[0].text, 71);                               // "GG"
        PlotTextPush(items[0].text, 88);                               // "GGX"
        PlotLegendPush(items[1], vec3(0.95, 0.32, 0.28), 67);          // "C"
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 114);                              // 'r'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 101);                              // 'e'
        count = 2;
    }
    else if (mode == PLOT_MODE_HAIR)
    {
        PlotLegendPush(items[0], vec3(0.95, 0.72, 0.25), 82);          // "R"
        PlotLegendPush(items[1], vec3(0.30, 0.80, 0.92), 84);          // "T"
        PlotTextPush(items[1].text, 84);                               // "TT"
        PlotLegendPush(items[2], vec3(0.90, 0.40, 0.85), 84);          // "T"
        PlotTextPush(items[2].text, 82);                               // 'R'
        PlotTextPush(items[2].text, 84);                               // "TRT"
        count = 3;
    }
    else if (mode == PLOT_MODE_G1)
    {
        PlotLegendPush(items[0], vec3(0.95, 0.32, 0.28), 83);          // "S"
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 104);                              // 'h'
        PlotTextPush(items[0].text, 108);                              // 'l'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 107);                              // 'k'
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 101);                              // 'e'
        PlotTextPush(items[0].text, 110);                              // 'n'
        PlotTextPush(items[0].text, 103);                              // 'g'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 110);                              // 'n'
        PlotTextPush(items[0].text, 101);                              // 'e'
        PlotTextPush(items[0].text, 41);                               // ')'
        PlotLegendPush(items[1], vec3(0.28, 0.85, 0.36), 83);          // "S"
        PlotTextPush(items[1].text, 109);                              // 'm'
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 116);                              // 't'
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 32);                               // ' '
        PlotTextPush(items[1].text, 40);                               // '('
        PlotTextPush(items[1].text, 101);                              // 'e'
        PlotTextPush(items[1].text, 120);                              // 'x'
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 116);                              // 't'
        PlotTextPush(items[1].text, 41);                               // ')'
        count = 2;
    }
    else if (mode == PLOT_MODE_G1_VARIANTS)
    {
        // mode 4：三支曲线，名字带上各自的 k，免得"哪支是引擎在用的"要靠文档才说得清。
        PlotLegendPush(items[0], vec3(0.95, 0.32, 0.28), 83);          // "S"
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 104);                              // 'h'
        PlotTextPush(items[0].text, 108);                              // 'l'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 107);                              // 'k'
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 100);                              // 'd'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 114);                              // 'r'
        PlotTextPush(items[0].text, 101);                              // 'e'
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 116);                              // 't'
        PlotTextPush(items[0].text, 41);                               // ')'
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 107);                              // 'k'
        PlotTextPush(items[0].text, 61);                               // '='
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 114);                              // 'r'
        PlotTextPush(items[0].text, 43);                               // '+'
        PlotTextPush(items[0].text, 49);                               // '1'
        PlotTextPush(items[0].text, 41);                               // ')'
        PlotTextPush(items[0].text, 94);                               // '^'
        PlotTextPush(items[0].text, 50);                               // '2'
        PlotTextPush(items[0].text, 47);                               // '/'
        PlotTextPush(items[0].text, 56);                               // '8'

        PlotLegendPush(items[1], vec3(0.30, 0.80, 0.92), 83);          // "S"
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 107);                              // 'k'
        PlotTextPush(items[1].text, 32);                               // ' '
        PlotTextPush(items[1].text, 40);                               // '('
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 98);                               // 'b'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 41);                               // ')'
        PlotTextPush(items[1].text, 32);                               // ' '
        PlotTextPush(items[1].text, 107);                              // 'k'
        PlotTextPush(items[1].text, 61);                               // '='
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 112);                              // 'p'
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 47);                               // '/'
        PlotTextPush(items[1].text, 50);                               // '2'

        PlotLegendPush(items[2], vec3(0.28, 0.85, 0.36), 83);          // "S"
        PlotTextPush(items[2].text, 109);                              // 'm'
        PlotTextPush(items[2].text, 105);                              // 'i'
        PlotTextPush(items[2].text, 116);                              // 't'
        PlotTextPush(items[2].text, 104);                              // 'h'
        PlotTextPush(items[2].text, 32);                               // ' '
        PlotTextPush(items[2].text, 40);                               // '('
        PlotTextPush(items[2].text, 101);                              // 'e'
        PlotTextPush(items[2].text, 120);                              // 'x'
        PlotTextPush(items[2].text, 97);                               // 'a'
        PlotTextPush(items[2].text, 99);                               // 'c'
        PlotTextPush(items[2].text, 116);                              // 't'
        PlotTextPush(items[2].text, 41);                               // ')'
        count = 3;
    }
    else if (mode == PLOT_MODE_BECKMANN)
    {
        // mode 5：两条分布的身份 + 各自的 α 约定（都在同一个 α 下比较）。
        PlotLegendPush(items[0], vec3(0.28, 0.85, 0.36), 71);          // "G"
        PlotTextPush(items[0].text, 71);                               // "GG"
        PlotTextPush(items[0].text, 88);                               // "GGX"
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 84);                               // 'T'
        PlotTextPush(items[0].text, 82);                               // 'R'
        PlotTextPush(items[0].text, 41);                               // ')'

        PlotLegendPush(items[1], vec3(0.95, 0.32, 0.28), 66);          // "B"
        PlotTextPush(items[1].text, 101);                              // 'e'
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 107);                              // 'k'
        PlotTextPush(items[1].text, 109);                              // 'm'
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 110);                              // 'n'
        PlotTextPush(items[1].text, 110);                              // 'n'
        count = 2;
    }
    else if (mode == PLOT_MODE_ALBEDO)
    {
        // mode 6：四条「G 臂」，名字与颜色都沿用 mode 2/4 的约定，两张图可以直接叠着读——
        // 同名同色在两处指的是同一个函数，读者不需要重新对身份。
        // 名字同时是离线判定表（verify_brdf_integrals.py 的 arms）里的臂名。
        PlotLegendPush(items[0], vec3(0.95, 0.32, 0.28), 83);          // "S"
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 104);                              // 'h'
        PlotTextPush(items[0].text, 108);                              // 'l'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 107);                              // 'k'
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 100);                              // 'd'
        PlotTextPush(items[0].text, 105);                              // 'i'
        PlotTextPush(items[0].text, 114);                              // 'r'
        PlotTextPush(items[0].text, 101);                              // 'e'
        PlotTextPush(items[0].text, 99);                               // 'c'
        PlotTextPush(items[0].text, 116);                              // 't'
        PlotTextPush(items[0].text, 41);                               // ')'
        PlotTextPush(items[0].text, 32);                               // ' '
        PlotTextPush(items[0].text, 107);                              // 'k'
        PlotTextPush(items[0].text, 61);                               // '='
        PlotTextPush(items[0].text, 40);                               // '('
        PlotTextPush(items[0].text, 114);                              // 'r'
        PlotTextPush(items[0].text, 43);                               // '+'
        PlotTextPush(items[0].text, 49);                               // '1'
        PlotTextPush(items[0].text, 41);                               // ')'
        PlotTextPush(items[0].text, 94);                               // '^'
        PlotTextPush(items[0].text, 50);                               // '2'
        PlotTextPush(items[0].text, 47);                               // '/'
        PlotTextPush(items[0].text, 56);                               // '8'

        PlotLegendPush(items[1], vec3(0.30, 0.80, 0.92), 83);          // "S"
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 99);                               // 'c'
        PlotTextPush(items[1].text, 107);                              // 'k'
        PlotTextPush(items[1].text, 32);                               // ' '
        PlotTextPush(items[1].text, 40);                               // '('
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 98);                               // 'b'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 41);                               // ')'
        PlotTextPush(items[1].text, 32);                               // ' '
        PlotTextPush(items[1].text, 107);                              // 'k'
        PlotTextPush(items[1].text, 61);                               // '='
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 108);                              // 'l'
        PlotTextPush(items[1].text, 112);                              // 'p'
        PlotTextPush(items[1].text, 104);                              // 'h'
        PlotTextPush(items[1].text, 97);                               // 'a'
        PlotTextPush(items[1].text, 47);                               // '/'
        PlotTextPush(items[1].text, 50);                               // '2'

        PlotLegendPush(items[2], vec3(0.28, 0.85, 0.36), 83);          // "S"
        PlotTextPush(items[2].text, 109);                              // 'm'
        PlotTextPush(items[2].text, 105);                              // 'i'
        PlotTextPush(items[2].text, 116);                              // 't'
        PlotTextPush(items[2].text, 104);                              // 'h'
        PlotTextPush(items[2].text, 32);                               // ' '
        PlotTextPush(items[2].text, 40);                               // '('
        PlotTextPush(items[2].text, 101);                              // 'e'
        PlotTextPush(items[2].text, 120);                              // 'x'
        PlotTextPush(items[2].text, 97);                               // 'a'
        PlotTextPush(items[2].text, 99);                               // 'c'
        PlotTextPush(items[2].text, 116);                              // 't'
        PlotTextPush(items[2].text, 41);                               // ')'

        // 第四条臂：完全不做遮蔽，即论文里被省略 G 的那种写法 D*F/4。
        PlotLegendPush(items[3], vec3(0.90, 0.40, 0.85), 110);         // "n"
        PlotTextPush(items[3].text, 111);                              // 'o'
        PlotTextPush(items[3].text, 32);                               // ' '
        PlotTextPush(items[3].text, 71);                               // 'G'
        PlotTextPush(items[3].text, 32);                               // ' '
        PlotTextPush(items[3].text, 40);                               // '('
        PlotTextPush(items[3].text, 68);                               // 'D'
        PlotTextPush(items[3].text, 42);                               // '*'
        PlotTextPush(items[3].text, 70);                               // 'F'
        PlotTextPush(items[3].text, 47);                               // '/'
        PlotTextPush(items[3].text, 52);                               // '4'
        PlotTextPush(items[3].text, 41);                               // ')'
        count = 4;
    }
    else if (mode == PLOT_MODE_SPLIT_SUM)
    {
        // mode 7：四个环境（低频 / 中频 / 高频 / 真实 HDRI）。名字必须与
        // validationSplitSumTable.glsl 里 `// arm N = ...` 的短名逐字一致——
        // "图上这条线是哪个环境"只有这一处锚点，不能各写一套拼法。
        PlotLegendPush(items[0], vec3(0.28, 0.85, 0.36), 99);          // "c"
        PlotTextPush(items[0].text, 111);                              // 'o'
        PlotTextPush(items[0].text, 110);                              // 'n'
        PlotTextPush(items[0].text, 115);                              // 's'
        PlotTextPush(items[0].text, 116);                              // 't'
        PlotTextPush(items[0].text, 97);                               // 'a'
        PlotTextPush(items[0].text, 110);                              // 'n'
        PlotTextPush(items[0].text, 116);                              // 't'

        PlotLegendPush(items[1], vec3(0.95, 0.32, 0.28), 99);          // "c"
        PlotTextPush(items[1].text, 111);                              // 'o'
        PlotTextPush(items[1].text, 115);                              // 's'
        PlotTextPush(items[1].text, 105);                              // 'i'
        PlotTextPush(items[1].text, 110);                              // 'n'
        PlotTextPush(items[1].text, 101);                              // 'e'

        PlotLegendPush(items[2], vec3(0.95, 0.72, 0.25), 115);         // "s"
        PlotTextPush(items[2].text, 117);                              // 'u'
        PlotTextPush(items[2].text, 110);                              // 'n'

        PlotLegendPush(items[3], vec3(0.90, 0.40, 0.85), 98);          // "b"
        PlotTextPush(items[3].text, 105);                              // 'i'
        PlotTextPush(items[3].text, 115);                              // 's'
        PlotTextPush(items[3].text, 116);                              // 't'
        PlotTextPush(items[3].text, 114);                              // 'r'
        PlotTextPush(items[3].text, 111);                              // 'o'
        PlotTextPush(items[3].text, 32);                               // ' '
        PlotTextPush(items[3].text, 52);                               // '4'
        PlotTextPush(items[3].text, 107);                              // 'k'
        count = 4;
    }

    if (count == 0)
    {
        return;
    }

    // 位置：绘图区**外面**的左下角，一条一行竖排，从轴框左边缘起、落在 x 轴名下方。
    // 放外面有两个硬理由：① 绘图区内任何位置都会被曲线穿过，图例压上去要么挡曲线、
    // 要么被曲线染色；② 图例色线"与曲线同色同粗细"（这是它的价值），留在绘图区内会
    // 被逐列测量误认成曲线——实测 mode 0 的平均误差会从 0.153 px 涨到 4.990 px。
    // 放外面两个问题都不存在，也就不需要保留区、底板与边框。
    //
    // 位置：绘图区**上方**、外面。自上而下是「说明行（这张图在说什么 / 结论）」+「图例各一行」，
    // 两行都从轴框左边缘起排；参数注记右对齐在说明行上。
    //
    // 为什么放上面：说明与图例合成一个"图头"，读者先读到"这是什么、结论是什么"再进入坐标区；
    // 放下面则要先读完坐标轴才看到说明。放外面（而不是绘图区内）还有两条硬理由：
    // ① 绘图区内任何位置都会被曲线穿过，压上去要么挡曲线、要么被曲线染色；
    // ② 图例色线"与曲线同色同粗细"（这正是它的价值），留在绘图区内会被逐列测量
    //    误认成曲线——实测 mode 0 的平均误差会从 0.153 px 涨到 4.990 px。
    //
    // 高度：u_plotRectPixels.w（上留白）必须容纳「1 行说明 + 最多 3 行图例」。留白 85 px、
    // 行高 cell*7 = 14 px，所以 4 行图例（mode 6/7）必须把行距收到 cell*8 = 16 px 才装得下
    // （1 + 4 行 × 16 = 80 < 85）。为什么不改成两列：单列竖排是这张图的读法，
    // 而"4 条曲线"的图已经用收紧行距解决了，多一条布局分支就多一处没被验证过的渲染路径。
    float rowStep = cell * 7.0 + u_plotTextPixels.w;
    if (count >= 4)
    {
        rowStep = cell * 8.0;
    }
    float legendBaselineY = PlotCaptionBaselineY(plot) - rowStep;
    for (int i = 0; i < 4; ++i)
    {
        if (i >= count)
        {
            break;
        }
        PlotDrawLegendEntry(
            color, pixel, plot.pixelMin.x, legendBaselineY - rowStep * float(i),
            items[i].color, items[i].text);
    }
}


// 轴值字面量：mode 0 的 x 轴用角度，其余用无量纲数。
void PlotBuildRoughnessAnnotation(inout PlotText text, int mode, float roughness, float thetaV)
{
    PlotTextPush(text, 114);          // 'r'
    PlotTextPush(text, 61);           // '='
    PlotTextPushFixed(text, roughness, 2);
    if (mode == PLOT_MODE_HAIR)
    {
        PlotTextPush(text, 32);
        PlotTextPush(text, 116);      // 't'
        PlotTextPush(text, 118);      // 'v'
        PlotTextPush(text, 61);       // '='
        PlotTextPushFixed(text, thetaV, 2);
    }
    else if (mode == PLOT_MODE_ALBEDO)
    {
        // mode 6 的表是按 α 索引的，而参数是感知粗糙度，所以两个都写出来——
        // 读者不必自己心算 r²，也就不会怀疑"图上这条曲线取的是哪一档 α"。
        PlotTextPush(text, 32);
        PlotTextPush(text, 97);       // 'a'
        PlotTextPush(text, 108);      // 'l'
        PlotTextPush(text, 112);      // 'p'
        PlotTextPush(text, 104);      // 'h'
        PlotTextPush(text, 97);       // 'a'
        PlotTextPush(text, 61);       // '='
        PlotTextPushFixed(text, roughness * roughness, 2);
    }
}

// 纵轴名 "D (norm,log10)"：mode 0 / mode 5 的纵轴量相同（都是按峰值归一化的分布）。
// 必须定义在 PlotDrawLogYAxis 之前：GLSL 没有函数提升。
PlotText PlotDistributionYTitle()
{
    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 68);          // 'D'
    PlotTextPush(title, 32);
    PlotTextPush(title, 40);          // '('
    PlotTextPush(title, 110);         // 'n'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 114);         // 'r'
    PlotTextPush(title, 109);         // 'm'
    PlotTextPush(title, 44);          // ','
    PlotTextPush(title, 32);
    PlotTextPush(title, 108);         // 'l'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 103);         // 'g'
    PlotTextPush(title, 49);          // '1'
    PlotTextPush(title, 48);          // '0'
    PlotTextPush(title, 41);          // ')'
    return title;
}

// 兼容旧调用点：mode 0 / 1 的 log 轴只用这一个轴名。
void PlotDrawDistributionYTitle(inout vec3 color, vec2 pixel, PlotLayout plot, float widest)
{
    PlotDrawYTitle(color, pixel, plot, PlotDistributionYTitle(), widest);
}

// log10 轴（1e-3..1）的主刻度：四个十倍程，标签按十进制小数写。
// hairMode 只影响纵轴名的首字母（D / Mp），刻度与网格完全相同。
void PlotDrawLogYAxis(inout vec3 color, vec2 pixel, PlotLayout plot, bool hairMode)
{
    float widest = 0.0;

    for (int i = 0; i < 4; ++i)
    {
        float exponent = -3.0 + float(i);
        float axisY = (exponent + 3.0) / 3.0;
        PlotDrawGridY(color, pixel, plot, axisY);
        PlotDrawYTick(color, pixel, plot, axisY, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, pow(10.0, exponent), 3);
        PlotDrawYLabel(color, pixel, plot, axisY, label);
        widest = max(widest, PlotTextWidth(label, u_plotTextPixels.x));
    }

    // 次刻度：每个十倍程内的 2x 与 5x（log10(2)=0.30103, log10(5)=0.69897）。
    for (int i = 0; i < 3; ++i)
    {
        float base = -3.0 + float(i);
        PlotDrawYTick(color, pixel, plot, (base + 0.30103 + 3.0) / 3.0, false);
        PlotDrawYTick(color, pixel, plot, (base + 0.69897 + 3.0) / 3.0, false);
    }

    // 纵轴量：mode 0 是归一化分布 D，mode 1 是 pbrt 的纵向函数 Mp，其余刻度完全一致。
    if (!hairMode)
    {
        PlotDrawDistributionYTitle(color, pixel, plot, widest);
        return;
    }
    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 77);      // 'M'
    PlotTextPush(title, 112);     // 'p'
    PlotTextPush(title, 32);
    PlotTextPush(title, 40);          // '('
    PlotTextPush(title, 110);         // 'n'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 114);         // 'r'
    PlotTextPush(title, 109);         // 'm'
    PlotTextPush(title, 44);          // ','
    PlotTextPush(title, 32);
    PlotTextPush(title, 108);         // 'l'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 103);         // 'g'
    PlotTextPush(title, 49);          // '1'
    PlotTextPush(title, 48);          // '0'
    PlotTextPush(title, 41);          // ')'
    PlotDrawYTitle(color, pixel, plot, title, widest);
}

// x = θh（mode 0/5）或 θv（mode 6）：0..90°，主刻度 15°，次刻度 7.5°。三个 mode 共用。
void PlotDrawThetaXAxis(inout vec3 color, vec2 pixel, PlotLayout plot, bool viewAngle)
{
    for (int i = 0; i < 7; ++i)
    {
        float degrees = 15.0 * float(i);
        float axisX = degrees / 90.0;
        PlotDrawGridX(color, pixel, plot, axisX);
        PlotDrawXTick(color, pixel, plot, axisX, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, degrees, 0);
        PlotDrawXLabel(color, pixel, plot, axisX, label);
    }
    for (int i = 0; i < 6; ++i)
    {
        PlotDrawXTick(color, pixel, plot, (7.5 + 15.0 * float(i)) / 90.0, false);
    }

    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 104);         // 'h'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 97);          // 'a'
    PlotTextPush(title, 95);          // '_'
    // mode 6 扫的是**入射/视线角** θv，其余 mode 扫的是半角 θh——图上必须写清是哪一个。
    PlotTextPush(title, viewAngle ? 118 : 104);   // 'v' : 'h'
    PlotTextPush(title, 32);
    PlotTextPush(title, 40);          // '('
    PlotTextPush(title, 100);         // 'd'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 103);         // 'g'
    PlotTextPush(title, 41);          // ')'
    PlotDrawXTitle(color, pixel, plot, title);
}

// 线性 0..1 的纵轴（主刻度 0.25、次刻度 0.125）+ 指定轴名。
// mode 2/4 用 "G1"，mode 6 用 "albedo"——刻度完全相同，只有量名不同。
void PlotDrawLinearYAxis01(inout vec3 color, vec2 pixel, PlotLayout plot, PlotText title)
{
    float widest = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        float value = 0.25 * float(i);
        PlotDrawGridY(color, pixel, plot, value);
        PlotDrawYTick(color, pixel, plot, value, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, value, 2);
        PlotDrawYLabel(color, pixel, plot, value, label);
        widest = max(widest, PlotTextWidth(label, u_plotTextPixels.x));
    }
    for (int i = 0; i < 4; ++i)
    {
        PlotDrawYTick(color, pixel, plot, 0.125 + 0.25 * float(i), false);
    }
    PlotDrawYTitle(color, pixel, plot, title, widest);
}

// 轴名 "G1"（mode 2/4 的纵轴量）。
PlotText PlotG1YTitle()
{
    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 71);          // 'G'
    PlotTextPush(title, 49);          // '1'
    return title;
}

// 轴名 "albedo"（mode 6 的纵轴量）。
PlotText PlotAlbedoYTitle()
{
    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 97);          // 'a'
    PlotTextPush(title, 108);         // 'l'
    PlotTextPush(title, 98);          // 'b'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 100);         // 'd'
    PlotTextPush(title, 111);         // 'o'
    return title;
}

// log10 轴（**六个数量级** 1e-6..1）的主刻度：隔一个数量级标一次。
//
// 为什么要加深到 6 个数量级：mode 5 要在同一 α 下对照 GGX 与 Beckmann 的**尾部**，
// 而两者按各自峰值归一化后掉得极快——α = 0.5 时 GGX 在 85° 只剩 3.65e-6、Beckmann 在 60° 就
// 已经落到 1e-21。3 个数量级的轴（mode 0/1 用的那支）根本看不到尾部，也就无法验证
// PBRT Fig 9.23 那句 "Trowbridge–Reitz has higher tails"。
// mode 7 借用同一支轴：split-sum 的相对误差跨 0.001% 到 72%（近 5 个数量级），
// 线性轴会把低频环境那几条曲线全部压成 0，看不出"误差随环境频率增长"。
// 标签隔一个数量级写（1e-6 / 1e-4 / 1e-2 / 1），否则 "0.000001" 这种 8 字符标签会捅穿左侧留白。
// 轴名由调用方给：mode 5 是 "D (norm,log10)"，mode 7 是 "rel err"。
void PlotDrawLogYAxisSixDecades(
    inout vec3 color, vec2 pixel, PlotLayout plot, PlotText title)
{
    float widest = 0.0;

    for (int i = 0; i < 4; ++i)
    {
        float exponent = -6.0 + 2.0 * float(i);
        float axisY = (exponent + 6.0) / 6.0;
        PlotDrawGridY(color, pixel, plot, axisY);
        PlotDrawYTick(color, pixel, plot, axisY, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, pow(10.0, exponent), 6);
        PlotDrawYLabel(color, pixel, plot, axisY, label);
        widest = max(widest, PlotTextWidth(label, u_plotTextPixels.x));
    }

    // 次刻度：每个十倍程内的 2x 与 5x（log10(2)=0.30103, log10(5)=0.69897）。
    for (int i = 0; i < 6; ++i)
    {
        float base = -6.0 + float(i);
        PlotDrawYTick(color, pixel, plot, (base + 0.30103 + 6.0) / 6.0, false);
        PlotDrawYTick(color, pixel, plot, (base + 0.69897 + 6.0) / 6.0, false);
    }

    PlotDrawYTitle(color, pixel, plot, title, widest);
}

// mode 0：x = θh（0..90°，主刻度 15°，次刻度 7.5°），y = 归一化分布（log10 轴，3 个数量级）。
void PlotDrawAxesGgxCharlie(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    PlotDrawThetaXAxis(color, pixel, plot, false);
    PlotDrawLogYAxis(color, pixel, plot, false);
}

// mode 5：与 mode 0 同一个 x 轴（θh），y 轴加深到 6 个数量级（尾部对照用）。
void PlotDrawAxesBeckmann(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    PlotDrawThetaXAxis(color, pixel, plot, false);
    PlotDrawLogYAxisSixDecades(color, pixel, plot, PlotDistributionYTitle());
}

// mode 6：x = θv（入射角），y = 线性 0..1 的方向反照率。
void PlotDrawAxesAlbedo(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    PlotDrawThetaXAxis(color, pixel, plot, true);
    PlotDrawLinearYAxis01(color, pixel, plot, PlotAlbedoYTitle());
}

// 轴名 "rel err"（mode 7 的纵轴量：相对误差 |split-sum − 参考| / 参考）。
PlotText PlotRelativeErrorYTitle()
{
    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 114);         // 'r'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 108);         // 'l'
    PlotTextPush(title, 32);
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 114);         // 'r'
    PlotTextPush(title, 114);         // 'r'
    return title;
}

// mode 7：x = roughness（0..1，主刻度 0.25），y = 相对误差的 6 数量级 log10 轴（与 mode 5 共用）。
void PlotDrawAxesSplitSum(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    for (int i = 0; i < 5; ++i)
    {
        float value = 0.25 * float(i);
        PlotDrawGridX(color, pixel, plot, value);
        PlotDrawXTick(color, pixel, plot, value, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, value, 2);
        PlotDrawXLabel(color, pixel, plot, value, label);
    }
    for (int i = 0; i < 4; ++i)
    {
        PlotDrawXTick(color, pixel, plot, 0.125 + 0.25 * float(i), false);
    }

    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 114);         // 'r'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 117);         // 'u'
    PlotTextPush(title, 103);         // 'g'
    PlotTextPush(title, 104);         // 'h'
    PlotTextPush(title, 110);         // 'n'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 115);         // 's'
    PlotTextPush(title, 115);         // 's'
    PlotDrawXTitle(color, pixel, plot, title);

    PlotDrawLogYAxisSixDecades(color, pixel, plot, PlotRelativeErrorYTitle());
}

// mode 1：x = sinθl（-1..1，主刻度 0.5），y = 三条路径归一化后的 log10 轴。
void PlotDrawAxesHair(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    for (int i = 0; i < 5; ++i)
    {
        float value = -1.0 + 0.5 * float(i);
        float axisX = (value + 1.0) * 0.5;
        PlotDrawGridX(color, pixel, plot, axisX);
        PlotDrawXTick(color, pixel, plot, axisX, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, value, 1);
        PlotDrawXLabel(color, pixel, plot, axisX, label);
    }
    for (int i = 0; i < 4; ++i)
    {
        float value = -0.75 + 0.5 * float(i);
        PlotDrawXTick(color, pixel, plot, (value + 1.0) * 0.5, false);
    }

    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 115);         // 's'
    PlotTextPush(title, 105);         // 'i'
    PlotTextPush(title, 110);         // 'n'
    PlotTextPush(title, 32);
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 104);         // 'h'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 97);          // 'a'
    PlotTextPush(title, 95);          // '_'
    PlotTextPush(title, 108);         // 'l'
    PlotDrawXTitle(color, pixel, plot, title);

    // 纵轴量级与 mode 0 相同（各自按峰值归一化的 log10 轴），共用同一套刻度绘制。
    PlotDrawLogYAxis(color, pixel, plot, true);
}

// mode 2：x = cosθ（左端 1、右端 0，主刻度 0.25），y = G1 线性轴 0..1。
void PlotDrawAxesG1(inout vec3 color, vec2 pixel, PlotLayout plot)
{
    for (int i = 0; i < 5; ++i)
    {
        float value = 1.0 - 0.25 * float(i);
        float axisX = 1.0 - value;
        PlotDrawGridX(color, pixel, plot, axisX);
        PlotDrawXTick(color, pixel, plot, axisX, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, value, 2);
        PlotDrawXLabel(color, pixel, plot, axisX, label);
    }
    for (int i = 0; i < 4; ++i)
    {
        float value = 0.875 - 0.25 * float(i);
        PlotDrawXTick(color, pixel, plot, 1.0 - value, false);
    }

    PlotText title;
    PlotTextClear(title);
    PlotTextPush(title, 99);          // 'c'
    PlotTextPush(title, 111);         // 'o'
    PlotTextPush(title, 115);         // 's'
    PlotTextPush(title, 32);
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 104);         // 'h'
    PlotTextPush(title, 101);         // 'e'
    PlotTextPush(title, 116);         // 't'
    PlotTextPush(title, 97);          // 'a'
    PlotDrawXTitle(color, pixel, plot, title);

    float widest = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        float value = 0.25 * float(i);
        PlotDrawGridY(color, pixel, plot, value);
        PlotDrawYTick(color, pixel, plot, value, true);

        PlotText label;
        PlotTextClear(label);
        PlotTextPushFixed(label, value, 2);
        PlotDrawYLabel(color, pixel, plot, value, label);
        widest = max(widest, PlotTextWidth(label, u_plotTextPixels.x));
    }
    for (int i = 0; i < 4; ++i)
    {
        PlotDrawYTick(color, pixel, plot, 0.125 + 0.25 * float(i), false);
    }

    PlotText yTitle;
    PlotTextClear(yTitle);
    PlotTextPush(yTitle, 71);         // 'G'
    PlotTextPush(yTitle, 49);         // '1'
    PlotDrawYTitle(color, pixel, plot, yTitle, widest);
}

// ---------------------------------------------------------------------------

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    vec2 uv = context.texCoord;
    int mode = int(floor(u_plotMode + 0.5));
    PlotLayout plot = BuildPlotLayout(uv);

    // 底板：深色底，帮助定位峰值角度。
    vec3 color = PLOT_BASE_COLOR;

    if (mode == PLOT_MODE_UV)
    {
        // mode 3：UV 诊断。baseColor = (uv.x, uv.y, 0)。用于确定引擎实际传入 Material
        // Surface 的 texCoord 与屏幕方向的对应关系：读回像素就知道 uv.x / uv.y 各自朝
        // 哪边走、是否被翻转。曲线探针的横纵轴映射必须以此为准，不能靠假设。
        //
        // **本模式不画任何坐标标注**：叠加会直接污染这份读数。M-07 的结论
        // （texCoord.y 沿屏幕向上增大、fov 是水平视场角）就是靠它读出来的。
        color = vec3(clamp(uv.x, 0.0, 1.0), clamp(uv.y, 0.0, 1.0), 0.0);
    }
    else
    {
        vec2 pixel = uv * plot.panelSize;
        float axisX = PlotAxisX(pixel, plot);
        float curveY = PlotAxisY(pixel, plot);
        // 曲线裁剪：见 PlotInsidePlot 的说明（不裁剪会在留白上画出假曲线）。
        float curveMask = PlotInsidePlot(pixel, plot);

        // 先画网格、刻度、刻度值、轴名，再画轴框，最后画曲线。
        //
        // 轴框必须**在网格之后**画：网格的主刻度与边框共线，若先画框，边框会被网格
        // 盖成网格色，看起来"没有坐标轴"，测量脚本的边框核对也会失配。
        // 曲线压在最上层，保证"曲线最亮处就是曲线颜色"这一测量前提成立。
        if (mode == PLOT_MODE_GGX_CHARLIE)
        {
            PlotDrawAxesGgxCharlie(color, pixel, plot);
        }
        else if (mode == PLOT_MODE_HAIR)
        {
            PlotDrawAxesHair(color, pixel, plot);
        }
        else if (mode == PLOT_MODE_G1 || mode == PLOT_MODE_G1_VARIANTS)
        {
            // mode 4 与 mode 2 共用同一套坐标轴（x = cosθ 左端 1，y = G1 线性 0..1），
            // 只有曲线条数不同：mode 4 多画一支 Schlick 变体。共用轴定义是刻意的——
            // 两张图可以直接叠着读，不需要再做坐标换算。
            PlotDrawAxesG1(color, pixel, plot);
        }
        else if (mode == PLOT_MODE_BECKMANN)
        {
            // mode 5 与 mode 0 共用 x 轴（θh），y 轴加深到 6 个数量级。
            PlotDrawAxesBeckmann(color, pixel, plot);
        }
        else if (mode == PLOT_MODE_ALBEDO)
        {
            PlotDrawAxesAlbedo(color, pixel, plot);
        }
        else if (mode == PLOT_MODE_SPLIT_SUM)
        {
            PlotDrawAxesSplitSum(color, pixel, plot);
        }
        PlotDrawFrame(color, pixel, plot);

        PlotText annotation;
        PlotTextClear(annotation);
        // mode 7 的横轴就是 roughness（参数注记会与轴名重复），t 值改写在说明行里，
        // 所以这个 mode 不画参数注记：图上不留"r=0.50 到底指哪根轴"的歧义。
        if (mode != PLOT_MODE_SPLIT_SUM)
        {
            PlotBuildRoughnessAnnotation(annotation, mode, u_plotRoughness, u_plotThetaV);
            PlotDrawAnnotation(color, pixel, plot, annotation);
        }

        // 图头：说明行（这是什么 / 结论）+ 图例（哪条线是哪条），都在绘图区上方外面。
        PlotText caption;
        PlotTextClear(caption);
        PlotBuildCaption(caption, mode, u_plotRoughness);
        PlotDrawCaption(color, pixel, plot, caption);
        PlotDrawLegend(color, pixel, plot, mode);

        // 曲线的纵坐标走**轴坐标**（0..1，经 PlotAxisToPixelY 换算到面板像素），
        // 与 uv 解耦：绘图区留白只改变版式，不改变曲线定义域。
        if (mode == PLOT_MODE_GGX_CHARLIE)
        {
            // --- mode 0：GGX vs Charlie 分布（Neubelt & Pettineo 2013 Fig 6 左） ---
            // 横轴 θh = 0..90°，纵轴两条曲线各自按峰值归一化后取 log10 刻度。
            float thetaH = axisX * (PI * 0.5);
            float ggxPeak = PlotSamplePeak(false, u_plotRoughness);
            float charliePeak = PlotSamplePeak(true, u_plotRoughness);
            float ggxValue = PlotGgxCurve(thetaH, u_plotRoughness) / ggxPeak;
            float charlieValue = PlotCharlieCurve(thetaH, u_plotRoughness) / charliePeak;

            color = mix(color, vec3(0.28, 0.85, 0.36), PlotCurveLine(PlotLogY(ggxValue) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.95, 0.32, 0.28), PlotCurveLine(PlotLogY(charlieValue) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
        }
        else if (mode == PLOT_MODE_HAIR)
        {
            // --- mode 1：头发 R / TT / TRT 纵向散射（Marschner 2003 Fig 5、pbrt Fig 9） ---
            // 横轴是沿发丝方向的 sinθl ∈ [-1, 1]，0 在正中间。
            float sinThetaL = -1.0 + 2.0 * axisX;
            float sinThetaV = clamp(sin(u_plotThetaV), -1.0, 1.0);
            vec3 peaks = SampleHairPeaks(sinThetaV, u_plotRoughness);
            vec3 paths = SampleHairPaths(sinThetaL, sinThetaV, u_plotRoughness) / peaks;

            // R 琥珀色、TT 青色、TRT 品红，对应三条路径身份。
            color = mix(color, vec3(0.95, 0.72, 0.25), PlotCurveLine(PlotLogY(paths.x) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.30, 0.80, 0.92), PlotCurveLine(PlotLogY(paths.y) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.90, 0.40, 0.85), PlotCurveLine(PlotLogY(paths.z) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
        }
        else if (mode == PLOT_MODE_G1)
        {
            // --- mode 2：G1 近似误差（Karis 2013 Fig 2 的对照结构） ---
            // 横轴是 cosθ，左端 1（法线入射）、右端 0（掠射）。
            // 红线 = 引擎直接光路径使用的 GeometrySchlickGGX，它是论文式(4) 的
            //        **analytic light source 变体** k=(r+1)²/8（论文明确禁止把它用于 IBL）；
            // 绿线 = 精确 Smith GGX G1。两者都在 [0,1]，因此用线性纵轴。
            float cosTheta = 1.0 - axisX;
            float schlick = GeometrySchlickGGX(cosTheta, u_plotRoughness);
            float smith = SmithG1Ggx(cosTheta, u_plotRoughness);

            color = mix(color, vec3(0.95, 0.32, 0.28), PlotCurveLine(schlick - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.28, 0.85, 0.36), PlotCurveLine(smith - curveY, PlotCurveHalfWidthPixels()) * curveMask);
        }
        else if (mode == PLOT_MODE_BECKMANN)
        {
            // --- mode 5：GGX vs Beckmann 的分布形状与尾部（PBRT 4ed §9.6.1 / Fig 9.23） ---
            // 两条曲线都按峰值归一化，而且**两者的峰值本来就相同**：θh = 0 处
            // GGX 给 α²/(π·α⁴) = 1/(πα²)，Beckmann 给 1/(πα²)——所以"各自归一化"与
            // "用同一个分母归一化"是同一件事，图上两条曲线可以直接比高低。
            // 判据是论文那句 "Trowbridge–Reitz has higher tails at larger values of θ"：
            // 大角度处绿线（GGX）必须压在红线（Beckmann）之上。
            float thetaH = axisX * (PI * 0.5);
            float ggxValue = PlotGgxCurve(thetaH, u_plotRoughness) /
                max(PlotPeakValue(false, u_plotRoughness), 1.0e-30);
            float beckmannValue = DistributionBeckmann(cos(thetaH), u_plotRoughness) /
                max(PlotPeakValue(true, u_plotRoughness), 1.0e-30);

            color = mix(color, vec3(0.28, 0.85, 0.36), PlotCurveLine(PlotLogYRange(ggxValue, 6.0) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.95, 0.32, 0.28), PlotCurveLine(PlotLogYRange(beckmannValue, 6.0) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
        }
        else if (mode == PLOT_MODE_ALBEDO)
        {
            // --- mode 6：四条 G 臂的**方向反照率**（D-05 的图面化） ---
            // 曲线值完全来自 validationAlbedoTable.glsl——那是离线脚本
            // tool/validation/verify_brdf_integrals.py 用重要性采样算出来的表（每个格子 8192 样本），
            // shader 只做双线性插值。这是刻意的：**图上这条曲线与记录里那个结论必须是同一份数**，
            // 若让 shader 再实现一遍积分，就会出现两个"真值"，图与记录一旦不一致就没人知道信谁。
            //
            // 纵轴是线性 0..1，所以"伽马校正有没有漏、能量有没有超 1"是直接读出来的：
            // 所有曲线都必须待在 1 以下——这就是 D-05 的判据本身。
            //
            // 四条臂的含义（与离线判定表的臂名一致，颜色与 mode 2/4 同名同色）：
            //   红   = Schlick (direct) k=(r+1)²/8   引擎直接光路径那支（也是论文给 analytic 光源的变体）
            //   青   = Schlick (ibl)    k=α/2        引擎 IBL 路径那支（brfdLut.comp 生成 LUT 用的同一支）
            //   绿   = Smith (exact)                 精确 Smith GGX，参考曲线
            //   品红 = no G (D*F/4)                  完全不做遮蔽，用来量化"省略 G 会损失多少能量"
            float alpha = u_plotRoughness * u_plotRoughness;
            float thetaDeg = axisX * 90.0;

            color = mix(color, vec3(0.95, 0.32, 0.28), PlotCurveLine(
                PlotAlbedoTable(PLOT_ALBEDO_ARM_ANALYTIC, alpha, thetaDeg) - curveY,
                PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.30, 0.80, 0.92), PlotCurveLine(
                PlotAlbedoTable(PLOT_ALBEDO_ARM_IBL, alpha, thetaDeg) - curveY,
                PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.28, 0.85, 0.36), PlotCurveLine(
                PlotAlbedoTable(PLOT_ALBEDO_ARM_SMITH, alpha, thetaDeg) - curveY,
                PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.90, 0.40, 0.85), PlotCurveLine(
                PlotAlbedoTable(PLOT_ALBEDO_ARM_NO_G, alpha, thetaDeg) - curveY,
                PlotCurveHalfWidthPixels()) * curveMask);
        }
        else if (mode == PLOT_MODE_SPLIT_SUM)
        {
            // --- mode 7：split-sum 相对误差随 roughness 与环境频率的变化（Karis 2013 §5） ---
            // 五条曲线 = 五个环境（由一个环境的空间频率决定误差量级），值全部来自
            // validationSplitSumTable.glsl——由 verify_split_sum.py 生成的离线表；
            // shader 只做 roughness 方向的线性插值。表里 -1 表示"该配置参考值太小、
            // 相对误差无意义"，曲线在那里断开（低频环境在低粗糙度下就是这种情况）。
            //
            // 纵轴用 6 个数量级：误差跨 0.001% 到 72%，线性轴会把低频那几条压成一条 0 线。
            float roughness = axisX;
            for (int arm = 0; arm < PLOT_SPLITSUM_ENV_COUNT; ++arm)
            {
                float error = PlotSplitSumTable(arm, roughness);
                if (error < 0.0)
                {
                    continue;
                }
                vec3 curveColor = vec3(0.28, 0.85, 0.36);
                if (arm == PLOT_SPLITSUM_ARM_COSINE) { curveColor = vec3(0.95, 0.32, 0.28); }
                if (arm == PLOT_SPLITSUM_ARM_SUN) { curveColor = vec3(0.95, 0.72, 0.25); }
                if (arm == PLOT_SPLITSUM_ARM_BISTRO) { curveColor = vec3(0.90, 0.40, 0.85); }
                color = mix(color, curveColor, PlotCurveLine(
                    PlotLogYRange(error, 6.0) - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            }
        }
        else
        {
            // --- mode 4：两支 Schlick 变体 vs 精确 Smith（同一坐标系下并排） ---
            // 存在的理由（D-20 / D-21 的收口证据）：Karis 2013 §Specular G 给的是**两支**Schlick，
            // 它们分别属于两条路径，不是同一条曲线的两种写法：
            //   红 = GeometrySchlickGGX    k=(r+1)²/8  —— analytic light source（直接光）专用；
            //   青 = GeometrySchlickGGXIbl  k=α/2      —— IBL 域，也是 brfdLut.comp 生成 LUT 用的那支；
            //   绿 = SmithG1Ggx            精确解（参考）。
            // 三支都直接调用引擎/共享头里的真实实现，探针不另抄公式。
            float cosTheta = 1.0 - axisX;
            float schlickDirect = GeometrySchlickGGX(cosTheta, u_plotRoughness);
            float schlickIbl = GeometrySchlickGGXIbl(cosTheta, u_plotRoughness);
            float smith = SmithG1Ggx(cosTheta, u_plotRoughness);

            color = mix(color, vec3(0.95, 0.32, 0.28), PlotCurveLine(schlickDirect - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.30, 0.80, 0.92), PlotCurveLine(schlickIbl - curveY, PlotCurveHalfWidthPixels()) * curveMask);
            color = mix(color, vec3(0.28, 0.85, 0.36), PlotCurveLine(smith - curveY, PlotCurveHalfWidthPixels()) * curveMask);
        }
    }

    MaterialInputs inputs = CreateDefaultMaterialInputs();
    inputs.baseColor = color * u_tintColor.rgb;
    inputs.opacity = 1.0;
    inputs.opacityMask = 1.0;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif
