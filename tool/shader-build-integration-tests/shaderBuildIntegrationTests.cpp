#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "shaderCompiler.h"

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::filesystem::path CreateTestRoot(const std::string& name)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("vulkanlearn-shader-build-" + name + "-" +
         std::to_string(
             std::chrono::steady_clock::now()
                 .time_since_epoch()
                 .count()));
    std::filesystem::create_directories(root / "glsl" / "common");
    return root;
}

void WriteText(
    const std::filesystem::path& path,
    const std::string& text)
{
    VL::WriteTextFileAtomically(path, text);
}

void PopulateFixture(const std::filesystem::path& root)
{
    WriteText(
        root / "glsl" / "common" / "shared.glsl",
        "vec4 SharedColor() { return vec4(0.25, 0.5, 0.75, 1.0); }\n");
    WriteText(
        root / "glsl" / "alpha.vert",
        "#version 450\n"
        "layout(location = 0) in vec3 inPosition;\n"
        "void main() { gl_Position = vec4(inPosition, 1.0); }\n");
    WriteText(
        root / "glsl" / "alpha.frag",
        "#version 450\n"
        "#include \"common/shared.glsl\"\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = SharedColor(); }\n");
    WriteText(
        root / "glsl" / "beta.vert",
        "#version 450\n"
        "layout(location = 0) in vec3 inPosition;\n"
        "void main() { gl_Position = vec4(inPosition.xy, 0.0, 1.0); }\n");
    WriteText(
        root / "glsl" / "beta.frag",
        "#version 450\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = vec4(1.0); }\n");
    WriteText(
        root / "glsl" / "simple.comp",
        "#version 450\n"
        "layout(local_size_x = 1) in;\n"
        "void main() {}\n");
}

VL::ShaderBuildStatistics Build(
    const std::filesystem::path& root,
    bool forceRebuild = false)
{
    ShaderCompiler compiler;
    return compiler.StartCompile(root, forceRebuild);
}

void RequireWarmBuild(const VL::ShaderBuildStatistics& statistics)
{
    Require(statistics.artifacts == 3, "Unexpected fixture artifact count");
    Require(statistics.cacheHits == 3, "Warm build did not hit every artifact");
    Require(statistics.compiledArtifacts == 0, "Warm build compiled an artifact");
    Require(statistics.shadercInvocations == 0, "Warm build invoked shaderc");
}

void TestWarmStartAndTargetedInvalidation()
{
    const std::filesystem::path root = CreateTestRoot("incremental");
    PopulateFixture(root);

    const VL::ShaderBuildStatistics cold = Build(root);
    Require(cold.artifacts == 3, "Cold build artifact count mismatch");
    Require(cold.compiledArtifacts == 3, "Cold build did not compile every artifact");
    Require(cold.cacheHits == 0, "Cold build unexpectedly hit the cache");
    Require(cold.shadercInvocations > 0, "Cold build did not invoke shaderc");
    RequireWarmBuild(Build(root));

    WriteText(
        root / "glsl" / "alpha.frag",
        "#version 450\n"
        "#include \"common/shared.glsl\"\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = SharedColor() * 0.5; }\n");
    const VL::ShaderBuildStatistics leafChanged = Build(root);
    Require(
        leafChanged.compiledArtifacts == 1 &&
            leafChanged.cacheHits == 2,
        "Leaf edit did not invalidate exactly one graphics pair");

    WriteText(
        root / "glsl" / "common" / "shared.glsl",
        "vec4 SharedColor() { return vec4(0.5, 0.25, 0.75, 1.0); }\n");
    const VL::ShaderBuildStatistics includeChanged = Build(root);
    Require(
        includeChanged.compiledArtifacts == 1 &&
            includeChanged.cacheHits == 2,
        "Common include edit invalidated an unrelated artifact");

    ShaderCompiler indexCompiler;
    indexCompiler.Initialize(root);
    const std::vector<std::string> affected =
        indexCompiler.FindLogicalBuildIdsDependingOn("common/shared.glsl");
    Require(
        affected.size() == 1,
        "Reverse dependency index did not identify exactly one dependent");

    std::filesystem::remove(root / "spv" / "alpha_frag.debug");
    const auto vertexOutput = root / "spv" / "alpha_vert.spv";
    const auto oldVertexTime =
        std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24);
    std::filesystem::last_write_time(vertexOutput, oldVertexTime);
    const VL::ShaderBuildStatistics missingOutput = Build(root);
    Require(
        missingOutput.compiledArtifacts == 1 &&
            missingOutput.cacheHits == 2,
        "Missing debug output did not rebuild exactly one complete artifact");
    Require(
        std::filesystem::last_write_time(vertexOutput) != oldVertexTime,
        "Missing pair member did not republish the complete graphics pair");

    const VL::ShaderBuildStatistics forced = Build(root, true);
    Require(
        forced.compiledArtifacts == 3 &&
            forced.cacheHits == 0,
        "Force rebuild did not compile every startup artifact");

    std::filesystem::remove_all(root);
}

void MutateManifestField(
    const std::filesystem::path& manifestPath,
    const std::string& field,
    const nlohmann::json& value)
{
    const std::vector<uint8_t> bytes = VL::ReadBinaryFile(manifestPath);
    nlohmann::json manifest = nlohmann::json::parse(
        bytes.begin(),
        bytes.end());
    manifest[field] = value;
    WriteText(manifestPath, manifest.dump(2) + "\n");
}

void TestManifestInvalidation()
{
    const std::filesystem::path root = CreateTestRoot("manifest");
    PopulateFixture(root);
    Build(root);
    const std::filesystem::path manifest =
        root / "spv" / "shader-build-cache.json";

    WriteText(manifest, "{not-json\n");
    const VL::ShaderBuildStatistics corrupt = Build(root);
    Require(
        corrupt.compiledArtifacts == 3,
        "Corrupt manifest did not safely rebuild every artifact");

    MutateManifestField(manifest, "schemaVersion", 999);
    const VL::ShaderBuildStatistics schemaChanged = Build(root);
    Require(
        schemaChanged.compiledArtifacts == 3,
        "Cache schema change did not invalidate every artifact");

    MutateManifestField(
        manifest,
        "compilePolicy",
        "ShaderCompilePolicyFuture");
    const VL::ShaderBuildStatistics policyChanged = Build(root);
    Require(
        policyChanged.compiledArtifacts == 3,
        "Compile policy change did not invalidate every artifact");

    std::filesystem::remove_all(root);
}

void TestStrictFailureAndRecovery()
{
    const std::filesystem::path root = CreateTestRoot("failure");
    PopulateFixture(root);
    Build(root);

    const std::filesystem::path manifest =
        root / "spv" / "shader-build-cache.json";
    const std::filesystem::path runtimeOutput =
        root / "spv" / "alpha_frag.spv";
    const std::string manifestDigest =
        VL::ContentHasher::HashFile(manifest).ToHex();
    const std::string outputDigest =
        VL::ContentHasher::HashFile(runtimeOutput).ToHex();

    WriteText(
        root / "glsl" / "alpha.frag",
        "#version 450\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { this is not valid GLSL; }\n");

    bool failed = false;
    try
    {
        Build(root);
    }
    catch (const std::exception&)
    {
        failed = true;
    }
    Require(failed, "Dirty shader syntax failure did not fail the build");
    Require(
        VL::ContentHasher::HashFile(manifest).ToHex() == manifestDigest,
        "Failed compile changed the authoritative manifest");
    Require(
        VL::ContentHasher::HashFile(runtimeOutput).ToHex() == outputDigest,
        "Failed compile overwrote the last successful artifact");

    WriteText(
        root / "glsl" / "alpha.frag",
        "#version 450\n"
        "#include \"common/shared.glsl\"\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = SharedColor() * 0.75; }\n");
    const VL::ShaderBuildStatistics recovered = Build(root);
    Require(
        recovered.compiledArtifacts == 1 &&
            recovered.cacheHits == 2,
        "Fixed syntax error did not recover through targeted rebuild");

    std::filesystem::remove_all(root);
}

VL::ShaderBuildRequest CreateVirtualGraphicsRequest(
    const std::filesystem::path& root,
    const std::string& fragmentExpression)
{
    VL::ShaderBuildRequest request;
    request.kind = VL::ShaderBuildKind::MaterialGraphicsPair;
    request.logicalBuildId = "virtual-graphics-test";
    request.normalizedKey = "virtual-graphics-test";
    request.stages = {
        {
            "vertex",
            "__composed__/virtual-graphics-test.vert",
            root / "glsl" / "__composed__" / "virtual-graphics-test.vert",
            "#version 450\n"
            "layout(location = 0) in vec3 inPosition;\n"
            "void main() { gl_Position = vec4(inPosition, 1.0); }\n",
            shaderc_vertex_shader},
        {
            "fragment",
            "__composed__/virtual-graphics-test.frag",
            root / "glsl" / "__composed__" / "virtual-graphics-test.frag",
            "#version 450\n"
            "#include \"../common/shared.glsl\"\n"
            "layout(location = 0) out vec4 outColor;\n"
            "void main() { outColor = " + fragmentExpression + "; }\n",
            shaderc_fragment_shader}};
    request.outputPaths = {
        {"runtimeVertex", root / "spv" / "virtual.vert.spv"},
        {"runtimeFragment", root / "spv" / "virtual.frag.spv"},
        {"debugVertex", root / "spv" / "virtual.vert.debug"},
        {"debugFragment", root / "spv" / "virtual.frag.debug"}};
    return request;
}

void TestVirtualSourceChangedScanAndCommitRollback()
{
    const std::filesystem::path root = CreateTestRoot("candidate");
    PopulateFixture(root);

    ShaderCompiler compiler;
    compiler.Initialize(root);
    std::vector<VL::ShaderBuildArtifact> initialArtifacts;
    initialArtifacts.push_back(
        compiler.CompileCandidate(
            CreateVirtualGraphicsRequest(root, "SharedColor()")));
    compiler.CommitArtifacts(initialArtifacts);

    Require(
        compiler.FindChangedSourcePaths().empty(),
        "Virtual composed primary sources were reported as missing files");

    const std::filesystem::path runtimeOutput =
        root / "spv" / "virtual.frag.spv";
    const std::string oldOutputDigest =
        VL::ContentHasher::HashFile(runtimeOutput).ToHex();
    const std::filesystem::path manifest =
        root / "spv" / "shader-build-cache.json";
    const std::vector<uint8_t> oldManifestBytes =
        VL::ReadBinaryFile(manifest);
    const std::string oldManifestDigest =
        VL::ContentHasher::HashFile(manifest).ToHex();

    std::vector<VL::ShaderBuildArtifact> candidateArtifacts;
    candidateArtifacts.push_back(
        compiler.CompileCandidate(
            CreateVirtualGraphicsRequest(
                root,
                "SharedColor() * 0.5")));

    std::filesystem::remove(manifest);
    std::filesystem::create_directory(manifest);
    bool failed = false;
    try
    {
        compiler.CommitArtifacts(candidateArtifacts);
    }
    catch (const std::exception&)
    {
        failed = true;
    }
    Require(failed, "Manifest publication fault did not fail the candidate commit");
    Require(
        VL::ContentHasher::HashFile(runtimeOutput).ToHex() ==
            oldOutputDigest,
        "Failed manifest publication did not restore the formal SPIR-V output");

    std::filesystem::remove_all(manifest);
    VL::WriteBinaryFileAtomically(
        manifest,
        oldManifestBytes);
    Require(
        VL::ContentHasher::HashFile(manifest).ToHex() ==
            oldManifestDigest,
        "Test fixture did not restore the formal manifest bytes");
    std::filesystem::remove_all(root);
}

void TestFrozenIncludeAndCommitTimeSourceValidation()
{
    const std::filesystem::path root =
        CreateTestRoot("candidate-source-validation");
    PopulateFixture(root);

    ShaderCompiler compiler;
    compiler.Initialize(root);
    VL::ShaderBuildRequest request =
        CreateVirtualGraphicsRequest(
            root,
            "SharedColor()");
    compiler.FreezeSourceSnapshot(request);

    const std::filesystem::path includePath =
        root / "glsl" / "common" / "shared.glsl";
    std::filesystem::remove(includePath);

    const VL::ShaderBuildArtifact candidate =
        compiler.CompileCandidate(request);
    const ShaderCompiler::CandidateSourceValidationResult validation =
        compiler.ValidateCandidateSourcesStillCurrent(
            request,
            candidate);
    Require(
        !validation.current &&
            validation.reason == "dependency missing" &&
            validation.sourceIdentity == "common/shared.glsl",
        "Deleted frozen include was not rejected at commit-time validation");

    WriteText(
        includePath,
        "vec4 SharedColor() { return vec4(0.75, 0.5, 0.25, 1.0); }\n");
    const ShaderCompiler::CandidateSourceValidationResult changedValidation =
        compiler.ValidateCandidateSourcesStillCurrent(
            request,
            candidate);
    Require(
        !changedValidation.current &&
            changedValidation.reason ==
                "dependency digest changed",
        "Changed frozen include was not rejected by digest validation");

    std::filesystem::remove_all(root);
}

void TestMixedCacheHitPublicationPathConflict()
{
    const std::filesystem::path root =
        CreateTestRoot("mixed-publication-path-conflict");
    PopulateFixture(root);

    ShaderCompiler compiler;
    compiler.Initialize(root);

    const VL::ShaderBuildRequest cachedRequest =
        CreateVirtualGraphicsRequest(
            root,
            "SharedColor()");
    std::vector<VL::ShaderBuildArtifact> initialArtifacts;
    initialArtifacts.push_back(
        compiler.CompileCandidate(cachedRequest));
    compiler.CommitArtifacts(initialArtifacts);

    const std::filesystem::path outputPath =
        root / "spv" / "virtual.frag.spv";
    const std::filesystem::path manifestPath =
        root / "spv" / "shader-build-cache.json";
    const std::string outputDigest =
        VL::ContentHasher::HashFile(outputPath).ToHex();
    const std::string manifestDigest =
        VL::ContentHasher::HashFile(manifestPath).ToHex();

    VL::ShaderBuildArtifact cacheHit =
        compiler.PrepareCandidate(cachedRequest);
    Require(
        cacheHit.cacheHit,
        "Mixed publication conflict fixture did not produce a cache hit");

    VL::ShaderBuildRequest conflictingRequest =
        CreateVirtualGraphicsRequest(
            root,
            "SharedColor() * 0.5");
    conflictingRequest.logicalBuildId =
        "virtual-graphics-conflicting-logical-build";
    conflictingRequest.normalizedKey =
        "virtual-graphics-conflicting-normalized-key";
    VL::ShaderBuildArtifact cacheMiss =
        compiler.CompileCandidate(conflictingRequest);
    Require(
        !cacheMiss.cacheHit,
        "Mixed publication conflict fixture unexpectedly produced a cache hit");

    std::vector<VL::ShaderBuildArtifact> artifacts;
    artifacts.push_back(std::move(cacheHit));
    artifacts.push_back(std::move(cacheMiss));
    bool failed = false;
    std::string failureMessage;
    try
    {
        compiler.CommitArtifacts(artifacts);
    }
    catch (const std::exception& exception)
    {
        failed = true;
        failureMessage = exception.what();
    }

    Require(
        failed &&
            failureMessage.find(
                "conflicting output path before publication") !=
                std::string::npos &&
            failureMessage.find("firstCacheHit=1") !=
                std::string::npos &&
            failureMessage.find("secondCacheHit=0") !=
                std::string::npos,
        "Mixed cache-hit/cache-miss output alias was not rejected with artifact identities");
    Require(
        VL::ContentHasher::HashFile(outputPath).ToHex() ==
            outputDigest,
        "Rejected mixed publication conflict changed the formal output");
    Require(
        VL::ContentHasher::HashFile(manifestPath).ToHex() ==
            manifestDigest,
        "Rejected mixed publication conflict changed the formal manifest");

    std::filesystem::remove_all(root);
}

void TestStaleCacheHitRejectedBeforePublication()
{
    const std::filesystem::path root =
        CreateTestRoot("stale-cache-hit-publication");
    PopulateFixture(root);

    ShaderCompiler compiler;
    compiler.Initialize(root);

    const VL::ShaderBuildRequest cachedRequest =
        CreateVirtualGraphicsRequest(
            root,
            "SharedColor()");
    std::vector<VL::ShaderBuildArtifact> initialArtifacts;
    initialArtifacts.push_back(
        compiler.CompileCandidate(cachedRequest));
    compiler.CommitArtifacts(initialArtifacts);

    VL::ShaderBuildArtifact staleCacheHit =
        compiler.PrepareCandidate(cachedRequest);
    Require(
        staleCacheHit.cacheHit,
        "Stale cache-hit fixture did not produce a cache hit");

    VL::ShaderBuildRequest pendingRequest =
        CreateVirtualGraphicsRequest(
            root,
            "SharedColor() * 0.25");
    pendingRequest.logicalBuildId =
        "pending-unrelated-logical-build";
    pendingRequest.normalizedKey =
        "pending-unrelated-normalized-key";
    pendingRequest.outputPaths = {
        {"runtimeVertex", root / "spv" / "pending.vert.spv"},
        {"runtimeFragment", root / "spv" / "pending.frag.spv"},
        {"debugVertex", root / "spv" / "pending.vert.debug"},
        {"debugFragment", root / "spv" / "pending.frag.debug"}};
    VL::ShaderBuildArtifact pendingArtifact =
        compiler.CompileCandidate(pendingRequest);

    const std::filesystem::path manifestPath =
        root / "spv" / "shader-build-cache.json";
    const std::string manifestDigest =
        VL::ContentHasher::HashFile(manifestPath).ToHex();
    WriteText(
        root / "spv" / "virtual.frag.spv",
        "newer formal generation");

    std::vector<VL::ShaderBuildArtifact> artifacts;
    artifacts.push_back(std::move(staleCacheHit));
    artifacts.push_back(std::move(pendingArtifact));
    bool failed = false;
    std::string failureMessage;
    try
    {
        compiler.CommitArtifacts(artifacts);
    }
    catch (const std::exception& exception)
    {
        failed = true;
        failureMessage = exception.what();
    }

    Require(
        failed &&
            failureMessage.find(
                "Cached shader output changed before publication") !=
                std::string::npos,
        "Stale cache-hit output was not rejected before publication");
    Require(
        !std::filesystem::exists(
            root / "spv" / "pending.frag.spv"),
        "Stale cache-hit rejection published an unrelated pending output");
    Require(
        VL::ContentHasher::HashFile(manifestPath).ToHex() ==
            manifestDigest,
        "Stale cache-hit rejection changed the formal manifest");

    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try
    {
        TestWarmStartAndTargetedInvalidation();
        TestManifestInvalidation();
        TestStrictFailureAndRecovery();
        TestVirtualSourceChangedScanAndCommitRollback();
        TestFrozenIncludeAndCommitTimeSourceValidation();
        TestMixedCacheHitPublicationPathConflict();
        TestStaleCacheHitRejectedBeforePublication();
        std::cout << "Shader build integration tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shader build integration tests failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
