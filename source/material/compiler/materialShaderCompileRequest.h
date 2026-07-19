#pragma once

// 文件职责：定义材质 Shader 编译请求及其稳定身份数据，供 Shader 组合、编译和管线缓存使用；
// 不保存 MI 的运行时参数，也不负责创建 GPU 资源。
// File responsibility: Defines material shader compile requests and their stable identities for
// shader composition, compilation, and pipeline caching; it owns no runtime MI data or GPU resources.

#include <cstddef>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "shaderVariant.h"

namespace VL
{

// 标识材质代码参与的引擎 Pass；该枚举只描述编译目标，不表示运行时渲染顺序。
// Identifies the engine pass compiled for material code; it does not describe runtime pass order.
enum class MaterialPass
{
    Base,
    ShadowDepth
};

inline std::string MaterialPassToString(MaterialPass pass)
{
    switch (pass)
    {
    case MaterialPass::Base:
        return "Base";
    case MaterialPass::ShadowDepth:
        return "ShadowDepth";
    }
    throw std::runtime_error("Unknown MaterialPass");
}

// M_ 默认值与 MI_ 覆盖合并后得到的编译期材质特性。
// Pass 路由和 Shader 身份使用这些值，逐帧变化的运行时参数不进入此结构。
// Compile-time features resolved after merging M_ defaults with MI_ overrides.
// Pass routing and shader identity consume this key; per-frame runtime values do not.
struct MaterialFeatureKey
{
    bool writesEveryPixel = true;
    bool usesOpacityMask = false;
    bool modifiesMeshPosition = false;
    bool twoSided = false;

    std::string GetNormalizedKey() const
    {
        std::ostringstream stream;
        stream << "writesEveryPixel=" << writesEveryPixel
               << "|usesOpacityMask=" << usesOpacityMask
               << "|modifiesMeshPosition=" << modifiesMeshPosition
               << "|twoSided=" << twoSided;
        return stream.str();
    }
};

// 记录 M_ 资产声明的材质求值模块，路径均相对 shader/glsl。
// Composer 只负责把这些模块装配进引擎 Pass 模板，不解析 GLSL 语义。
// Describes material evaluation modules declared by an M_ asset; paths are relative to shader/glsl.
// The Composer assembles them into engine pass templates without parsing GLSL semantics.
struct MaterialEvaluationSourceDesc
{
    std::string materialSourcePath;
    std::string vertexEvaluationPath;
    std::string surfaceEvaluationPath;
    std::string parameterIncludePath;
};

// 一份生成材质 Pass Shader 所需的完整身份与源码输入。
// 管线固定状态和 MI 动态数据由其他对象持有，避免进入 Shader 编译缓存键。
// Complete identity and source input for one generated material pass shader.
// Pipeline state and dynamic MI data remain outside the shader compilation cache key.
struct MaterialShaderCompileRequest
{
    ShaderVariantKey shaderVariantKey;
    MaterialPass pass = MaterialPass::Base;
    MaterialFeatureKey features;
    std::string vertexFactoryKey = "StaticMesh";
    MaterialEvaluationSourceDesc source;

    std::string GetNormalizedKey() const
    {
        std::ostringstream stream;
        stream << "materialSource=" << source.materialSourcePath
               << "|pass=" << MaterialPassToString(pass)
               << "|features=" << features.GetNormalizedKey()
               << "|vertexFactory=" << vertexFactoryKey
               << "|compileTarget=Vulkan1.4"
               << "|variant=" << shaderVariantKey.GetNormalizedKey()
               << "|vertexEvaluation=" << source.vertexEvaluationPath
               << "|surfaceEvaluation=" << source.surfaceEvaluationPath
               << "|parameterInclude=" << source.parameterIncludePath;
        return stream.str();
    }

    std::string GetRequestHash() const
    {
        const std::size_t hashValue = std::hash<std::string>{}(GetNormalizedKey());
        std::ostringstream stream;
        stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0')
               << static_cast<unsigned long long>(hashValue);
        return stream.str();
    }

    std::string GetDisplayName() const
    {
        return shaderVariantKey.shaderName + "." + MaterialPassToString(pass) + "@" + GetRequestHash();
    }
};

} // namespace VL
