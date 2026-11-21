#include "render/resource/terrainCompositedProducerSnapshot.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

void AddEntry(CanonicalFieldHasher& hash, const TerrainCompositedProducerEntry& entry)
{
    const auto& artifact = entry.artifact;
    const auto& closure = entry.closure;
    if (entry.compilePolicy.empty() || entry.targetEnvironment != ShaderBuildTargetEnvironment ||
        artifact.logicalBuildId.empty() || artifact.sourceFingerprint.empty() ||
        artifact.artifactGenerationKey.empty() || closure.primarySources.size() != 1 ||
        artifact.logicalBuildId != closure.logicalBuildId ||
        artifact.normalizedKey != closure.normalizedKey ||
        artifact.sourceFingerprint != closure.sourceFingerprint ||
        artifact.artifactGenerationKey != closure.artifactGenerationKey ||
        artifact.abiSignature != closure.abiSignature)
        throw std::runtime_error("Incomplete or inconsistent terrain producer closure");
    hash.AddString("shader", artifact.shaderName);
    hash.AddString("policy", entry.compilePolicy);
    hash.AddString("target", entry.targetEnvironment);
    hash.AddString("logical", artifact.logicalBuildId);
    hash.AddString("key", artifact.normalizedKey);
    hash.AddString("source", artifact.sourceFingerprint);
    hash.AddString("generation", artifact.artifactGenerationKey);
    hash.AddString("abi", artifact.abiSignature.GetFingerprint());
    auto sources = closure.primarySources;
    const auto validateSource = [&](const std::string& path, const std::string& digest) {
        const auto found = entry.sourceBytes.find(path);
        if (found == entry.sourceBytes.end() || ContentHasher::HashString(found->second).ToHex() != digest)
            throw std::runtime_error("Terrain producer frozen source digest mismatch");
    };
    std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
        return left.identity < right.identity;
    });
    for (const auto& source : sources)
    {
        validateSource(source.identity, source.digest);
        hash.AddString("primary", source.identity);
        hash.AddDigest("digest", ContentDigest::FromHex(source.digest));
    }
    auto dependencies = closure.dependencies;
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    for (const auto& dependency : dependencies)
    {
        validateSource(dependency.path, dependency.digest);
        hash.AddString("include", dependency.path);
        hash.AddDigest("digest", ContentDigest::FromHex(dependency.digest));
    }
    for (const auto& role : {"runtimeCompute", "debugCompute"})
    {
        const auto found = closure.outputs.find(role);
        const auto& spirv = std::string(role) == "runtimeCompute" ?
            artifact.runtimeSpirv : artifact.debugSpirv;
        if (found == closure.outputs.end() || spirv.empty() || found->second.spirv != spirv)
            throw std::runtime_error("Terrain producer SPV closure mismatch");
        const auto digest = ContentHasher::HashBytes(spirv.data(), spirv.size() * sizeof(uint32_t));
        if (digest.ToHex() != found->second.digest)
            throw std::runtime_error("Terrain producer SPV digest mismatch");
        hash.AddString("role", role);
        hash.AddDigest("spv", digest);
    }
}

}

TerrainCompositedProducerSnapshot::TerrainCompositedProducerSnapshot(
    Entries selectedEntries, Descriptors selectedDescriptors, bool preparing)
    : entries(std::move(selectedEntries)), descriptors(std::move(selectedDescriptors))
{
    CanonicalFieldHasher hash("terrain-composited-producer-pair-v1");
    for (size_t index = 0; index < entries.size(); ++index)
    {
        const std::string expected = index == 0 ?
            "terrain/terrainCompositedBakeWorld" : "terrain/terrainCompositedBakeWorldPadding";
        if (entries[index].artifact.shaderName != expected)
            throw std::runtime_error("Terrain producer entry order/name mismatch");
        AddEntry(hash, entries[index]);
    }
    if (entries[0].compilePolicy != entries[1].compilePolicy ||
        entries[0].targetEnvironment != entries[1].targetEnvironment)
        throw std::runtime_error("Terrain producer pair policy mismatch");
    if (!preparing && !SharedClosureConsistent())
        throw std::runtime_error("Terrain producer pair shared include mismatch");
    identity = hash.Finalize().ToHex();
}

bool TerrainCompositedProducerSnapshot::SharedClosureConsistent() const noexcept
{
    for (const auto& left : entries[0].closure.dependencies)
        for (const auto& right : entries[1].closure.dependencies)
            if (left.path == right.path && left.digest != right.digest)
                return false;
    return true;
}

std::shared_ptr<const TerrainCompositedProducerSnapshot>
TerrainCompositedProducerSnapshot::Create(Entries entries, Descriptors descriptors)
{
    return std::shared_ptr<const TerrainCompositedProducerSnapshot>(
        new TerrainCompositedProducerSnapshot(std::move(entries), std::move(descriptors)));
}

void TerrainCompositedProducerSnapshot::ValidateReplacement(
    const TerrainCompositedProducerEntry& active, const TerrainCompositedProducerEntry& candidate)
{
    if (active.compilePolicy != candidate.compilePolicy ||
        active.targetEnvironment != candidate.targetEnvironment ||
        active.artifact.shaderName != candidate.artifact.shaderName ||
        active.artifact.logicalBuildId != candidate.artifact.logicalBuildId ||
        active.artifact.normalizedKey != candidate.artifact.normalizedKey ||
        active.artifact.abiSignature != candidate.artifact.abiSignature)
        throw std::runtime_error("Terrain producer reload ABI/policy mismatch");
}

struct TerrainCompositedProducerReloadState::Prepared
{
    Snapshot base;
    Snapshot candidate;
    std::array<bool, 2> entries{};
    size_t count = 0;
    size_t committed = 0;
};

TerrainCompositedProducerReloadState::TerrainCompositedProducerReloadState(Snapshot initial)
    : current(std::move(initial))
{
    if (!current)
        throw std::invalid_argument("Terrain producer requires an initial pair");
}

std::shared_ptr<TerrainCompositedProducerReloadState::Prepared>
TerrainCompositedProducerReloadState::Prepare(
    size_t entry, TerrainCompositedProducerEntry candidate, const DescriptorBuilder& buildDescriptors)
{
    if (entry >= 2)
        throw std::out_of_range("Terrain producer entry");
    auto draft = preparing.lock();
    const auto selected = SelectedProducer();
    if (!draft || draft->committed != 0 || draft->base != selected)
    {
        draft = std::make_shared<Prepared>();
        draft->base = selected;
        draft->candidate = selected;
    }
    if (draft->entries[entry])
        throw std::logic_error("Duplicate terrain producer entry in reload batch");
    auto entries = draft->candidate->Pipelines();
    TerrainCompositedProducerSnapshot::ValidateReplacement(
        draft->base->Pipelines()[entry], candidate);
    entries[entry] = std::move(candidate);
    const auto validated = std::shared_ptr<const TerrainCompositedProducerSnapshot>(
        new TerrainCompositedProducerSnapshot(entries, {}, true));
    auto descriptors = buildDescriptors ? buildDescriptors(entries) :
        TerrainCompositedProducerSnapshot::Descriptors{};
    auto prepared = std::shared_ptr<const TerrainCompositedProducerSnapshot>(
        new TerrainCompositedProducerSnapshot(std::move(entries), std::move(descriptors), true));
    draft->candidate = std::move(prepared);
    draft->entries[entry] = true;
    ++draft->count;
    preparing = draft;
    return draft;
}

void TerrainCompositedProducerReloadState::Commit(const std::shared_ptr<Prepared>& prepared) noexcept
{
    if (!prepared || prepared->base != SelectedProducer() || prepared->committed >= prepared->count)
        return;
    ++prepared->committed;
    if (prepared->committed == prepared->count && prepared->candidate->SharedClosureConsistent())
        pending = prepared->candidate;
}

bool TerrainCompositedProducerReloadState::PublishProducer(const Snapshot& selected) noexcept
{
    if (!selected || selected != pending)
        return false;
    current = selected;
    pending.reset();
    return true;
}

void TerrainCompositedProducerReloadState::DiscardPendingProducer(const Snapshot& selected) noexcept
{
    if (pending == selected)
        pending.reset();
}

}
