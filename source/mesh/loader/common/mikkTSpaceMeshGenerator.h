#pragma once

#include <cstdint>
#include <vector>
#include "../../../vertexDataStruct.h"

// Generates MikkTSpace tangents for VulkanLearn Vertex meshes.
// Input comes from importers after positions, normals, UVs, and indices are available. The output
// may expand indexed vertices at UV/tangent-space seams and rewrites indices to the generated
// vertex order. It does not generate normals or import source model data.
class MikkTSpaceMeshGenerator
{
public:
    static bool Generate(
        std::vector<Vertex>& vertices,
        std::vector<uint32_t>& indices,
        std::vector<uint32_t>* outputSourceVertexIndices = nullptr);
};
