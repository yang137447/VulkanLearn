#pragma once

#include <cstddef>
#include <filesystem>

namespace SpeedTreeParser
{
    struct ProbeOptions
    {
        std::filesystem::path inputPath;
        std::filesystem::path outputPath;
        std::filesystem::path objOutputPath;
        size_t minStringLength = 4;
    };

    void WriteProbeFiles(const ProbeOptions& options);
}
