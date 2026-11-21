#include <gtest/gtest.h>
#include "terrain/terrainFaultOwnerRegistration.h"
#include "render/resource/terrainCompositedReloadRegistration.h"
#include <memory>
#include <vector>

namespace VL
{
namespace
{
class ActiveService
{
public:
    ActiveService(TerrainFaultInjectionController& controller, uint64_t world,
        std::vector<uint64_t>& callbacks)
        : controller(controller), world(world), callbacks(callbacks) {}
    ~ActiveService() { owners.Deactivate(controller); }

    void SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator)
    {
        callbacks.push_back(world * 10 + (coordinator ? 1 : 0));
        if (!coordinator)
            owners.Deactivate(controller);
        else
            EXPECT_TRUE(owners.Activate(controller, world, {
                [](uint32_t, uint64_t) -> std::optional<TerrainCompositedSlotDiagnostics> { return {}; },
                [](uint32_t) { return false; },
                [](uint64_t) -> std::optional<TerrainCompositedSlotReuseDiagnostics> { return {}; },
                [] {}}));
    }
    TerrainFaultOwnerRegistration owners;
    std::shared_ptr<int> normalGpuOwner = std::make_shared<int>(1);

private:
    TerrainFaultInjectionController& controller;
    uint64_t world;
    std::vector<uint64_t>& callbacks;
};

TEST(TerrainFaultOwnerRegistration, CandidatePrepareDoesNotSpendOwnersAndRealRebindReleasesOldBeforeNewFive)
{
    static_assert(TerrainFaultInjectionController::MaxOwners == 8);
    TerrainFaultInjectionController controller;
    std::vector<uint64_t> callbacks;
    std::weak_ptr<ActiveService> registered;
    auto old = std::make_shared<ActiveService>(controller, 1, callbacks);
    auto* coordinator = reinterpret_cast<ShaderReloadCoordinator*>(uintptr_t(1));
    RebindTerrainCompositedReloadRegistration(registered, old, 1, 1, coordinator);
    ASSERT_TRUE(old->owners.Complete());
    std::weak_ptr<int> oldGpu = old->normalGpuOwner;
    auto candidate = std::make_shared<ActiveService>(controller, 2, callbacks);
    EXPECT_FALSE(candidate->owners.Complete());
    std::vector<uint64_t> extraOwners;
    for (uint64_t world = 100; world < 103; ++world)
    {
        const auto owner = controller.RegisterStatusHook(world);
        ASSERT_NE(owner, 0u);
        extraOwners.push_back(owner);
    }
    EXPECT_EQ(controller.RegisterStatusHook(104), 0u);
    for (const auto owner : extraOwners)
        controller.UnregisterStatusHook(owner);
    RebindTerrainCompositedReloadRegistration(registered, candidate, 2, 2, coordinator);
    ASSERT_EQ(callbacks.size(), 3u);
    EXPECT_EQ(callbacks[1], 10u);
    EXPECT_EQ(callbacks[2], 21u);
    EXPECT_FALSE(old->owners.Complete());
    EXPECT_TRUE(candidate->owners.Complete());
    EXPECT_FALSE(oldGpu.expired());
    auto retiredDescriptorOwner = old->normalGpuOwner;
    old.reset();
    EXPECT_FALSE(oldGpu.expired());
    retiredDescriptorOwner.reset();
    EXPECT_TRUE(oldGpu.expired());
    RebindTerrainCompositedReloadRegistration(registered, std::shared_ptr<ActiveService>{}, 0, 0, nullptr);
    EXPECT_FALSE(candidate->owners.Complete());
    extraOwners.clear();
    for (uint64_t world = 200; world < 208; ++world)
    {
        const auto owner = controller.RegisterStatusHook(world);
        ASSERT_NE(owner, 0u);
        extraOwners.push_back(owner);
    }
    EXPECT_EQ(controller.RegisterStatusHook(208), 0u);
    for (const auto owner : extraOwners)
        controller.UnregisterStatusHook(owner);
}

TEST(TerrainFaultOwnerRegistration, PartialRegistrationRollsBackWithinOriginalEightLimit)
{
    TerrainFaultInjectionController controller;
    std::vector<uint64_t> existing;
    for (uint64_t world = 1; world <= 4; ++world)
        existing.push_back(controller.RegisterStatusHook(world));
    TerrainFaultOwnerRegistration owner;
    EXPECT_FALSE(owner.Activate(controller, 50, {
        [](uint32_t, uint64_t) -> std::optional<TerrainCompositedSlotDiagnostics> { return {}; },
        [](uint32_t) { return false; },
        [](uint64_t) -> std::optional<TerrainCompositedSlotReuseDiagnostics> { return {}; },
        [] {}}));
    EXPECT_EQ(owner.faultStatusHook, 0u);
    EXPECT_EQ(owner.faultSubmitHook, 0u);
    EXPECT_EQ(owner.faultSlotHook, 0u);
    EXPECT_EQ(owner.faultStaleHook, 0u);
    EXPECT_EQ(owner.faultBudgetHook, 0u);
    for (uint64_t world = 60; world < 64; ++world)
    {
        const auto registration = controller.RegisterStatusHook(world);
        ASSERT_NE(registration, 0u);
        existing.push_back(registration);
    }
    EXPECT_EQ(controller.RegisterStatusHook(100), 0u);
    for (const auto registration : existing)
        controller.UnregisterStatusHook(registration);
}
}
}
