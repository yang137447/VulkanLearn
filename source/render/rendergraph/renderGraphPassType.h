#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace VL
{

// RenderGraph Pass 的有限执行行为。JSON 只在编译边界使用字符串，
// CompiledRenderGraph 及其后的运行时统一消费这个强类型合同。
enum class RenderGraphPassType
{
    Unknown,
    Shadow,
    Geometry,
    ForwardOpaque,
    ForwardTransparent,
    PostProcess,
    // 编辑器参考网格：世界空间线条几何，和 sky 一样只向 sceneColor 合成；
    // 区别是它保留深度测试以复用场景 sceneDepth 做遮挡，因此需要独立行为类型。
    EditorGrid,
    AutoExposure,
    // BeforeLighting DBuffer decal：独立 DecalRenderer 用 unit cube 投影，
    // 写 DBuffer A/B/C/D + Mask，必须排在 geometry 之后、deferredLighting 之前。
    Decal,
    // Decal 硬件 stencil 预标记：只写 D24S8 的 stencil bit0，不写颜色。
    // 与 Decal pass 分开，确保 decal pass 采样 sceneDepth 时不再把同一
    // depth 资源同时作为 attachment，避免采样/写入反馈环。
    DecalStencil,
    // 粒子 billboard 不经过 Material schema，由 ParticleRenderer 直接绑定
    // 自己的 UBO/SSBO；该类型只负责在图中声明 sceneColor/sceneDepth 的合成顺序。
    // 半分辨率软粒子深度过滤：读 sceneDepth，写线性视深度 texture2D。
    ParticleSoftDepth,
    Particle,
    // 屏幕空间折射/热扰动：粒子 emitter 只写 UV 偏移缓冲，不碰 sceneColor；
    // toneMapping 在最终合成时采样该缓冲并偏移场景颜色。独立 pass 让粒子
    // 与场景颜色之间没有读写反馈环。
    Distortion,
    // 局部光阴影：每个 pass 负责 localShadowMap 的一个数组层。点光在一个
    // pass 内按八象限多次 begin/end 投影，聚光一次透视绘制。
    LocalShadow,
    // 体积雾注入：独立 compute 把 32 个深度层的 in-scatter/transmittance
    // 写进 texture2DArray（storage image），不经过 framebuffer。
    VolumetricFog,
    // 体积雾合成：全屏 pass 读取相邻两层并按像素深度插值，用
    // One / OneMinusSrcAlpha 预乘混合写回 sceneColor。
    FogComposite,
    // 屏幕空间 tile 光照聚类：纯 compute 节点，不写任何图资源；输出缓冲
    // （lightGrid/lightIndices/lightGridParams）由 RendererFrameResources
    // 按 swapchain image 持有，graph 只负责把它排在光照数据上传之后、
    // geometry 之前。
    LightCulling
};

struct RenderGraphPassTypeName
{
    RenderGraphPassType type;
    std::string_view name;
};

// 十六个：编辑器网格、自动曝光、Decal、DecalStencil、软深度、粒子、屏幕空间扰动、
// 局部光阴影、体积雾注入与体积雾合成各自追加，
// 合并后必须都在表里，
// 否则 ParseRenderGraphPassType 解析不出对应名字（配置里写了却当成未知 pass）。
inline constexpr std::array<RenderGraphPassTypeName, 16>
    RenderGraphPassTypeNames = {{
        {RenderGraphPassType::Shadow, "shadow"},
        {RenderGraphPassType::Geometry, "geometry"},
        {RenderGraphPassType::ForwardOpaque, "forwardOpaque"},
        {RenderGraphPassType::ForwardTransparent, "forwardTransparent"},
        {RenderGraphPassType::PostProcess, "postProcess"},
        {RenderGraphPassType::EditorGrid, "editorGrid"},
        {RenderGraphPassType::AutoExposure, "autoExposure"},
        {RenderGraphPassType::Decal, "decal"},
        {RenderGraphPassType::DecalStencil, "decalStencil"},
        {RenderGraphPassType::ParticleSoftDepth, "particleSoftDepth"},
        {RenderGraphPassType::Particle, "particle"},
        {RenderGraphPassType::Distortion, "distortion"},
        {RenderGraphPassType::LocalShadow, "localShadow"},
        {RenderGraphPassType::VolumetricFog, "volumetricFog"},
        {RenderGraphPassType::FogComposite, "fogComposite"},
        {RenderGraphPassType::LightCulling, "lightCulling"}
    }};

inline std::optional<RenderGraphPassType> ParseRenderGraphPassType(
    std::string_view value)
{
    for (const RenderGraphPassTypeName& entry :
         RenderGraphPassTypeNames)
    {
        if (entry.name == value)
        {
            return entry.type;
        }
    }
    return std::nullopt;
}

inline std::string_view RenderGraphPassTypeToString(
    RenderGraphPassType type)
{
    for (const RenderGraphPassTypeName& entry :
         RenderGraphPassTypeNames)
    {
        if (entry.type == type)
        {
            return entry.name;
        }
    }
    return "unknown";
}

} // namespace VL
