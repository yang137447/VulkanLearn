#include <iostream>
#include "VertexDataStruct.h"

int main()
{
    std::cout << "{\"vertexStrideBytes\":" << sizeof(Vertex)
        << ",\"indexStrideBytes\":" << sizeof(uint32_t)
        << ",\"instanceStrideBytes\":" << sizeof(InstanceVertex) << "}\n";
}
