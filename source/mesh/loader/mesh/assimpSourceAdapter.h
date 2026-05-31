#pragma once

#include <string>
#include "../../meshAssetTypes.h"

struct aiMesh;
struct aiNode;
struct aiScene;

// Adapter boundary for reading ordinary mesh source files through Assimp.
// It translates Assimp scene data into importer-owned ModelResource sections and does not expose
// Assimp types to RendererMeshLoader, MeshAssetValidator, or rendering code.
class AssimpSourceAdapter
{
public:
    void ValidateSource(const std::string& sourcePath, const std::string& modelDataPath) const;
    ModelResource ReadSource(const std::string& sourcePath, const std::string& modelDataPath) const;

private:
    void ProcessNode(aiNode* node, const aiScene* scene, ModelResource& outModelResource) const;
    MeshSection ProcessMesh(aiMesh* mesh, const aiScene* scene) const;
};
