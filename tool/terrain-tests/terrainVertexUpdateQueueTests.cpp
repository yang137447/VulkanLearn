#include <gtest/gtest.h>
#include <stdexcept>

#include "terrain/terrainVertexUpdateQueue.h"
#include "vertexDataStruct.h"

namespace
{
std::shared_ptr<const std::vector<Vertex>> Vertices(float height)
{
    std::vector<Vertex> vertices(1);
    vertices[0].position = Eigen::Vector3f(0, height, 0);
    return std::make_shared<const std::vector<Vertex>>(std::move(vertices));
}
}

TEST(TerrainVertexUpdateQueue, PreparationIsNotPublishedAndFailedPreparationPreservesOldValues)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    auto prepared = queue.Prepare(1, {{"chunk", Vertices(2)}});
    int calls = 0;
    auto consume = [&](const std::string&, const std::vector<Vertex>& vertices) {
        ++calls;
        EXPECT_FLOAT_EQ(vertices[0].position.y(), 2);
        return true;
    };
    queue.Replay(1, 0, consume);
    EXPECT_EQ(calls, 0);
    queue.Commit(std::move(prepared));
    EXPECT_THROW(queue.Prepare(1, {{"unknown", Vertices(9)}}), std::invalid_argument);
    queue.Replay(1, 0, consume);
    EXPECT_EQ(calls, 1);
}

TEST(TerrainVertexUpdateQueue, EachActualSlotConsumesOnceAndHiddenChunksStayPending)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    int calls = 0;
    queue.Replay(1, 0, [&](const auto&, const auto&) { ++calls; return false; });
    EXPECT_EQ(calls, 1);
    for (uint32_t slot = 0; slot < 3; ++slot)
    {
        queue.Replay(1, slot, [&](const auto&, const auto&) { ++calls; return true; });
        queue.Replay(1, slot, [&](const auto&, const auto&) { ++calls; return true; });
    }
    EXPECT_EQ(calls, 4);
    queue.Replay(2, 0, [&](const auto&, const auto&) { ++calls; return true; });
    queue.Replay(1, 3, [&](const auto&, const auto&) { ++calls; return true; });
    EXPECT_EQ(calls, 4);
}

TEST(TerrainVertexUpdateQueue, LateOldSlotConfirmationCannotConsumeNewRevision)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    queue.Replay(1, 0, [&](const auto&, const auto& vertices) {
        EXPECT_FLOAT_EQ(vertices[0].position.y(), 1);
        queue.Commit(queue.Prepare(1, {{"chunk", Vertices(2)}}));
        return true;
    });
    int calls = 0;
    queue.Replay(1, 0, [&](const auto&, const auto& vertices) {
        EXPECT_FLOAT_EQ(vertices[0].position.y(), 2);
        ++calls;
        return true;
    });
    EXPECT_EQ(calls, 1);
}

TEST(TerrainVertexUpdateQueue, InvalidWorldDuplicateChunkAndNullPayloadAreRejected)
{
    VL::TerrainVertexUpdateQueue queue;
    EXPECT_THROW(queue.RegisterDynamicChunk(""), std::invalid_argument);
    queue.RegisterDynamicChunk("chunk");
    EXPECT_THROW(queue.Prepare(0, {{"chunk", Vertices(1)}}), std::invalid_argument);
    EXPECT_THROW(queue.Prepare(1, {{"chunk", {}}}), std::invalid_argument);
    EXPECT_THROW(queue.Prepare(1, {{"chunk", Vertices(1)}, {"chunk", Vertices(2)}}),
        std::invalid_argument);
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    EXPECT_THROW(queue.Prepare(2, {{"chunk", Vertices(2)}}), std::invalid_argument);
}

TEST(TerrainVertexUpdateQueue, ResidencyRevisionTracksStructuralChangesOnly)
{
    VL::TerrainVertexUpdateQueue queue;
    EXPECT_EQ(queue.ResidencyRevision(), 0u);
    queue.RegisterDynamicChunk("chunk");
    EXPECT_EQ(queue.ResidencyRevision(), 1u);
    queue.RegisterDynamicChunk("chunk");
    queue.UnregisterDynamicChunk("missing");
    EXPECT_EQ(queue.ResidencyRevision(), 1u);
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    queue.Replay(1, 0, [](const auto&, const auto&) { return true; });
    EXPECT_EQ(queue.ResidencyRevision(), 1u);
    queue.UnregisterDynamicChunk("chunk");
    EXPECT_EQ(queue.ResidencyRevision(), 2u);
    EXPECT_FALSE(queue.HasDynamicChunk("chunk"));
    queue.RegisterDynamicChunk("chunk");
    EXPECT_EQ(queue.ResidencyRevision(), 3u);
}

TEST(TerrainVertexUpdateQueue, UnregisterDropsPayloadAndRejectsOldPreparedBatch)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    auto vertices = Vertices(1);
    std::weak_ptr<const std::vector<Vertex>> retained = vertices;
    queue.Commit(queue.Prepare(1, {{"chunk", vertices}}));
    vertices.reset();
    queue.UnregisterDynamicChunk("chunk");
    EXPECT_TRUE(retained.expired());
    queue.Replay(1, 0, [](const auto&, const auto&) { ADD_FAILURE(); return true; });
    queue.RegisterDynamicChunk("chunk");
    auto oldPrepared = queue.Prepare(1, {{"chunk", Vertices(2)}});
    queue.UnregisterDynamicChunk("chunk");
    queue.RegisterDynamicChunk("chunk");
    EXPECT_THROW(queue.Commit(std::move(oldPrepared)), std::invalid_argument);
    queue.Replay(1, 0, [](const auto&, const auto&) { ADD_FAILURE(); return true; });
}

TEST(TerrainVertexUpdateQueue, SameKeyRecreationRequiresAllThreeSlotsAgain)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    for (uint32_t slot = 0; slot < 3; ++slot)
        queue.Replay(1, slot, [](const auto&, const auto&) { return true; });
    queue.UnregisterDynamicChunk("chunk");
    queue.RegisterDynamicChunk("chunk");
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(2)}}));
    int calls = 0;
    for (uint32_t slot = 0; slot < 3; ++slot)
        for (int frame = 0; frame < 2; ++frame)
            queue.Replay(1, slot, [&](const auto&, const auto& vertices) {
                EXPECT_FLOAT_EQ(vertices[0].position.y(), 2);
                ++calls;
                return true;
            });
    EXPECT_EQ(calls, 3);
}

TEST(TerrainVertexUpdateQueue, OldAcknowledgementCannotConsumeRecreatedKey)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(1)}}));
    queue.Replay(1, 0, [&](const auto&, const auto&) {
        queue.UnregisterDynamicChunk("chunk");
        queue.RegisterDynamicChunk("chunk");
        queue.Commit(queue.Prepare(1, {{"chunk", Vertices(2)}}));
        return true;
    });
    int calls = 0;
    queue.Replay(1, 0, [&](const auto&, const auto& vertices) {
        EXPECT_FLOAT_EQ(vertices[0].position.y(), 2);
        ++calls;
        return true;
    });
    EXPECT_EQ(calls, 1);
}

TEST(TerrainVertexUpdateQueue, RemovingOneChunkPreservesOtherChunksAcknowledgements)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("first");
    queue.RegisterDynamicChunk("second");
    queue.Commit(queue.Prepare(1, {{"first", Vertices(1)}, {"second", Vertices(2)}}));
    queue.Replay(1, 0, [](const auto&, const auto&) { return true; });
    auto prepared = queue.Prepare(1, {{"first", Vertices(3)}});
    queue.UnregisterDynamicChunk("first");
    EXPECT_THROW(queue.Commit(std::move(prepared)), std::invalid_argument);
    queue.Replay(1, 0, [](const auto&, const auto&) { ADD_FAILURE(); return true; });
    int calls = 0;
    queue.Replay(1, 1, [&](const auto& key, const auto&) {
        EXPECT_EQ(key, "second");
        ++calls;
        return true;
    });
    EXPECT_EQ(calls, 1);
}

TEST(TerrainVertexUpdateQueue, ReplaySkipsChunkRemovedAfterSnapshotWasCaptured)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("first");
    queue.RegisterDynamicChunk("second");
    queue.Commit(queue.Prepare(1, {{"first", Vertices(1)}, {"second", Vertices(2)}}));
    int calls = 0;
    queue.Replay(1, 0, [&](const auto& key, const auto&) {
        EXPECT_EQ(key, "first");
        ++calls;
        queue.UnregisterDynamicChunk("second");
        return true;
    });
    EXPECT_EQ(calls, 1);
}

TEST(TerrainVertexUpdateQueue, StaleOrForeignPreparationCannotOverwritePublishedState)
{
    VL::TerrainVertexUpdateQueue queue;
    queue.RegisterDynamicChunk("chunk");
    auto old = queue.Prepare(1, {{"chunk", Vertices(1)}});
    queue.Commit(queue.Prepare(1, {{"chunk", Vertices(2)}}));
    EXPECT_THROW(queue.Commit(std::move(old)), std::invalid_argument);
    VL::TerrainVertexUpdateQueue otherWorld;
    otherWorld.RegisterDynamicChunk("chunk");
    auto foreign = queue.Prepare(1, {{"chunk", Vertices(3)}});
    EXPECT_THROW(otherWorld.Commit(std::move(foreign)), std::invalid_argument);
    queue.Replay(1, 0, [](const auto&, const auto& vertices) {
        EXPECT_FLOAT_EQ(vertices[0].position.y(), 2);
        return true;
    });
    otherWorld.Commit(otherWorld.Prepare(2, {{"chunk", Vertices(4)}}));
    otherWorld.Replay(1, 0, [](const auto&, const auto&) { ADD_FAILURE(); return true; });
}

TEST(TerrainVertexUpdateQueue, EvictionReturnsRegistryCapacityForNewChunks)
{
    VL::TerrainVertexUpdateQueue queue;
    for (size_t index = 0; index < 4096; ++index)
        queue.RegisterDynamicChunk(std::to_string(index));
    EXPECT_THROW(queue.RegisterDynamicChunk("new"), std::invalid_argument);
    queue.UnregisterDynamicChunk("0");
    EXPECT_NO_THROW(queue.RegisterDynamicChunk("new"));
    EXPECT_TRUE(queue.HasDynamicChunk("new"));
}
