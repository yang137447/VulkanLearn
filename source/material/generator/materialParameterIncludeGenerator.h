#pragma once

#include <filesystem>

// Generates GLSL include files derived from M_*.json material definitions.
// The generated file declares material macros, shading-model constants, material UBO fields,
// and material texture bindings for shader authors to include from the matching vert/frag pair.
// Generated includes are build artifacts under shader/glsl/**/generate/ and are gitignored.
class MaterialParameterIncludeGenerator
{
public:
    // Scans shader/glsl recursively and regenerates includes for every M_*.json file.
    static void GenerateAllIncludes();

    // Generates the include for one M_*.json next to that material file under generate/.
    static void GenerateInclude(const std::filesystem::path& materialFilePath);
};
