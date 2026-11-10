#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "renderableObject.h"

namespace
{

std::vector<Vertex> MakeVertices(float x)
{
    std::vector<Vertex> vertices;
    vertices.emplace_back(
        Eigen::Vector3f(x, 0.0f, 0.0f),
        Eigen::Vector3f::UnitY(),
        Eigen::Vector4f::Ones(),
        Eigen::Vector2f::Zero());
    return vertices;
}

} // namespace

TEST(DynamicMeshUpdateQueue, LatestWinsForSameMeshKey)
{
    VL::DynamicMeshUpdateQueue queue;
    queue.QueueVertices("mesh", MakeVertices(1.0f));
    queue.QueueMesh("mesh", MakeVertices(2.0f), {0, 1, 2});
    queue.QueueVertices("mesh", MakeVertices(3.0f));

    ASSERT_EQ(queue.PendingCount(), 1u);
    auto drained = queue.Drain();
    ASSERT_EQ(drained.size(), 1u);
    const VL::DynamicMeshUpdate& update = drained.at("mesh");
    EXPECT_FALSE(update.replacesIndices);
    ASSERT_EQ(update.vertices.size(), 1u);
    EXPECT_FLOAT_EQ(update.vertices.front().position.x(), 3.0f);

    EXPECT_TRUE(queue.Empty());
    EXPECT_TRUE(queue.Drain().empty());
}

TEST(DynamicMeshUpdateQueue, KeepsDistinctMeshKeys)
{
    VL::DynamicMeshUpdateQueue queue;
    queue.QueueVertices("soft", MakeVertices(1.0f));
    queue.QueueMesh("glass", MakeVertices(2.0f), {0, 2, 1});

    auto drained = queue.Drain();
    ASSERT_EQ(drained.size(), 2u);
    EXPECT_FALSE(drained.at("soft").replacesIndices);
    EXPECT_TRUE(drained.at("soft").indices.empty());
    EXPECT_TRUE(drained.at("glass").replacesIndices);
    EXPECT_EQ(drained.at("glass").indices,
        (std::vector<uint32_t>{0, 2, 1}));
}

TEST(DynamicMeshUpdateQueue, MeshReplacementOverridesPendingIndices)
{
    VL::DynamicMeshUpdateQueue queue;
    queue.QueueMesh("mesh", MakeVertices(1.0f), {0, 1, 2, 3});
    queue.QueueMesh("mesh", MakeVertices(2.0f), {4, 5, 6});

    auto drained = queue.Drain();
    const VL::DynamicMeshUpdate& update = drained.at("mesh");
    EXPECT_TRUE(update.replacesIndices);
    EXPECT_EQ(update.indices, (std::vector<uint32_t>{4, 5, 6}));
    EXPECT_EQ(queue.PendingCount(), 0u);
}

TEST(DynamicMeshUpdateQueue, RequeueDoesNotOverwriteNewerUpdate)
{
    VL::DynamicMeshUpdateQueue queue;
    VL::DynamicMeshUpdate older;
    older.vertices = MakeVertices(1.0f);
    queue.RequeueIfAbsent("mesh", std::move(older));

    queue.QueueVertices("mesh", MakeVertices(2.0f));
    VL::DynamicMeshUpdate stale;
    stale.vertices = MakeVertices(3.0f);
    queue.RequeueIfAbsent("mesh", std::move(stale));

    auto drained = queue.Drain();
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_FLOAT_EQ(
        drained.at("mesh").vertices.front().position.x(), 2.0f);
}

TEST(DynamicMeshUpdateQueue, ConcurrentPublishAndDrain)
{
    VL::DynamicMeshUpdateQueue queue;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer)
    {
        producers.emplace_back([&queue, producer]()
        {
            for (int iteration = 0; iteration < 200; ++iteration)
            {
                const std::string key =
                    "mesh" + std::to_string(producer % 2);
                queue.QueueVertices(
                    key, MakeVertices(static_cast<float>(iteration)));
            }
        });
    }

    for (int iteration = 0; iteration < 200; ++iteration)
    {
        auto drained = queue.Drain();
        EXPECT_LE(drained.size(), 2u);
    }
    for (std::thread& producer : producers)
        producer.join();

    const size_t remaining = queue.PendingCount();
    queue.Drain();
    EXPECT_LE(remaining, 2u);
    EXPECT_TRUE(queue.Empty());
}
