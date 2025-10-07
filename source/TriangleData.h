#pragma once

#include <vector>
#include "vertexDataStruct.h"

class TriangleData
{
public:
    static void GenVertexData();

    static std::vector<Vertex>& GetVertexData();
    static std::vector<uint32_t>& GetIndexData();
private:
    inline static std::vector<Vertex> vertices;
    inline static std::vector<uint32_t> indices;
};