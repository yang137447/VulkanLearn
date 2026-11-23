#include <gtest/gtest.h>

#include "render/resource/terrainCompositedProducerSnapshot.h"
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

TerrainCompositedProducerEntry Entry(size_t index, uint32_t version = 1)
{
    TerrainCompositedProducerEntry entry;
    entry.compilePolicy = "test-compile-policy";
    entry.targetEnvironment = ShaderBuildTargetEnvironment;
    auto& closure = entry.closure;
    closure.logicalBuildId = "logical-" + std::to_string(index);
    closure.normalizedKey = "compute-" + std::to_string(index);
    closure.sourceFingerprint = "source-" + std::to_string(version);
    closure.artifactGenerationKey = "generation-" + std::to_string(version);
    const auto shader = index == 0 ? "terrain/terrainCompositedBakeWorld" :
        "terrain/terrainCompositedBakeWorldPadding";
    const auto path = std::string(shader) + ".comp";
    const auto bytes = "source " + std::to_string(version);
    entry.sourceBytes[path] = bytes;
    closure.primarySources.push_back({path, ContentHasher::HashString(bytes).ToHex()});
    entry.sourceBytes["shared.glsl"] = "shared";
    closure.dependencies.push_back({"shared.glsl", ContentHasher::HashString("shared").ToHex(), {}, 1});
    closure.abiSignature.workgroupSize = {64, 1, 1, true};
    for (const auto& role : {"runtimeCompute", "debugCompute"})
    {
        ShaderBuildOutput output;
        output.path = std::string(shader) + role + ".spv";
        output.spirv = {0x07230203, version, static_cast<uint32_t>(index)};
        output.digest = ContentHasher::HashBytes(output.spirv.data(),
            output.spirv.size() * sizeof(uint32_t)).ToHex();
        closure.outputs.emplace(role, std::move(output));
    }
    entry.artifact = BuildComputeShaderArtifact(closure, shader);
    return entry;
}

auto Initial()
{
    return TerrainCompositedProducerSnapshot::Create({Entry(0), Entry(1)});
}

}

TEST(TerrainCompositedProducerSnapshot, PrepareDoesNotOfferOrPublish)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    auto prepared = state.Prepare(0, Entry(0, 2), {});
    EXPECT_EQ(state.CaptureProducer(), initial);
    EXPECT_FALSE(state.PendingProducer());
    state.Commit(prepared);
    EXPECT_TRUE(state.PendingProducer());
    EXPECT_EQ(state.CaptureProducer(), initial);
}

TEST(TerrainCompositedProducerSnapshot, PartialOneEntryRetainsSelectedOldCompanion)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    auto prepared = state.Prepare(1, Entry(1, 2), {});
    state.Commit(prepared);
    auto pending = state.PendingProducer();
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->Pipelines()[0].artifact.runtimeSpirv,
        initial->Pipelines()[0].artifact.runtimeSpirv);
    EXPECT_NE(pending->Identity(), initial->Identity());
    EXPECT_TRUE(state.PublishProducer(pending));
    EXPECT_EQ(state.CaptureProducer(), pending);
}

TEST(TerrainCompositedProducerSnapshot, BothEntriesOfferOnceAfterLastCommit)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    auto world = state.Prepare(0, Entry(0, 2), {});
    auto padding = state.Prepare(1, Entry(1, 3), {});
    ASSERT_EQ(world, padding);
    state.Commit(world);
    EXPECT_FALSE(state.PendingProducer());
    state.Commit(padding);
    ASSERT_TRUE(state.PendingProducer());
    EXPECT_EQ(state.PendingProducer()->Pipelines()[0].artifact.runtimeSpirv[1], 2u);
    EXPECT_EQ(state.PendingProducer()->Pipelines()[1].artifact.runtimeSpirv[1], 3u);
    EXPECT_EQ(state.CaptureProducer(), initial);
}

TEST(TerrainCompositedProducerSnapshot, FailedPolicyPreservesCurrentAndPending)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    auto accepted = state.Prepare(0, Entry(0, 2), {});
    state.Commit(accepted);
    auto pending = state.PendingProducer();
    auto wrong = Entry(1, 3);
    wrong.compilePolicy = "different-policy";
    EXPECT_THROW(state.Prepare(1, wrong, {}), std::runtime_error);
    EXPECT_EQ(state.CaptureProducer(), initial);
    EXPECT_EQ(state.PendingProducer(), pending);
}

TEST(TerrainCompositedProducerSnapshot, AbiMismatchPreservesOld)
{
    TerrainCompositedProducerReloadState state(Initial());
    auto changed = Entry(0, 2);
    changed.closure.abiSignature.workgroupSize.x = 32;
    changed.artifact.abiSignature = changed.closure.abiSignature;
    EXPECT_THROW(state.Prepare(0, changed, {}), std::runtime_error);
    EXPECT_FALSE(state.PendingProducer());
}

TEST(TerrainCompositedProducerSnapshot, DescriptorPreparationFailureRollsBack)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    {
        auto first = state.Prepare(0, Entry(0, 2), {});
        EXPECT_THROW(state.Prepare(1, Entry(1, 2), [](const auto&) ->
            TerrainCompositedProducerSnapshot::Descriptors {
                throw std::runtime_error("allocation failure");
            }), std::runtime_error);
    }
    EXPECT_EQ(state.CaptureProducer(), initial);
    EXPECT_FALSE(state.PendingProducer());
    auto retry = state.Prepare(1, Entry(1, 3), {});
    state.Commit(retry);
    EXPECT_EQ(state.PendingProducer()->Pipelines()[0].artifact.runtimeSpirv[1], 1u);
}

TEST(TerrainCompositedProducerSnapshot, OldSnapshotOwnsResourcesUntilTaskFenceRelease)
{
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = resource;
    TerrainCompositedProducerSnapshot::Descriptors descriptors;
    descriptors.owner = resource;
    auto initial = TerrainCompositedProducerSnapshot::Create(
        {Entry(0), Entry(1)}, std::move(descriptors));
    resource.reset();
    TerrainCompositedProducerReloadState state(initial);
    auto inFlightTask = initial;
    auto prepared = state.Prepare(0, Entry(0, 2), {});
    state.Commit(prepared);
    ASSERT_TRUE(state.PublishProducer(state.PendingProducer()));
    prepared.reset();
    initial.reset();
    EXPECT_FALSE(lifetime.expired());
    inFlightTask.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST(TerrainCompositedProducerSnapshot, DiscardAndStalePublishPreserveCurrent)
{
    auto initial = Initial();
    TerrainCompositedProducerReloadState state(initial);
    auto prepared = state.Prepare(0, Entry(0, 2), {});
    state.Commit(prepared);
    auto pending = state.PendingProducer();
    state.DiscardPendingProducer(pending);
    EXPECT_FALSE(state.PublishProducer(pending));
    EXPECT_EQ(state.CaptureProducer(), initial);
}

TEST(TerrainCompositedProducerSnapshot, NextOfferUsesPendingCompanion)
{
    TerrainCompositedProducerReloadState state(Initial());
    auto first = state.Prepare(0, Entry(0, 2), {});
    state.Commit(first);
    auto oldOffer = state.PendingProducer();
    auto second = state.Prepare(1, Entry(1, 3), {});
    state.Commit(second);
    EXPECT_FALSE(state.PublishProducer(oldOffer));
    EXPECT_EQ(state.PendingProducer()->Pipelines()[0].artifact.runtimeSpirv[1], 2u);
}

TEST(TerrainCompositedProducerSnapshot, ActualRuntimeAndDebugSpvDigestsAreChecked)
{
    for (const auto& role : {"runtimeCompute", "debugCompute"})
    {
        auto changed = Entry(0);
        changed.closure.outputs.at(role).spirv[1] = 5;
        EXPECT_THROW(TerrainCompositedProducerSnapshot::Create({changed, Entry(1)}),
            std::runtime_error);
    }
}

TEST(TerrainCompositedProducerSnapshot, FrozenSourceBytesAreChecked)
{
    auto changed = Entry(0);
    changed.sourceBytes["shared.glsl"] = "modified after capture";
    EXPECT_THROW(TerrainCompositedProducerSnapshot::Create({changed, Entry(1)}),
        std::runtime_error);
}

TEST(TerrainCompositedProducerSnapshot, SharedIncludeChangesNeedConsistentPair)
{
    TerrainCompositedProducerReloadState state(Initial());
    auto world = Entry(0, 2);
    auto padding = Entry(1, 2);
    for (auto* entry : {&world, &padding})
    {
        entry->sourceBytes["shared.glsl"] = "shared new";
        entry->closure.dependencies[0].digest = ContentHasher::HashString("shared new").ToHex();
    }
    auto first = state.Prepare(0, world, {});
    auto second = state.Prepare(1, padding, {});
    state.Commit(first);
    EXPECT_FALSE(state.PendingProducer());
    state.Commit(second);
    EXPECT_TRUE(state.PendingProducer());
}

TEST(TerrainCompositedProducerSnapshot, MismatchedSharedClosureIsNeverOffered)
{
    TerrainCompositedProducerReloadState state(Initial());
    auto world = Entry(0, 2);
    world.sourceBytes["shared.glsl"] = "shared new";
    world.closure.dependencies[0].digest = ContentHasher::HashString("shared new").ToHex();
    auto prepared = state.Prepare(0, world, {});
    state.Commit(prepared);
    EXPECT_FALSE(state.PendingProducer());
    EXPECT_EQ(state.CaptureProducer()->Pipelines()[0].artifact.runtimeSpirv[1], 1u);
}

}
