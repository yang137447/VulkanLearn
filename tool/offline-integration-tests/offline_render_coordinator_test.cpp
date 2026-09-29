#include <algorithm>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "render/offline/offlineRenderCoordinator.h"
#include "shader/build/contentHash.h"

namespace
{
using namespace VL;

struct Input
{
    WorldSnapshotPtr snapshot;
    std::shared_ptr<const OfflineWorldAssets> assets;
};

Input MakeInput(std::size_t triangles = 1)
{
    auto snapshot = std::make_shared<WorldSnapshot>();
    snapshot->worldGeneration = 7;
    snapshot->environment.type = EnvironmentType::None;
    snapshot->camera.position = Eigen::Vector3f(0, 0, 2);
    snapshot->meshDraws.emplace_back();
    snapshot->meshDraws.back().mesh.key = "mesh|0|cloth";
    snapshot->meshDraws.back().materialInstance.key = "MI_cloth";

    auto assets = std::make_shared<OfflineWorldAssets>();
    assets->worldGeneration = 7;
    assets->scenePath = "Maps/SC_offline/SC_offline.json";
    auto imported = std::make_shared<MeshAssetImportResult>();
    imported->buildPlan.assetType = "mesh";
    imported->buildPlan.meshAssetPath = "SM_cloth";
    MeshSection section;
    section.sectionName = "cloth";
    section.vertices.emplace_back(Eigen::Vector3f(-1, -1, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0, 0));
    section.vertices.emplace_back(Eigen::Vector3f(1, -1, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(1, 0));
    section.vertices.emplace_back(Eigen::Vector3f(0, 1, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0.5f, 1));
    section.indices.reserve(triangles * 3);
    for (std::size_t i = 0; i < triangles; ++i)
        section.indices.insert(section.indices.end(), {0, 1, 2});
    imported->modelResource.sections.push_back(std::move(section));
    assets->sections.emplace("mesh|0|cloth", OfflineWorldAssets::Section{imported, 0});
    assets->effectiveMaterialInstances.emplace("MI_cloth", nlohmann::json{
        {"offlineModel", "Lambert"},
        {"renderStates", {{"renderMode", "Opaque"}, {"cullMode", "None"}}},
        {"macros", nlohmann::json::object()},
        {"textures", nlohmann::json::object()},
        {"parameters", {{"u_baseColor", {0.2, 0.4, 0.6}}}}});
    return {snapshot, assets};
}

OfflineRenderSettings Settings(int samples = 1)
{
    return {8, 6, samples, 0, 1, 3};
}

std::shared_ptr<const OfflinePublishedResult> WaitForSamples(
    OfflineRenderCoordinator& coordinator, int samples)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = coordinator.Capture();
        if (result && result->film->completedSamples == samples &&
            result->manifest.at("complete").get<bool>())
            return result;
        if (coordinator.State() == Offline::SessionState::Failed)
            return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

std::shared_ptr<const Offline::PreviewSnapshot> WaitForPreview(
    OfflineRenderCoordinator& coordinator)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto preview = coordinator.CapturePreview()) return preview;
        if (coordinator.State() == Offline::SessionState::Failed) return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

TEST(OfflineRenderCoordinator, RapidTargetChangesUseLastTarget)
{
    OfflineRenderCoordinator coordinator;
    // 重复三角形增加 CPU prepare 负载；正确性不依赖 worker 的具体调度时刻。
    const auto input = MakeInput(8192);
    coordinator.Observe(input.snapshot, input.assets, Settings());
    for (int samples = 2; samples <= 7; ++samples)
        coordinator.Observe(input.snapshot, input.assets, Settings(samples));

    const auto result = WaitForSamples(coordinator, 7);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_EQ(result->taskGeneration, 1u);
    EXPECT_EQ(result->film->targetSamples, 7);
    EXPECT_EQ(result->manifest.at("targetSamples"), 7);
}

TEST(OfflineRenderCoordinator, NewInputRejectsOldGenerationPublication)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto oldResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(oldResult, nullptr) << coordinator.Error();

    auto next = std::make_shared<WorldSnapshot>(*input.snapshot);
    next->camera.position.x() = 0.5f;
    auto longSettings = Settings(100000);
    coordinator.Observe(next, input.assets, longSettings);
    EXPECT_EQ(coordinator.Capture(), nullptr);
    auto newestSettings = Settings(2);
    newestSettings.seed = 9;
    coordinator.Observe(next, input.assets, newestSettings);
    const auto result = WaitForSamples(coordinator, 2);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, oldResult->taskGeneration);
    EXPECT_EQ(result->manifest.at("integrator").at("seed"), 9);
    EXPECT_EQ(result->manifest.at("camera").at("position")[0], 0.5);
    EXPECT_EQ(oldResult->film->completedSamples, 1);
}

TEST(OfflineRenderCoordinator, InteractivePreviewNeverBecomesExportableFilm)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    auto settings = Settings();
    settings.interactivePreview = true;
    coordinator.Observe(input.snapshot, input.assets, settings);
    const auto preview = WaitForPreview(coordinator);
    ASSERT_NE(preview, nullptr) << coordinator.Error();
    EXPECT_EQ(preview->width, settings.width);
    EXPECT_EQ(preview->height, settings.height);
    EXPECT_EQ(coordinator.Capture(), nullptr);

    // 停稳切换回正式输入必须换代；之前的稀疏结果不能混入完整 spp。
    settings.interactivePreview = false;
    coordinator.Observe(input.snapshot, input.assets, settings);
    EXPECT_EQ(coordinator.CapturePreview(), nullptr);
    const auto film = WaitForSamples(coordinator, 1);
    ASSERT_NE(film, nullptr) << coordinator.Error();
    EXPECT_EQ(film->film->completedSamples, 1);
}

TEST(OfflineRenderCoordinator, LowerTargetRestartsWithoutExcessSamples)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings(5));
    const auto oldResult = WaitForSamples(coordinator, 5);
    ASSERT_NE(oldResult, nullptr) << coordinator.Error();

    coordinator.Observe(input.snapshot, input.assets, Settings(2));
    EXPECT_EQ(coordinator.Capture(), nullptr);
    const auto result = WaitForSamples(coordinator, 2);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, oldResult->taskGeneration);
    EXPECT_EQ(result->film->targetSamples, 2);
    EXPECT_EQ(result->manifest.at("samples"), 2);
}

TEST(OfflineRenderCoordinator, StopRetainsLastImageUntilExplicitRestart)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto oldResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(oldResult, nullptr) << coordinator.Error();

    coordinator.Stop();
    EXPECT_EQ(coordinator.State(), Offline::SessionState::Canceled);
    const auto retained = coordinator.Capture();
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->taskGeneration, oldResult->taskGeneration);
    auto changed = Settings();
    changed.seed = 11;
    coordinator.Observe(input.snapshot, input.assets, changed);
    EXPECT_EQ(coordinator.State(), Offline::SessionState::Canceled);
    ASSERT_NE(coordinator.Capture(), nullptr);
    EXPECT_EQ(coordinator.Capture()->taskGeneration, oldResult->taskGeneration);

    coordinator.Restart();
    coordinator.Observe(input.snapshot, input.assets, changed);
    EXPECT_EQ(coordinator.Capture(), nullptr);
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, oldResult->taskGeneration);
    EXPECT_EQ(result->manifest.at("integrator").at("seed"), 11);
}

TEST(OfflineRenderCoordinator, FrameOnlySnapshotKeepsPublishedInputAndDigest)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto next = std::make_shared<WorldSnapshot>(*input.snapshot);
    next->frameIndex = 100;
    next->camera.previousViewProjection(0, 0) = 2.0f;
    next->meshDraws.front().previousModel(0, 0) = 2.0f;
    coordinator.Observe(next, input.assets, Settings());
    const auto unchanged = coordinator.Capture();
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->taskGeneration, first->taskGeneration);
    EXPECT_EQ(unchanged->manifest.at("inputDigest"), first->manifest.at("inputDigest"));

    coordinator.Restart();
    coordinator.Observe(next, input.assets, Settings());
    const auto restarted = WaitForSamples(coordinator, 1);
    ASSERT_NE(restarted, nullptr) << coordinator.Error();
    EXPECT_GT(restarted->taskGeneration, first->taskGeneration);
    EXPECT_EQ(restarted->manifest.at("inputDigest"), first->manifest.at("inputDigest"));
}

TEST(OfflineRenderCoordinator, InputDigestExcludesPublicationMetadata)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();

    auto inputOnly = result->manifest;
    const auto digest = inputOnly.at("inputDigest").get<std::string>();
    inputOnly.erase("inputDigest");
    inputOnly.erase("taskGeneration");
    inputOnly.erase("samples");
    inputOnly.erase("targetSamples");
    inputOnly.erase("complete");
    EXPECT_EQ(digest, ContentHasher::HashString(inputOnly.dump()).ToHex());
    EXPECT_TRUE(inputOnly.contains("meshDigests"));
}

TEST(OfflineRenderCoordinator, MaterialRevisionInvalidatesFrozenInput)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto revised = std::make_shared<OfflineWorldAssets>(*input.assets);
    revised->effectiveMaterialInstances.at("MI_cloth")["parameters"]["u_baseColor"] =
        {0.7, 0.3, 0.1};
    coordinator.Observe(input.snapshot, revised, Settings());
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, first->taskGeneration);
    EXPECT_NE(result->manifest.at("inputDigest"), first->manifest.at("inputDigest"));
    EXPECT_EQ(result->manifest.at("materials").at("MI_cloth")
        .at("parameters").at("u_baseColor")[0], 0.7);
}

TEST(OfflineRenderCoordinator, MeshBytesInvalidateDigestAndPreparedGeometry)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    auto lit = std::make_shared<WorldSnapshot>(*input.snapshot);
    lit->lights.emplace_back();
    lit->lights.back().type = LightSnapshotType::Directional;
    lit->lights.back().direction = Eigen::Vector3f(0, 0, -1);
    lit->lights.back().color = Eigen::Vector3f::Ones();
    lit->lights.back().intensity = 1.0f;
    coordinator.Observe(lit, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto revised = std::make_shared<OfflineWorldAssets>(*input.assets);
    auto imported = std::make_shared<MeshAssetImportResult>(
        *revised->sections.at("mesh|0|cloth").imported);
    for (auto& vertex : imported->modelResource.sections.front().vertices)
        vertex.position.x() += 100.0f;
    revised->sections.at("mesh|0|cloth").imported = imported;
    coordinator.Observe(lit, revised, Settings());
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, first->taskGeneration);
    EXPECT_NE(result->manifest.at("meshDigests"), first->manifest.at("meshDigests"));
    EXPECT_NE(result->manifest.at("inputDigest"), first->manifest.at("inputDigest"));
    // 相同 World generation 下不能把旧 BVH 用于新网格版本。
    EXPECT_TRUE(std::any_of(first->film->direct.begin(), first->film->direct.end(),
        [](const Offline::Vec3& color) { return color.x > 0; }));
    EXPECT_TRUE(std::all_of(result->film->direct.begin(), result->film->direct.end(),
        [](const Offline::Vec3& color) {
            return color.x == 0 && color.y == 0 && color.z == 0;
        }));
}

TEST(OfflineRenderCoordinator, CameraClipPlanesRestartAndChangeInputIdentity)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();
    EXPECT_EQ(first->manifest.at("camera").at("clipNear"), 0.1f);
    EXPECT_EQ(first->manifest.at("camera").at("clipFar"), 1000.0f);

    auto nearChanged = std::make_shared<WorldSnapshot>(*input.snapshot);
    nearChanged->camera.clipNear = 0.5f;
    coordinator.Observe(nearChanged, input.assets, Settings());
    EXPECT_EQ(coordinator.Capture(), nullptr);
    const auto nearResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(nearResult, nullptr) << coordinator.Error();
    EXPECT_GT(nearResult->taskGeneration, first->taskGeneration);
    EXPECT_EQ(nearResult->manifest.at("camera").at("clipNear"), 0.5f);
    EXPECT_NE(nearResult->manifest.at("inputDigest"), first->manifest.at("inputDigest"));

    auto farChanged = std::make_shared<WorldSnapshot>(*nearChanged);
    farChanged->camera.clipFar = 3.0f;
    coordinator.Observe(farChanged, input.assets, Settings());
    EXPECT_EQ(coordinator.Capture(), nullptr);
    const auto farResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(farResult, nullptr) << coordinator.Error();
    EXPECT_GT(farResult->taskGeneration, nearResult->taskGeneration);
    EXPECT_EQ(farResult->manifest.at("camera").at("clipFar"), 3.0f);
    EXPECT_NE(farResult->manifest.at("inputDigest"), nearResult->manifest.at("inputDigest"));
}

TEST(OfflineRenderCoordinator, EquivalentFrozenPackageKeepsPersistentDigest)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto equivalent = std::make_shared<OfflineWorldAssets>(*input.assets);
    coordinator.Observe(input.snapshot, equivalent, Settings());
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, first->taskGeneration);
    EXPECT_EQ(result->manifest.at("inputDigest"), first->manifest.at("inputDigest"));
    EXPECT_EQ(result->manifest.at("meshDigests"), first->manifest.at("meshDigests"));
}

TEST(OfflineRenderCoordinator, LightAndDrawChangesInvalidateInput)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto lit = std::make_shared<WorldSnapshot>(*input.snapshot);
    lit->lights.emplace_back();
    lit->lights.back().type = LightSnapshotType::Directional;
    lit->lights.back().direction = Eigen::Vector3f(0, 0, -1);
    coordinator.Observe(lit, input.assets, Settings());
    const auto lightResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(lightResult, nullptr) << coordinator.Error();
    EXPECT_GT(lightResult->taskGeneration, first->taskGeneration);
    EXPECT_NE(lightResult->manifest.at("inputDigest"), first->manifest.at("inputDigest"));

    auto moved = std::make_shared<WorldSnapshot>(*lit);
    moved->meshDraws.front().model(0, 3) = 0.25f;
    coordinator.Observe(moved, input.assets, Settings());
    const auto drawResult = WaitForSamples(coordinator, 1);
    ASSERT_NE(drawResult, nullptr) << coordinator.Error();
    EXPECT_GT(drawResult->taskGeneration, lightResult->taskGeneration);
    EXPECT_NE(drawResult->manifest.at("inputDigest"), lightResult->manifest.at("inputDigest"));
    EXPECT_EQ(drawResult->manifest.at("draws")[0].at("model")[3], 0.25);
}

TEST(OfflineRenderCoordinator, WorldGenerationInvalidatesEvenWithSameDraws)
{
    OfflineRenderCoordinator coordinator;
    const auto input = MakeInput();
    coordinator.Observe(input.snapshot, input.assets, Settings());
    const auto first = WaitForSamples(coordinator, 1);
    ASSERT_NE(first, nullptr) << coordinator.Error();

    auto next = std::make_shared<WorldSnapshot>(*input.snapshot);
    next->worldGeneration = 8;
    auto assets = std::make_shared<OfflineWorldAssets>(*input.assets);
    assets->worldGeneration = 8;
    coordinator.Observe(next, assets, Settings());
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    EXPECT_GT(result->taskGeneration, first->taskGeneration);
    EXPECT_NE(result->manifest.at("inputDigest"), first->manifest.at("inputDigest"));
    EXPECT_EQ(result->manifest.at("worldGeneration"), 8);
}
} // namespace
