#include "TriangleData.h"s

void TriangleData::GenVertexData()
{
    vertices = { //数据数组
		0,  75, 0, 1, 0, 0,//每一行前三个是顶点坐标
		-45, 0, 0, 0, 1, 0,//每一行后三个是颜色RGB值
		45, 0, 0, 0, 0, 1
	};
}

std::vector<float>& TriangleData::GetVertexData()
{
    if (vertices.empty())
    {
        GenVertexData();
    }
    
    return vertices;
}
