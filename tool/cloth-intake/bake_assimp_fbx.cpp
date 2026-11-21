#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: bake_assimp_fbx <input.fbx> <output.fbx>\n";
        return 2;
    }

    const std::filesystem::path input = std::filesystem::u8path(argv[1]);
    const std::filesystem::path output = std::filesystem::u8path(argv[2]);

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        input.string(),
        aiProcess_Triangulate |
            aiProcess_GlobalScale |
            aiProcess_PreTransformVertices |
            aiProcess_ValidateDataStructure);
    if (scene == nullptr || scene->mRootNode == nullptr)
    {
        std::cerr << "import failed: " << importer.GetErrorString() << '\n';
        return 3;
    }

    Assimp::Exporter exporter;
    if (exporter.Export(scene, "fbx", output.string()) != AI_SUCCESS)
    {
        std::cerr << "export failed: " << exporter.GetErrorString() << '\n';
        return 4;
    }

    std::cout << "baked " << scene->mNumMeshes << " meshes to " << output << '\n';
    return 0;
}
