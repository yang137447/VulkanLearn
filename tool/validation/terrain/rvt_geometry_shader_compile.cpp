#include "shaderCompiler.h"
#include "material/compiler/materialShaderComposer.h"
#include "terrain/terrainCompositedProduction.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5) return 2;
    const auto stage = std::filesystem::absolute(argv[1]);
    const bool baseOnly = argc == 5 && std::string(argv[4]) == "--base-only";
    const auto reportRoot = baseOnly ? stage / "geometry-diagnostic" : stage.parent_path();
    std::filesystem::create_directories(reportRoot);
    std::filesystem::current_path(stage);
    ShaderCompiler compiler;
    compiler.Initialize(stage / "shader");
    nlohmann::json report = nlohmann::json::array();
    const auto record = [&](const VL::ShaderBuildArtifact& built)
    {
        nlohmann::json outputs = nlohmann::json::array();
        for (const auto& output : built.outputs)
            outputs.push_back({{"role", output.first}, {"path", output.second.path.string()},
                               {"digest", output.second.digest}});
        report.push_back({{"normalizedKey", built.normalizedKey},
            {"sourceFingerprint", built.sourceFingerprint},
            {"artifactGenerationKey", built.artifactGenerationKey},
            {"shadercInvocations", built.shadercInvocations}, {"committed", built.committed},
            {"outputs", outputs}, {"abiFingerprint", built.abiFingerprint}});
    };
    for (const auto pass : {VL::MaterialPass::Base, VL::MaterialPass::ShadowDepth, VL::MaterialPass::LocalShadow})
        for (const int tess : {0, 1})
        {
            if (baseOnly && pass != VL::MaterialPass::Base) continue;
            VL::MaterialShaderCompileRequest request;
            request.shaderVariantKey.shaderName = "terrain";
            request.shaderVariantKey.macros = {"VL_TERRAIN_TESSELLATION=" + std::to_string(tess)};
            request.pass = pass;
            request.source.materialSourcePath = "M_terrain.json";
            request.source.vertexEvaluationPath = "M_terrain.vertex.glsl";
            request.source.surfaceEvaluationPath = "M_terrain.surface.glsl";
            request.source.parameterIncludePath = "generate/M_terrainParamter.glsl";
            const auto composed = VL::MaterialShaderComposer::Compose(request);
            const auto built = compiler.EnsureMaterialGraphicsVariantCompiled(request, composed, true);
            record(built);
            const auto directory = stage / "link-inputs" / (VL::MaterialPassToString(pass) + std::to_string(tess));
            std::filesystem::create_directories(directory);
            const auto emit = [&](const std::string& text, const std::string& extension)
            {
                if (text.empty()) return;
                auto linked = text;
                const std::string prefix = "#version 450";
                linked.replace(0, prefix.size(), prefix +
                    "\n#extension GL_GOOGLE_include_directive : require\n#define VL_TERRAIN_TESSELLATION " +
                    std::to_string(tess));
                std::ofstream(directory / ("terrain." + extension)) << linked;
            };
            emit(composed.vertexSource, "vert");
            emit(composed.fragmentSource, "frag");
            emit(composed.tessControlSource, "tesc");
            emit(composed.tessEvaluationSource, "tese");
        }
    for (const auto* model : {"SHADING_MODEL_DEFAULT_LIT", "SHADING_MODEL_UNLIT"})
    {
        ShaderVariantKey key;
        key.shaderName = "pass/terrainFeedback";
        key.shadingModelMacro = model;
        record(compiler.EnsureGraphicsVariantCompiled(key, true));
    }
    std::ofstream(reportRoot / "shader-compile.json") << report.dump(2) << '\n';
    std::ifstream manifestStream(argv[2]);
    const auto manifest = nlohmann::json::parse(manifestStream);
    const auto original = VL::TerrainCompositedProductionFromJson(manifest.at("production"));
    auto updated = original;
    const auto digest = VL::ContentHasher::HashFile(argv[3]);
    std::vector<std::string> changedObjects;
    for (auto& surface : updated.surfaces)
        for (auto& dependency : surface.authorDependencies)
            if (dependency.path == "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl")
            {
                dependency.digest = digest.ToHex();
                changedObjects.insert(changedObjects.end(),
                    dependency.productionObjectIds.begin(), dependency.productionObjectIds.end());
            }
    for (auto& object : updated.objects)
        if (std::find(changedObjects.begin(), changedObjects.end(), object.objectId) != changedObjects.end())
        {
            object.digest = digest;
            object.bytes = std::filesystem::file_size(argv[3]);
            object.relativePath = "author-inputs/" + digest.ToHex() + ".bin";
        }
    nlohmann::json hashes;
    hashes["scope"] = "Native identity counterfactual only; does not publish or validate a new closure";
    hashes["productionBefore"] = original.ContentHash().ToHex();
    hashes["productionAfter"] = updated.ContentHash().ToHex();
    hashes["surfaces"] = nlohmann::json::array();
    for (const auto& surface : original.surfaces)
    {
        const auto before = original.SurfaceContentHash(surface.surfaceId).ToHex();
        const auto after = updated.SurfaceContentHash(surface.surfaceId).ToHex();
        if (before != after) throw std::runtime_error("Consumer-only edit changed base surface hash");
        hashes["surfaces"].push_back({{"surface", surface.surfaceId}, {"before", before}, {"after", after}});
    }
    if (hashes["productionBefore"] == hashes["productionAfter"])
        throw std::runtime_error("Author closure failed to track the changed source");
    std::ofstream(reportRoot / "identity-counterfactual.json") << hashes.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
}
