#include <gtest/gtest.h>
#include "render/backend/resolvedRenderScene.h"

namespace VL
{
TEST(RendererObjectPublication, EmptySceneNeedsNoObjectResources)
{
    ResolvedRenderScene scene;
    EXPECT_FALSE(scene.NeedsObjectResources());
    scene.materialGroups.resize(1);
    scene.materialGroups[0].materialInstances.resize(1);
    EXPECT_FALSE(scene.NeedsObjectResources());
}

TEST(RendererObjectPublication, NewDrawInSameWorldRequiresObjectPackageBeforeRecording)
{
    ResolvedRenderScene scene;
    scene.materialGroups.resize(2);
    scene.materialGroups[1].materialInstances.resize(2);
    scene.materialGroups[1].materialInstances[1].draws.resize(1);
    EXPECT_TRUE(scene.NeedsObjectResources());
    scene.materialGroups[1].materialInstances[1].draws.clear();
    EXPECT_FALSE(scene.NeedsObjectResources());
}
} // namespace VL
