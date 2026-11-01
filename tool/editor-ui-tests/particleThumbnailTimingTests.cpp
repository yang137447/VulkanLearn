#include <gtest/gtest.h>

#include "engine/editorThumbnailRenderSession.h"
#include "render/offline/offlineParticleSimulation.h"

namespace
{

TEST(ParticleThumbnailTiming, UsesBoundedFixedSteps)
{
    const VL::ParticleThumbnailStepPolicy policy =
        VL::ParticleThumbnailSteps();
    EXPECT_FLOAT_EQ(policy.fixedDeltaSeconds, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(policy.previewSeconds, 0.5f);
    EXPECT_EQ(policy.maxSteps, 32u);
    // 0.5s / (1/60s) = 30 步；上限 32 只用于防御未来参数放大。
    EXPECT_EQ(VL::ParticleThumbnailStepCount(), 30u);
}

TEST(ParticleThumbnailTiming, OfflinePreviewUsesSameStepDefinition)
{
    const VL::ParticleThumbnailStepPolicy thumbnail =
        VL::ParticleThumbnailSteps();
    const VL::OfflineParticlePreviewPolicy defaults =
        VL::OfflineParticlePreviewDefaults();
    EXPECT_FLOAT_EQ(thumbnail.fixedDeltaSeconds, defaults.fixedDeltaSeconds);
    EXPECT_FLOAT_EQ(thumbnail.previewSeconds, defaults.previewSeconds);
    EXPECT_EQ(thumbnail.maxSteps, defaults.maxSteps);
    const VL::OfflineParticlePreviewPolicy offline{
        thumbnail.fixedDeltaSeconds,
        thumbnail.previewSeconds,
        thumbnail.maxSteps};
    EXPECT_EQ(VL::ComputeOfflineParticleStepCount(offline),
        VL::ParticleThumbnailStepCount());

    // 合法输入至少一步，且任何预览时长都不能突破 maxSteps。
    const VL::OfflineParticlePreviewPolicy zero{
        thumbnail.fixedDeltaSeconds, 0.0f, thumbnail.maxSteps};
    EXPECT_EQ(VL::ComputeOfflineParticleStepCount(zero), 0u);
    const VL::OfflineParticlePreviewPolicy longPreview{
        thumbnail.fixedDeltaSeconds, 10.0f, thumbnail.maxSteps};
    EXPECT_EQ(VL::ComputeOfflineParticleStepCount(longPreview),
        thumbnail.maxSteps);
}

} // namespace
