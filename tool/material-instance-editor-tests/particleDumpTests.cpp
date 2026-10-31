#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "particles/particleDump.h"

namespace
{

using namespace VL;

ParticleDumpSample MakeSample(
    uint32_t index,
    float age,
    float lifetime)
{
    ParticleDumpSample sample;
    sample.index = index;
    sample.position = Eigen::Vector3f(
        static_cast<float>(index),
        static_cast<float>(index) + 1.0f,
        static_cast<float>(index) + 2.0f);
    sample.age = age;
    sample.velocity = Eigen::Vector3f(3.0f, 4.0f, 5.0f);
    sample.lifetime = lifetime;
    return sample;
}

std::vector<ParticleDumpEmitter> MakeEmitters()
{
    ParticleDumpEmitter fire;
    fire.key = "fire";
    fire.maxParticles = 64;
    fire.aliveCount = 2;
    fire.sortParticles = true;
    fire.boundsCenter = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    fire.boundsRadius = 12.5f;
    fire.elapsedSeconds = 4.5f;
    fire.particles = {
        MakeSample(0, 0.2f, 1.0f),
        MakeSample(1, 1.0f, 1.0f),
        MakeSample(2, 0.4f, 1.0f)};

    ParticleDumpEmitter smoke;
    smoke.key = "smoke";
    smoke.maxParticles = 32;
    smoke.aliveCount = 1;
    smoke.culled = true;
    smoke.localSpace = true;
    smoke.boundsCenter = Eigen::Vector3f(-1.0f, 0.0f, 0.0f);
    smoke.boundsRadius = 100.0f;
    smoke.elapsedSeconds = 2.0f;
    smoke.particles = {MakeSample(9, 0.5f, 2.0f)};
    return {smoke, fire};
}

} // namespace

TEST(ParticleDump, SerializesEmitterStatisticsAndTotals)
{
    const nlohmann::ordered_json dump =
        BuildParticleDumpJson(MakeEmitters(), 4);

    EXPECT_EQ(dump.at("kind"), "particleDump");
    EXPECT_EQ(dump.at("version"), 1);
    EXPECT_EQ(dump.at("maxPerEmitter"), 4);
    EXPECT_EQ(dump.at("totalEmitters"), 2);
    EXPECT_EQ(dump.at("visibleEmitters"), 1);
    EXPECT_EQ(dump.at("totalParticles"), 96);
    EXPECT_EQ(dump.at("aliveParticles"), 3);
    // 只有 fire 可见；culled 的 smoke 不计入预计 draw。
    EXPECT_EQ(dump.at("estimatedDrawnVertices"), 384);

    const auto& emitters = dump.at("emitters");
    ASSERT_EQ(emitters.size(), 2u);
    EXPECT_EQ(emitters.at(0).at("key"), "fire");
    EXPECT_EQ(emitters.at(0).at("aliveCount"), 2);
    EXPECT_EQ(emitters.at(0).at("totalParticles"), 64);
    EXPECT_EQ(emitters.at(0).at("culled"), false);
    EXPECT_EQ(emitters.at(0).at("sortParticles"), true);
    EXPECT_EQ(emitters.at(0).at("drawnVertices"), 384);
    EXPECT_FLOAT_EQ(
        emitters.at(0).at("bounds").at("radius").get<float>(),
        12.5f);
    EXPECT_EQ(emitters.at(1).at("key"), "smoke");
    EXPECT_EQ(emitters.at(1).at("culled"), true);
    EXPECT_EQ(emitters.at(1).at("localSpace"), true);

    // 未提供 frameStats 时 GPU 字段仍存在，但不支持计时且数值为 0。
    EXPECT_FALSE(dump.at("gpuTimingSupported").get<bool>());
    EXPECT_DOUBLE_EQ(dump.at("gpuMilliseconds").get<double>(), 0.0);
    EXPECT_EQ(dump.at("gpuSampleCount").get<uint64_t>(), 0u);
}

TEST(ParticleDump, RootCarriesGpuPassTimingFromFrameStats)
{
    ParticleFrameStats stats;
    stats.gpuTimingSupported = true;
    stats.gpuMilliseconds = 0.42;
    stats.gpuAverageMilliseconds = 0.37;
    stats.gpuMaxMilliseconds = 0.91;
    stats.gpuSampleCount = 64;

    const nlohmann::ordered_json dump =
        BuildParticleDumpJson(MakeEmitters(), 4, stats);

    EXPECT_TRUE(dump.at("gpuTimingSupported").get<bool>());
    EXPECT_DOUBLE_EQ(dump.at("gpuMilliseconds").get<double>(), 0.42);
    EXPECT_DOUBLE_EQ(
        dump.at("gpuAverageMilliseconds").get<double>(), 0.37);
    EXPECT_DOUBLE_EQ(dump.at("gpuMaxMilliseconds").get<double>(), 0.91);
    EXPECT_EQ(dump.at("gpuSampleCount").get<uint64_t>(), 64u);
}

TEST(ParticleDump, FiltersDeadParticlesAndTruncatesPerEmitter)
{
    const nlohmann::ordered_json dump =
        BuildParticleDumpJson(MakeEmitters(), 1);

    const auto& fire = dump.at("emitters").at(0);
    const auto& particles = fire.at("particles");
    ASSERT_EQ(particles.size(), 1u);
    EXPECT_EQ(particles.at(0).at("index"), 0);
    EXPECT_EQ(particles.at(0).at("position").at(0), 0.0);
    EXPECT_EQ(particles.at(0).at("velocity").at(2), 5.0);
    EXPECT_FLOAT_EQ(
        particles.at(0).at("age").get<float>(),
        0.2f);
    EXPECT_FLOAT_EQ(
        particles.at(0).at("lifetime").get<float>(),
        1.0f);
}

TEST(ParticleDump, RibbonEmittersCountSegmentedVertices)
{
    std::vector<ParticleDumpEmitter> emitters = MakeEmitters();
    for (ParticleDumpEmitter& emitter : emitters)
    {
        if (emitter.key != "fire")
            continue;
        emitter.renderMode = "ribbon";
        emitter.verticesPerParticle = 18;
    }

    const nlohmann::ordered_json dump =
        BuildParticleDumpJson(emitters, 4);
    const auto& fire = dump.at("emitters").at(0);

    EXPECT_EQ(fire.at("renderMode"), "ribbon");
    // fire 的 maxParticles = 64，ribbon 3 段 => 64 * 18 = 1152。
    EXPECT_EQ(fire.at("drawnVertices"), 1152);
    EXPECT_EQ(dump.at("estimatedDrawnVertices"), 1152);
}

TEST(ParticleDump, ZeroDetailLimitStillWritesCounts)
{
    const nlohmann::ordered_json dump =
        BuildParticleDumpJson(MakeEmitters(), 0);

    EXPECT_EQ(dump.at("aliveParticles"), 3);
    for (const auto& emitter : dump.at("emitters"))
    {
        EXPECT_TRUE(emitter.at("particles").empty());
    }
}

TEST(ParticleDump, ResolvesSafeRelativeOutputPath)
{
    std::filesystem::path outputPath;
    std::string error;
    ASSERT_TRUE(ResolveParticleDumpPath(
        "Resources",
        "captures/dump_smoke",
        outputPath,
        error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(outputPath.filename(), "dump_smoke.json");
    EXPECT_EQ(
        outputPath.parent_path().filename(),
        "captures");
    EXPECT_EQ(
        outputPath.parent_path().parent_path().filename(),
        "ParticleDumps");
    EXPECT_EQ(
        outputPath.parent_path().parent_path().parent_path().filename(),
        "Generated");

    ASSERT_TRUE(ResolveParticleDumpPath(
        "Resources",
        "dump.JSON",
        outputPath,
        error));
    EXPECT_EQ(outputPath.filename(), "dump.json");

    ASSERT_TRUE(ResolveParticleDumpPath(
        "Resources",
        "dump.txt",
        outputPath,
        error));
    EXPECT_EQ(outputPath.filename(), "dump.json");
}

TEST(ParticleDump, RejectsEmptyAbsoluteAndParentTraversalPaths)
{
    std::filesystem::path outputPath;
    std::string error;

    EXPECT_FALSE(ResolveParticleDumpPath("", "", outputPath, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(ResolveParticleDumpPath(
        "Resources", "/escape.json", outputPath, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(ResolveParticleDumpPath(
        "Resources", "../escape.json", outputPath, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(ResolveParticleDumpPath(
        "Resources", "nested/../../escape.json", outputPath, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(ResolveParticleDumpPath(
        "Resources", "dump.json:stream", outputPath, error));
    EXPECT_FALSE(error.empty());
}
