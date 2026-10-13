#include <gtest/gtest.h>

#include <memory>

#include "editor/ui/editorResourceEditorController.h"

namespace
{
using namespace VL::EditorUi;

std::shared_ptr<EditorJsonDocumentSnapshot> Document(const char* path, EditorAssetKind kind)
{
    auto result = std::make_shared<EditorJsonDocumentSnapshot>();
    result->assetPath = path;
    result->kind = kind;
    return result;
}

TEST(EditorResourceEditorControllerTest, OpensUniqueTabsAndActivatesExistingPath)
{
    EditorResourceEditorController controller;
    EXPECT_TRUE(controller.Open("Meshes/SM_a.json", EditorAssetKind::Mesh));
    EXPECT_TRUE(controller.Open("Textures/T_a.json", EditorAssetKind::Texture));
    EXPECT_TRUE(controller.Open("Meshes/SM_a.json", EditorAssetKind::Mesh));
    ASSERT_NE(controller.ActiveTab(), nullptr);
    EXPECT_EQ(controller.ActiveTab()->path, "Meshes/SM_a.json");
    EXPECT_EQ(controller.Tabs().size(), 2u);
}

TEST(EditorResourceEditorControllerTest, LateDocumentCannotOverwriteAnotherActiveTab)
{
    EditorResourceEditorController controller;
    ASSERT_TRUE(controller.Open("Meshes/SM_a.json", EditorAssetKind::Mesh));
    ASSERT_TRUE(controller.Open("Textures/T_a.json", EditorAssetKind::Texture));
    auto mesh = Document("Meshes/SM_a.json", EditorAssetKind::Mesh);
    auto texture = Document("Textures/T_a.json", EditorAssetKind::Texture);
    EXPECT_FALSE(controller.SetDocument(mesh, false));
    EXPECT_TRUE(controller.SetDocument(texture, false));
    ASSERT_NE(controller.ActiveTab(), nullptr);
    EXPECT_EQ(controller.ActiveTab()->snapshot, texture);
    EXPECT_TRUE(controller.Activate("Meshes/SM_a.json"));
    EXPECT_EQ(controller.ActiveTab()->snapshot, nullptr);
}

TEST(EditorResourceEditorControllerTest, ReloadMarksOnlyActiveTabLoadingAndConsumesRequest)
{
    EditorResourceEditorController controller;
    ASSERT_TRUE(controller.Open("Textures/T_a.json", EditorAssetKind::Texture));
    auto document = Document("Textures/T_a.json", EditorAssetKind::Texture);
    ASSERT_TRUE(controller.SetDocument(document, false));
    EXPECT_EQ(controller.RequestReload(), "Textures/T_a.json");
    ASSERT_NE(controller.ActiveTab(), nullptr);
    EXPECT_TRUE(controller.ActiveTab()->loading);
    EXPECT_EQ(controller.ConsumeReloadRequest(), "Textures/T_a.json");
    EXPECT_EQ(controller.ConsumeReloadRequest(), "");
}
} // namespace
