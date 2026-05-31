#pragma once

#include <string_view>
#include <vector>
#include "../../meshAssetTypes.h"

// Load request shared by scene validation and runtime mesh loading.
// It contains resolved mesh descriptor data and the import request consumed by model importers.
struct MeshAssetLoadRequest
{
    MeshAssetBuildPlan buildPlan;
    ModelImportRequest importRequest;
};

// Result of importing one mesh asset into CPU-side sections plus per-section material mapping.
// RendererMeshLoader consumes this output to create renderables and bind
// material instances.
struct MeshAssetImportResult
{
    MeshAssetBuildPlan buildPlan;
    ModelResource modelResource;
    std::vector<MeshSectionLoadPlan> sectionPlans;
};

// Mesh-side loader used by scene preflight and RendererMeshLoader. It
// centralizes mesh JSON resolve/validate/import operations and keeps that
// policy out of WorldBuilder.
class MeshAssetLoader
{
public:
    static MeshAssetLoadRequest Prepare(std::string_view meshAssetPath);
    static void ValidateImportRequest(const MeshAssetLoadRequest& loadRequest);
    static void ValidateSectionLoadable(const MeshAssetLoadRequest& loadRequest);
    static MeshAssetImportResult ImportSections(const MeshAssetLoadRequest& loadRequest);
};
