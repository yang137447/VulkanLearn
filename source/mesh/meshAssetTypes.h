#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../vertexDataStruct.h"

// Result of resolving one mesh asset JSON into the effective JSON consumed by mesh validation.
// It keeps resolver output separate from runtime loading so later defaults or platform overrides can
// be introduced without moving that policy into RendererMeshLoader.
struct MeshAssetResolveResult
{
    nlohmann::json effectiveMeshAssetJson;
};

// One material slot declared by VulkanLearn mesh asset JSON.
// Slots are stored in JSON array order. The slot name is a readable config/debug label owned by
// the asset descriptor; runtime section mapping uses source material order, not this name.
struct MeshMaterialSlot
{
    std::string name;
    std::string materialInstancePath;
};

// Extra per-vertex SpeedTree attributes parsed from .stsdk but not yet part of Vulkan vertex input.
// Source packed TBN/AO bytes are preserved exactly. Current render Vertex data keeps source normals
// where available, while tangent/sign are generated through the shared MikkTSpace path.
struct SpeedTreeVertexAux
{
    Eigen::Vector3f lodPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f renderBinormal = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceNormal = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceTangent = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBinormal = Eigen::Vector3f::Zero();
    bool hasSourceNormalVector = false;
    bool hasSourceTangentVector = false;
    bool hasSourceBinormalVector = false;
    uint8_t sourcePackedNormal = 0;
    uint8_t sourcePackedBinormal = 0;
    uint8_t sourcePackedTangent = 0;
    uint8_t ambientOcclusion = 255;
    std::array<uint8_t, 4> sourcePackedTbnAo = {0, 0, 0, 255};
    std::array<uint8_t, 4> windBranch1 = {0, 0, 0, 0};
    std::array<uint8_t, 4> windBranch2 = {0, 0, 0, 0};
};

// Validated mesh asset data needed before importing the source model file.
// RendererMeshLoader uses this plan to select an importer and later bind section
// material slots.
// This structure does not own imported geometry or Vulkan resources.
struct MeshAssetBuildPlan
{
    std::string assetType;
    std::string meshAssetPath;
    std::string modelDataPath;
    std::vector<MeshMaterialSlot> materialSlots;
    std::string modelCacheKey;
};

// Import request passed from RendererMeshLoader to the selected model importer strategy.
// It combines the validated mesh asset plan with the resolved absolute model path; importers return
// CPU-side ModelResource data and do not create scene objects, material instances, or GPU buffers.
struct ModelImportRequest
{
    MeshAssetBuildPlan buildPlan;
    std::string absoluteModelPath;
};

// One imported geometry section from a model file, preserving the section and material slot names.
// IModelImporter implementations fill this data; validation consumes the names to map each section
// to a VulkanLearn material instance path.
struct MeshSection
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SpeedTreeVertexAux> speedTreeAuxVertices;
    std::string sectionName;
    std::string materialSlotName;
};

// Runtime loading plan for one imported mesh section after material slot validation.
// RendererMeshLoader consumes this to create the section renderable and load
// the referenced material instance. It does not contain vertex or index buffers.
struct MeshSectionLoadPlan
{
    std::string sectionName;
    std::string materialSlotName;
    std::string materialInstancePath;
};

// Full model import result preserving all geometry sections from the source model.
// Importer strategies return this object; mesh validation reads section and material slot names.
struct ModelResource
{
    std::vector<MeshSection> sections;
    std::vector<std::string> sourceMaterialSlotNames;
};
