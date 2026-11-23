#include "terrainTesCapture.h"
#include "terrainTesCapturePipeline.h"
#include "terrainTesDiagnostic.h"
#include "shader/build/contentHash.h"
#include "shaderCompiler.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;
using namespace VL;

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--hash")
    {
        std::cout << ContentHasher::HashFile(argv[2]).ToHex() << '\n';
        return 0;
    }
    Json results = Json::array();
    auto check = [&](const std::string& name, bool passed)
    {
        results.push_back({{"name", name}, {"passed", passed}});
        if (!passed) throw std::runtime_error(name);
    };
    auto rejected = [&](const std::string& name, const std::function<void()>& operation)
    {
        bool failed = false;
        try { operation(); } catch (const std::exception&) { failed = true; }
        check(name, failed);
    };
    TerrainTesDrawWitness witness;
    witness.frame = 3;
    witness.submissionEpoch = 4;
    witness.worldGeneration = 5;
    witness.poseId = 6;
    witness.actualLod = 0;
    witness.topology = vk::PrimitiveTopology::ePatchList;
    witness.firstPrimitive = 2;
    witness.primitiveCount = 4;
    witness.actualIndexCount = 32;
    witness.actualInstanceCount = 1;
    witness.vertexBuffer = vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t(1)));
    witness.patchIndexBuffer = vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t(2)));
    for (auto& descriptor : witness.actualDescriptorSets)
        descriptor = vk::DescriptorSet(reinterpret_cast<VkDescriptorSet>(uintptr_t(3)));
    witness.actualDescriptorSetMask = 15;
    for (auto& owner : witness.immutableDrawOwners)
        owner = std::make_shared<const int>(1);
    witness.surfaceId = "terrain|test";
    witness.publicationSha256 = witness.capturedShaderSha256 = witness.inputClosureSha256 = std::string(64, 'a');
    witness.displacementScale = 1;
    witness.tessRadiusCm = 6000;
    witness.baseFineResidentPages = 0;
    TerrainTesCapture::ValidateActualDraw(witness);
    check("complete-native-witness-policy", true);
    auto changed = witness;
    changed.actualLod = 2;
    rejected("resident-coarse-is-not-active-tes", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    changed = witness;
    changed.topology = vk::PrimitiveTopology::eTriangleList;
    rejected("triangle-is-not-patch", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    changed = witness;
    changed.primitiveCount = 5;
    rejected("more-than-four-patches", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    changed = witness;
    changed.immutableDrawOwners[0].reset();
    rejected("actual-owner-required", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    changed = witness;
    changed.displacementScale = 0;
    rejected("displacement-off-not-accepted", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    changed = witness;
    changed.baseFineResidentPages = 1;
    rejected("fine-base-independent-height-witness", [&] { TerrainTesCapture::ValidateActualDraw(changed); });
    std::array<uint32_t, 16> expected{};
    expected[0] = 81;
    expected[3] = 1;
    auto header = expected;
    TerrainTesCapture::ValidateHeader(header, expected, 512);
    check("finite-native-header", true);
    header[0] = 0;
    rejected("empty-is-missing", [&] { TerrainTesCapture::ValidateHeader(header, expected, 512); });
    header = expected;
    header[1] = 1;
    rejected("overflow-is-missing", [&] { TerrainTesCapture::ValidateHeader(header, expected, 512); });
    header = expected;
    header[7] = 1;
    rejected("native-epoch-header-must-match", [&] { TerrainTesCapture::ValidateHeader(header, expected, 512); });
    TerrainTesCaptureLimits limits;
    limits.explicitMetadataCost = {4096, 65536, 0, 0};
    const auto quote = TerrainTesCapture::AllocationQuote(105984, 110592, limits);
    check("actual-vk-requirements-plus-metadata", quote.total.gpuBytes == 220672 && quote.payloadBytes == 98368);
    check("host-padding-is-cpu-accounted", quote.total.cpuBytes == 176128);
    rejected("no-unquoted-allocation", [&] { TerrainTesCapture::AllocationQuote(1, 1, limits); });
    auto oversized = limits;
    oversized.explicitMetadataCost.gpuBytes = UINT64_MAX;
    rejected("no-quote-overflow-budget-bypass", [&] { TerrainTesCapture::AllocationQuote(105984, 110592, oversized); });
    TerrainRvtBudgetCaps caps;
    caps.workspaceGpuBytes = caps.totalGpuBytes = 220671;
    caps.workspaceCpuBytes = caps.totalCpuBytes = 1024 * 1024;
    auto outer = std::make_shared<TerrainRvtBudgetOwner>(caps);
    caps.workspaceGpuBytes = caps.totalGpuBytes = 32 * 1024 * 1024;
    TerrainRvtBudgetOwner world(caps, outer);
    auto lease = world.TryReserve(quote.total, TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    check("outer-owner-rejects-before-vulkan-allocation", !lease);
    check("rejected-lease-releases-world-cost", world.Statistics().current.gpuBytes == 0);
    _putenv_s("VULKANLEARN_TERRAIN_TES_CAPTURE", "0");
    check("ordinary-run-disabled", !TerrainTesDiagnostic::Requested());
    _putenv_s("VULKANLEARN_TERRAIN_TES_CAPTURE", "1");
    check("explicit-diagnostic-enable", TerrainTesDiagnostic::Requested());
    _putenv_s("VULKANLEARN_TERRAIN_TES_CAPTURE", "invalid");
    rejected("invalid-mode-fails", [] { TerrainTesDiagnostic::Requested(); });
    _putenv_s("VULKANLEARN_TERRAIN_TES_CAPTURE", "");
    Json report{{"gpuExecuted", false}, {"scope", "Native CPU policy tests, not a renderer draw"},
        {"tests", results}};
    if (argc == 4)
    {
        std::filesystem::current_path(argv[1]);
        ShaderCompiler compiler;
        compiler.Initialize(std::filesystem::path(argv[1]) / "shader");
        MaterialShaderCompileRequest request;
        request.shaderVariantKey.shaderName = "terrain";
        request.shaderVariantKey.macros = {"VL_TERRAIN_TESSELLATION=1"};
        request.source.materialSourcePath = "M_terrain.json";
        request.source.vertexEvaluationPath = "M_terrain.vertex.glsl";
        request.source.surfaceEvaluationPath = "M_terrain.surface.glsl";
        request.source.parameterIncludePath = "generate/M_terrainParamter.glsl";
        std::ifstream source(argv[2]);
        std::string text((std::istreambuf_iterator<char>(source)), {});
        auto build = CompileTerrainTesCaptureVariant(compiler, request, text);
        auto artifact = BuildGraphicsShaderVariantArtifact(build, "Test-only actual terrain TES capture");
        auto binding = std::find_if(artifact.shaderBindings.begin(), artifact.shaderBindings.end(),
            [](const auto& item) { return item.set == 4 && item.binding == 0; });
        check("native-compiler-real-tes-set4", artifact.hasTessellation &&
            binding != artifact.shaderBindings.end() &&
            binding->stageFlags == vk::ShaderStageFlagBits::eTessellationEvaluation &&
            binding->name == "terrainTestTesCapture");
        report["nativeShaderCompile"] = {{"shadercInvocations", build.shadercInvocations},
            {"sourceFingerprint", build.sourceFingerprint}, {"artifactGenerationKey", build.artifactGenerationKey},
            {"committed", build.committed}};
        report["tests"] = results;
        std::ofstream(argv[3]) << report.dump(2);
    }
    std::cout << report.dump(2) << '\n';
}
