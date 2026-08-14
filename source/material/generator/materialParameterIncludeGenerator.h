#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Generates GLSL include files derived from M_*.json material definitions.
// The generated file declares material macros, shading-model constants, material UBO fields,
// and material texture bindings for shader authors to include from the matching vert/frag pair.
// Generated includes are build artifacts under shader/glsl/**/generate/ and are gitignored.
class MaterialParameterIncludeGenerator
{
public:
    struct GeneratedIncludeCandidate
    {
        const std::filesystem::path materialSourcePath;
        const std::filesystem::path outputPath;
        const std::string generatedBytes;
    };

    // Parses and validates one material definition and builds its include in memory.
    // This function does not create directories or write any file.
    static GeneratedIncludeCandidate BuildGeneratedIncludeContent(
        const std::filesystem::path& materialFilePath);

    // Commits a prepared batch after validating that every output path is unique.
    // The input candidates are not modified.
    static void CommitGeneratedIncludesIfChanged(
        const std::vector<GeneratedIncludeCandidate>& candidates);

    // Scans shader/glsl recursively and regenerates includes for every M_*.json file.
    static void GenerateAllIncludes();

    // Generates the include for one M_*.json next to that material file under generate/.
    static void GenerateInclude(const std::filesystem::path& materialFilePath);
};
