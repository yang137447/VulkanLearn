#include "pipeline/graphicsShaderVariantArtifact.h"

#include <stdexcept>

#include "shaderCompilerInternal.h"

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
        RequireBuildOutput(
            buildArtifact,
            VL::ShaderCompilerDetail::RuntimeVertexRole);
    const VL::ShaderBuildOutput& runtimeFragment =
        RequireBuildOutput(
            buildArtifact,
            VL::ShaderCompilerDetail::RuntimeFragmentRole);
    const VL::ShaderBuildOutput& debugVertex =
        RequireBuildOutput(
            buildArtifact,
            VL::ShaderCompilerDetail::DebugVertexRole);
    const VL::ShaderBuildOutput& debugFragment =
        RequireBuildOutput(
            buildArtifact,
            VL::ShaderCompilerDetail::DebugFragmentRole);

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

    // tesc/tese 是可选阶段：只有 runtime 两个角色同时存在时才构成完整细分变体。
    // 只出现其中一个时不猜测、也不抛错，按非细分变体处理。
    const auto runtimeTessControlIt = buildArtifact.outputs.find(
        VL::ShaderCompilerDetail::RuntimeTessControlRole);
    const auto runtimeTessEvaluationIt = buildArtifact.outputs.find(
        VL::ShaderCompilerDetail::RuntimeTessEvaluationRole);
    if (runtimeTessControlIt != buildArtifact.outputs.end() &&
        runtimeTessEvaluationIt != buildArtifact.outputs.end())
    {
        artifact.hasTessellation = true;
        artifact.tessControlSpvPath =
            runtimeTessControlIt->second.path.string();
        artifact.tessEvaluationSpvPath =
            runtimeTessEvaluationIt->second.path.string();
        artifact.tessControlSpirv =
            runtimeTessControlIt->second.spirv;
        artifact.tessEvaluationSpirv =
            runtimeTessEvaluationIt->second.spirv;
    }

    // debug 角色缺失是正常路径（例如未生成调试反射），不能按必需输出处理。
    const auto debugTessControlIt = buildArtifact.outputs.find(
        VL::ShaderCompilerDetail::DebugTessControlRole);
    const auto debugTessEvaluationIt = buildArtifact.outputs.find(
        VL::ShaderCompilerDetail::DebugTessEvaluationRole);
    if (debugTessControlIt != buildArtifact.outputs.end() &&
        debugTessEvaluationIt != buildArtifact.outputs.end())
    {
        artifact.tessControlDebugPath =
            debugTessControlIt->second.path.string();
        artifact.tessEvaluationDebugPath =
            debugTessEvaluationIt->second.path.string();
        artifact.tessControlDebugSpirv =
            debugTessControlIt->second.spirv;
        artifact.tessEvaluationDebugSpirv =
            debugTessEvaluationIt->second.spirv;
    }

    artifact.shaderBindings = buildArtifact.shaderBindings;
    artifact.abiSignature = buildArtifact.abiSignature;
    return artifact;
}
