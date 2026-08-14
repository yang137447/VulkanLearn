#pragma once

// File responsibility: Defines the hot-reload participant contract for owners
// that hold a compute pipeline and its descriptor resources across frames.

#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "shader/reload/computeShaderArtifact.h"
#include "shader/reload/preparedRetiredResourcePackage.h"

class ComputePipeline;
class PipelineFactory;

namespace VL
{

// Newly allocated descriptor pool/sets built against a candidate pipeline's
// layout objects. Prepared before any live swap so the commit remains
// all-or-nothing.
struct ComputeDescriptorReplacement
{
    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> descriptorSets;
    // Destroys the prepared pool when the batch rolls back before commit.
    std::function<void()> release;
    // Starts disarmed so every prepare/rollback failure leaves the still-live
    // old pool untouched. The coordinator activates it immediately before the
    // guaranteed no-throw live swap.
    PreparedRetiredResourcePackage retirement;
};

class ComputePipelineReloadParticipant
{
public:
    virtual ~ComputePipelineReloadParticipant() = default;

    virtual std::string GetShaderName() const = 0;
    virtual std::shared_ptr<ComputePipeline> GetActivePipeline() const = 0;
    virtual const ComputeShaderArtifact& GetActiveArtifact() const = 0;

    // Default V1 policy: only ABI-identical candidates are accepted. Throws a
    // precise diff when descriptors, push constants, or workgroup size change.
    virtual void ValidateCandidateAbi(
        const ComputeShaderArtifact& candidate) const;

    // Prepares replacement descriptor pool/sets for a candidate pipeline
    // without mutating live state. Throws on allocation/validation failure.
    virtual ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact& candidate,
        const std::shared_ptr<ComputePipeline>& replacementPipeline) const = 0;

    // Swaps the live pipeline and descriptor resources after the caller has
    // already retained the old pipeline and descriptor package for epoch
    // retirement. All fallible work is prevalidated before the swap begins.
    virtual void CommitReplacement(
        ComputeShaderArtifact committedArtifact,
        std::shared_ptr<ComputePipeline> replacementPipeline,
        ComputeDescriptorReplacement&& replacementDescriptors) noexcept = 0;
};

} // namespace VL
