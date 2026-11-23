#pragma once

// Terrain composited VT service: internal helper declarations.
// Definitions live in terrainCompositedServiceDetail.cpp; this header keeps
// the shared helpers visible to the split implementation TUs.

#include "render/resource/terrainCompositedService.h"
#include "core/runtimeEvidence.h"
#include "core/runtimeFileTrace.h"
#include "core/tracingIfstream.h"

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainCompositedBakeGpu.h"
#include "render/resource/terrainCompositedProducerSnapshot.h"
#include "render/resource/terrainCompositedWorldBindings.h"
#include "render/resource/rendererResourceCache.h"
#include "render/frame/frameSubmitFault.h"
#include "render/resource/terrainCompositedProductionGpu.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialInstanceStateBatch.h"
#include "shaderCompiler.h"
#include "render/resource/terrainHeightAtlas.h"
#include "render/terrain/terrainCompositedRuntime.h"
#include "render/terrain/terrainCompositedProductionPriority.h"
#include "render/terrain/terrainCompositedServiceRequestDiagnostics.h"
#include "shader/build/atomicFile.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedAuthorRecipe.h"
#include "terrain/terrainCompositedFeedback.h"
#include "terrain/terrainCompositedProductionIdentity.h"
#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/terrainPaintSha256.h"
#include "terrain/terrainRvtScheduler.h"
#include "terrain/terrainCompositedRefresh.h"
#include "terrain/terrainCompositedShaderPath.h"
#include "terrain/terrainCompositedQualityClock.h"
#include "terrain/terrainCompositedSourceSnapshots.h"
#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainFaultInjection.h"
#include "terrain/terrainFaultOwnerRegistration.h"
#include "terrain/terrainCompositedPageSourceBatch.h"
#include "render/resource/terrainCompositedResourceSnapshot.h"
#include "render/frame/terrainCompositedReadiness.h"
#include "renderGraph.h"

#include "render/resource/terrainCompositedQualityGpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#endif

namespace VL
{
struct TerrainCompositedFrozenConfig
{
    TerrainCompositedRuntimeBudget runtime;
    TerrainRvtSchedulerConfig production;
    TerrainCompositedProductionAllocationQuery allocations;
    uint32_t packagedMinMip = 0;
    TerrainRvtBudgetCaps steadyCaps;
    TerrainRvtBudgetCaps peakCaps;
    TerrainRvtBudgetCaps terrainSteadyCaps;
    std::shared_ptr<TerrainRvtBudgetOwner> terrainOwner;
    uint64_t geometryGpuBytes = 0;
    uint64_t geometryCpuBytes = 0;
    uint64_t metadataGpuBytes = 0;
    double initialQualityLimitMs = 0;
    double poseQualityLimitMs = 0;
};

namespace TerrainServiceDetail
{
using Baker = TerrainCompositedBakeGpu;

extern std::atomic<bool> GlobalCookMode;

extern std::atomic<bool> GlobalAuthoringMode;

extern std::atomic<uint64_t> NextGeneration;

extern std::atomic<bool> GlobalWorldHasBeenActivated;

extern std::mutex FrozenConfigMutex;

extern std::optional<TerrainCompositedFrozenConfig> FrozenConfig;

extern std::mutex EvidenceBudgetMutex;

extern std::optional<TerrainRvtBudgetLease> EvidenceProcessLease;

extern std::optional<TerrainRvtBudgetLease> FileTraceProcessLease;

constexpr uint64_t ReadOnlyPackageLimit = 4ull * 1024 * 1024 * 1024;

void Require(bool condition, const std::string& message);

void ServiceEvidence(const char* eventClass, const char* prefix, const nlohmann::json& payload);

double QualityMonotonicMs();

struct TerrainServicePublishedSurfaceView
{
    std::string surfaceId;
    uint64_t revision = 0;
    uint64_t acknowledgedRevision = 0;
    std::string inputIdentity;
};

struct TerrainServicePublishedObservation
{
    nlohmann::json summary;
    std::vector<nlohmann::json> pages;
};

std::optional<TerrainServicePublishedObservation> BuildTerrainServicePublishedObservation(
    const TerrainCompositedRuntime& runtime,
    const std::vector<TerrainServicePublishedSurfaceView>& views,
    uint64_t world, uint64_t revision, uint64_t descriptorRevision,
    uint64_t descriptorWorld, uint64_t completedEpoch, uint64_t inputEpoch,
    uint64_t statusEpoch, double sampledMs);

nlohmann::json TerrainServiceCostJson(const TerrainRvtCost& cost);

nlohmann::json BuildTerrainServiceOuterLedgerObservation(
    const TerrainRvtBudgetOwner& outer, uint64_t world, uint64_t completedEpoch, double sampledMs);

const double ModuleStartedMonotonicMs = QualityMonotonicMs();

double ProcessStartedMonotonicMs();

float Parameter(const MaterialInstance& material, const char* name, float fallback);

std::shared_ptr<const MaterialInstanceStateSnapshot> AuthorMaterialState(
    const MaterialInstanceStateSnapshot& snapshot, const std::shared_ptr<Texture>& runtimeLocalWeights,
    const std::shared_ptr<Texture>& authorWeights = {},
    const std::optional<std::string>& authoredWeightIdentity = std::nullopt);

std::optional<std::string> AuthoredWeightIdentity(const std::string& materialPath);

TerrainCompositedDependencyRoots Roots();

std::filesystem::path ManifestPath(const TerrainBuildPlan& plan);

HostImage Image(uint32_t width, uint32_t height, bool floating, std::vector<uint8_t> bytes);

void PublishSampledImage(
    vk::CommandBuffer command, const Texture& texture,
    vk::ImageLayout previousLayout = vk::ImageLayout::eShaderReadOnlyOptimal);

std::shared_ptr<Texture> Upload(RendererBackendVulkan& backend, const HostImage& image,
    const std::string& name, vk::Filter filter = vk::Filter::eNearest, bool array = false);

Baker::SampledTexture Sample(RendererBackendVulkan& backend, const std::shared_ptr<Texture>& texture);

std::array<float, 16> Placement(const Eigen::Matrix4f& model);

nlohmann::json BuildConfig(const MaterialInstance& material);

std::pair<HostImage, HostImage> Frames(const TerrainBuildPlan& plan);

TerrainCompositedVTUVRect SnapshotDirty(const TerrainBuildPlan& previous, const TerrainBuildPlan& next);

TerrainCompositedFrozenConfig ReadFrozenConfig();

TerrainRvtBudgetCaps FrozenCaps(const nlohmann::json& value);

TerrainRvtBudgetCaps TerrainCaps(uint64_t gpu, uint64_t cpu, const TerrainRvtBudgetCaps& rvt);

TerrainCompositedProductionAllocationOverheads FrozenOverheads();

uint64_t SourceTextureBytes(RendererBackendVulkan& backend, const Texture& texture);
} // namespace TerrainServiceDetail
} // namespace VL
