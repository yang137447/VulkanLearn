#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include "terrain/terrainCompositedDirty.h"
#include "terrain/terrainCompositedRefresh.h"

namespace VL
{
struct TerrainCompositedDependencyRoots;

namespace Editor
{

class TerrainCompositedAuthorMonitor
{
public:
    using Clock = std::chrono::steady_clock;

    // 仅适配 baseline 的 terrain verificationOwner，批末关闭共享 owner；不维护第二份 proof/payload。
    class BaselineLoadSession
    {
    public:
        struct Statistics
        {
            uint64_t objectRequests = 0;
            uint64_t requestedBytes = 0;
            uint64_t objectReads = 0;
            uint64_t readBytes = 0;
            uint64_t cacheHits = 0;
            uint64_t failedReads = 0;
            uint64_t bindings = 0;
            bool closed = false;
        };

        BaselineLoadSession();
        ~BaselineLoadSession();
        BaselineLoadSession(const BaselineLoadSession&) = delete;
        BaselineLoadSession& operator=(const BaselineLoadSession&) = delete;
        void VerifyObject(
            const TerrainCompositedAuthorBaseline& baseline, const std::string& objectId);
        Statistics Stats() const;
        void Close();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    struct ChangedDependency
    {
        std::string root;
        std::string path;
        std::string mode;
        std::string expectedDigest;
        std::string observedDigest;
        std::string diagnostic;
    };

    struct SurfaceStatus
    {
        std::string surfaceId;
        std::string manifestHash;
        std::string diagnostic;
        bool dirty = false;
        bool checked = false;
        bool externalChange = false;
        bool conflict = false;
        uint64_t generation = 0;
        uint64_t editRevision = 0;
        uint64_t worldGeneration = 0;
        std::vector<ChangedDependency> changedDependencies;
        std::vector<TerrainCompositedDependencyChange> localChanges;
    };

    struct Statistics
    {
        size_t surfaces = 0;
        size_t dirtySurfaces = 0;
        size_t changedSurfaces = 0;
        size_t conflictSurfaces = 0;
        size_t uncheckedSurfaces = 0;
        uint64_t completedChecks = 0;
        uint64_t failedChecks = 0;
        uint64_t discardedChecks = 0;
        uint64_t metadataChecks = 0;
        uint64_t metadataCacheHits = 0;
        uint64_t fullReadCount = 0;
        uint64_t fullReadBytes = 0;
        bool scanPending = false;
    };

    explicit TerrainCompositedAuthorMonitor(
        std::chrono::milliseconds interval = std::chrono::milliseconds(1000),
        std::chrono::milliseconds fullHashInterval = std::chrono::milliseconds(60000));
    ~TerrainCompositedAuthorMonitor();
    TerrainCompositedAuthorMonitor(const TerrainCompositedAuthorMonitor&) = delete;
    TerrainCompositedAuthorMonitor& operator=(const TerrainCompositedAuthorMonitor&) = delete;

    bool RegisterSurface(
        const std::string& surfaceId,
        const TerrainCompositedDependencyRoots& roots,
        const nlohmann::json& dependencyManifest,
        std::string& error);
    bool LoadBaseline(
        const TerrainCompositedAuthorBaseline& baseline,
        const TerrainCompositedDependencyRoots& roots,
        std::string& error);
    bool LoadBaseline(
        const TerrainCompositedAuthorBaseline& baseline,
        const TerrainCompositedDependencyRoots& roots,
        const std::shared_ptr<BaselineLoadSession>& session,
        std::string& error);
    void UnregisterSurface(const std::string& surfaceId);
    std::vector<SurfaceStatus> Poll(Clock::time_point nowTime);
    bool MarkEdited(const std::string& surfaceId);
    bool MarkEdited(const std::string& surfaceId, TerrainCompositedDependencyChange kind);
    bool MarkSaved(const std::string& surfaceId);
    bool MarkSaved(const std::string& surfaceId,
        TerrainCompositedDependencyChange kind, uint64_t editRevision, uint64_t generation);
    bool CanRecook(const std::string& surfaceId) const;
    bool CheckBeforeRecook(const std::string& surfaceId, std::string& error);
    bool GetStatus(const std::string& surfaceId, SurfaceStatus& result) const;
    Statistics Stats() const;
    BaselineLoadSession::Statistics BaselineStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
}
