#include <gtest/gtest.h>

#include "particles/particleRenderIdentity.h"

namespace
{

using namespace VL;

ParticleRenderIdentity Identity()
{
    ParticleRenderIdentity identity;
    identity.ownerGeneration = 9;
    identity.particleRenderPass = 0x1000;
    identity.swapchainImageCount = 3;
    identity.passWidth = 1920;
    identity.passHeight = 1080;
    identity.pipelineContractKey = "pass-contract";
    return identity;
}

} // namespace

TEST(ParticleRenderIdentity, EqualIdentityMatches)
{
    EXPECT_EQ(Identity(), Identity());
}

TEST(ParticleRenderIdentity, EveryGraphicsDependencyChangesIdentity)
{
    const ParticleRenderIdentity baseline = Identity();

    ParticleRenderIdentity generationChanged = baseline;
    generationChanged.ownerGeneration += 1;
    EXPECT_NE(baseline, generationChanged);

    ParticleRenderIdentity renderPassChanged = baseline;
    renderPassChanged.particleRenderPass += 0x20;
    EXPECT_NE(baseline, renderPassChanged);

    ParticleRenderIdentity imageCountChanged = baseline;
    imageCountChanged.swapchainImageCount += 1;
    EXPECT_NE(baseline, imageCountChanged);

    ParticleRenderIdentity widthChanged = baseline;
    widthChanged.passWidth += 1;
    EXPECT_NE(baseline, widthChanged);

    ParticleRenderIdentity heightChanged = baseline;
    heightChanged.passHeight += 1;
    EXPECT_NE(baseline, heightChanged);

    ParticleRenderIdentity contractChanged = baseline;
    contractChanged.pipelineContractKey += "-changed";
    EXPECT_NE(baseline, contractChanged);
}

TEST(ParticleRenderIdentity, ResizeWithReusedOwnerGenerationStillRebuilds)
{
    ParticleRenderIdentity before = Identity();
    ParticleRenderIdentity afterResize = before;
    afterResize.passWidth = 1280;
    afterResize.passHeight = 720;

    EXPECT_EQ(before.ownerGeneration, afterResize.ownerGeneration);
    EXPECT_NE(before, afterResize);
}
