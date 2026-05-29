#include "speedTreeModelImporter.h"

void SpeedTreeModelImporter::ValidateImportRequest(const ModelImportRequest& request) const
{
    sourceAdapter.ValidateSource(request.absoluteModelPath, request.buildPlan.modelDataPath);
}

ModelResource SpeedTreeModelImporter::Import(const ModelImportRequest& request)
{
    return sourceAdapter.ReadSource(request.absoluteModelPath, request.buildPlan.modelDataPath).modelResource;
}
