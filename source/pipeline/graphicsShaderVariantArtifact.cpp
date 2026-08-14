#include "pipeline/graphicsShaderVariantArtifact.h"

#include <stdexcept>

namespace
{

const VL::ShaderBuildOutput& RequireBuildOutput(
    const VL::ShaderBuildArtifact& artifact,
    const std::string& role)
{
    const auto outputIt = artifact.outputs.find(role);
    if (outputIt == artifact.outputs.end())
    {
        throw std::runtime_error(
            "Shader build artifact is missing output role '" + role + "'");
    }
    return outputIt->second;
}

} // namespace

GraphicsShaderVariantArtifact BuildGraphicsShaderVariantArtifact(
    const VL::ShaderBuildArtifact& buildArtifact,
    const std::string& displayName)
{
    const VL::ShaderBuildOutput& runtimeVertex =
        RequireBuildOutput(buildArtifact, "runtimeVertex");
    const VL::ShaderBuildOutput& runtimeFragment =
        RequireBuildOutput(buildArtifact, "runtimeFragment");
    const VL::ShaderBuildOutput& debugVertex =
        RequireBuildOutput(buildArtifact, "debugVertex");
    const VL::ShaderBuildOutput& debugFragment =
        RequireBuildOutput(buildArtifact, "debugFragment");

    GraphicsShaderVariantArtifact artifact;
    artifact.logicalBuildId = buildArtifact.logicalBuildId;
    artifact.normalizedKey = buildArtifact.normalizedKey;
    artifact.displayName = displayName;
    artifact.sourceFingerprint = buildArtifact.sourceFingerprint;
    artifact.artifactGenerationKey =
        buildArtifact.artifactGenerationKey;
    artifact.vertexSpvPath = runtimeVertex.path.string();
    artifact.fragmentSpvPath = runtimeFragment.path.string();
    artifact.vertexDebugPath = debugVertex.path.string();
    artifact.fragmentDebugPath = debugFragment.path.string();
    artifact.vertexSpirv = runtimeVertex.spirv;
    artifact.fragmentSpirv = runtimeFragment.spirv;
    artifact.vertexDebugSpirv = debugVertex.spirv;
    artifact.fragmentDebugSpirv = debugFragment.spirv;
    artifact.shaderBindings = buildArtifact.shaderBindings;
    artifact.abiSignature = buildArtifact.abiSignature;
    return artifact;
}
