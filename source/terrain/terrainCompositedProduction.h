#pragma once

#include "shader/build/contentHash.h"
#include "terrain/terrainCompositedAdjacency.h"
#include "terrain/terrainCompositedAuthorDependency.h"
#include "terrain/terrainCompositedIdentity.h"
#include "terrain/terrainCompositedVT.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL
{

enum class TerrainCompositedWeightSource { CoverageAtlas, LocalLayerArray };
enum class TerrainCompositedProductionObjectKind { Image, ShaderSource, ShaderArtifact, RawAuthorSource };
enum class TerrainCompositedProductionTransfer { Linear, Srgb };
enum class TerrainCompositedProductionAddress { Clamp, Repeat };
enum class TerrainCompositedProductionFilter { Nearest, Linear };

struct TerrainCompositedProductionImageDesc
{
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t mipLevels = 1;
    TerrainCompositedProductionTransfer transfer = TerrainCompositedProductionTransfer::Linear;
    TerrainCompositedProductionAddress address = TerrainCompositedProductionAddress::Clamp;
    TerrainCompositedProductionFilter filter = TerrainCompositedProductionFilter::Nearest;

    uint64_t ByteSize() const;
};

struct TerrainCompositedProductionObject
{
    std::string objectId;
    std::string relativePath;
    uint64_t bytes = 0;
    ContentDigest digest;
    ContentDigest baseCatalogDigest;
    ContentDigest heightBlendCatalogDigest;
    TerrainCompositedProductionObjectKind kind = TerrainCompositedProductionObjectKind::Image;
    TerrainCompositedProductionImageDesc image;
};

struct TerrainCompositedProductionBindings
{
    std::string coveragePageTable;
    std::string coverageIds;
    std::string coverageWeights;
    std::string localLayerWeights;
    std::string layerCatalog;
    std::string layerAlbedo;
    std::string layerNormal;
    std::string layerOrm;
    std::string layerHeight;
    std::string surfaceLocalNormal;
    std::string surfaceLocalTangent;
    std::string heightAtlas;
    std::string heightLayerMap;
    std::string globalColour;
    std::string globalMasks;
};

struct TerrainCompositedProductionSampling
{
    uint32_t coveragePageResolution = 128;
    std::array<double, 2> surfaceUvOffset{};
    std::array<double, 2> surfaceUvScale{1, 1};
    std::array<double, 2> coverageUvOffset{};
    std::array<double, 2> coverageUvScale{1, 1};
    std::array<double, 2> frameUvOffset{};
    std::array<double, 2> frameUvScale{1, 1};
};

struct TerrainCompositedProductionStaticParameters
{
    double uvScale = 1;
    double heightBlendStrength = 0;
    double frozenHeightBlendFade = 1;
    bool retainFragmentPostprocess = true;
};

struct TerrainCompositedSurfaceProduction
{
    std::string surfaceId;
    std::string runtimeMaterialInstancePath;
    std::string runtimeDetailMaterialInstancePath;
    TerrainCompositedWeightSource weightSource = TerrainCompositedWeightSource::CoverageAtlas;
    uint32_t sourceSet = 0;
    std::array<double, 16> localToWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<uint32_t> localToGlobal;
    std::array<uint32_t, 256> globalToSourceSlice = [] {
        std::array<uint32_t, 256> mapping{};
        mapping.fill(UINT32_MAX);
        return mapping;
    }();
    TerrainCompositedProductionBindings inputs;
    TerrainCompositedProductionSampling sampling;
    TerrainCompositedProductionStaticParameters parameters;
    std::vector<TerrainCompositedAuthorDependency> authorDependencies;
};

struct TerrainCompositedGeneratorIdentity
{
    uint32_t version = 0;
    std::string evaluator;
    std::string filterRule;
    std::string encoding;
    std::string compilePolicy;
    std::string computeSource;
    std::vector<std::string> includeSources;
    std::vector<std::string> compiledArtifacts;
};

struct TerrainCompositedProductionClosure
{
    TerrainCompositedGeneratorIdentity generator;
    std::vector<TerrainCompositedProductionObject> objects;
    std::vector<TerrainCompositedSurfaceProduction> surfaces;
    TerrainCompositedAdjacencyGraph adjacency;

    void Validate() const;
    ContentDigest ContentHash() const;
    ContentDigest SurfaceContentHash(const std::string& surfaceId) const;
    const TerrainCompositedProductionObject& Object(const std::string& objectId) const;
};

nlohmann::json TerrainCompositedProductionToJson(const TerrainCompositedProductionClosure& closure);
TerrainCompositedProductionClosure TerrainCompositedProductionFromJson(const nlohmann::json& document);
void PopulateTerrainCompositedCatalogDigests(
    TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes);
void ValidateTerrainCompositedProductionPayload(
    const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes);

}
