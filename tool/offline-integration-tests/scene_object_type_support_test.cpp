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

TEST(SceneObjectTypeSupport, RejectsUnknownObjectType)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "unknownLight"}}}}
    };

    EXPECT_FALSE(HasSupportedSceneObjectTypes(scene));
}
} // namespace VL
