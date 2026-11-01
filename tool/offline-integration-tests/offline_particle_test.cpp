#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "particles/particleAsset.h"
#include "render/offline/offlineParticleCompositor.h"
#include "render/offline/offlineParticleSimulation.h"
#include "render/offline/offlineRenderCoordinator.h"

namespace
{

constexpr int kGoldenWidth = 64;
constexpr int kGoldenHeight = 48;
constexpr int kGoldenGridWidth = 8;
constexpr int kGoldenGridHeight = 6;

std::filesystem::path DataDirectory()
{
    return std::filesystem::path(VL_PARTICLE_TEST_DATA_DIR);
}

std::filesystem::path GoldenPath()
{
    return DataDirectory() / "goldens" / "particle_billboard_64x48.txt";
}

VL::CameraSnapshot MakeCamera(
    int width,
    int height,
    const Eigen::Vector3f& position)
{
    VL::CameraSnapshot camera;
    camera.position = position;
    camera.right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    camera.up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    camera.forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    camera.horizontalFovDegrees = 60.0f;
    camera.clipNear = 0.1f;
    camera.clipFar = 10000.0f;
    camera.view = Eigen::Matrix4f::Identity();
    camera.view(0, 3) = -position.x();
    camera.view(1, 3) = -position.y();
    camera.view(2, 3) = -position.z();

    const float nearPlane = -camera.clipNear;
    const float farPlane = -camera.clipFar;
    const float fovRadians =
        camera.horizontalFovDegrees * 3.14159265358979323846f / 180.0f;
    const float k = -1.0f / std::tan(fovRadians / 2.0f);
    const float aspect =
        static_cast<float>(width) / static_cast<float>(height);
    Eigen::Matrix4f projection;
    projection <<
        -k, 0.0f, 0.0f, 0.0f,
        0.0f, -aspect * k, 0.0f, 0.0f,
        0.0f, 0.0f, -(nearPlane + farPlane) / (nearPlane - farPlane),
            2.0f * nearPlane * farPlane / (nearPlane - farPlane),
        0.0f, 0.0f, -1.0f, 0.0f;
    Eigen::Matrix4f ndc = Eigen::Matrix4f::Identity();
    ndc(1, 1) = -1.0f;
    ndc(2, 2) = -0.5f;
    ndc(2, 3) = 0.5f;
    camera.projection = ndc * projection;
    camera.viewProjection = camera.projection * camera.view;
    return camera;
}

VL::OfflineParticleCamera OfflineCamera(
    int width,
    int height,
    const Eigen::Vector3f& position)
{
    return VL::MakeOfflineParticleCamera(
        MakeCamera(width, height, position), width, height);
}

VL::ParticleSystemAsset LoadGoldenAsset()
{
    const std::filesystem::path path =
        DataDirectory() / "fixtures" / "PS_offline_particle_golden.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("Missing particle test fixture: " +
            path.string());
    }
    nlohmann::json json;
    file >> json;
    return VL::ParticleAssetLoader::ParseJson(
        json, "fixtures/PS_offline_particle_golden.json");
}

VL::OfflineParticleSystemInput GoldenInput()
{
    VL::ParticleSystemAsset asset = LoadGoldenAsset();
    VL::OfflineParticleSystemInput input;
    input.name = asset.name;
    input.assetPath = asset.sourcePath;
    input.asset = std::move(asset);
    input.seed = 7;
    return input;
}

VL::ParticleSystemAsset ContinuousAsset()
{
    VL::ParticleSystemAsset asset;
    asset.name = "PS_continuous_determinism";
    asset.sourcePath = "test/PS_continuous_determinism.json";
    VL::ParticleEmitterAsset emitter;
    emitter.name = "spawn";
    emitter.maxParticles = 64;
    emitter.spawnRate = 90.0f;
    emitter.lifetimeMin = 1.0f;
    emitter.lifetimeMax = 2.0f;
    emitter.shapeType = "box";
    emitter.shapeExtent = Eigen::Vector3f(20.0f, 20.0f, 20.0f);
    emitter.velocityMin = Eigen::Vector3f(-40.0f, 20.0f, -40.0f);
    emitter.velocityMax = Eigen::Vector3f(40.0f, 120.0f, 40.0f);
    emitter.gravity = Eigen::Vector3f(0.0f, -100.0f, 0.0f);
    emitter.drag = 0.3f;
    emitter.sizeStart = 10.0f;
    emitter.sizeEnd = 4.0f;
    emitter.additive = true;
    asset.emitters.push_back(emitter);
    return asset;
}

VL::ParticleSystemAsset BurstAsset()
{
    VL::ParticleSystemAsset asset;
    asset.name = "PS_burst_development";
    asset.sourcePath = "test/PS_burst_development.json";
    VL::ParticleEmitterAsset emitter;
    emitter.name = "burst";
    emitter.maxParticles = 32;
    emitter.spawnRate = 32.0f;
    emitter.burst = true;
    emitter.lifetimeMin = 2.0f;
    emitter.lifetimeMax = 2.0f;
    emitter.shapeType = "sphere";
    emitter.shapeRadius = 0.0f;
    emitter.velocityMin = Eigen::Vector3f(0.0f, 100.0f, 0.0f);
    emitter.velocityMax = Eigen::Vector3f(0.0f, 100.0f, 0.0f);
    emitter.gravity = Eigen::Vector3f::Zero();
    emitter.sizeStart = 10.0f;
    emitter.sizeEnd = 10.0f;
    emitter.additive = true;
    asset.emitters.push_back(emitter);
    return asset;
}

VL::OfflineParticleSystemInput InputFromAsset(
    VL::ParticleSystemAsset asset,
    uint32_t seed)
{
    VL::OfflineParticleSystemInput input;
    input.name = asset.name;
    input.assetPath = asset.sourcePath;
    input.asset = std::move(asset);
    input.seed = seed;
    return input;
}

void ExpectSameParticles(
    const VL::OfflineParticleSystemState& left,
    const VL::OfflineParticleSystemState& right)
{
    ASSERT_EQ(left.emitters.size(), right.emitters.size());
    for (size_t emitter = 0; emitter < left.emitters.size(); ++emitter)
    {
        const auto& leftParticles = left.emitters[emitter].particles;
        const auto& rightParticles = right.emitters[emitter].particles;
        ASSERT_EQ(leftParticles.size(), rightParticles.size());
        for (size_t index = 0; index < leftParticles.size(); ++index)
        {
            const VL::OfflineParticle& a = leftParticles[index];
            const VL::OfflineParticle& b = rightParticles[index];
            EXPECT_EQ(a.positionAge, b.positionAge) << index;
            EXPECT_EQ(a.velocityLife, b.velocityLife) << index;
            EXPECT_EQ(a.history0, b.history0) << index;
            EXPECT_EQ(a.history1, b.history1) << index;
            EXPECT_EQ(a.history2, b.history2) << index;
        }
    }
}

bool ParticlesDiffer(
    const VL::OfflineParticleSystemState& left,
    const VL::OfflineParticleSystemState& right)
{
    if (left.emitters.size() != right.emitters.size()) return true;
    for (size_t emitter = 0; emitter < left.emitters.size(); ++emitter)
    {
        const auto& leftParticles = left.emitters[emitter].particles;
        const auto& rightParticles = right.emitters[emitter].particles;
        if (leftParticles.size() != rightParticles.size()) return true;
        for (size_t index = 0; index < leftParticles.size(); ++index)
        {
            if (leftParticles[index].positionAge !=
                    rightParticles[index].positionAge ||
                leftParticles[index].velocityLife !=
                    rightParticles[index].velocityLife)
            {
                return true;
            }
        }
    }
    return false;
}

double MeanPositionLength(const VL::OfflineParticleSystemState& state)
{
    double total = 0.0;
    size_t count = 0;
    for (const auto& emitter : state.emitters)
    {
        for (const auto& particle : emitter.particles)
        {
            if (!particle.Alive()) continue;
            total += particle.positionAge.head<3>().cast<double>().norm();
            ++count;
        }
    }
    return count == 0 ? 0.0 : total / static_cast<double>(count);
}

size_t AliveCount(const VL::OfflineParticleSystemState& state)
{
    size_t count = 0;
    for (const auto& emitter : state.emitters)
        for (const auto& particle : emitter.particles)
            if (particle.Alive()) ++count;
    return count;
}

VL::OfflineParticleSystemState SingleParticleState(
    bool additive,
    const Eigen::Vector3f& position)
{
    VL::OfflineParticleSystemInput input;
    input.name = additive ? "additive" : "alpha";
    input.asset.name = input.name;
    input.asset.sourcePath = "test/single.json";
    input.seed = 1;
    VL::ParticleEmitterAsset emitter;
    emitter.name = "single";
    emitter.maxParticles = 1;
    emitter.spawnRate = 1.0f;
    emitter.burst = true;
    emitter.lifetimeMin = 10.0f;
    emitter.lifetimeMax = 10.0f;
    emitter.shapeRadius = 0.0f;
    emitter.velocityMin = Eigen::Vector3f::Zero();
    emitter.velocityMax = Eigen::Vector3f::Zero();
    emitter.gravity = Eigen::Vector3f::Zero();
    emitter.sizeStart = 20.0f;
    emitter.sizeEnd = 20.0f;
    emitter.colorStart = Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    emitter.colorEnd = Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    emitter.softness = 0.5f;
    emitter.additive = additive;
    emitter.sortParticles = !additive;
    input.asset.emitters.push_back(emitter);

    VL::OfflineParticleSystemState state =
        VL::CreateOfflineParticleSystemState(input);
    state.emitters[0].prewarm = false;
    state.emitters[0].elapsedSeconds = 0.5f;
    VL::OfflineParticle& particle = state.emitters[0].particles[0];
    particle.positionAge = Eigen::Vector4f(
        position.x(), position.y(), position.z(), 0.5f);
    particle.velocityLife =
        Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    particle.history0 = particle.positionAge;
    particle.history1 = particle.positionAge;
    particle.history2 = particle.positionAge;
    return state;
}

struct GridValue
{
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

std::vector<GridValue> DownsampleGolden(
    const VL::OfflineParticleOverlay& overlay)
{
    std::vector<GridValue> result;
    result.reserve(kGoldenGridWidth * kGoldenGridHeight);
    const int blockWidth = overlay.width / kGoldenGridWidth;
    const int blockHeight = overlay.height / kGoldenGridHeight;
    for (int gridY = 0; gridY < kGoldenGridHeight; ++gridY)
    {
        for (int gridX = 0; gridX < kGoldenGridWidth; ++gridX)
        {
            GridValue value;
            int samples = 0;
            for (int y = gridY * blockHeight;
                 y < (gridY + 1) * blockHeight; ++y)
            {
                for (int x = gridX * blockWidth;
                     x < (gridX + 1) * blockWidth; ++x)
                {
                    const VL::Offline::Vec3& pixel =
                        overlay.color[static_cast<size_t>(y) *
                            overlay.width + x];
                    value.red += pixel.x;
                    value.green += pixel.y;
                    value.blue += pixel.z;
                    ++samples;
                }
            }
            if (samples > 0)
            {
                value.red /= samples;
                value.green /= samples;
                value.blue /= samples;
            }
            result.push_back(value);
        }
    }
    return result;
}

std::string GoldenText(const std::vector<GridValue>& values)
{
    std::ostringstream stream;
    stream << "# VL offline particle golden: 64x48 -> 8x6 average linear RGB\n";
    stream << "# produced by OfflineParticle.GoldenRegression\n";
    stream << std::fixed << std::setprecision(6);
    size_t index = 0;
    for (int gridY = 0; gridY < kGoldenGridHeight; ++gridY)
    {
        for (int gridX = 0; gridX < kGoldenGridWidth; ++gridX)
        {
            stream << gridX << ' ' << gridY << ' '
                << values[index].red << ' '
                << values[index].green << ' '
                << values[index].blue << '\n';
            ++index;
        }
    }
    return stream.str();
}

std::shared_ptr<const VL::OfflinePublishedResult> WaitForSamples(
    VL::OfflineRenderCoordinator& coordinator,
    int samples)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = coordinator.Capture();
        if (result && result->film->completedSamples == samples &&
            result->manifest.at("complete").get<bool>())
        {
            return result;
        }
        if (coordinator.State() == VL::Offline::SessionState::Failed)
        {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

} // namespace

TEST(OfflineParticleSimulation, FixtureUsesParticleAssetLoader)
{
    const VL::ParticleSystemAsset asset = LoadGoldenAsset();
    ASSERT_EQ(asset.name, "PS_offline_particle_golden");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(asset.emitters[0].maxParticles, 1u);
    EXPECT_TRUE(asset.emitters[0].burst);
    EXPECT_FALSE(asset.emitters[0].additive);
    EXPECT_TRUE(asset.emitters[0].sortParticles);
}

TEST(OfflineParticleSimulation, FixedStepIsDeterministic)
{
    const VL::OfflineParticlePreviewPolicy policy =
        VL::OfflineParticlePreviewDefaults();
    const VL::OfflineParticleSystemInput input =
        InputFromAsset(ContinuousAsset(), 1234u);
    const VL::OfflineParticleSystemState first =
        VL::SimulateOfflineParticles(
            input, policy, Eigen::Vector3f(0.0f, 0.0f, 300.0f));
    const VL::OfflineParticleSystemState second =
        VL::SimulateOfflineParticles(
            input, policy, Eigen::Vector3f(0.0f, 0.0f, 300.0f));
    ExpectSameParticles(first, second);
    EXPECT_GT(AliveCount(first), 0u);

    const VL::OfflineParticleSystemInput otherSeed =
        InputFromAsset(ContinuousAsset(), 5678u);
    const VL::OfflineParticleSystemState other =
        VL::SimulateOfflineParticles(
            otherSeed, policy, Eigen::Vector3f(0.0f, 0.0f, 300.0f));
    EXPECT_TRUE(ParticlesDiffer(first, other));
}

TEST(OfflineParticleSimulation, SnapshotOrderDoesNotChangeSimulationOrder)
{
    VL::WorldSnapshot ordered;
    ordered.worldGeneration = 1;
    VL::WorldSnapshot reversed;
    reversed.worldGeneration = 1;
    VL::ParticleSystemAsset asset = ContinuousAsset();
    for (const char* name : {"zeta", "alpha"})
    {
        VL::ParticleSystemSnapshot system;
        system.name = name;
        system.assetPath = "test/" + std::string(name) + ".json";
        system.asset = asset;
        ordered.particleSystems.push_back(system);
    }
    reversed.particleSystems.push_back(ordered.particleSystems[1]);
    reversed.particleSystems.push_back(ordered.particleSystems[0]);

    const auto first = VL::BuildOfflineParticleInputs(ordered, 5u);
    const auto second = VL::BuildOfflineParticleInputs(reversed, 5u);
    ASSERT_EQ(first.size(), 2u);
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(first[0].name, "alpha");
    EXPECT_EQ(first[1].name, "zeta");
    EXPECT_EQ(second[0].name, first[0].name);
    EXPECT_EQ(second[1].name, first[1].name);
    EXPECT_EQ(second[0].seed, first[0].seed);
    EXPECT_EQ(second[1].seed, first[1].seed);
}

TEST(OfflineParticleSimulation, PreviewPolicyDevelopsBurstParticles)
{
    const VL::OfflineParticleSystemInput input =
        InputFromAsset(BurstAsset(), 9u);
    VL::OfflineParticlePreviewPolicy oneStep =
        VL::OfflineParticlePreviewDefaults();
    oneStep.previewSeconds = oneStep.fixedDeltaSeconds;
    const VL::OfflineParticlePreviewPolicy full =
        VL::OfflineParticlePreviewDefaults();
    ASSERT_EQ(VL::ComputeOfflineParticleStepCount(full), 30u);

    const VL::OfflineParticleSystemState one =
        VL::SimulateOfflineParticles(
            input, oneStep, Eigen::Vector3f(0.0f, 0.0f, 300.0f));
    const VL::OfflineParticleSystemState developed =
        VL::SimulateOfflineParticles(
            input, full, Eigen::Vector3f(0.0f, 0.0f, 300.0f));
    ASSERT_EQ(AliveCount(one), AliveCount(developed));
    EXPECT_GT(AliveCount(one), 0u);
    // 30 个固定步后粒子应明显离开发射点；1 步只移动 dt 的量。
    EXPECT_GT(MeanPositionLength(developed),
        10.0 * MeanPositionLength(one));
}

TEST(OfflineParticleCompositor, BoundsAndAlphaCoverage)
{
    const VL::OfflineParticleCamera camera =
        OfflineCamera(64, 48, Eigen::Vector3f(0.0f, 0.0f, 100.0f));
    const VL::OfflineParticleSystemState state = SingleParticleState(
        false, Eigen::Vector3f::Zero());
    const std::vector<VL::OfflineParticleSystemState> systems{state};
    const VL::OfflineParticleOverlay overlay =
        VL::CompositeOfflineParticles(systems, camera);
    ASSERT_FALSE(overlay.Empty());
    const size_t center =
        static_cast<size_t>(24) * overlay.width + 32;
    EXPECT_LT(overlay.transmittance[center], 0.999f);
    EXPECT_GT(overlay.color[center].x, 0.0);
    EXPECT_FLOAT_EQ(overlay.transmittance[0], 1.0f);
    EXPECT_DOUBLE_EQ(overlay.color[0].x, 0.0);

    // 视锥外的粒子不写任何像素。
    const VL::OfflineParticleSystemState outside = SingleParticleState(
        false, Eigen::Vector3f(100000.0f, 0.0f, 0.0f));
    const std::vector<VL::OfflineParticleSystemState> outsideSystems{outside};
    const VL::OfflineParticleOverlay outsideOverlay =
        VL::CompositeOfflineParticles(outsideSystems, camera);
    for (const float transmittance : outsideOverlay.transmittance)
        EXPECT_FLOAT_EQ(transmittance, 1.0f);
    for (const VL::Offline::Vec3& color : outsideOverlay.color)
        EXPECT_DOUBLE_EQ(color.x, 0.0);
}

TEST(OfflineParticleCompositor, AlphaAndAdditiveBlendModes)
{
    const VL::OfflineParticleCamera camera =
        OfflineCamera(64, 48, Eigen::Vector3f(0.0f, 0.0f, 100.0f));
    const std::vector<VL::OfflineParticleSystemState> alphaSystems{
        SingleParticleState(false, Eigen::Vector3f::Zero())};
    const std::vector<VL::OfflineParticleSystemState> additiveSystems{
        SingleParticleState(true, Eigen::Vector3f::Zero())};
    const VL::OfflineParticleOverlay alpha =
        VL::CompositeOfflineParticles(alphaSystems, camera);
    const VL::OfflineParticleOverlay additive =
        VL::CompositeOfflineParticles(additiveSystems, camera);
    const size_t center = static_cast<size_t>(24) * 64 + 32;
    EXPECT_LT(alpha.transmittance[center], 1.0f);
    // additive 只累加颜色，背景可见比例必须保持 1。
    for (const float transmittance : additive.transmittance)
        EXPECT_FLOAT_EQ(transmittance, 1.0f);
    EXPECT_GT(additive.color[center].x, 0.0);

    // 预乘颜色 + 背景 * transmittance 的精确公式。
    VL::OfflineParticleOverlay synthetic;
    synthetic.width = 2;
    synthetic.height = 1;
    synthetic.color.assign(2, VL::Offline::Vec3{});
    synthetic.color[0] = VL::Offline::Vec3{0.5, 0.0, 0.0};
    synthetic.transmittance = {0.5f, 1.0f};
    const std::vector<VL::Offline::Vec3> background{
        VL::Offline::Vec3{0.2, 0.4, 0.6},
        VL::Offline::Vec3{0.2, 0.4, 0.6}};
    const std::vector<VL::Offline::Vec3> composed =
        VL::CompositeOfflineParticleOverBackground(background, synthetic);
    ASSERT_EQ(composed.size(), 2u);
    EXPECT_NEAR(composed[0].x, 0.6, 1.0e-6);
    EXPECT_NEAR(composed[0].y, 0.2, 1.0e-6);
    EXPECT_NEAR(composed[0].z, 0.3, 1.0e-6);
    EXPECT_DOUBLE_EQ(composed[1].x, 0.2);
    EXPECT_DOUBLE_EQ(composed[1].y, 0.4);
    EXPECT_DOUBLE_EQ(composed[1].z, 0.6);
}

TEST(OfflineParticleRendering, PixelGoldenRegression)
{
    const VL::OfflineParticleSystemInput input = GoldenInput();
    const VL::OfflineParticlePreviewPolicy policy =
        VL::OfflineParticlePreviewDefaults();
    const Eigen::Vector3f cameraPosition(0.0f, 0.0f, 100.0f);
    const VL::OfflineParticleSystemState state =
        VL::SimulateOfflineParticles(input, policy, cameraPosition);
    ASSERT_EQ(AliveCount(state), 1u);
    const VL::OfflineParticleCamera camera =
        OfflineCamera(kGoldenWidth, kGoldenHeight, cameraPosition);
    const std::vector<VL::OfflineParticleSystemState> systems{state};
    const VL::OfflineParticleOverlay overlay =
        VL::CompositeOfflineParticles(systems, camera);
    ASSERT_FALSE(overlay.Empty());
    const std::vector<GridValue> actual = DownsampleGolden(overlay);

    const std::filesystem::path goldenPath = GoldenPath();
    if (!std::filesystem::exists(goldenPath))
    {
        std::filesystem::create_directories(goldenPath.parent_path());
        std::ofstream output(goldenPath);
        output << GoldenText(actual);
        output.flush();
        FAIL() << "Wrote particle golden " << goldenPath.string()
               << "; rerun the test to verify the checked-in values.";
    }

    std::ifstream inputFile(goldenPath);
    ASSERT_TRUE(inputFile.is_open()) << goldenPath.string();
    std::vector<GridValue> expected;
    std::string line;
    while (std::getline(inputFile, line))
    {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream stream(line);
        int gridX = 0;
        int gridY = 0;
        GridValue value;
        if (!(stream >> gridX >> gridY >> value.red >> value.green >>
              value.blue))
        {
            FAIL() << "Malformed golden line: " << line;
        }
        ASSERT_EQ(gridX, static_cast<int>(expected.size()) %
            kGoldenGridWidth);
        ASSERT_EQ(gridY, static_cast<int>(expected.size()) /
            kGoldenGridWidth);
        expected.push_back(value);
    }
    ASSERT_EQ(expected.size(), actual.size());
    constexpr double tolerance = 2.0e-4;
    for (size_t index = 0; index < actual.size(); ++index)
    {
        EXPECT_NEAR(actual[index].red, expected[index].red, tolerance)
            << "cell " << index;
        EXPECT_NEAR(actual[index].green, expected[index].green, tolerance)
            << "cell " << index;
        EXPECT_NEAR(actual[index].blue, expected[index].blue, tolerance)
            << "cell " << index;
    }
}

TEST(OfflineParticleRendering, CoordinatorPublishesAndExportsParticleLayer)
{
    VL::WorldSnapshot snapshot;
    snapshot.worldGeneration = 11;
    snapshot.environment.type = VL::EnvironmentType::None;
    snapshot.camera = MakeCamera(16, 12, Eigen::Vector3f(0.0f, 0.0f, 100.0f));
    VL::ParticleSystemSnapshot system;
    system.particleSystemId = 42;
    system.name = "preview_particle";
    system.assetPath = "fixtures/PS_offline_particle_golden.json";
    system.asset = LoadGoldenAsset();
    snapshot.particleSystems.push_back(std::move(system));

    auto assets = std::make_shared<VL::OfflineWorldAssets>();
    assets->worldGeneration = snapshot.worldGeneration;
    assets->scenePath = "fixtures/SC_offline_particle_test.json";

    VL::OfflineRenderCoordinator coordinator;
    VL::OfflineRenderSettings settings{16, 12, 1, 0, 1, 3, false};
    coordinator.Observe(
        std::make_shared<const VL::WorldSnapshot>(snapshot),
        assets,
        settings);
    const auto result = WaitForSamples(coordinator, 1);
    ASSERT_NE(result, nullptr) << coordinator.Error();
    ASSERT_NE(result->particles, nullptr);
    EXPECT_EQ(result->particles->width, 16);
    EXPECT_EQ(result->particles->height, 12);
    bool covered = false;
    for (const float transmittance : result->particles->transmittance)
        if (transmittance < 0.999f) covered = true;
    EXPECT_TRUE(covered);
    ASSERT_TRUE(result->manifest.contains("particles"));
    EXPECT_EQ(result->manifest.at("particles").at("steps"), 30);
    EXPECT_EQ(result->manifest.at("particles").at("seed"), 3);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("vl_particle_export_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    VL::ExportOfflineResult(*result, output, 1.0f);
    EXPECT_TRUE(std::filesystem::exists(output / "combined.pfm"));
    EXPECT_TRUE(std::filesystem::exists(output / "particles.pfm"));
    std::ifstream manifest(output / "manifest.json");
    ASSERT_TRUE(manifest.is_open());
    nlohmann::json parsed;
    manifest >> parsed;
    EXPECT_TRUE(parsed.contains("particleComposite"));
    EXPECT_TRUE(parsed.at("outputs").contains("particles.pfm"));
    manifest.close();
    std::filesystem::remove_all(output);
}

