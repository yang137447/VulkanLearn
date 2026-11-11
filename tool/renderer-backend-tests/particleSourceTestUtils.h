#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ParticleSourceTest
{

inline std::string ReadSource(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

// Source guards must not depend on how particleRenderer is split across TUs.
inline std::string ReadParticleRendererSources()
{
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path directory =
        std::string(VL_TEST_SOURCE_ROOT) + "/source/particles";
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        const std::string filename = entry.path().filename().string();
        if ((entry.path().extension() == ".cpp" ||
             entry.path().extension() == ".h") &&
            filename.rfind("particleRenderer", 0) == 0)
            paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::string sources;
    for (const auto& path : paths)
        sources += ReadSource(
            "source/particles/" + path.filename().string());
    return sources;
}

} // namespace ParticleSourceTest
