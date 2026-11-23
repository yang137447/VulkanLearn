#include "material/validation/materialAssetValidator.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "materialInstanceValidator.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "shaderCompiler.h"

#include <fstream>
#include <iostream>

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 3)
        return 2;
    const std::filesystem::path stage = arguments[1];
    const std::filesystem::path output = arguments[2];
    auto read = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        return nlohmann::json::parse(stream);
    };
    uint32_t definitions = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(stage / "shader/glsl"))
        if (entry.is_regular_file() && entry.path().extension() == ".json" &&
            entry.path().filename().string().rfind("M_", 0) == 0)
        {
            MaterialAssetValidator::ValidateDefinition(read(entry.path()), entry.path().string());
            ++definitions;
        }
    auto graph = read(stage / "config/renderGraphConfig.json");
    const auto compiled = VL::RenderGraphCompiler{}.Compile(graph);
    if (!compiled.IsSuccess())
        throw std::runtime_error("Production graph compilation rejected winner MRTs");
    bool sevenColors = false;
    for (const auto& pass : compiled.Value().passes)
        if (pass.type == VL::RenderGraphPassType::TerrainFeedback)
            sevenColors = pass.colorOutputCount == 7 && pass.outputResources.size() == 8;
    if (!sevenColors)
        throw std::runtime_error("Feedback compatibility key requires seven colors plus depth");
    const std::string materialPath = "shader/glsl/pass/M_terrainFeedback.json";
    const auto definition = read(stage / materialPath);
    const nlohmann::json instance{{"type", "materialInstance"}, {"name", "winner-cpu"},
        {"material", materialPath}};
    const auto resolved = MaterialInstanceResolver::ResolveWithDefinition(
        "fixture/MI_winner.json", instance, definition);
    const auto plan = MaterialInstanceValidator::BuildLoadPlan(
        "fixture/MI_winner.json", {}, resolved.effectiveMaterialInstanceJson,
        resolved.materialPath, resolved.materialJson, false, false);
    const auto generated = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(stage / materialPath);
    ShaderCompiler compiler;
    compiler.Initialize(stage / "shader");
    auto request = compiler.CreateGraphicsVariantBuildRequest(plan.shaderVariantKey);
    request.sourceSnapshot[std::filesystem::relative(
        generated.outputPath, stage / "shader/glsl").generic_string()] = generated.generatedBytes;
    const auto candidate = compiler.CompileCandidate(request);
    plan.materialDescriptorSchema.ValidateShaderBindings(candidate.shaderBindings, request.normalizedKey);
    if (candidate.committed || !candidate.shadercInvocations)
        throw std::runtime_error("Winner check must perform real CPU shader compilation without committing");
    const auto& fragment = candidate.outputs.at("runtimeFragment").spirv;
    std::ofstream spirv(output / "native-winner.frag.spv", std::ios::binary);
    spirv.write(reinterpret_cast<const char*>(fragment.data()), fragment.size() * sizeof(uint32_t));
    nlohmann::json report{{"materialDefinitionsValidated", definitions},
        {"graphColorOutputs", 7}, {"graphDepthOutputs", 1},
        {"shadercInvocations", candidate.shadercInvocations},
        {"sourceFingerprint", candidate.sourceFingerprint},
        {"gpuExecuted", false}, {"committed", candidate.committed}};
    std::ofstream(output / "native-check.json") << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
}
