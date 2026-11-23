#include <gtest/gtest.h>

#include "pipeline/pipelineFactory.h"
#include "render/resource/terrainCompositedReloadRegistration.h"
#include "shader/reload/shaderReloadCoordinator.h"
#include "shaderCompiler.h"

namespace VL
{
namespace
{

class MockComputeParticipant final : public ComputePipelineReloadParticipant
{
public:
    explicit MockComputeParticipant(std::string name = "mock")
    {
        artifact.shaderName = std::move(name);
    }
    std::string GetShaderName() const override { return artifact.shaderName; }
    std::shared_ptr<ComputePipeline> GetActivePipeline() const override { return {}; }
    const ComputeShaderArtifact& GetActiveArtifact() const override { return artifact; }
    ComputeDescriptorReplacement PrepareReplacementDescriptors(
        const ComputeShaderArtifact&, const std::shared_ptr<ComputePipeline>&) const override
    {
        return {};
    }
    void CommitReplacement(ComputeShaderArtifact, std::shared_ptr<ComputePipeline>,
        ComputeDescriptorReplacement&&) noexcept override
    {
    }
private:
    ComputeShaderArtifact artifact;
};

class MockTerrainService
{
public:
    explicit MockTerrainService(bool initialized = true) : initialized(initialized) {}
    ~MockTerrainService() { SetShaderReloadCoordinator(nullptr); }
    void SetShaderReloadCoordinator(ShaderReloadCoordinator* selected)
    {
        if (coordinator == selected)
            return;
        if (coordinator && initialized)
            for (auto& participant : participants)
                coordinator->UnregisterComputeParticipant(&participant);
        coordinator = selected;
        if (coordinator && initialized)
        {
            participantCountBeforeRegistration = coordinator->computeParticipants.size();
            for (auto& participant : participants)
                coordinator->RegisterComputeParticipant(&participant);
        }
    }
    void InitializeProducer()
    {
        ASSERT_FALSE(initialized);
        initialized = true;
        if (coordinator)
            for (auto& participant : participants)
                coordinator->RegisterComputeParticipant(&participant);
    }
    std::array<MockComputeParticipant, 2> participants{
        MockComputeParticipant("terrain/terrainCompositedBakeWorld"),
        MockComputeParticipant("terrain/terrainCompositedBakeWorldPadding")};
    size_t participantCountBeforeRegistration = 0;
private:
    ShaderReloadCoordinator* coordinator = nullptr;
    bool initialized;
};

class TerrainCompositedReloadRegistrationTest : public testing::Test
{
protected:
    vk::Device device{};
    ShaderCompiler compiler;
    PipelineFactory factory{nullptr, device};
    ShaderReloadCoordinator coordinator{compiler, factory};
    std::weak_ptr<MockTerrainService> registered;

    void Select(const std::shared_ptr<MockTerrainService>& service,
        uint64_t active = 1, uint64_t resources = 1)
    {
        RebindTerrainCompositedReloadRegistration(registered, service, active, resources, &coordinator);
    }
    size_t Count() const { return coordinator.computeParticipants.size(); }
};

TEST_F(TerrainCompositedReloadRegistrationTest, ActiveReplacementUnregistersOldPair)
{
    auto oldService = std::make_shared<MockTerrainService>();
    auto nextService = std::make_shared<MockTerrainService>();
    Select(oldService);
    ASSERT_EQ(Count(), 2u);
    Select(nextService, 2, 2);
    ASSERT_EQ(Count(), 2u);
    EXPECT_EQ(nextService->participantCountBeforeRegistration, 0u);
    EXPECT_EQ(coordinator.computeParticipants[0], &nextService->participants[0]);
    EXPECT_EQ(coordinator.computeParticipants[1], &nextService->participants[1]);
    oldService.reset();
    EXPECT_EQ(Count(), 2u);
    nextService.reset();
    EXPECT_EQ(Count(), 0u);
}

TEST_F(TerrainCompositedReloadRegistrationTest, CandidateAndGenerationMismatchNeverRegister)
{
    auto active = std::make_shared<MockTerrainService>();
    auto provisional = std::make_shared<MockTerrainService>();
    Select(active);
    EXPECT_EQ(Count(), 2u);
    provisional.reset();
    EXPECT_EQ(Count(), 2u);
    Select(active, 2, 1);
    EXPECT_EQ(Count(), 0u);
    EXPECT_TRUE(registered.expired());
    Select(active, 0, 0);
    EXPECT_EQ(Count(), 0u);
}

TEST_F(TerrainCompositedReloadRegistrationTest, SameWorldRebindIsIdempotent)
{
    auto service = std::make_shared<MockTerrainService>();
    for (size_t iteration = 0; iteration < 128; ++iteration)
        Select(service);
    EXPECT_EQ(Count(), 2u);
    EXPECT_EQ(service.use_count(), 1u);
    Select(nullptr, 0, 0);
    EXPECT_EQ(Count(), 0u);
    EXPECT_TRUE(registered.expired());
}

TEST_F(TerrainCompositedReloadRegistrationTest, CoordinatorReplacementDetachesOldBeforeNew)
{
    ShaderReloadCoordinator replacement(compiler, factory);
    auto service = std::make_shared<MockTerrainService>();
    Select(service);
    RebindTerrainCompositedReloadRegistration(registered, service, 1, 1, &replacement);
    EXPECT_EQ(Count(), 0u);
    EXPECT_EQ(replacement.computeParticipants.size(), 2u);
    RebindTerrainCompositedReloadRegistration(registered, service, 1, 1, nullptr);
    EXPECT_TRUE(replacement.computeParticipants.empty());
    service.reset();
    EXPECT_TRUE(registered.expired());
}

TEST_F(TerrainCompositedReloadRegistrationTest, LazyProducerUsesStoredCoordinator)
{
    auto service = std::make_shared<MockTerrainService>(false);
    Select(service);
    EXPECT_EQ(Count(), 0u);
    service->InitializeProducer();
    EXPECT_EQ(Count(), 2u);
    Select(nullptr, 0, 0);
    EXPECT_EQ(Count(), 0u);
}

TEST_F(TerrainCompositedReloadRegistrationTest, ShutdownDetachesBeforeServiceDestruction)
{
    auto service = std::make_shared<MockTerrainService>();
    Select(service);
    if (auto selected = registered.lock())
        selected->SetShaderReloadCoordinator(nullptr);
    registered.reset();
    EXPECT_EQ(Count(), 0u);
    EXPECT_EQ(service.use_count(), 1u);
    service.reset();
    EXPECT_EQ(Count(), 0u);
}

TEST_F(TerrainCompositedReloadRegistrationTest, ThousandWorldReplacementsKeepOnlySevenParticipants)
{
    std::array<MockComputeParticipant, 5> existing;
    for (auto& participant : existing)
        coordinator.RegisterComputeParticipant(&participant);
    auto current = std::make_shared<MockTerrainService>();
    Select(current);
    const auto initialCapacity = coordinator.computeParticipants.capacity();
    for (uint64_t generation = 2; generation <= 1025; ++generation)
    {
        std::weak_ptr<MockTerrainService> retired = current;
        auto next = std::make_shared<MockTerrainService>();
        Select(next, generation, generation);
        current = std::move(next);
        EXPECT_TRUE(retired.expired());
        ASSERT_EQ(Count(), 7u);
        ASSERT_EQ(coordinator.computeParticipants.capacity(), initialCapacity);
        EXPECT_EQ(current->participantCountBeforeRegistration, 5u);
    }
    Select(nullptr, 0, 0);
    EXPECT_EQ(Count(), 5u);
    current.reset();
    for (auto& participant : existing)
        coordinator.UnregisterComputeParticipant(&participant);
    EXPECT_EQ(Count(), 0u);
}

}
}
