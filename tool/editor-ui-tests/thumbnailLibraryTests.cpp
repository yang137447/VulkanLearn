#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "editor/assets/editorThumbnailLibrary.h"
#include "shader/build/atomicFile.h"
#include "engine/editorThumbnailGraph.h"

namespace
{
using namespace VL;
using namespace VL::Editor;
using namespace VL::EditorUi;

class ThumbnailLibraryTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("vl-thumbnail-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence++));
        std::filesystem::create_directories(root);
        Source(255, 0, 0, 255);
        Descriptor("T_color.json");
        Start();
    }
    void TearDown() override
    {
        library.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void Start()
    {
        library = std::make_unique<EditorThumbnailLibrary>();
        std::string error;
        ASSERT_TRUE(library->Initialize(root, root / "cache", error)) << error;
    }
    void Source(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        std::vector<uint8_t> bytes(18 + 64 * 32 * 4, 0);
        bytes[2] = 2; bytes[12] = 64; bytes[14] = 32; bytes[16] = 32; bytes[17] = 0x28;
        for (size_t i = 18; i < bytes.size(); i += 4)
        { bytes[i] = b; bytes[i + 1] = g; bytes[i + 2] = r; bytes[i + 3] = a; }
        WriteBinaryFileAtomically(root / "image.tga", bytes);
    }
    void Descriptor(const std::string& path, const std::string& space = "srgb", const std::string& alpha = "unused")
    {
        WriteTextFileAtomically(root / path, "{\"type\":\"texture\",\"source\":\"image.tga\",\"colorSpace\":\"" +
            space + "\",\"channelsDescription\":{\"a\":\"" + alpha + "\"}}");
    }
    EditorThumbnail Wait(const std::string& path)
    {
        for (size_t i = 0; i < 2000; ++i)
        {
            library->Poll();
            const auto snapshot = library->GetSnapshot();
            const auto it = snapshot->entries.find(path);
            if (it != snapshot->entries.end()) return it->second;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "Thumbnail did not complete: " << path;
        return {};
    }
    std::optional<ThumbnailRenderRequest> WaitRender()
    {
        for (size_t i = 0; i < 2000; ++i)
        {
            if (auto request = library->TakeRenderRequest()) return request;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return {};
    }
    std::filesystem::path root;
    std::unique_ptr<EditorThumbnailLibrary> library;
};

TEST_F(ThumbnailLibraryTest, RenderRequestsAreTakenOnceAndPublishOnlyCompletedPixels)
{
    WriteTextFileAtomically(root / "MI_ball.json", R"({"type":"materialInstance","material":"M_ball.json"})");
    library->RequestVisible({"MI_ball.json"});
    auto request = WaitRender();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->kind, EditorAssetKind::MaterialInstance);
    EXPECT_FALSE(library->TakeRenderRequest());
    library->Poll();
    EXPECT_TRUE(library->GetSnapshot()->entries.empty());
    auto image = std::make_shared<EditorTexturePreview>();
    image->width = image->height = 256;
    image->pixels.assign(256 * 256 * 4, 128);
    library->CompleteRenderRequest(*request, image, {});
    EXPECT_EQ(Wait("MI_ball.json").image, image);
    EXPECT_FALSE(library->TakeRenderRequest());
}

TEST_F(ThumbnailLibraryTest, RefreshRejectsOldRenderCompletion)
{
    WriteTextFileAtomically(root / "SM_ball.json", R"({"type":"mesh","modelDataPath":"ball.obj"})");
    library->RequestVisible({"SM_ball.json"});
    auto old = WaitRender();
    ASSERT_TRUE(old);
    library->Refresh();
    auto current = WaitRender();
    ASSERT_TRUE(current);
    EXPECT_FALSE(library->IsRenderRequestCurrent(*old));
    library->CompleteRenderRequest(*old, {}, "obsolete");
    EXPECT_TRUE(library->IsRenderRequestCurrent(*current));
    library->CompleteRenderRequest(*current, {}, "current failure");
    EXPECT_EQ(Wait("SM_ball.json").diagnostic, "current failure");
}

TEST_F(ThumbnailLibraryTest, InvisibleRenderRequestDoesNotBlockTextureQueue)
{
    WriteTextFileAtomically(root / "SM_ball.json", R"({"type":"mesh","modelDataPath":"ball.obj"})");
    library->RequestVisible({"SM_ball.json"});
    auto old = WaitRender();
    ASSERT_TRUE(old);
    library->RequestVisible({"T_color.json"});
    EXPECT_FALSE(library->IsRenderRequestCurrent(*old));
    library->CompleteRenderRequest(*old, {}, "obsolete");
    EXPECT_NE(Wait("T_color.json").image, nullptr);
    EXPECT_EQ(library->GetSnapshot()->entries.count("SM_ball.json"), 0u);
}

TEST_F(ThumbnailLibraryTest, ReinitializeRejectsRequestFromPreviousLibrary)
{
    WriteTextFileAtomically(root / "SM_ball.json", R"({"type":"mesh","modelDataPath":"ball.obj"})");
    library->RequestVisible({"SM_ball.json"});
    auto old = WaitRender();
    ASSERT_TRUE(old);
    Start();
    library->RequestVisible({"SM_ball.json"});
    auto current = WaitRender();
    ASSERT_TRUE(current);
    EXPECT_NE(old->epoch, current->epoch);
    library->CompleteRenderRequest(*old, {}, "obsolete");
    EXPECT_TRUE(library->IsRenderRequestCurrent(*current));
}

TEST_F(ThumbnailLibraryTest, PausingPreventsTakingRenderWork)
{
    WriteTextFileAtomically(root / "SM_ball.json", R"({"type":"mesh","modelDataPath":"ball.obj"})");
    library->SetPaused(true);
    library->RequestVisible({"SM_ball.json"});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(library->TakeRenderRequest());
    library->SetPaused(false);
    auto request = WaitRender();
    ASSERT_TRUE(request);
    library->SetPaused(true);
    library->CompleteRenderRequest(*request, {}, "unsupported material");
    library->Poll();
    EXPECT_TRUE(library->GetSnapshot()->entries.empty());
    library->SetPaused(false);
    EXPECT_EQ(Wait("SM_ball.json").diagnostic, "unsupported material");
}

TEST(EditorThumbnailGraph, KeepsPassInputsButReplacesPresentWithFixedSizeReadback)
{
    const auto original = nlohmann::json::parse(R"({"resources":[
        {"name":"swapChain","widthScale":1,"heightScale":1,"usage":["present"]},
        {"name":"bloom","widthScale":0.25,"heightScale":0.25},
        {"name":"autoExposure","widthSize":1,"heightSize":1}],
        "passes":[{"name":"grid","type":"editorGrid","output":[]},
        {"name":"toneMapping","type":"postProcess","input":[{"resource":"bloom","binding":2}],
        "output":[{"resource":"swapChain","loadOp":"clear"}]}]})");
    const auto graph = BuildEditorThumbnailGraph(original);
    EXPECT_EQ(original["resources"][0]["name"], "swapChain");
    EXPECT_EQ(graph["resources"][0]["name"], "thumbnailOutput");
    EXPECT_EQ(graph["resources"][0]["widthSize"], 256);
    EXPECT_EQ(graph["resources"][1]["widthSize"], 64);
    EXPECT_EQ(graph["resources"][2]["widthSize"], 1);
    EXPECT_FALSE(graph["resources"][0].contains("widthScale"));
    ASSERT_EQ(graph["passes"].size(), 1u);
    EXPECT_EQ(graph["passes"][0]["input"], original["passes"][1]["input"]);
    EXPECT_EQ(graph["passes"][0]["output"][0]["resource"], "thumbnailOutput");
    EXPECT_EQ(graph["passes"][0]["needMsaa"], false);
}

TEST_F(ThumbnailLibraryTest, PreservesAspectAndUsesOpaqueRgbForPackedAlpha)
{
    Source(255, 0, 0, 0);
    library->RequestVisible({"T_color.json"});
    const auto thumbnail = Wait("T_color.json");
    ASSERT_NE(thumbnail.image, nullptr) << thumbnail.diagnostic;
    EXPECT_EQ(thumbnail.image->width, 256u);
    EXPECT_EQ(thumbnail.image->pixels.size(), 256u * 256u * 4u);
    const size_t center = (128 * 256 + 128) * 4;
    EXPECT_EQ(thumbnail.image->pixels[center], 255);
    EXPECT_EQ(thumbnail.image->pixels[center + 1], 0);
    EXPECT_EQ(thumbnail.image->pixels[center + 3], 255);
    EXPECT_EQ(thumbnail.image->pixels[0], 35);
}

TEST_F(ThumbnailLibraryTest, ExplicitOpacityCompositesCheckerboard)
{
    Source(255, 0, 0, 0);
    Descriptor("T_color.json", "srgb", "opacity");
    library->RequestVisible({"T_color.json"});
    const auto thumbnail = Wait("T_color.json");
    ASSERT_NE(thumbnail.image, nullptr);
    const size_t center = (128 * 256 + 128) * 4;
    EXPECT_EQ(thumbnail.image->pixels[center], thumbnail.image->pixels[center + 1]);
    EXPECT_NE(thumbnail.image->pixels[center], thumbnail.image->pixels[center + 12 * 4]);
}

TEST_F(ThumbnailLibraryTest, ReusesDiskCacheAcrossLibraryLifetimes)
{
    library->RequestVisible({"T_color.json"});
    const auto first = Wait("T_color.json");
    ASSERT_NE(first.image, nullptr);
    EXPECT_FALSE(first.diskCacheHit);
    library.reset();
    Start();
    library->RequestVisible({"T_color.json"});
    const auto second = Wait("T_color.json");
    EXPECT_TRUE(second.diskCacheHit);
    EXPECT_EQ(second.source, first.source);
    ASSERT_NE(second.image, nullptr);
    EXPECT_EQ(second.image->pixels, first.image->pixels);
}

TEST_F(ThumbnailLibraryTest, SourceContentInvalidatesEvenWithUnchangedTimestamp)
{
    library->RequestVisible({"T_color.json"});
    const auto first = Wait("T_color.json");
    const auto time = std::filesystem::last_write_time(root / "image.tga");
    Source(0, 255, 0, 255);
    std::filesystem::last_write_time(root / "image.tga", time);
    library->Refresh();
    const auto second = Wait("T_color.json");
    EXPECT_NE(second.source, first.source);
    ASSERT_NE(second.image, nullptr);
    EXPECT_EQ(second.image->pixels[(128 * 256 + 128) * 4 + 1], 255);
}

TEST_F(ThumbnailLibraryTest, DescriptorColorSpaceChangesDisplayAndCacheKey)
{
    Source(128, 128, 128, 255);
    library->RequestVisible({"T_color.json"});
    const auto first = Wait("T_color.json");
    Descriptor("T_color.json", "linear");
    library->Refresh();
    const auto second = Wait("T_color.json");
    ASSERT_NE(first.image, nullptr);
    ASSERT_NE(second.image, nullptr);
    EXPECT_NE(first.source, second.source);
    EXPECT_GT(second.image->pixels[(128 * 256 + 128) * 4], first.image->pixels[(128 * 256 + 128) * 4]);
}

TEST_F(ThumbnailLibraryTest, CorruptCacheRegeneratesInsteadOfPublishingGarbage)
{
    library->RequestVisible({"T_color.json"});
    const auto first = Wait("T_color.json");
    ASSERT_NE(first.image, nullptr);
    library.reset();
    for (const auto& file : std::filesystem::directory_iterator(root / "cache"))
        WriteBinaryFileAtomically(file.path(), std::vector<uint8_t>(256 * 256 * 4 + 32, 0));
    Start();
    library->RequestVisible({"T_color.json"});
    const auto second = Wait("T_color.json");
    EXPECT_FALSE(second.diskCacheHit);
    ASSERT_NE(second.image, nullptr);
    EXPECT_EQ(second.image->pixels, first.image->pixels);
}

TEST_F(ThumbnailLibraryTest, RejectsEscapingSourceAndRetriesOnlyOnRefresh)
{
    WriteTextFileAtomically(root / "T_color.json", R"({"type":"texture","source":"../outside.png","colorSpace":"srgb"})");
    library->RequestVisible({"T_color.json"});
    const auto failed = Wait("T_color.json");
    EXPECT_EQ(failed.image, nullptr);
    EXPECT_FALSE(failed.diagnostic.empty());
    const auto revision = library->GetSnapshot()->revision;
    Descriptor("T_color.json");
    for (size_t i = 0; i < 10; ++i) { library->RequestVisible({"T_color.json"}); library->Poll(); }
    EXPECT_EQ(library->GetSnapshot()->revision, revision);
    library->Refresh();
    EXPECT_NE(Wait("T_color.json").image, nullptr);
}

TEST_F(ThumbnailLibraryTest, PausedQueueUsesLatestVisibleSetAndReleasesHiddenResults)
{
    Descriptor("T_next.json");
    library->SetPaused(true);
    library->RequestVisible({"T_color.json"});
    library->RequestVisible({"T_next.json"});
    library->Poll();
    EXPECT_TRUE(library->GetSnapshot()->entries.empty());
    library->SetPaused(false);
    EXPECT_NE(Wait("T_next.json").image, nullptr);
    EXPECT_FALSE(library->GetSnapshot()->entries.count("T_color.json"));
    library->RequestVisible({});
    library->Poll();
    EXPECT_TRUE(library->GetSnapshot()->entries.empty());
}

TEST_F(ThumbnailLibraryTest, WarmPollDoesNotRepublishUnchangedSnapshot)
{
    library->RequestVisible({"T_color.json", "T_color.json"});
    Wait("T_color.json");
    const auto snapshot = library->GetSnapshot();
    for (size_t i = 0; i < 100; ++i) library->Poll();
    EXPECT_EQ(snapshot, library->GetSnapshot());
    EXPECT_EQ(snapshot->entries.size(), 1u);
}

TEST_F(ThumbnailLibraryTest, RadianceHdrUsesFixedDisplayMapping)
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), {128, 128, 128, 129});
    WriteBinaryFileAtomically(root / "image.hdr", bytes);
    WriteTextFileAtomically(root / "T_hdr.json", R"({"type":"texture","source":"image.hdr","colorSpace":"linear"})");
    library->RequestVisible({"T_hdr.json"});
    const auto thumbnail = Wait("T_hdr.json");
    ASSERT_NE(thumbnail.image, nullptr) << thumbnail.diagnostic;
    EXPECT_NEAR(thumbnail.image->pixels[0], 188, 1);
}

TEST_F(ThumbnailLibraryTest, OversizedImageHeaderIsRejectedBeforeDecode)
{
    std::vector<uint8_t> bytes(18, 0);
    bytes[2] = 2; bytes[12] = bytes[13] = bytes[14] = bytes[15] = 255; bytes[16] = 32;
    WriteBinaryFileAtomically(root / "image.tga", bytes);
    library->RequestVisible({"T_color.json"});
    const auto thumbnail = Wait("T_color.json");
    EXPECT_EQ(thumbnail.image, nullptr);
    EXPECT_NE(thumbnail.diagnostic.find("pixel limit"), std::string::npos);
}

TEST_F(ThumbnailLibraryTest, MemoryCachePublicationIsLimitedToTwoEntriesPerPoll)
{
    const std::vector<std::string> paths{"T_color.json", "T_second.json", "T_third.json"};
    Descriptor("T_second.json");
    Descriptor("T_third.json");
    library->RequestVisible(paths);
    for (const auto& path : paths) ASSERT_NE(Wait(path).image, nullptr);
    library->RequestVisible({});
    library->Poll();
    library->RequestVisible(paths);
    library->Poll();
    EXPECT_EQ(library->GetSnapshot()->entries.size(), 2u);
    library->Poll();
    EXPECT_EQ(library->GetSnapshot()->entries.size(), 3u);
}
} // namespace
