#include "terrainTesCapturePipeline.h"
#include "terrainTesDiagnostic.h"
#include "material/compiler/materialShaderComposer.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "shaderCompiler.h"
#include <algorithm>
#include <stdexcept>

namespace VL
{
ShaderBuildArtifact CompileTerrainTesCaptureVariant(ShaderCompiler& compiler,
    MaterialShaderCompileRequest actualMaterialRequest, const std::string& instrumentedTesSource)
{
    if (actualMaterialRequest.pass != MaterialPass::Base ||
        actualMaterialRequest.shaderVariantKey.shaderName != "terrain" ||
        actualMaterialRequest.shaderVariantKey.renderMode != RenderMode::Opaque ||
        !HasMaterialMacroValue(actualMaterialRequest.shaderVariantKey.macros, "VL_TERRAIN_TESSELLATION", "1"))
        throw std::runtime_error("TES capture only accepts the actual normal opaque terrain TES1 request");
    actualMaterialRequest.shaderVariantKey.macros.push_back("VL_TERRAIN_TEST_CAPTURE=1");
    actualMaterialRequest.shaderVariantKey.macros = NormalizeMaterialMacros(actualMaterialRequest.shaderVariantKey.macros);
    auto composed = MaterialShaderComposer::Compose(actualMaterialRequest);
    if (instrumentedTesSource.find("TerrainTestTesCapture") == std::string::npos ||
        instrumentedTesSource.find("gl_TessCoord") == std::string::npos)
        throw std::runtime_error("TES capture source lacks actual stage instrumentation");
    composed.tessEvaluationSource = instrumentedTesSource;
    auto request = compiler.CreateMaterialGraphicsBuildRequest(actualMaterialRequest, composed);
    compiler.FreezeSourceSnapshot(request);
    auto artifact = compiler.CompileCandidate(request);
    const auto current = compiler.ValidateCandidateSourcesStillCurrent(request, artifact);
    if (!current.current)
        throw std::runtime_error("TES diagnostic source changed while compiling: " + current.reason);
    return artifact;
}

GraphicsPipelineLayoutDesc TerrainTesCaptureLayout(const PipelineBase& originalPipeline,
    const GraphicsShaderVariantArtifact& diagnosticArtifact)
{
    if (!originalPipeline.UsesTessellation() || !diagnosticArtifact.hasTessellation)
        throw std::runtime_error("TES capture requires original and diagnostic patch topology");
    GraphicsPipelineLayoutDesc layout;
    for (uint32_t set = 0; set < MAX_DESCRIPTOR_SETS; ++set)
        layout.overrideSets[set] = true;
    for (const auto& binding : originalPipeline.GetDescriptorLayoutBindings())
    {
        if (binding.set >= MAX_DESCRIPTOR_SETS)
            throw std::runtime_error("Regular terrain pipeline already has a nonstandard set");
        layout.setBindings[binding.set].push_back(binding);
    }
    uint32_t captureBindings = 0;
    for (const auto& binding : diagnosticArtifact.shaderBindings)
    {
        if (binding.set == 4)
        {
            if (binding.binding != 0 || binding.descriptorCount != 1 ||
                binding.type != vk::DescriptorType::eStorageBuffer ||
                binding.stageFlags != vk::ShaderStageFlagBits::eTessellationEvaluation ||
                binding.name != "terrainTestTesCapture")
                throw std::runtime_error("Diagnostic set4 differs from actual TES capture ABI");
            ++captureBindings;
            continue;
        }
        if (binding.set >= MAX_DESCRIPTOR_SETS)
            throw std::runtime_error("Unexpected diagnostic descriptor set");
        const auto& regular = layout.setBindings[binding.set];
        const auto found = std::find_if(regular.begin(), regular.end(),
            [&](const ShaderBinding& item) { return item.binding == binding.binding; });
        if (found == regular.end() || !HasSameShaderBindingLayout(*found, binding) ||
            (binding.stageFlags & ~found->stageFlags) != vk::ShaderStageFlags{})
            throw std::runtime_error("TES diagnostic cannot reuse actual sets0..3");
    }
    if (captureBindings != 1)
        throw std::runtime_error("TES diagnostic storage binding missing or duplicated");
    return layout;
}

#if !defined(VL_TERRAIN_CAPTURE_CPU_TEST_ONLY)
std::shared_ptr<PipelineBase> CreateTerrainTesCapturePipeline(PipelineFactory& factory,
    vk::RenderPass& actualRenderPass, const PassPipelineContractKey& actualPassContract,
    const PipelineBase& originalPipeline, const GraphicsShaderVariantArtifact& diagnosticArtifact,
    vk::CullModeFlags actualCullMode, GraphicsPipelineBlendMode actualBlendMode)
{
    if (!TerrainTesDiagnostic::Requested() || !TerrainTesDiagnostic::DeviceFeatureEnabled())
        throw std::runtime_error("Diagnostic pipeline requires explicit TES capture enable");
    const auto layout = TerrainTesCaptureLayout(originalPipeline, diagnosticArtifact);
    PipelineFactory::GraphicsCandidateState candidate;
    return factory.CreateGraphicsPipelineCandidate(candidate, &actualRenderPass, actualPassContract,
        diagnosticArtifact, actualCullMode, actualBlendMode, layout);
}
#endif
}
