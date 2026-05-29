#pragma once

#include "../common/modelImporter.h"
#include "speedTreeSourceAdapter.h"

// Strategy implementation for type="speedtree".
// It delegates source parsing to SpeedTreeSourceAdapter and returns adapter-converted ModelResource.
class SpeedTreeModelImporter final : public IModelImporter
{
public:
    void ValidateImportRequest(const ModelImportRequest& request) const override;
    ModelResource Import(const ModelImportRequest& request) override;

private:
    SpeedTreeSourceAdapter sourceAdapter;
};
