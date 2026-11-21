#include "shaderCompiler.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 3)
        return 2;
    ShaderCompiler compiler;
    compiler.Initialize(std::filesystem::path(arguments[1]) / "shader");
    const auto snapshot = compiler.CaptureManifestSnapshot();
    nlohmann::json report = nlohmann::json::array();
    for (const auto& entry : snapshot.artifacts)
    {
        const auto& record = entry.second;
        if (record.normalizedKey.find("|shaderName=pass/terrainFeedback|") == std::string::npos)
            continue;
        ShaderVariantKey key;
        key.shaderName = "pass/terrainFeedback";
        if (record.normalizedKey.find("|renderMode=Opaque|") == std::string::npos ||
            record.normalizedKey.find("|macros=|") == std::string::npos)
            throw std::runtime_error("Review non-default feedback variant before cache publication");
        if (record.normalizedKey.find("|shadingModel=SHADING_MODEL_UNLIT|") != std::string::npos)
            key.shadingModelMacro = "SHADING_MODEL_UNLIT";
        else if (record.normalizedKey.find("|shadingModel=SHADING_MODEL_DEFAULT_LIT|") == std::string::npos)
            throw std::runtime_error("Review feedback shading model before cache publication");
        const auto built = compiler.EnsureGraphicsVariantCompiled(key);
        report.push_back({{"normalizedKey", built.normalizedKey},
            {"sourceFingerprint", built.sourceFingerprint},
            {"artifactGenerationKey", built.artifactGenerationKey},
            {"shadercInvocations", built.shadercInvocations},
            {"cacheHit", built.cacheHit}, {"committed", built.committed}});
    }
    if (report.empty())
        throw std::runtime_error("No real feedback graphics cache entry found");
    std::ofstream(arguments[2]) << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
}
