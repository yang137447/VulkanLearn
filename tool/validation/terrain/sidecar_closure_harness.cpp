#include "terrainPackageClosure.h"

#include <iostream>

namespace fs = std::filesystem;
using Json = nlohmann::json;

void WriteJson(const fs::path& path, const Json& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value.dump();
    if (!output) throw std::runtime_error("Harness JSON write failed");
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!output) throw std::runtime_error("Harness payload write failed");
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("Usage: sidecar_closure_harness <isolated-output-root>");
        const auto root = fs::absolute(argv[1]);
        fs::create_directories(root);
        const auto absent = VL::ContentHasher::HashString("terrain-optional-absent-v1");
        VL::CanonicalFieldHasher identity("terrain-composited-dependencies-v1");
        identity.AddString("config", "{}");
        identity.AddUInt64("dependencyCount", 1);
        identity.AddString("root", "resource");
        identity.AddString("path", "optional.paint");
        identity.AddString("mode", "absent");
        identity.AddDigest("digest", absent);
        const auto dependency = identity.Finalize();
        const auto prefix = root / dependency.ToHex();
        const auto dependenciesPath = fs::path(prefix.string() + ".dependencies.json");
        const Json dependencies{
            {"kind", "terrain-composited-dependencies"}, {"version", 1}, {"config", Json::object()},
            {"dependencies", Json::array({{{"root", "resource"}, {"path", "optional.paint"},
                {"mode", "absent"}, {"digest", absent.ToHex()}}})}, {"hash", dependency.ToHex()}};
        WriteJson(dependenciesPath, dependencies);
        VL::TerrainCompositedVTBuildDesc desc;
        desc.surfaceId = "terrain|Harness";
        desc.width = desc.height = 1;
        desc.dependencyHash = dependency;
        desc.encoding = "closure-harness";
        desc.channels = {VL::TerrainCompositedVTFormat::RGBA8UNorm};
        VL::TerrainCompositedVTStore::Publish(root / "manifest.json", {desc},
            {{{desc.surfaceId, 0, 0, 0}, std::vector<uint8_t>(desc.PageBytes(), 127)}});
        const auto coveragePath = fs::path(prefix.string() + ".coverage.json");
        std::vector<uint8_t> coverageBytes(4 + 2 * 128 * 128 * 4, 0);
        coverageBytes[3] = 255;
        auto coverage = [&](const std::vector<uint8_t>& bytes)
        {
            const auto hash = VL::ContentHasher::HashBytes(bytes.data(), bytes.size()).ToHex();
            WriteBytes(root / (hash + ".coverage.bin"), bytes);
            Json document{{"kind", "terrain-composited-coverage"}, {"version", 1},
                {"dependencyHash", dependency.ToHex()}, {"tilesPerSide", 1}, {"pageResolution", 128},
                {"bytes", bytes.size()}, {"hash", hash}, {"package", hash + ".coverage.bin"}};
            WriteJson(coveragePath, document);
            return document;
        };
        const auto originalCoverage = coverage(coverageBytes);
        const auto heightPath = fs::path(prefix.string() + ".height.json");
        VL::TerrainHeightAtlasStoreData height;
        height.desc = {136, 136, 1, 1};
        height.atlas.resize(136 * 136 * 2, 0);
        height.layerMap.push_back({0.f, 1.f, 128.f, 128.f});
        VL::WriteTerrainHeightAtlasStore(heightPath, height);
        auto validate = [&]()
        {
            return VL::TerrainPackageClosure::Files(root, "manifest.json", true, 8388608, 1);
        };
        if (validate().size() != 7) throw std::runtime_error("Expected seven closure files");
        size_t rejected = 0;
        auto reject = [&](const char* name, auto operation)
        {
            bool failed = false;
            try { operation(); }
            catch (const std::exception&) { failed = true; }
            if (!failed) throw std::runtime_error(std::string("Expected rejection: ") + name);
            ++rejected;
        };
        auto changed = originalCoverage;
        changed["tilesPerSide"] = UINT64_MAX;
        WriteJson(coveragePath, changed);
        reject("overflow", validate);
        changed = originalCoverage;
        changed["package"] = "../escape.coverage.bin";
        WriteJson(coveragePath, changed);
        reject("traversal", validate);
        changed = originalCoverage;
        changed["version"] = 2;
        WriteJson(coveragePath, changed);
        reject("version", validate);
        coverageBytes[0] = 1;
        coverage(coverageBytes);
        reject("identity table", validate);
        WriteJson(coveragePath, originalCoverage);
        reject("TR tiles mismatch", [&]() {
            VL::TerrainPackageClosure::Files(root, "manifest.json", true, 8388608, 2);
        });
        reject("height budget", [&]() {
            VL::TerrainPackageClosure::Files(root, "manifest.json", true, 1, 1);
        });
        auto damagedDependencies = dependencies;
        damagedDependencies["config"]["changed"] = true;
        WriteJson(dependenciesPath, damagedDependencies);
        reject("dependency anchor", validate);
        WriteJson(dependenciesPath, dependencies);
        const auto validHeight = VL::ReadTerrainHeightAtlasStore(heightPath, 8388608);
        WriteBytes(validHeight.packagePath, std::vector<uint8_t>(fs::file_size(validHeight.packagePath), 1));
        reject("height corruption", validate);
        fs::remove(validHeight.packagePath);
        VL::WriteTerrainHeightAtlasStore(heightPath, height);
        if (validate().size() != 7) throw std::runtime_error("Restored fixture failed");
        std::cout << Json{{"closureFiles", 7}, {"negativeCasesRejected", rejected},
            {"rendererStarted", false}, {"sharedBuildUsed", false}}.dump() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
