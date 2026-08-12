#include "render/environment/environmentUpdateScheduler.h"

#include <algorithm>
#include <stdexcept>

namespace VL
{

void EnvironmentUpdateScheduler::Reset()
{
    pendingSnapshot = EnvironmentSnapshot{};
    stage = EnvironmentUpdateStage::Idle;
    activeGeneration = 0;
    pendingGeneration = 0;
    nextCubemapFaceIndex = 0;
    cubemapFaceCount = 0;
    shCursor = 0;
    prefilterMipCursor = 0;
    prefilterMipCount = 0;
    updating = false;
}

void EnvironmentUpdateScheduler::SetBudget(const EnvironmentUpdateBudget& newBudget)
{
    if (newBudget.cubemapFacesPerFrame == 0 ||
        newBudget.shUpdatesPerFrame == 0 ||
        newBudget.prefilterMipsPerFrame == 0 ||
        newBudget.commitsPerFrame == 0)
    {
        throw std::invalid_argument("Environment update budgets must be greater than zero.");
    }

    budget = newBudget;
}

bool EnvironmentUpdateScheduler::RequestUpdate(
    uint64_t generation,
    const EnvironmentSnapshot& sourceSnapshot,
    uint32_t requestedPrefilterMipCount)
{
    if (requestedPrefilterMipCount == 0)
    {
        throw std::invalid_argument("Environment prefilter mip count must be greater than zero.");
    }

    if (updating && generation == pendingGeneration)
    {
        return false;
    }

    pendingSnapshot = sourceSnapshot;
    pendingGeneration = generation;
    nextCubemapFaceIndex = 0;
    cubemapFaceCount = sourceSnapshot.type == EnvironmentType::ProceduralSky
        ? kCubemapFaceCount
        : 0;
    shCursor = 0;
    prefilterMipCursor = 0;
    prefilterMipCount = requestedPrefilterMipCount;
    stage = cubemapFaceCount > 0
        ? EnvironmentUpdateStage::Cubemap
        : EnvironmentUpdateStage::SphericalHarmonics;
    updating = true;
    return true;
}

EnvironmentUpdateFramePlan EnvironmentUpdateScheduler::BuildFramePlan()
{
    EnvironmentUpdateFramePlan plan;
    if (!updating)
    {
        return plan;
    }

    plan.generation = pendingGeneration;

    if (stage == EnvironmentUpdateStage::Cubemap)
    {
        const uint32_t remainingFaces = cubemapFaceCount - nextCubemapFaceIndex;
        // 这里只为当前渲染帧生成一个受预算约束的 face 批次，并将下一待处理 face 推进到下一帧。
        // 默认 cubemapFacesPerFrame == 1，因此连续调用 BuildFramePlan() 才会依次得到 Face 0~5。
        plan.cubemapFaceCount = std::min(budget.cubemapFacesPerFrame, remainingFaces);
        for (uint32_t faceOffset = 0; faceOffset < plan.cubemapFaceCount; ++faceOffset)
        {
            plan.cubemapFaces[faceOffset] = nextCubemapFaceIndex + faceOffset;
        }
        nextCubemapFaceIndex += plan.cubemapFaceCount;

        if (nextCubemapFaceIndex == cubemapFaceCount)
        {
            stage = EnvironmentUpdateStage::SphericalHarmonics;
        }
    }

    if (stage == EnvironmentUpdateStage::SphericalHarmonics &&
        budget.shUpdatesPerFrame > 0)
    {
        plan.projectSphericalHarmonics = true;
        shCursor = 1;
        stage = EnvironmentUpdateStage::Prefilter;
    }

    if (stage == EnvironmentUpdateStage::Prefilter)
    {
        const uint32_t remainingMips = prefilterMipCount - prefilterMipCursor;
        const uint32_t mipWorkCount = std::min(
            budget.prefilterMipsPerFrame,
            remainingMips);
        plan.prefilterMips.reserve(mipWorkCount);
        for (uint32_t mipOffset = 0; mipOffset < mipWorkCount; ++mipOffset)
        {
            plan.prefilterMips.push_back(prefilterMipCursor + mipOffset);
        }
        prefilterMipCursor += mipWorkCount;

        if (prefilterMipCursor == prefilterMipCount)
        {
            stage = EnvironmentUpdateStage::Commit;
        }
    }

    if (stage == EnvironmentUpdateStage::Commit && budget.commitsPerFrame > 0)
    {
        plan.commit = true;
        stage = EnvironmentUpdateStage::AwaitingCommit;
    }

    return plan;
}

void EnvironmentUpdateScheduler::CompleteCommit(uint64_t generation)
{
    if (!updating ||
        stage != EnvironmentUpdateStage::AwaitingCommit ||
        generation != pendingGeneration)
    {
        throw std::runtime_error("Environment update commit does not match the pending generation.");
    }

    activeGeneration = generation;
    stage = EnvironmentUpdateStage::Idle;
    updating = false;
}

EnvironmentUpdateProgress EnvironmentUpdateScheduler::GetProgress() const
{
    EnvironmentUpdateProgress progress;
    progress.activeGeneration = activeGeneration;
    progress.pendingGeneration = updating ? pendingGeneration : 0;
    progress.stage = stage;
    progress.cubemapFacesCompleted = nextCubemapFaceIndex;
    progress.cubemapFaceCount = cubemapFaceCount;
    progress.shUpdatesCompleted = shCursor;
    progress.prefilterMipsCompleted = prefilterMipCursor;
    progress.prefilterMipCount = prefilterMipCount;
    progress.usingPreviousResources = updating;
    return progress;
}

const char* ToString(EnvironmentUpdateStage stage)
{
    switch (stage)
    {
    case EnvironmentUpdateStage::Idle:
        return "Idle";
    case EnvironmentUpdateStage::Cubemap:
        return "Cubemap";
    case EnvironmentUpdateStage::SphericalHarmonics:
        return "SH";
    case EnvironmentUpdateStage::Prefilter:
        return "Prefilter";
    case EnvironmentUpdateStage::Commit:
        return "Commit";
    case EnvironmentUpdateStage::AwaitingCommit:
        return "AwaitingCommit";
    }

    return "Unknown";
}

} // namespace VL
