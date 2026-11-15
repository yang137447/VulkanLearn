// 文件职责：实现材质 Pass 装配，把特性宏、材质上下文、生成参数 include 与
// 引擎顶点/片元/细分模板拼成完整 GLSL；不解析 GLSL 语义、不编译 SPIR-V。
// File responsibility: Implements material pass composition by joining feature
// macros, material context, generated parameter includes, and engine templates;
// it neither parses GLSL semantics nor compiles SPIR-V.

#include "material/compiler/materialShaderComposer.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>

#include "commonFunction.h"

namespace VL
{
namespace
{

constexpr const char* kTerrainTessellationMacro = "VL_TERRAIN_TESSELLATION";

std::string BuildFeatureDefines(
    const MaterialFeatureKey& features,
    MaterialPass pass)
{
    std::ostringstream stream;
    stream << "#define MATERIAL_WRITES_EVERY_PIXEL " << features.writesEveryPixel << "\n"
           << "#define MATERIAL_USES_OPACITY_MASK " << features.usesOpacityMask << "\n"
           << "#define MATERIAL_MODIFIES_MESH_POSITION " << features.modifiesMeshPosition << "\n"
           << "#define MATERIAL_TWO_SIDED " << features.twoSided << "\n"
           << "#define VL_LOCAL_SHADOW " << (pass == MaterialPass::LocalShadow ? 1 : 0) << "\n\n";
    return stream.str();
}

std::string BuildInclude(
    const std::filesystem::path& virtualSourceDirectory,
    const std::filesystem::path& shaderGlslRoot,
    const std::string& relativePath,
    bool allowPreparedOverlay = false)
{
    const std::filesystem::path targetPath = shaderGlslRoot / relativePath;
    if (!allowPreparedOverlay &&
        !std::filesystem::is_regular_file(targetPath))
    {
        throw std::runtime_error(
            "Material shader composition source does not exist: " + targetPath.generic_string());
    }

    const std::filesystem::path includePath =
        std::filesystem::relative(targetPath, virtualSourceDirectory);
    return "#include \"" + includePath.generic_string() + "\"\n";
}

void AppendInclude(
    std::ostringstream& stream,
    const std::filesystem::path& virtualSourceDirectory,
    const std::filesystem::path& shaderGlslRoot,
    const std::string& relativePath,
    bool allowPreparedOverlay = false)
{
    stream << BuildInclude(
        virtualSourceDirectory,
        shaderGlslRoot,
        relativePath,
        allowPreparedOverlay);
}

std::string BuildStageSource(
    const MaterialShaderCompileRequest& request,
    const std::filesystem::path& virtualSourceDirectory,
    const std::filesystem::path& shaderGlslRoot,
    bool vertexStage)
{
    std::ostringstream stream;
    stream << "#version 450\n\n";
    // index=1 的第二颜色源只在 fragment stage 使用。仅对已确认启用双源混合的
    // Thin Translucent variant 声明扩展，避免普通材质无故增加平台编译要求。
    if (!vertexStage &&
        request.shaderVariantKey.renderMode == RenderMode::ThinTranslucent &&
        HasMaterialMacroValue(
            request.shaderVariantKey.macros,
            kThinTranslucentDualSourceMacro,
            "1"))
    {
        stream << "#extension GL_EXT_blend_func_extended : require\n\n";
    }
    stream << BuildFeatureDefines(request.features, request.pass);

    if (vertexStage)
    {
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, "engine/materialContext.glsl");
        AppendInclude(
            stream,
            virtualSourceDirectory,
            shaderGlslRoot,
            request.source.parameterIncludePath,
            request.source.parameterIncludeBytes.has_value());
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, request.source.vertexEvaluationPath);
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, request.pass == MaterialPass::Base
            ? "engine/passTemplate/base.vert.glsl"
            : "engine/passTemplate/shadowDepth.vert.glsl");
    }
    else if (request.pass == MaterialPass::Base ||
             request.pass == MaterialPass::LocalShadow ||
             request.features.usesOpacityMask)
    {
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, "engine/materialContext.glsl");
        // 生成参数 include 必须先于 materialSurface；它定义 MATERIAL_SHADING_MODEL，
        // 否则 materialSurface 的 DefaultLit 兜底会吞掉 Skin/Cloth 等真实模型选择。
        AppendInclude(
            stream,
            virtualSourceDirectory,
            shaderGlslRoot,
            request.source.parameterIncludePath,
            request.source.parameterIncludeBytes.has_value());
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, "engine/materialSurface.glsl");
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, request.source.surfaceEvaluationPath);
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, request.pass == MaterialPass::Base
            ? "engine/passTemplate/base.frag.glsl"
            : "engine/passTemplate/shadowDepth.frag.glsl");
    }
    else
    {
        AppendInclude(
            stream,
            virtualSourceDirectory,
            shaderGlslRoot,
            "engine/passTemplate/shadowDepth.frag.glsl");
    }

    return stream.str();
}

std::string BuildTessellationStageSource(
    const MaterialShaderCompileRequest& request,
    const std::filesystem::path& virtualSourceDirectory,
    const std::filesystem::path& shaderGlslRoot,
    bool tessControlStage)
{
    std::ostringstream stream;
    stream << "#version 450\n\n";
    stream << BuildFeatureDefines(request.features, request.pass);
    // 与顶点/片元一致：共享材质上下文和生成参数先进入编译单元，
    // 细分模板只负责控制点透传、因子选择与位移。
    AppendInclude(
        stream,
        virtualSourceDirectory,
        shaderGlslRoot,
        "engine/materialContext.glsl");
    AppendInclude(
        stream,
        virtualSourceDirectory,
        shaderGlslRoot,
        request.source.parameterIncludePath,
        request.source.parameterIncludeBytes.has_value());
    AppendInclude(
        stream,
        virtualSourceDirectory,
        shaderGlslRoot,
        tessControlStage
            ? "engine/passTemplate/base.tesc.glsl"
            : "engine/passTemplate/base.tese.glsl");
    return stream.str();
}

} // namespace

ComposedMaterialShaderSource MaterialShaderComposer::Compose(
    const MaterialShaderCompileRequest& request)
{
    if (request.vertexFactoryKey != "StaticMesh" && request.vertexFactoryKey != "SkinnedMesh" &&
        request.vertexFactoryKey != "TressFXStrands")
    {
        throw std::runtime_error(
            "Unsupported material vertex factory: " + request.vertexFactoryKey);
    }

    const std::filesystem::path shaderGlslRoot =
        std::filesystem::path(CommonFunction::GetProjectPath()) / "shader" / "glsl";
    const std::filesystem::path virtualSourceDirectory = shaderGlslRoot / "__composed__";
    const std::string requestHash = request.GetRequestHash();

    ComposedMaterialShaderSource result;
    result.vertexVirtualPath =
        (virtualSourceDirectory / (requestHash + ".vert")).string();
    result.fragmentVirtualPath =
        (virtualSourceDirectory / (requestHash + ".frag")).string();
    result.vertexSource = BuildStageSource(
        request,
        virtualSourceDirectory,
        shaderGlslRoot,
        true);
    result.fragmentSource = BuildStageSource(
        request,
        virtualSourceDirectory,
        shaderGlslRoot,
        false);

    // 地形细分只服务 Base pass：shadowDepth 顶点模板不写 MaterialVaryings，
    // 而 base.tese 的接口必须与 base.vert 完全一致。若在这里为阴影 pass
    // 也生成细分阶段，管线创建会因接口不匹配失败。
    const bool terrainTessellation =
        request.pass == MaterialPass::Base &&
        HasMaterialMacroValue(
            request.shaderVariantKey.macros,
            kTerrainTessellationMacro,
            "1");
    if (terrainTessellation)
    {
        result.tessControlVirtualPath =
            (virtualSourceDirectory / (requestHash + ".tesc")).string();
        result.tessEvaluationVirtualPath =
            (virtualSourceDirectory / (requestHash + ".tese")).string();
        result.tessControlSource = BuildTessellationStageSource(
            request,
            virtualSourceDirectory,
            shaderGlslRoot,
            true);
        result.tessEvaluationSource = BuildTessellationStageSource(
            request,
            virtualSourceDirectory,
            shaderGlslRoot,
            false);
    }
    return result;
}

} // namespace VL
