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

struct MeshImportOptions
{
    bool generateSmoothNormals = false;
};

// Runtime SDK 10 keeps the Games 9 wind authoring model. Curves are sampled
// at twenty authored strength points in the .stsdk table.
struct SpeedTreeWindCurve
{
    std::array<float, 20> values{};
};

struct SpeedTreeWindCommonConfig
{
    float strengthResponse = 0.0f;
    float directionResponse = 0.0f;
    float gustFrequency = 0.0f;
    float gustStrengthMin = 0.0f;
    float gustStrengthMax = 0.0f;
    float gustDurationMin = 0.0f;
    float gustDurationMax = 0.0f;
    float gustRiseScalar = 0.0f;
    float gustFallScalar = 0.0f;
};

struct SpeedTreeWindBranchConfig
{
    SpeedTreeWindCurve bend;
    SpeedTreeWindCurve oscillation;
    SpeedTreeWindCurve speed;
    SpeedTreeWindCurve turbulence;
    SpeedTreeWindCurve flexibility;
    float independence = 0.0f;
};

struct SpeedTreeWindRippleConfig
{
    SpeedTreeWindCurve planar;
    SpeedTreeWindCurve directional;
    SpeedTreeWindCurve speed;
    SpeedTreeWindCurve flexibility;
    float shimmer = 0.0f;
    float independence = 0.0f;
};

struct SpeedTreeWindConfig
{
    SpeedTreeWindCommonConfig common;
    SpeedTreeWindBranchConfig shared;
    SpeedTreeWindBranchConfig branch1;
    SpeedTreeWindBranchConfig branch2;
    SpeedTreeWindRippleConfig ripple;
    float sharedStartHeight = 0.0f;
    float branch1StretchLimit = 0.0f;
    float branch2StretchLimit = 0.0f;
    bool doShared = false;
    bool doBranch1 = false;
    bool doBranch2 = false;
    bool doRipple = false;
    bool doShimmer = false;
};

// One imported tree species' authored wind data. The key follows the mesh
// cache identity so all sections and instances of one source asset share the
// same live CPU wind state without sharing it with other species.
struct SpeedTreeWindProfile
{
    std::string key;
    Eigen::Vector3f sourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBoundsMax = Eigen::Vector3f::Zero();
    SpeedTreeWindConfig config;
};

// Extra per-vertex SpeedTree attributes parsed from .stsdk. The raw source bytes
// remain available for probes while the normalized branch streams are uploaded
// as Vulkan vertex attributes for the runtime wind shader. Render tangent/sign
// is generated through MikkTSpace.
struct SpeedTreeVertexAux
{
    // Current limitation: the renderer uses only the highest-detail 3D block,
    // so LOD position and blend data remain import-side metadata. A future
    // foliage vertex stream must apply geometry morph before SDK wind.
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
    std::array<float, 4> sourceNormalizedTbnAo = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<uint8_t, 4> windBranch1 = {0, 0, 0, 0};
    std::array<uint8_t, 4> windBranch2 = {0, 0, 0, 0};
    std::array<float, 4> windBranch1Normalized = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> windBranch2Normalized = {0.0f, 0.0f, 0.0f, 0.0f};
    Eigen::Vector3f windBranch1Direction = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch2Direction = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch1NoiseOffsetNormalized = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch2NoiseOffsetNormalized = Eigen::Vector3f::Zero();
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
    MeshImportOptions importOptions;
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
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string materialInstancePath;
};

// Full model import result preserving all geometry sections from the source model.
// Importer strategies return this object; mesh validation reads section and material slot names.
struct ModelResource
{
    std::vector<MeshSection> sections;
    std::vector<std::string> sourceMaterialSlotNames;
    bool hasSpeedTreeWind = false;
    Eigen::Vector3f speedTreeSourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f speedTreeSourceBoundsMax = Eigen::Vector3f::Zero();
    SpeedTreeWindConfig speedTreeWind;
};
