#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shader/reload/computeShaderArtifact.h"

class ComputePipeline;

namespace VL
{

struct TerrainCompositedProducerEntry
{
    ComputeShaderArtifact artifact;
    ShaderBuildArtifact closure;
    std::string compilePolicy;
    std::string targetEnvironment;
    std::shared_ptr<ComputePipeline> pipeline;
    std::map<std::string, std::string> sourceBytes;
};

class TerrainCompositedProducerSnapshot
{
public:
    using Entries = std::array<TerrainCompositedProducerEntry, 2>;
    struct Descriptors
    {
        std::shared_ptr<void> owner;
        std::vector<vk::DescriptorSet> world;
        std::vector<vk::DescriptorSet> padding;
    };

    static std::shared_ptr<const TerrainCompositedProducerSnapshot> Create(
        Entries entries, Descriptors descriptors = {});
    const Entries& Pipelines() const noexcept { return entries; }
    const Descriptors& DescriptorResources() const noexcept { return descriptors; }
    const std::string& Identity() const noexcept { return identity; }
    static void ValidateReplacement(
        const TerrainCompositedProducerEntry& active,
        const TerrainCompositedProducerEntry& candidate);

private:
    friend class TerrainCompositedProducerReloadState;
    TerrainCompositedProducerSnapshot(Entries entries, Descriptors descriptors, bool preparing = false);
    bool SharedClosureConsistent() const noexcept;
    Entries entries;
    Descriptors descriptors;
    std::string identity;
};

class TerrainCompositedProducerReloadState
{
public:
    using Snapshot = std::shared_ptr<const TerrainCompositedProducerSnapshot>;
    using DescriptorBuilder = std::function<TerrainCompositedProducerSnapshot::Descriptors(
        const TerrainCompositedProducerSnapshot::Entries&)>;
    struct Prepared;

    explicit TerrainCompositedProducerReloadState(Snapshot initial);
    Snapshot CaptureProducer() const noexcept { return current; }
    Snapshot PendingProducer() const noexcept { return pending; }
    Snapshot SelectedProducer() const noexcept { return pending ? pending : current; }
    std::shared_ptr<Prepared> Prepare(
        size_t entry, TerrainCompositedProducerEntry candidate,
        const DescriptorBuilder& buildDescriptors);
    void Commit(const std::shared_ptr<Prepared>& prepared) noexcept;
    bool PublishProducer(const Snapshot& selected) noexcept;
    void DiscardPendingProducer(const Snapshot& selected) noexcept;

private:
    Snapshot current;
    Snapshot pending;
    std::weak_ptr<Prepared> preparing;
};

}
