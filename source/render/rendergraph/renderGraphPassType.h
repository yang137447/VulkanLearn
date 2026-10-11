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
    EditorGrid
};

struct RenderGraphPassTypeName
{
    RenderGraphPassType type;
    std::string_view name;
};

inline constexpr std::array<RenderGraphPassTypeName, 6>
    RenderGraphPassTypeNames = {{
        {RenderGraphPassType::Shadow, "shadow"},
        {RenderGraphPassType::Geometry, "geometry"},
        {RenderGraphPassType::ForwardOpaque, "forwardOpaque"},
        {RenderGraphPassType::ForwardTransparent, "forwardTransparent"},
        {RenderGraphPassType::PostProcess, "postProcess"},
        {RenderGraphPassType::EditorGrid, "editorGrid"}
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
