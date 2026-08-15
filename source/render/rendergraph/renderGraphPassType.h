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
    ForwardTransparent,
    PostProcess
};

struct RenderGraphPassTypeName
{
    RenderGraphPassType type;
    std::string_view name;
};

inline constexpr std::array<RenderGraphPassTypeName, 4>
    RenderGraphPassTypeNames = {{
        {RenderGraphPassType::Shadow, "shadow"},
        {RenderGraphPassType::Geometry, "geometry"},
        {RenderGraphPassType::ForwardTransparent, "forwardTransparent"},
        {RenderGraphPassType::PostProcess, "postProcess"}
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
