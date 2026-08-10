#include "assimpModelImporter.h"

void AssimpModelImporter::ValidateImportRequest(const ModelImportRequest& request) const
{
    sourceAdapter.ValidateSource(request.absoluteModelPath, request.buildPlan.modelDataPath);
}

ModelResource AssimpModelImporter::Import(const ModelImportRequest& request)
{
    ValidateImportRequest(request);
    return sourceAdapter.ReadSource(
        request.absoluteModelPath,
        request.buildPlan.modelDataPath,
        request.buildPlan.importOptions);
}
