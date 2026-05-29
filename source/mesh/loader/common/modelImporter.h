#pragma once

#include "../../meshAssetTypes.h"

// Interface layer for mesh importing.
// The importer pipeline uses three design layers:
// 1) Interface: IModelImporter
// 2) Strategy: concrete importers selected by assetType (for example Assimp/SpeedTree)
// 3) Adapter: strategy-internal source adapters that isolate third-party format parsers
// This interface stays focused on ModelResource output and does not create scene or Vulkan objects.
class IModelImporter
{
public:
    virtual ~IModelImporter() = default;

    virtual void ValidateImportRequest(const ModelImportRequest& request) const = 0;
    virtual ModelResource Import(const ModelImportRequest& request) = 0;
};
