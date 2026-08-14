#pragma once

// File responsibility: Defines the reload participant contract for the UI
// Overlay's graphics pipeline pair. The overlay owns two blend variants of one
// shader pair and must swap them as one transaction.

#include <memory>
#include <functional>
#include <string>

#include <vulkan/vulkan.hpp>

#include "pipeline/graphicsShaderVariantArtifact.h"
#include "shader/reload/preparedRetiredResourcePackage.h"

namespace VL
{

struct UiOverlayPipelineReplacement
{
    vk::Pipeline straightAlphaPipeline;
    vk::PipelineCache straightAlphaPipelineCache;
    vk::Pipeline premultipliedAlphaPipeline;
    vk::PipelineCache premultipliedAlphaPipelineCache;
    // Destroys both pipelines/caches when the batch rolls back before commit.
    std::function<void()> release;
    // Starts disarmed and is activated only when the no-throw live swap is
    // about to make the current pipeline pair old.
    PreparedRetiredResourcePackage retirement;
};

class UiOverlayReloadParticipant
{
public:
    virtual ~UiOverlayReloadParticipant() = default;

    virtual std::string GetShaderName() const = 0;
    virtual const GraphicsShaderVariantArtifact&
    GetActiveArtifact() const = 0;

    // Builds both blend-mode pipelines from the candidate artifact without
    // mutating live state. Throws on pipeline creation failure.
    virtual UiOverlayPipelineReplacement PrepareReplacementPipelines(
        const GraphicsShaderVariantArtifact& candidate) = 0;

    // Swaps both pipelines after the caller has retained the old pair's
    // prepared retirement package. All fallible work is prevalidated before
    // the swap.
    virtual void CommitReplacement(
        GraphicsShaderVariantArtifact committedArtifact,
        UiOverlayPipelineReplacement&& replacement) noexcept = 0;
};

} // namespace VL
