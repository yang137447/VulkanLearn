#include <gtest/gtest.h>

#include <RmlUi/Core/Core.h>

#include "ui/rmlUiInterfaces.h"

namespace
{
using namespace VL;

class TextureTestSystem final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return 0.0; }
};

class RmlUiTextureTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
    }
    void TearDown() override
    {
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    std::shared_ptr<EditorUi::EditorThumbnailSnapshot> PublishGpuImage()
    {
        auto snapshot = std::make_shared<EditorUi::EditorThumbnailSnapshot>();
        auto& thumbnail = snapshot->entries["MI_ball.json"];
        thumbnail.source = "vl://thumbnail/render/1/1";
        thumbnail.gpuImage = std::make_shared<const RendererGpuImageToken>(7, 256, 256);
        renderer.PublishThumbnails(snapshot);
        return snapshot;
    }
    TextureTestSystem system;
    RmlUiRenderInterface renderer;
};

TEST_F(RmlUiTextureTest, GpuThumbnailSnapshotContainsIdentityAndDimensionsWithoutPixels)
{
    const auto thumbnails = PublishGpuImage();
    Rml::Vector2i dimensions;
    const auto texture = renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1");
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(dimensions, Rml::Vector2i(256, 256));
    std::vector<UiTextureSnapshot> textures;
    renderer.AppendTextureSnapshots(textures);
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_EQ(textures[0].gpuImage, thumbnails->entries.at("MI_ball.json").gpuImage);
    EXPECT_FALSE(textures[0].rgba8Pixels);
    EXPECT_TRUE(textures[0].linearSampling);
    renderer.ReleaseTexture(texture);
}

TEST_F(RmlUiTextureTest, UnormOutputUsesEncodedSamplingForGpuThumbnail)
{
    renderer.SetLinearColorOutput(false);
    const auto thumbnails = PublishGpuImage();
    Rml::Vector2i dimensions;
    const auto texture = renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1");
    ASSERT_NE(texture, 0u);
    std::vector<UiTextureSnapshot> textures;
    renderer.AppendTextureSnapshots(textures);
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_FALSE(textures[0].linearSampling);
    EXPECT_FALSE(textures[0].rgba8Pixels);
    renderer.ReleaseTexture(texture);
}

TEST_F(RmlUiTextureTest, EmptyPublicationRemovesSourcesAfterPreviousSnapshotExpired)
{
    auto thumbnails = PublishGpuImage();
    Rml::Vector2i dimensions;
    const auto texture = renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1");
    ASSERT_NE(texture, 0u);
    thumbnails.reset();
    renderer.PublishThumbnails({});
    // 旧 texture 仍固定 token；清除 source 后也不能重新注册这个旧 URL。
    EXPECT_EQ(renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1"), 0u);
    renderer.ReleaseTexture(texture);
}

TEST_F(RmlUiTextureTest, FrozenSnapshotPinsGpuImageAfterUiTextureRelease)
{
    auto thumbnails = PublishGpuImage();
    const std::weak_ptr<const RendererGpuImageToken> lease = thumbnails->entries.at("MI_ball.json").gpuImage;
    Rml::Vector2i dimensions;
    const auto texture = renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1");
    ASSERT_NE(texture, 0u);
    std::vector<UiTextureSnapshot> frozen;
    renderer.AppendTextureSnapshots(frozen);
    renderer.PublishThumbnails({});
    thumbnails.reset();
    renderer.ReleaseTexture(texture);
    EXPECT_FALSE(lease.expired());
    std::vector<UiTextureSnapshot> next;
    renderer.AppendTextureSnapshots(next);
    EXPECT_TRUE(next.empty());
    frozen.clear();
    EXPECT_TRUE(lease.expired());
    EXPECT_EQ(renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1"), 0u);
}

TEST_F(RmlUiTextureTest, TextureReleaseDuringFrameIsDeferredUntilSnapshotIsFrozen)
{
    auto thumbnails = PublishGpuImage();
    const std::weak_ptr<const RendererGpuImageToken> lease = thumbnails->entries.at("MI_ball.json").gpuImage;
    Rml::Vector2i dimensions;
    const auto texture = renderer.LoadTexture(dimensions, "vl://thumbnail/render/1/1");
    ASSERT_NE(texture, 0u);
    UiRenderSnapshot frame;
    renderer.BeginFrame(frame);
    renderer.ReleaseTexture(texture);
    renderer.AppendTextureSnapshots(frame.textures);
    renderer.EndFrame();
    ASSERT_EQ(frame.textures.size(), 1u);
    renderer.PublishThumbnails({});
    thumbnails.reset();
    EXPECT_FALSE(lease.expired());
    frame.textures.clear();
    EXPECT_TRUE(lease.expired());
}

} // namespace
