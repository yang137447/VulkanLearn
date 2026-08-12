#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "world/worldSnapshot.h"

namespace VL
{

enum class EnvironmentUpdateStage
{
    Idle,
    Cubemap,
    SphericalHarmonics,
    Prefilter,
    Commit,
    AwaitingCommit
};

struct EnvironmentUpdateBudget
{
    uint32_t cubemapFacesPerFrame = 1;
    uint32_t shUpdatesPerFrame = 1;
    uint32_t prefilterMipsPerFrame = 1;
    uint32_t commitsPerFrame = 1;
};

struct EnvironmentUpdateFramePlan
{
    uint64_t generation = 0;
    std::array<uint32_t, 6> cubemapFaces{};
    uint32_t cubemapFaceCount = 0;
    bool projectSphericalHarmonics = false;
    std::vector<uint32_t> prefilterMips;
    bool commit = false;

    bool HasWork() const
    {
        return cubemapFaceCount > 0 ||
            projectSphericalHarmonics ||
            !prefilterMips.empty() ||
            commit;
    }
};

struct EnvironmentUpdateProgress
{
    uint64_t activeGeneration = 0;
    uint64_t pendingGeneration = 0;
    EnvironmentUpdateStage stage = EnvironmentUpdateStage::Idle;
    uint32_t cubemapFacesCompleted = 0;
    uint32_t cubemapFaceCount = 0;
    uint32_t shUpdatesCompleted = 0;
    uint32_t shUpdateCount = 1;
    uint32_t prefilterMipsCompleted = 0;
    uint32_t prefilterMipCount = 0;
    bool usingPreviousResources = false;
};

// 只负责环境派生资源的代际、游标和每帧预算。
// Vulkan 资源与命令录制由 generator/baker 持有，调度器因此可以独立验证状态推进。
class EnvironmentUpdateScheduler
{
public:
    void Reset();
    void SetBudget(const EnvironmentUpdateBudget& budget);

    // 新请求会放弃尚未提交的 pending 内容并从游标零重新生成，
    // 已提交的 active 代际保持不变，直到新代际完成原子提交。
    bool RequestUpdate(
        uint64_t generation,
        const EnvironmentSnapshot& sourceSnapshot,
        uint32_t prefilterMipCount);

    // 每个渲染帧调用一次：按当前阶段和预算生成本帧工作批次，并推进内部阶段与游标。
    // 此函数不是只读查询；同一渲染帧不要重复调用，返回的 plan 也不代表整个更新已完成。
    EnvironmentUpdateFramePlan BuildFramePlan();
    void CompleteCommit(uint64_t generation);

    bool IsUpdating() const { return updating; }
    uint64_t GetPendingGeneration() const { return pendingGeneration; }
    const EnvironmentSnapshot& GetPendingSnapshot() const { return pendingSnapshot; }
    EnvironmentUpdateProgress GetProgress() const;

private:
    static constexpr uint32_t kCubemapFaceCount = 6;

    EnvironmentUpdateBudget budget;
    EnvironmentSnapshot pendingSnapshot;
    EnvironmentUpdateStage stage = EnvironmentUpdateStage::Idle;
    uint64_t activeGeneration = 0;
    uint64_t pendingGeneration = 0;
    uint32_t nextCubemapFaceIndex = 0;
    uint32_t cubemapFaceCount = 0;
    uint32_t shCursor = 0;
    uint32_t prefilterMipCursor = 0;
    uint32_t prefilterMipCount = 0;
    bool updating = false;
};

const char* ToString(EnvironmentUpdateStage stage);

} // namespace VL
