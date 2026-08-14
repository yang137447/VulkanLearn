#include "material/compiler/materialShaderComposer.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>

#include "commonFunction.h"

namespace VL
{
namespace
{

std::string BuildFeatureDefines(const MaterialFeatureKey& features)
{
    std::ostringstream stream;
    stream << "#define MATERIAL_WRITES_EVERY_PIXEL " << features.writesEveryPixel << "\n"
           << "#define MATERIAL_USES_OPACITY_MASK " << features.usesOpacityMask << "\n"
           << "#define MATERIAL_MODIFIES_MESH_POSITION " << features.modifiesMeshPosition << "\n"
           << "#define MATERIAL_TWO_SIDED " << features.twoSided << "\n\n";
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
    stream << BuildFeatureDefines(request.features);

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
    else if (request.pass == MaterialPass::Base || request.features.usesOpacityMask)
    {
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, "engine/materialContext.glsl");
        AppendInclude(stream, virtualSourceDirectory, shaderGlslRoot, "engine/materialSurface.glsl");
        AppendInclude(
            stream,
            virtualSourceDirectory,
            shaderGlslRoot,
            request.source.parameterIncludePath,
            request.source.parameterIncludeBytes.has_value());
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

} // namespace

ComposedMaterialShaderSource MaterialShaderComposer::Compose(
    const MaterialShaderCompileRequest& request)
{
    if (request.vertexFactoryKey != "StaticMesh")
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
    return result;
}

} // namespace VL
