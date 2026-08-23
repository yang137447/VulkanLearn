#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "render/eye/eyeAssets.h"

class ComputePipeline;
class PipelineFactory;
class Texture;

namespace VL
{
class RendererBackendVulkan;

struct EyeComputePipelineCandidate
{
    std::shared_ptr<ComputePipeline> pipeline;
    std::string artifactGenerationKey;
};

struct EyeLutReadbackReport
{
    bool executed = false;
    bool allFinite = true;
    size_t sampleCount = 0;
    size_t validDomainSampleCount = 0;
    float maxAbsoluteGainError = 0.0f;
    float meanAbsoluteGainError = 0.0f;
    float maxAbsoluteTransmissionError = 0.0f;
    float maxAbsoluteCoverageError = 0.0f;
    float maxAbsoluteJacobianError = 0.0f;
    float validDomainGainAverage = 0.0f;
    float normalizationError = 0.0f;
};

EyeComputePipelineCandidate CreateEyeComputePipelineCandidate(
    PipelineFactory& pipelineFactory);

// 生产 texel 只由 eyeCausticLut.comp 生成；CPU 仅提交 profile 参数和同步命令。
std::shared_ptr<Texture> GenerateEyeCausticLutTexture(
    RendererBackendVulkan& rendererBackend,
    const EyeComputePipelineCandidate& candidate,
    const std::vector<EyeProfileAsset>& profiles,
    std::string_view sourceIdentity,
    EyeLutReadbackReport* readbackReport = nullptr);

} // namespace VL
