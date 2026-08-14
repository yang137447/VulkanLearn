#include "shader/reload/computeShaderArtifact.h"

#include <stdexcept>

ComputeShaderArtifact BuildComputeShaderArtifact(
    const VL::ShaderBuildArtifact& buildArtifact,
    const std::string& shaderName)
{
    const auto runtimeIt = buildArtifact.outputs.find("runtimeCompute");
    const auto debugIt = buildArtifact.outputs.find("debugCompute");
    if (runtimeIt == buildArtifact.outputs.end() ||
        debugIt == buildArtifact.outputs.end())
    {
        throw std::runtime_error(
            "Compute shader build artifact is missing its runtime or debug output");
    }

    ComputeShaderArtifact artifact;
    artifact.logicalBuildId = buildArtifact.logicalBuildId;
    artifact.normalizedKey = buildArtifact.normalizedKey;
    artifact.shaderName = shaderName;
    artifact.sourceFingerprint = buildArtifact.sourceFingerprint;
    artifact.artifactGenerationKey = buildArtifact.artifactGenerationKey;
    artifact.runtimeSpvPath = runtimeIt->second.path.string();
    artifact.debugSpvPath = debugIt->second.path.string();
    artifact.runtimeSpirv = runtimeIt->second.spirv;
    artifact.debugSpirv = debugIt->second.spirv;
    artifact.shaderBindings = buildArtifact.shaderBindings;
    artifact.abiSignature = buildArtifact.abiSignature;
    return artifact;
}
