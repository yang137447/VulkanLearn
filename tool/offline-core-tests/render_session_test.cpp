#include <chrono>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "core/renderSession.h"

namespace
{
using namespace VL::Offline;

bool WaitUntil(RenderSession& session, SessionState target)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (session.State() == target) return true;
        if (session.State() == SessionState::Failed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

RenderInput Input(int samples)
{
    RenderInput input;
    input.width = 4;
    input.height = 3;
    input.targetSamples = samples;
    input.scene.cameraPosition = {0, 1.7, 8};
    input.cameraForward = normalize(Vec3{0, -1.7, -8});
    input.cameraUp = normalize(Vec3{0, 8, -1.7});
    // 显式实例构成唯一几何输入，避免依赖 CLI 固定球阵或隐式地面。
    auto mesh = std::make_shared<Mesh>();
    Triangle first;
    first.vertices = {{{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}}};
    first.normals = {{{0, 1, 0}, {0, 1, 0}, {0, 1, 0}}};
    Triangle second = first;
    second.vertices = {{{-100, 0, -100}, {100, 0, 100}, {-100, 0, 100}}};
    mesh->triangles = {first, second};
    mesh->order = {0, 1};
    for (const auto& triangle : mesh->triangles)
        expand(mesh->bounds, triangleBounds(triangle));
    buildBvh(*mesh, 0, mesh->triangles.size());
    input.instanceMeshes.push_back(mesh);
    MeshInstance instance;
    instance.mesh = mesh.get();
    instance.model = SurfaceModel::Lambert;
    instance.baseColor = {0.22, 0.23, 0.24};
    input.scene.instances.push_back(instance);
    input.scene.directionalLight = true;
    input.scene.toLight = normalize(Vec3{0.3, 1, 0.2});
    input.scene.directionalIrradiance = 2;
    input.scene.seed = 17;
    return input;
}

RenderInput PreviewInput(int width, int height,
                         std::chrono::steady_clock::time_point deadline)
{
    auto input = Input(1);
    input.width = width;
    input.height = height;
    input.interactivePreview = true;
    input.previewDeadline = deadline;
    return input;
}

RenderInput PerspectivePlane(double depth, double nearClip, double farClip,
                             double frontDepth = 0.0)
{
    auto input = Input(1);
    input.width = 2;
    input.height = 1;
    input.scene.cameraPosition = {0, 0, 0};
    input.scene.backdrop = {};
    input.scene.instances.clear();
    input.instanceMeshes.clear();
    input.scene.toLight = {0, 0, 1};
    input.cameraForward = {0, 0, -1};
    input.cameraUp = {0, 1, 0};
    input.horizontalFovDegrees = 90;
    input.cameraClipNear = nearClip;
    input.cameraClipFar = farClip;
    auto mesh = std::make_shared<Mesh>();
    Triangle triangle;
    triangle.vertices = {{{-10, -10, -depth}, {10, -10, -depth},
                          {0, 10, -depth}}};
    triangle.normals = {{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    mesh->triangles.push_back(triangle);
    expand(mesh->bounds, triangleBounds(triangle));
    if (frontDepth > 0.0)
    {
        Triangle front = triangle;
        front.vertices = {{{-2, -2, -frontDepth}, {2, -2, -frontDepth},
                           {0, 2, -frontDepth}}};
        mesh->triangles.push_back(front);
        expand(mesh->bounds, triangleBounds(front));
    }
    mesh->order.resize(mesh->triangles.size());
    std::iota(mesh->order.begin(), mesh->order.end(), 0);
    buildBvh(*mesh, 0, mesh->triangles.size());
    input.instanceMeshes.push_back(mesh);
    MeshInstance instance;
    instance.mesh = mesh.get();
    instance.model = SurfaceModel::Lambert;
    instance.baseColor = {1, 1, 1};
    input.scene.instances.push_back(instance);
    return input;
}

double RenderPlanePixel(double depth, double nearClip, double farClip)
{
    RenderSession session(1);
    session.Start(PerspectivePlane(depth, nearClip, farClip));
    EXPECT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto film = session.Latest();
    if (!film) return -1;
    return film->direct[0].x;
}

void ExpectSamePixels(const FilmSnapshot& actual, const FilmSnapshot& expected)
{
    ASSERT_EQ(actual.direct.size(), expected.direct.size());
    ASSERT_EQ(actual.indirect.size(), expected.indirect.size());
    for (std::size_t i = 0; i < actual.direct.size(); ++i)
    {
        EXPECT_EQ(actual.direct[i].x, expected.direct[i].x) << i;
        EXPECT_EQ(actual.direct[i].y, expected.direct[i].y) << i;
        EXPECT_EQ(actual.direct[i].z, expected.direct[i].z) << i;
        EXPECT_EQ(actual.indirect[i].x, expected.indirect[i].x) << i;
        EXPECT_EQ(actual.indirect[i].y, expected.indirect[i].y) << i;
        EXPECT_EQ(actual.indirect[i].z, expected.indirect[i].z) << i;
    }
}

TEST(OfflineRenderSession, PerspectivePrimaryNearPlaneKeepsBoundaryAndRejectsCloserSurface)
{
    EXPECT_GT(RenderPlanePixel(1.0, 1.0, 3.0), 0.0);
    EXPECT_EQ(RenderPlanePixel(0.5, 1.0, 3.0), 0.0);
}

TEST(OfflineRenderSession, PerspectivePrimaryNearPlaneDoesNotOccludeValidSurface)
{
    RenderSession session(1);
    auto input = PerspectivePlane(2.0, 1.0, 3.0, 0.5);
    // 前景仍可遮挡阴影射线；这里让侧向光绕开前景，只验证主射线的近面。
    input.scene.toLight = normalize(Vec3{4, 0, 1});
    session.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_GT(session.Latest()->direct[0].x, 0.0);
}

TEST(OfflineRenderSession, PerspectivePrimaryFarPlaneUsesForwardDepthAtImageCorner)
{
    // 宽视场边角的 ray length > 2，面仍位于 forward 深度 2 上。
    EXPECT_GT(RenderPlanePixel(2.0, 1.0, 2.0), 0.0);
    EXPECT_EQ(RenderPlanePixel(2.5, 1.0, 2.0), 0.0);
}

TEST(OfflineRenderSession, PerspectivePrimaryClipAlsoLimitsVisibleAreaEmitter)
{
    auto input = PerspectivePlane(3.0, 1.0, 2.0);
    input.scene.instances.clear();
    input.instanceMeshes.clear();
    input.scene.directionalLight = false;
    input.scene.hasAreaEmitter = true;
    input.scene.lightCenter = {0, 0, -2.5};
    input.scene.lightWidth = input.scene.lightDepth = 10.0;
    input.scene.areaRadiance = 3.0;
    aimAreaLight(input.scene, {0, 0, 0});
    RenderSession clipped(1);
    clipped.Start(input);
    ASSERT_TRUE(WaitUntil(clipped, SessionState::Completed)) << clipped.Error();
    ASSERT_NE(clipped.Latest(), nullptr);
    EXPECT_EQ(clipped.Latest()->direct[0].x, 0.0);

    input.cameraClipFar = 3.0;
    RenderSession visible(1);
    visible.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(visible, SessionState::Completed)) << visible.Error();
    ASSERT_NE(visible.Latest(), nullptr);
    EXPECT_GT(visible.Latest()->direct[0].x, 0.0);
}

TEST(OfflineRenderSession, NearClippedAreaEmitterCannotHideSurfaceBehindIt)
{
    auto input = PerspectivePlane(2.0, 1.0, 3.0);
    input.scene.directionalLight = false;
    input.scene.hasAreaEmitter = true;
    input.scene.lightCenter = {0, 0, -0.5};
    input.scene.lightWidth = input.scene.lightDepth = 10.0;
    input.scene.areaRadiance = 3.0;
    aimAreaLight(input.scene, {0, 0, -2});
    RenderSession session(1);
    session.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_GT(session.Latest()->direct[0].x, 0.0);
}

TEST(OfflineRenderSession, PerspectiveClipValidationDoesNotCancelActiveGeneration)
{
    RenderSession session(1);
    const auto generation = session.Start(Input(1));
    for (const auto [nearClip, farClip] :
         {std::pair{0.0, 2.0}, std::pair{2.0, 1.0},
          std::pair{1.0, 1.0},
          std::pair{1.0, std::numeric_limits<double>::infinity()},
          std::pair{std::numeric_limits<double>::quiet_NaN(), 2.0}})
    {
        auto invalid = PerspectivePlane(1.0, nearClip, farClip);
        EXPECT_THROW(session.Start(std::move(invalid)), std::invalid_argument);
    }
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_EQ(session.Latest()->generation, generation);
}

TEST(OfflineRenderSession, PreviewAlsoClipsPerspectivePrimaryRays)
{
    auto input = PerspectivePlane(2.5, 1.0, 2.0);
    input.interactivePreview = true;
    input.previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    RenderSession session(1);
    session.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto preview = session.LatestPreview();
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(preview->validCount, 2u);
    EXPECT_EQ(preview->direct[0].x, 0.0);
    EXPECT_EQ(preview->direct[1].x, 0.0);
    EXPECT_EQ(session.Latest(), nullptr);
}

TEST(OfflineRenderSession, ConcurrentSheenSessionsMatchSerialEveryPixel)
{
    auto input = Input(3);
    input.width = 32;
    input.height = 24;
    input.bounces = 2;
    input.scene.instances[0].model = SurfaceModel::ClothSheen;
    input.scene.instances[0].roughness = 0.4;
    input.scene.instances[0].sheenColor = {0.95, 0.75, 0.65};

    // 两个会话可能同时首次请求 LUT；比较非 Lambert 路径的全像素结果。
    RenderSession first(1), second(4);
    first.Start(input);
    second.Start(input);
    ASSERT_TRUE(WaitUntil(first, SessionState::Completed)) << first.Error();
    ASSERT_TRUE(WaitUntil(second, SessionState::Completed)) << second.Error();

    RenderSession reference(1);
    reference.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(reference, SessionState::Completed)) << reference.Error();
    ASSERT_NE(first.Latest(), nullptr);
    ASSERT_NE(second.Latest(), nullptr);
    ASSERT_NE(reference.Latest(), nullptr);
    EXPECT_NE(first.Latest()->direct[12 * 32 + 16].x, 0.035);
    ExpectSamePixels(*first.Latest(), *reference.Latest());
    ExpectSamePixels(*second.Latest(), *reference.Latest());
}

TEST(OfflineRenderSession, ParallelRowsMatchSerialEveryPixel)
{
    auto input = Input(4);
    input.width = 53;
    input.height = 37;
    input.bounces = 2;
    input.cameraForward = normalize(Vec3{0, -1.7, -8});
    input.cameraUp = {0, 1, 0};

    RenderSession serial(1);
    serial.Start(input);
    ASSERT_TRUE(WaitUntil(serial, SessionState::Completed)) << serial.Error();
    RenderSession parallel(4);
    parallel.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(parallel, SessionState::Completed)) << parallel.Error();
    ASSERT_NE(serial.Latest(), nullptr);
    ASSERT_NE(parallel.Latest(), nullptr);
    EXPECT_EQ(parallel.Latest()->completedSamples, 4);
    ExpectSamePixels(*parallel.Latest(), *serial.Latest());
}

TEST(OfflineRenderSession, SampleBatchesPreservePixelSequence)
{
    RenderSession session;
    const auto first = session.Start(Input(3));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto batched = session.Latest();
    ASSERT_NE(batched, nullptr);
    EXPECT_EQ(batched->generation, first);
    EXPECT_EQ(batched->completedSamples, 3);

    const auto second = session.Start(Input(3));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto repeated = session.Latest();
    ASSERT_NE(repeated, nullptr);
    EXPECT_GT(second, first);
    for (std::size_t i = 0; i < repeated->direct.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(batched->direct[i].x, repeated->direct[i].x);
        EXPECT_DOUBLE_EQ(batched->direct[i].y, repeated->direct[i].y);
        EXPECT_DOUBLE_EQ(batched->indirect[i].z, repeated->indirect[i].z);
    }
}

TEST(OfflineRenderSession, NewRequestRejectsOlderPublication)
{
    RenderSession session;
    const auto prior = session.Start(Input(1));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed));
    auto longInput = Input(100000);
    longInput.width = 64;
    longInput.height = 64;
    const auto oldGeneration = session.Start(std::move(longInput));
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_EQ(session.Latest()->generation, prior);
    const auto newGeneration = session.Start(Input(2));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto latest = session.Latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_GT(newGeneration, oldGeneration);
    EXPECT_EQ(latest->generation, newGeneration);
    EXPECT_EQ(latest->completedSamples, 2);
}

TEST(OfflineRenderSession, HigherSampleTargetContinuesCompletedAccumulation)
{
    RenderSession session(4);
    auto input = Input(1);
    input.width = 53;
    input.height = 37;
    const auto generation = session.Start(input);
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto firstSample = session.Latest();
    ASSERT_NE(firstSample, nullptr);
    ASSERT_EQ(firstSample->completedSamples, 1);
    EXPECT_EQ(firstSample->targetSamples, 1);
    ASSERT_TRUE(session.ExtendTargetSamples(32));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto continued = session.Latest();
    ASSERT_NE(continued, nullptr);
    EXPECT_EQ(continued->generation, generation);
    EXPECT_EQ(continued->completedSamples, 32);
    EXPECT_EQ(continued->targetSamples, 32);
    EXPECT_EQ(firstSample->completedSamples, 1);
    EXPECT_EQ(firstSample->targetSamples, 1);
    EXPECT_FALSE(session.ExtendTargetSamples(32));

    RenderSession reference(1);
    input.targetSamples = 32;
    reference.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(reference, SessionState::Completed)) << reference.Error();
    const auto singleRun = reference.Latest();
    ASSERT_NE(singleRun, nullptr);
    ASSERT_EQ(continued->direct.size(), singleRun->direct.size());
    ASSERT_EQ(continued->indirect.size(), singleRun->indirect.size());
    for (std::size_t i = 0; i < continued->direct.size(); ++i)
    {
        // 首样本留在累积中；逐分量精确相等可捕获重播种或重算造成的差异。
        EXPECT_EQ(continued->direct[i].x, singleRun->direct[i].x);
        EXPECT_EQ(continued->direct[i].y, singleRun->direct[i].y);
        EXPECT_EQ(continued->direct[i].z, singleRun->direct[i].z);
        EXPECT_EQ(continued->indirect[i].x, singleRun->indirect[i].x);
        EXPECT_EQ(continued->indirect[i].y, singleRun->indirect[i].y);
        EXPECT_EQ(continued->indirect[i].z, singleRun->indirect[i].z);
    }
}

TEST(OfflineRenderSession, CanceledParallelSampleCannotPublishOverNewGeneration)
{
    RenderSession session(4);
    auto oldInput = Input(100000);
    oldInput.width = 512;
    oldInput.height = 256;
    const auto oldGeneration = session.Start(std::move(oldInput));
    ASSERT_TRUE(WaitUntil(session, SessionState::Rendering));
    session.Pause();
    EXPECT_EQ(session.State(), SessionState::Paused);
    session.Stop();
    EXPECT_EQ(session.State(), SessionState::Canceled);

    const auto newGeneration = session.Start(Input(1));
    ASSERT_TRUE(session.ExtendTargetSamples(5));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto published = session.Latest();
    ASSERT_NE(published, nullptr);
    EXPECT_GT(newGeneration, oldGeneration);
    EXPECT_EQ(published->generation, newGeneration);
    EXPECT_EQ(published->completedSamples, 5);
    EXPECT_EQ(published->targetSamples, 5);

    RenderSession reference(1);
    reference.Start(Input(5));
    ASSERT_TRUE(WaitUntil(reference, SessionState::Completed)) << reference.Error();
    ASSERT_NE(reference.Latest(), nullptr);
    ExpectSamePixels(*published, *reference.Latest());
}

TEST(OfflineRenderSession, ExtendedTargetPublishesImmutableMetadataImmediately)
{
    RenderSession session;
    session.Start(Input(2));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto previous = session.Latest();
    ASSERT_NE(previous, nullptr);
    ASSERT_TRUE(session.ExtendTargetSamples(5));
    const auto current = session.Latest();
    ASSERT_NE(current, nullptr);
    EXPECT_NE(current, previous);
    EXPECT_EQ(previous->targetSamples, 2);
    EXPECT_EQ(current->targetSamples, 5);
    EXPECT_EQ(current->completedSamples, previous->completedSamples);
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    EXPECT_EQ(session.Latest()->completedSamples, 5);
}

TEST(OfflineRenderSession, PausedCompletedSessionWaitsForResumeAfterExtension)
{
    RenderSession session;
    session.Start(Input(2));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    session.Pause();
    EXPECT_EQ(session.State(), SessionState::Paused);
    ASSERT_TRUE(session.ExtendTargetSamples(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_EQ(session.Latest()->completedSamples, 2);
    EXPECT_EQ(session.Latest()->targetSamples, 5);
    session.Resume();
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto continued = session.Latest();
    ASSERT_NE(continued, nullptr);
    EXPECT_EQ(continued->completedSamples, 5);

    RenderSession reference;
    reference.Start(Input(5));
    ASSERT_TRUE(WaitUntil(reference, SessionState::Completed)) << reference.Error();
    const auto singleRun = reference.Latest();
    ASSERT_NE(singleRun, nullptr);
    for (std::size_t i = 0; i < continued->direct.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(continued->direct[i].x, singleRun->direct[i].x);
        EXPECT_DOUBLE_EQ(continued->direct[i].y, singleRun->direct[i].y);
        EXPECT_DOUBLE_EQ(continued->indirect[i].z, singleRun->indirect[i].z);
    }
}

TEST(OfflineRenderSession, ResumeWithoutExtensionKeepsCompletedState)
{
    RenderSession session;
    session.Start(Input(1));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    session.Pause();
    session.Resume();
    EXPECT_EQ(session.State(), SessionState::Completed);
    EXPECT_EQ(session.Latest()->completedSamples, 1);
}

TEST(OfflineRenderSession, PauseKeepsAccumulationAndStopKeepsLastImage)
{
    RenderSession session;
    auto input = Input(100000);
    input.width = 32;
    input.height = 32;
    session.Start(std::move(input));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!session.Latest() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_NE(session.Latest(), nullptr);
    session.Pause();
    const auto before = session.Latest()->completedSamples;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto after = session.Latest()->completedSamples;
    EXPECT_LE(after, before + 1); // 正在计算的完整 sample 最多再发布一次。
    session.Resume();
    const auto resumeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.Latest()->completedSamples <= after &&
           std::chrono::steady_clock::now() < resumeDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GT(session.Latest()->completedSamples, after);
    session.Stop();
    EXPECT_EQ(session.State(), SessionState::Canceled);
    EXPECT_NE(session.Latest(), nullptr);
}

TEST(OfflineRenderSession, StopPreventsFurtherPublication)
{
    RenderSession session;
    auto input = Input(100000);
    input.width = 32;
    input.height = 32;
    session.Start(std::move(input));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!session.Latest() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_NE(session.Latest(), nullptr);
    session.Stop();
    const auto retained = session.Latest();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(session.State(), SessionState::Canceled);
    EXPECT_EQ(session.Latest(), retained);
}

TEST(OfflineRenderSession, RejectsUnsupportedLightCombinationWithoutCancelingActive)
{
    RenderSession session;
    const auto current = session.Start(Input(2));
    auto unsupported = Input(1);
    unsupported.scene.hasAreaEmitter = true;
    EXPECT_THROW(session.Start(std::move(unsupported)), std::invalid_argument);
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    ASSERT_NE(session.Latest(), nullptr);
    EXPECT_EQ(session.Latest()->generation, current);
}

TEST(OfflineRenderSession, ExpiredPreviewPublishesImmutableZeroValidMask)
{
    RenderSession session(4);
    const auto generation = session.Start(PreviewInput(80, 45,
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto preview = session.LatestPreview();
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->generation, generation);
    EXPECT_EQ(preview->width, 80);
    EXPECT_EQ(preview->height, 45);
    EXPECT_EQ(preview->validCount, 0u);
    EXPECT_EQ(preview->valid.size(), 80u * 45u);
    EXPECT_EQ(preview->direct.size(), preview->valid.size());
    EXPECT_EQ(preview->indirect.size(), preview->valid.size());
    for (const auto valid : preview->valid) EXPECT_EQ(valid, 0);
    EXPECT_EQ(session.Latest(), nullptr);
    EXPECT_FALSE(session.ExtendTargetSamples(2));
}

TEST(OfflineRenderSession, PreviewRequiresExplicitAbsoluteDeadline)
{
    RenderSession session;
    auto input = Input(1);
    input.interactivePreview = true;
    EXPECT_THROW(session.Start(std::move(input)), std::invalid_argument);
    EXPECT_EQ(session.State(), SessionState::Idle);
}

TEST(OfflineRenderSession, DeadlinePublishesScatteredPartialPixelsWithoutFullFilm)
{
    RenderSession warmup(1);
    warmup.Start(Input(1));
    ASSERT_TRUE(WaitUntil(warmup, SessionState::Completed)) << warmup.Error();

    RenderSession session(4);
    const int width = 2048, height = 1536;
    const auto generation = session.Start(PreviewInput(width, height,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(80)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto preview = session.LatestPreview();
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->generation, generation);
    ASSERT_EQ(preview->valid.size(), static_cast<std::size_t>(width) * height);
    ASSERT_EQ(preview->direct.size(), preview->valid.size());
    ASSERT_EQ(preview->indirect.size(), preview->valid.size());
    EXPECT_GT(preview->validCount, 0u);
    EXPECT_LT(preview->validCount, preview->valid.size());
    std::size_t counted = 0;
    for (std::size_t index = 0; index < preview->valid.size(); ++index)
    {
        counted += preview->valid[index] != 0;
        if (preview->valid[index] == 0)
        {
            EXPECT_EQ(preview->direct[index].x, 0.0);
            EXPECT_EQ(preview->indirect[index].x, 0.0);
        }
    }
    EXPECT_EQ(counted, preview->validCount);
    EXPECT_EQ(session.Latest(), nullptr);
}

TEST(OfflineRenderSession, PreviewCancellationCannotPublishOverNextGeneration)
{
    RenderSession session(4);
    const auto oldGeneration = session.Start(PreviewInput(1536, 1024,
        std::chrono::steady_clock::now() + std::chrono::seconds(3)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Rendering)) << session.Error();
    const auto newGeneration = session.Start(PreviewInput(4, 3,
        std::chrono::steady_clock::now() + std::chrono::seconds(3)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto preview = session.LatestPreview();
    ASSERT_NE(preview, nullptr);
    EXPECT_GT(newGeneration, oldGeneration);
    EXPECT_EQ(preview->generation, newGeneration);
    EXPECT_EQ(preview->validCount, 12u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(session.LatestPreview(), preview);
}

TEST(OfflineRenderSession, PausedPreviewDoesNotPublishUntilResumed)
{
    RenderSession session(1);
    const auto generation = session.Start(PreviewInput(1536, 1024,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150)));
    // 在 Preparing 即暂停，不依赖冷启动 LUT/worker 的调度时间。
    session.Pause();
    ASSERT_EQ(session.State(), SessionState::Paused);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    EXPECT_EQ(session.State(), SessionState::Paused);
    EXPECT_EQ(session.LatestPreview(), nullptr);
    session.Resume();
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    ASSERT_NE(session.LatestPreview(), nullptr);
    EXPECT_EQ(session.LatestPreview()->generation, generation);
}

TEST(OfflineRenderSession, PreviewDoesNotReplacePriorCompleteSample)
{
    RenderSession session(2);
    session.Start(Input(2));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    const auto film = session.Latest();
    ASSERT_NE(film, nullptr);
    const auto generation = session.Start(PreviewInput(4, 3,
        std::chrono::steady_clock::now() + std::chrono::seconds(3)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    EXPECT_EQ(session.Latest(), film);
    const auto preview = session.LatestPreview();
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->generation, generation);
    EXPECT_EQ(preview->validCount, 12u);

    session.Start(Input(1));
    ASSERT_TRUE(WaitUntil(session, SessionState::Completed)) << session.Error();
    EXPECT_NE(session.Latest(), film);
    EXPECT_EQ(session.LatestPreview(), preview);
}

TEST(OfflineRenderSession, FullyValidPreviewMatchesFirstFormalSample)
{
    auto input = Input(1);
    input.width = 53;
    input.height = 37;
    input.bounces = 2;
    input.cameraForward = normalize(Vec3{0, -1.7, -8});
    input.cameraUp = {0, 1, 0};

    RenderSession formal(1);
    formal.Start(input);
    ASSERT_TRUE(WaitUntil(formal, SessionState::Completed)) << formal.Error();
    const auto film = formal.Latest();
    ASSERT_NE(film, nullptr);

    input.interactivePreview = true;
    input.previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    RenderSession previewSession(4);
    previewSession.Start(std::move(input));
    ASSERT_TRUE(WaitUntil(previewSession, SessionState::Completed))
        << previewSession.Error();
    const auto preview = previewSession.LatestPreview();
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(preview->validCount, film->direct.size());
    ASSERT_EQ(preview->valid.size(), film->direct.size());
    for (std::size_t i = 0; i < preview->valid.size(); ++i)
    {
        EXPECT_EQ(preview->valid[i], 1) << i;
        EXPECT_EQ(preview->direct[i].x, film->direct[i].x) << i;
        EXPECT_EQ(preview->direct[i].y, film->direct[i].y) << i;
        EXPECT_EQ(preview->direct[i].z, film->direct[i].z) << i;
        EXPECT_EQ(preview->indirect[i].x, film->indirect[i].x) << i;
        EXPECT_EQ(preview->indirect[i].y, film->indirect[i].y) << i;
        EXPECT_EQ(preview->indirect[i].z, film->indirect[i].z) << i;
    }
    EXPECT_EQ(previewSession.Latest(), nullptr);
}

TEST(OfflineRenderSession, StoppedPreviewCannotPublishAfterCancellation)
{
    RenderSession session(4);
    session.Start(PreviewInput(2048, 1536,
        std::chrono::steady_clock::now() + std::chrono::seconds(3)));
    ASSERT_TRUE(WaitUntil(session, SessionState::Rendering)) << session.Error();
    session.Stop();
    EXPECT_EQ(session.State(), SessionState::Canceled);
    const auto retained = session.LatestPreview();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(session.LatestPreview(), retained);
    EXPECT_EQ(session.State(), SessionState::Canceled);
}
} // namespace
