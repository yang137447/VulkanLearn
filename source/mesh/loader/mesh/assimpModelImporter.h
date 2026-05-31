#pragma once

#include "../common/modelImporter.h"
#include "assimpSourceAdapter.h"

// Strategy implementation for type="mesh".
// It delegates source parsing to AssimpSourceAdapter and only maps the import request to ModelResource.
// This class does not expose Assimp types to RendererMeshLoader or MeshAssetValidator.
class AssimpModelImporter final : public IModelImporter
{
public:
    void ValidateImportRequest(const ModelImportRequest& request) const override;
    ModelResource Import(const ModelImportRequest& request) override;

private:
    AssimpSourceAdapter sourceAdapter;
};
