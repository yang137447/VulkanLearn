#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "shader/reload/shaderFileMonitor.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "shaderVariant.h"

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestKnownVectors()
{
    Require(
        VL::ContentHasher::HashString("").ToHex() ==
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
        "BLAKE3 empty-input vector mismatch");
    Require(
        VL::ContentHasher::HashString("abc").ToHex() ==
            "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
        "BLAKE3 abc vector mismatch");
}

void TestCanonicalFieldBoundaries()
{
    VL::CanonicalFieldHasher first("ShaderLogicalBuildIdV1");
    first.AddString("left", "ab");
    first.AddString("right", "c");

    VL::CanonicalFieldHasher second("ShaderLogicalBuildIdV1");
    second.AddString("left", "a");
    second.AddString("right", "bc");

    VL::CanonicalFieldHasher differentType("ShaderLogicalBuildIdV1");
    differentType.AddBytes("left", "ab", 2);
    differentType.AddString("right", "c");

    Require(
        first.Finalize() != second.Finalize(),
        "Canonical field lengths did not preserve string boundaries");
    Require(
        VL::ContentHasher::HashString("stable").ToHex().size() == 64,
        "Persistent digest was truncated");
    Require(
        differentType.Finalize() !=
            []()
            {
                VL::CanonicalFieldHasher value("ShaderLogicalBuildIdV1");
                value.AddString("left", "ab");
                value.AddString("right", "c");
                return value.Finalize();
            }(),
        "Canonical field type tags were not included");
}

void TestWriteIfChanged()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("vulkanlearn-shader-build-core-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path output = root / "generate" / "material.glsl";

    Require(
        VL::WriteTextFileIfChangedAtomically(output, "first\n"),
        "Initial write-if-changed did not write");
    const auto initialTime = std::filesystem::last_write_time(output);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Require(
        !VL::WriteTextFileIfChangedAtomically(output, "first\n"),
        "Identical write-if-changed unexpectedly rewrote the file");
    Require(
        std::filesystem::last_write_time(output) == initialTime,
        "Identical generated include changed its modification time");
    Require(
        VL::WriteTextFileIfChangedAtomically(output, "second\n"),
        "Changed write-if-changed content was not committed");
    Require(
        VL::ContentHasher::HashFile(output) ==
            VL::ContentHasher::HashString("second\n"),
        "Atomic file replacement committed unexpected bytes");

    std::filesystem::remove_all(root);
}

void TestStablePersistentIdentities()
{
    ShaderVariantKey variant;
    variant.shaderName = "surface/example";
    variant.renderMode = RenderMode::OpaqueClip;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    variant.macros = NormalizeMaterialMacros({"B=2", "A=1"});

    const std::string variantId = variant.GetVariantHash();
    Require(
        variantId.size() == 64,
        "Graphics variant identity was not a full BLAKE3-256 digest");
    Require(
        variantId == variant.GetVariantHash(),
        "Graphics variant identity was not deterministic");

    VL::MaterialShaderCompileRequest request;
    request.shaderVariantKey = variant;
    request.source.materialSourcePath = "M_example.json";
    request.source.vertexEvaluationPath = "material/example.vert.glsl";
    request.source.surfaceEvaluationPath = "material/example.frag.glsl";
    request.source.parameterIncludePath = "generate/example.glsl";
    const std::string requestId = request.GetRequestHash();
    Require(
        requestId.size() == 64,
        "Material shader identity was not a full BLAKE3-256 digest");
    Require(
        requestId != variantId,
        "Typed identity domains did not distinguish material and graphics builds");
}

void RequireChangedSourcesEqual(
    const std::optional<VL::ShaderFileMonitor::ChangeBatch>& batch,
    const std::vector<std::string>& expected,
    const std::string& context)
{
    Require(batch.has_value(), context + ": expected a change batch");
    Require(
        batch->changedSources == expected,
        context + ": unexpected changed source set");
}

void RequireObservedSourcesEqual(
    const std::optional<VL::ShaderFileMonitor::ChangeBatch>& batch,
    const std::vector<std::string>& expectedObserved,
    const std::vector<std::string>& expectedStable,
    const std::string& context)
{
    Require(batch.has_value(), context + ": expected a monitor batch");
    Require(
        batch->observedSources == expectedObserved,
        context + ": unexpected observed source set");
    Require(
        batch->changedSources == expectedStable,
        context + ": unexpected stable source set");
}

void TestShaderFileMonitor()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("vulkanlearn-shader-monitor-" +
         std::to_string(
             std::chrono::steady_clock::now()
                 .time_since_epoch()
                 .count()));
    std::filesystem::create_directories(root / "glsl" / "common");
    std::filesystem::create_directories(root / "glsl" / "generate");

    VL::WriteTextFileAtomically(
        root / "glsl" / "alpha.frag",
        "void main() {}\n");
    VL::WriteTextFileAtomically(
        root / "glsl" / "common" / "shared.glsl",
        "vec4 Shared() { return vec4(1.0, 0.0, 0.0, 1.0); }\n");
    VL::WriteTextFileAtomically(
        root / "glsl" / "generate" / "gen.glsl",
        "generated\n");
    VL::WriteTextFileAtomically(
        root / "glsl" / "M_material.json",
        "{ \"name\": \"M_material\" }\n");
    VL::WriteTextFileAtomically(
        root / "glsl" / "alpha.frag.tmp",
        "temporary\n");

    VL::ShaderFileMonitor monitor;
    monitor.Initialize(
        root,
        std::chrono::milliseconds(0),
        1);
    Require(
        !monitor.Poll().has_value(),
        "File monitor seed poll emitted changes");
    Require(
        monitor.GetTrackedSourceCount() == 3,
        "File monitor tracked generated or temporary files");

    const auto originalAlphaWriteTime =
        std::filesystem::last_write_time(
            root / "glsl" / "alpha.frag");
    std::filesystem::last_write_time(
        root / "glsl" / "alpha.frag",
        originalAlphaWriteTime +
            std::chrono::seconds(1));
    Require(
        !monitor.Poll().has_value(),
        "Modification-time-only update emitted a content transition");

    VL::WriteTextFileAtomically(
        root / "glsl" / "alpha.frag",
        "void main() { discard; }\n");
    RequireChangedSourcesEqual(
        monitor.Poll(),
        {"alpha.frag"},
        "Leaf edit");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VL::WriteTextFileAtomically(
        root / "glsl" / "alpha.frag",
        "void main() { discard; }\n");
    Require(
        !monitor.Poll().has_value(),
        "Identical rewrite emitted a false change");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VL::WriteTextFileAtomically(
        root / "glsl" / "common" / "shared.glsl",
        "vec4 Shared() { return vec4(0.5, 0.25, 0.75, 0.1, 1.0); }\n");
    RequireChangedSourcesEqual(
        monitor.Poll(),
        {"common/shared.glsl"},
        "Include edit");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VL::WriteTextFileAtomically(
        root / "glsl" / "M_material.json",
        "{ \"name\": \"M_material\", \"shadingModel\": \"Unlit\" }\n");
    RequireChangedSourcesEqual(
        monitor.Poll(),
        {"M_material.json"},
        "Material definition edit");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VL::WriteTextFileAtomically(
        root / "glsl" / "generate" / "gen.glsl",
        "generated twice\n");
    VL::WriteTextFileAtomically(
        root / "glsl" / "alpha.frag.tmp",
        "temporary twice\n");
    Require(
        !monitor.Poll().has_value(),
        "Generated include or temporary file emitted a change");

    VL::WriteTextFileAtomically(
        root / "glsl" / "beta.vert",
        "void main() {}\n");
    RequireChangedSourcesEqual(
        monitor.Poll(),
        {"beta.vert"},
        "New source file");

    std::filesystem::remove(root / "glsl" / "alpha.frag");
    RequireChangedSourcesEqual(
        monitor.Poll(),
        {"alpha.frag"},
        "Source deletion");

    // RefreshBaselineForSources must suppress a re-report without requiring a
    // second write.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VL::WriteTextFileAtomically(
        root / "glsl" / "common" / "shared.glsl",
        "vec4 Shared() { return vec4(0.25, 0.0, 0.0, 0.0, 0.5); }\n");
    monitor.RefreshBaselineForSources({"common/shared.glsl"});
    Require(
        !monitor.Poll().has_value(),
        "Baseline refresh did not suppress a stale change report");

    VL::ShaderFileMonitor debouncedMonitor;
    debouncedMonitor.Initialize(
        root,
        std::chrono::milliseconds(0),
        2);
    Require(
        !debouncedMonitor.Poll().has_value(),
        "Debounced monitor seed poll emitted changes");
    VL::WriteTextFileAtomically(
        root / "glsl" / "M_material.json",
        "{ \"name\": \"M_material\", \"shadingModel\": \"Lit\" }\n");
    RequireObservedSourcesEqual(
        debouncedMonitor.Poll(),
        {"M_material.json"},
        {},
        "Debounce first observation");
    Require(
        debouncedMonitor.HasUnstableSourceChanges(),
        "Debounce first observation was not marked unstable");
    RequireObservedSourcesEqual(
        debouncedMonitor.Poll(),
        {},
        {"M_material.json"},
        "Debounce stable transition");
    Require(
        !debouncedMonitor.HasUnstableSourceChanges(),
        "Stable transition remained marked unstable");

    Require(
        VL::ShaderFileMonitor::IsSourceOfTruthPath("nested/a.frag") &&
            VL::ShaderFileMonitor::IsSourceOfTruthPath("M_test.json") &&
            !VL::ShaderFileMonitor::IsSourceOfTruthPath("generate/a.glsl") &&
            !VL::ShaderFileMonitor::IsSourceOfTruthPath("a.frag.swp") &&
            !VL::ShaderFileMonitor::IsSourceOfTruthPath("other.json"),
        "Source-of-truth path filter mismatch");

    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try
    {
        TestKnownVectors();
        TestCanonicalFieldBoundaries();
        TestWriteIfChanged();
        TestStablePersistentIdentities();
        TestShaderFileMonitor();
        std::cout << "Shader build core tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shader build core tests failed: " << exception.what() << std::endl;
        return 1;
    }
}
