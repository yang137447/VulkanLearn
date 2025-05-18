#pragma once

#include <vector>

class TriangleData
{
public:
    static void GenVertexData();

    static std::vector<float>& GetVertexData();
private:
    inline static std::vector<float> vertices;
};