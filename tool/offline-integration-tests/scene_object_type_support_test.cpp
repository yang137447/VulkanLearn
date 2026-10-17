#include <gtest/gtest.h>

#include "ui/sceneObjectTypeSupport.h"

namespace VL
{
TEST(SceneObjectTypeSupport, AcceptsOfflineAreaLightScene)
{
    const nlohmann::json scene = {
        {"objects", {
            {{"type", "mesh"}},
            {{"type", "squareAreaLight"}},
            {{"type", "camera"}}
        }}
    };

    EXPECT_TRUE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupport, RejectsLegacySunLight)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "sunLight"}}}}
    };

    EXPECT_FALSE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupport, AcceptsActorLevelForReturnFromAssetPreview)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "blueprintInstance"}}, {{"type", "camera"}}, {{"type", "environment"}}}}
    };
    EXPECT_TRUE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupport, RejectsEditorOnlyTerrain)
{
    EXPECT_FALSE(HasSupportedSceneObjectTypes(
        {{"objects", {{{"type", "terrain"}}}}}));
}

TEST(SceneObjectTypeSupport, RejectsUnknownObjectType)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "unknownLight"}}}}
    };

    EXPECT_FALSE(HasSupportedSceneObjectTypes(scene));
}
} // namespace VL
