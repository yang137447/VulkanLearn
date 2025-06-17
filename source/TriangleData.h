#pragma once

#include <vector>

struct Vertex;

class TriangleData
{
public:
    static void GenVertexData();

    static std::vector<Vertex>& GetVertexData();
private:
    inline static std::vector<Vertex> vertices;
};