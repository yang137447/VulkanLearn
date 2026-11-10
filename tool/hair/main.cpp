#include "hair/strandGeometry.h"

#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
int PrintError(const VL::RuntimeError& error)
{
    std::cerr << nlohmann::json{
        {"code", error.code}, {"message", error.message},
        {"sourcePath", error.sourcePath}, {"sourceNode", error.sourceNode}}.dump() << '\n';
    return 1;
}

int Inspect(const std::filesystem::path& path)
{
    const auto result = VL::Hair::LoadTfx(path);
    if (result.IsFailure()) return PrintError(result.Error());
    const auto& asset = result.Value();
    const auto prepared = VL::Hair::PrepareStrandGeometry(asset, path.u8string());
    if (prepared.IsFailure()) return PrintError(prepared.Error());
    const auto& geometry = prepared.Value();
    std::cout << nlohmann::json{
        {"source", path.u8string()}, {"version", asset.version},
        {"guideCount", asset.guideCount}, {"pointsPerStrand", asset.pointsPerStrand},
        {"vertexCount", asset.positions.size()},
        {"bounds", {{"min", asset.boundsMin}, {"max", asset.boundsMax}}},
        {"attributes", {
            {"strandUVs", !asset.strandUVs.empty()}, {"vertexUVs", !asset.vertexUVs.empty()},
            {"strandThickness", !asset.strandThickness.empty()}, {"vertexColors", !asset.vertexColors.empty()}}},
        {"geometry", {
            {"ribbonVertexCount", asset.positions.size() * 2u},
            {"ribbonIndexCount", geometry.ribbonIndices.size()},
            {"maxRestLength", geometry.maxRestLength}}}}.dump(2) << '\n';
    return 0;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        if (argc != 2)
        {
            std::cerr << "Usage: vl_tfx_inspect <hair.tfx>\n";
            return 1;
        }
        return Inspect(std::filesystem::path(argv[1]));
    }
    catch (const std::exception& error)
    {
        std::cerr << "TFX inspection failed: " << error.what() << '\n';
        return 1;
    }
}
