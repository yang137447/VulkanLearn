#include <gtest/gtest.h>

#include "ui/sceneObjectTypeSupport.h"

namespace VL
{

TEST(SceneObjectTypeSupportTest, AcceptsTerrainOnlyScene)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "terrain"}}}}
    };

    EXPECT_TRUE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupportTest, AcceptsTerrainWithStandardSceneObjects)
{
    const nlohmann::json scene = {
        {"objects", {
            {{"type", "terrain"}},
            {{"type", "directionalLight"}},
            {{"type", "camera"}},
            {{"type", "environment"}}
        }}
    };

    EXPECT_TRUE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupportTest, RejectsUnknownSceneObjectType)
{
    const nlohmann::json scene = {
        {"objects", {{{"type", "unknownTerrain"}}}}
    };

    EXPECT_FALSE(HasSupportedSceneObjectTypes(scene));
}

TEST(SceneObjectTypeSupportTest, AcceptsEmptyObjectsArray)
{
    const nlohmann::json scene = {
        {"objects", nlohmann::json::array()}
    };

    EXPECT_TRUE(HasSupportedSceneObjectTypes(scene));
}

} // namespace VL
