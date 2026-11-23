#pragma once

#include "terrainLitTarget.h"
#include "terrainReferenceAtlas.h"
#include "render/diagnostics/terrainCaptureHooks.h"
#include <filesystem>
#include <map>
#include <string>

namespace VL
{
struct PassRuntimeContext;

struct TerrainLitReplayConfig
{
    TerrainLitRect rect;
    std::filesystem::path expandedLightingFragment;
    std::filesystem::path expandedToneMappingFragment;
    std::filesystem::path output;
    std::string inputClosureSha256;
    std::string originalEvaluatorSha256;
    std::string referencePageSetSha256;
    std::vector<uint32_t> allActualRoiSampledSlots;
    // 页面身份绑定：key -> 实际 slot 的解析结果与来源（同一次运行的 TVQ1 导出帧）。
    std::string resolvedPageSlotJson;
    std::string pageMappingSource;
};

class TerrainLitReplaySession
{
public:
    TerrainLitReplaySession(const TerrainCaptureFrameInfo& actualFrame,
        std::shared_ptr<TerrainReferenceAtlas> admittedOriginalReference,
        TerrainLitReplayConfig config);
    ~TerrainLitReplaySession();
    void AfterActualPass(PassRuntimeContext& actualPass);
    std::shared_ptr<const void> Submitted(uint64_t actualEpoch);
    void AbortProvenUnsubmitted();
    bool ConsumeCompleted(uint64_t actualCompletedEpoch);
    std::shared_ptr<const void> Owner()const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
