#include <gtest/gtest.h>
#include <array>
#include <limits>

#include "render/offline/offlineDisplayVulkan.h"

namespace VL
{
namespace
{
Offline::FilmSnapshot CompleteFilm(int width, int height)
{
    Offline::FilmSnapshot film;
    film.width = width;
    film.height = height;
    film.generation = 7;
    film.completedSamples = 1;
    film.direct.resize(size_t(width) * height);
    film.indirect.resize(size_t(width) * height);
    return film;
}

std::shared_ptr<Offline::PreviewSnapshot> SparsePreview(int width, int height,
    uint64_t generation, std::initializer_list<size_t> validPixels)
{
    auto preview = std::make_shared<Offline::PreviewSnapshot>();
    preview->generation = generation;
    preview->width = width;
    preview->height = height;
    const size_t count = size_t(width) * height;
    preview->valid.resize(count);
    preview->direct.resize(count);
    preview->indirect.resize(count);
    for (const size_t index : validPixels) preview->valid.at(index) = 1;
    preview->validCount = validPixels.size();
    return preview;
}
}

TEST(OfflineDisplayData, NewSampleOrAovRequiresUpload)
{
    auto film = CompleteFilm(4, 3);
    film.completedSamples = 2;
    const OfflineDisplayUploadState uploaded{7, 2, OfflineAov::Combined, true};

    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Combined, 7, uploaded));
    film.completedSamples = 3;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Combined, 7, uploaded));
    film.completedSamples = 2;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Direct, 7, uploaded));
}

TEST(OfflineDisplayData, RejectsOldGenerationAndMismatchedResize)
{
    auto film = CompleteFilm(4, 3);
    const OfflineDisplayUploadState empty;

    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Combined, 8, empty));
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 5, 3, OfflineAov::Combined, 7, empty));
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Combined, 7, empty));
}

TEST(OfflineDisplayData, RejectsOlderSampleWithinSameGeneration)
{
    auto film = CompleteFilm(4, 3);
    const OfflineDisplayUploadState uploaded{7, 2, OfflineAov::Combined, true};

    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Direct, 7, uploaded));
    film.generation = 8;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Combined, 7, uploaded));
}

TEST(OfflineDisplayData, EverySwapchainSlotCatchesUpToRetainedCameraView)
{
    auto oldFilm = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(80, 45));
    auto newFilm = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(80, 45));
    newFilm->generation = 8;
    std::array<OfflineDisplayUploadState, 3> slots{};
    std::shared_ptr<const Offline::FilmSnapshot> retained;

    retained = SelectOfflineDisplayFilm(oldFilm, retained, 1280, 720);
    for (auto& slot : slots)
    {
        ASSERT_TRUE(NeedsOfflineFilmUpload(*retained, 1280, 720,
            OfflineAov::Combined, retained->generation, slot));
        slot = {retained->generation, retained->completedSamples, OfflineAov::Combined, true};
    }

    retained = SelectOfflineDisplayFilm(newFilm, retained, 1280, 720);
    ASSERT_EQ(retained, newFilm);
    ASSERT_TRUE(NeedsOfflineFilmUpload(*retained, 1280, 720,
        OfflineAov::Combined, retained->generation, slots[0]));
    slots[0] = {8, 1, OfflineAov::Combined, true};

    // 新任务准备期间可能无结果或仍传入旧结果，acquire 的 slot 仍要补新视角。
    retained = SelectOfflineDisplayFilm({}, retained, 1280, 720);
    ASSERT_EQ(retained, newFilm);
    ASSERT_TRUE(NeedsOfflineFilmUpload(*retained, 1280, 720,
        OfflineAov::Combined, retained->generation, slots[1]));
    slots[1] = {8, 1, OfflineAov::Combined, true};
    retained = SelectOfflineDisplayFilm(oldFilm, retained, 1280, 720);
    ASSERT_EQ(retained, newFilm);
    ASSERT_TRUE(NeedsOfflineFilmUpload(*retained, 1280, 720,
        OfflineAov::Combined, retained->generation, slots[2]));
    slots[2] = {8, 1, OfflineAov::Combined, true};

    for (const auto& slot : slots)
    {
        EXPECT_EQ(slot.generation, 8u);
        EXPECT_FALSE(NeedsOfflineFilmUpload(*retained, 1280, 720,
            OfflineAov::Combined, retained->generation, slot));
    }
}

TEST(OfflineDisplayData, RetainedFilmRejectsIncompleteAndOlderSamplesButAllowsAovRefresh)
{
    auto first = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(81, 46));
    first->generation = 8;
    first->completedSamples = 2;
    std::shared_ptr<const Offline::FilmSnapshot> retained =
        SelectOfflineDisplayFilm(first, {}, 1281, 721);
    ASSERT_EQ(retained, first);

    auto older = std::make_shared<Offline::FilmSnapshot>(*first);
    older->completedSamples = 1;
    EXPECT_EQ(SelectOfflineDisplayFilm(older, retained, 1281, 721), first);
    older->generation = 7;
    EXPECT_EQ(SelectOfflineDisplayFilm(older, retained, 1281, 721), first);
    older->generation = 9;
    older->direct.pop_back();
    EXPECT_EQ(SelectOfflineDisplayFilm(older, retained, 1281, 721), first);
    EXPECT_EQ(SelectOfflineDisplayFilm({}, retained, 1281, 721), first);

    const OfflineDisplayUploadState uploaded{8, 2, OfflineAov::Combined, true};
    EXPECT_TRUE(NeedsOfflineFilmUpload(*retained, 1281, 721,
        OfflineAov::Direct, retained->generation, uploaded));
    auto nextSample = std::make_shared<Offline::FilmSnapshot>(*first);
    nextSample->completedSamples = 3;
    EXPECT_EQ(SelectOfflineDisplayFilm(nextSample, retained, 1281, 721), nextSample);
}

TEST(OfflineDisplayData, SparsePreviewFillsOnlyFromSameTaskWithStableNearestTies)
{
    auto preview = SparsePreview(5, 1, 8, {0, 4});
    preview->direct[0] = {1, 2, 3};
    preview->indirect[0] = {10, 20, 30};
    preview->direct[4] = {4, 5, 6};
    preview->indirect[4] = {40, 50, 60};
    ASSERT_TRUE(IsOfflinePreviewUsable(*preview, 5, 1));
    const auto nearest = OfflinePreviewNearestValid(*preview);
    ASSERT_EQ(nearest, (std::vector<size_t>{0, 0, 0, 4, 4}));
    // 并列距离取行优先的第一个有效像素，不能从旧相机图像补洞。
    EXPECT_EQ(OfflinePreviewColor(*preview, nearest[2], OfflineAov::Direct).x, 1);
    EXPECT_EQ(OfflinePreviewColor(*preview, nearest[2], OfflineAov::Indirect).x, 10);
    EXPECT_EQ(OfflinePreviewColor(*preview, nearest[2], OfflineAov::Combined).x, 11);
    EXPECT_EQ(OfflinePreviewColor(*preview, nearest[3], OfflineAov::Combined).x, 44);
}

TEST(OfflineDisplayData, SparseTwoDimensionalMaskPropagatesAcrossRows)
{
    auto preview = SparsePreview(3, 3, 8, {0, 8});
    ASSERT_TRUE(IsOfflinePreviewUsable(*preview, 3, 3));
    EXPECT_EQ(OfflinePreviewNearestValid(*preview),
        (std::vector<size_t>{0, 0, 0, 0, 0, 8, 0, 8, 8}));
}

TEST(OfflineDisplayData, ZeroValidPreviewHoldsPreviousCompleteOrValidImage)
{
    auto film = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(80, 45));
    OfflineDisplaySource shown = SelectOfflineDisplaySource({}, film, {}, 1280, 720);
    ASSERT_EQ(shown.film, film);
    auto empty = SparsePreview(80, 45, 8, {});
    EXPECT_FALSE(IsOfflinePreviewUsable(*empty, 1280, 720));
    EXPECT_TRUE(OfflinePreviewNearestValid(*empty).empty());
    shown = SelectOfflineDisplaySource(shown, {}, empty, 1280, 720);
    EXPECT_EQ(shown.film, film);
    auto partial = SparsePreview(80, 45, 8, {0});
    shown = SelectOfflineDisplaySource(shown, {}, partial, 1280, 720);
    ASSERT_EQ(shown.preview, partial);
    shown = SelectOfflineDisplaySource(shown, {}, empty, 1280, 720);
    EXPECT_EQ(shown.preview, partial);
}

TEST(OfflineDisplayData, EverySlotCatchesUpToSparsePreviewAndThenCompleteFilm)
{
    auto film = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(81, 46));
    auto preview = SparsePreview(81, 46, 8, {0});
    std::array<OfflineDisplayUploadState, 3> slots{};
    OfflineDisplaySource shown = SelectOfflineDisplaySource({}, film, {}, 1281, 721);
    for (auto& slot : slots)
        slot = {7, 1, OfflineAov::Combined, true};

    shown = SelectOfflineDisplaySource(shown, {}, preview, 1281, 721);
    ASSERT_EQ(shown.preview, preview);
    for (auto& slot : slots)
    {
        // 后续 acquire 未必再带 preview 指针，但所有 slot 都应追同一代。
        shown = SelectOfflineDisplaySource(shown, {}, {}, 1281, 721);
        ASSERT_TRUE(NeedsOfflinePreviewUpload(*shown.preview, 1281, 721,
            OfflineAov::Combined, slot));
        slot = {8, 0, OfflineAov::Combined, true, true, 1};
    }
    auto more = SparsePreview(81, 46, 8, {0, 1});
    shown = SelectOfflineDisplaySource(shown, {}, more, 1281, 721);
    ASSERT_EQ(shown.preview, more);
    for (auto& slot : slots)
        EXPECT_TRUE(NeedsOfflinePreviewUpload(*shown.preview, 1281, 721,
            OfflineAov::Combined, slot));

    auto complete = std::make_shared<Offline::FilmSnapshot>(CompleteFilm(81, 46));
    complete->generation = 8;
    shown = SelectOfflineDisplaySource(shown, complete, more, 1281, 721);
    ASSERT_EQ(shown.film, complete);
    for (auto& slot : slots)
        EXPECT_TRUE(NeedsOfflineFilmUpload(*shown.film, 1281, 721,
            OfflineAov::Combined, shown.Generation(), slot));
    shown = SelectOfflineDisplaySource(shown, {}, more, 1281, 721);
    EXPECT_EQ(shown.film, complete);
}

TEST(OfflineDisplayData, PreviewRejectsStaleGenerationResizeAndIncompleteBuffers)
{
    auto preview = SparsePreview(81, 46, 9, {0});
    OfflineDisplaySource shown = SelectOfflineDisplaySource({}, {}, preview, 1281, 721);
    ASSERT_EQ(shown.preview, preview);
    auto older = SparsePreview(81, 46, 8, {0, 1});
    EXPECT_EQ(SelectOfflineDisplaySource(shown, {}, older, 1281, 721).preview, preview);
    auto resized = SparsePreview(81, 46, 10, {0});
    EXPECT_EQ(SelectOfflineDisplaySource(shown, {}, resized, 1280, 720).preview, preview);
    auto incomplete = SparsePreview(81, 46, 10, {0});
    incomplete->indirect.pop_back();
    EXPECT_EQ(SelectOfflineDisplaySource(shown, {}, incomplete, 1281, 721).preview, preview);
    const OfflineDisplayUploadState uploaded{9, 0, OfflineAov::Combined, true, true, 1};
    EXPECT_FALSE(NeedsOfflinePreviewUpload(*older, 1281, 721, OfflineAov::Direct, uploaded));
    EXPECT_TRUE(NeedsOfflinePreviewUpload(*preview, 1281, 721, OfflineAov::Direct, uploaded));
    EXPECT_FALSE(NeedsOfflinePreviewUpload(*preview, 1281, 721, OfflineAov::Combined, uploaded));
}

TEST(OfflineDisplayData, UploadIdentityDoesNotIncludeExposureOrTargetSamples)
{
    auto film = CompleteFilm(4, 3);
    film.completedSamples = 2;
    film.targetSamples = 8;
    const OfflineDisplayUploadState uploaded{7, 2, OfflineAov::Indirect, true};

    film.targetSamples = 16;
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 4, 3, OfflineAov::Indirect, 7, uploaded));
}

TEST(OfflineDisplayData, AcceptsExactAndRoundedHalfExtent)
{
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1281, 721, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(256, 144, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(640, 360, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(320, 180, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(640, 360, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(641, 361, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(640, 361, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(640, 361, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(600, 360, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1, 2, 16, 9));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1281, 721, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(0, 360, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(640, -1, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(640, 360, 0, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(640, 360, 1280, 0));
}

TEST(OfflineDisplayData, AcceptsRoundedQuarterExtentForEvenAndOddDisplays)
{
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(320, 180, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(321, 181, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(321, 180, 1281, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(320, 181, 1280, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(321, 181, 1283, 723));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 3, 2));
}

TEST(OfflineDisplayData, RejectsDistortedQuarterExtentAndInvalidBounds)
{
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(320, 181, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 180, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(322, 181, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 182, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(0, 181, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 0, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 181, 0, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 181, 1281, 0));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1282, 181, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(321, 722, 1281, 721));
}

TEST(OfflineDisplayData, AcceptsRoundedEighthExtentForEvenAndOddDisplays)
{
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(160, 90, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(160, 90, 1279, 719));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(161, 91, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(161, 90, 1281, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(160, 91, 1280, 721));
}

TEST(OfflineDisplayData, AcceptsRoundedSixteenthExtentForEvenAndOddDisplays)
{
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(80, 45, 1280, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(80, 45, 1279, 719));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(81, 46, 1281, 721));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(81, 45, 1281, 720));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(80, 46, 1280, 721));
}

TEST(OfflineDisplayData, RoundedPreviewsKeepTinyOddExtentsNonzero)
{
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 1, 1));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 7, 5));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 15, 9));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 16, 9));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 1, 15));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 1, 15, 1));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1, 2, 3, 17));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(2, 1, 17, 3));
}

TEST(OfflineDisplayData, RejectsDistortedOrMixedEighthAndSixteenthExtents)
{
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(160, 91, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(161, 90, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(162, 91, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(80, 46, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(81, 45, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(82, 46, 1281, 721));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(80, 44, 1279, 719));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(160, 45, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(80, 90, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1, 2, 17, 9));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(0, 45, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(80, 0, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1281, 45, 1280, 720));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(80, 721, 1280, 720));
}

TEST(OfflineDisplayData, LargeExtentsKeepQuarterRoundingAndRatioChecksInRange)
{
    constexpr uint32_t maxDisplay = std::numeric_limits<uint32_t>::max();
    constexpr int maxFilm = std::numeric_limits<int>::max();
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1073741824, 1073741824,
        maxDisplay, maxDisplay - 1));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(1073741824, 1073741823,
        maxDisplay - 1, maxDisplay - 3));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(maxFilm, maxFilm - 1,
        maxDisplay - 1, maxDisplay - 3));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(1073741823, 1073741824,
        maxDisplay, maxDisplay - 1));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(maxFilm, maxFilm - 2,
        maxDisplay, maxDisplay - 1));
}

TEST(OfflineDisplayData, LargeExtentsKeepEighthAndSixteenthRoundingInRange)
{
    constexpr uint32_t maxDisplay = std::numeric_limits<uint32_t>::max();
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(536870912, 536870912,
        maxDisplay, maxDisplay - 1));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(536870912, 536870911,
        maxDisplay - 1, maxDisplay - 7));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(268435456, 268435456,
        maxDisplay, maxDisplay - 1));
    EXPECT_TRUE(IsOfflineFilmExtentCompatible(268435456, 268435455,
        maxDisplay - 1, maxDisplay - 15));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(536870911, 536870912,
        maxDisplay, maxDisplay - 1));
    EXPECT_FALSE(IsOfflineFilmExtentCompatible(268435455, 268435456,
        maxDisplay, maxDisplay - 1));
}

TEST(OfflineDisplayData, MapsDestinationPixelCentersWithoutOutOfBounds)
{
    const uint32_t expectedX[] = {0, 0, 1, 1, 2, 2, 3, 3};
    for (uint32_t x = 0; x < 8; ++x)
        EXPECT_EQ(OfflineFilmSourceCoordinate(x, 8, 4), expectedX[x]);
    for (uint32_t x = 0; x < 7; ++x)
        EXPECT_LT(OfflineFilmSourceCoordinate(x, 7, 3), 3u);
    for (uint32_t y = 0; y < 5; ++y)
        EXPECT_LT(OfflineFilmSourceCoordinate(y, 5, 2), 2u);
    for (uint32_t x = 0; x < 5; ++x)
        EXPECT_EQ(OfflineFilmSourceCoordinate(x, 5, 5), x);
    EXPECT_EQ(OfflineFilmSourceCoordinate(1280, 1281, 641), 640u);
}

TEST(OfflineDisplayData, MapsQuarterPreviewPixelCentersAtOddAndLargeExtents)
{
    const uint32_t expected[] = {0, 0, 0, 1, 1, 1, 2, 2, 2};
    for (uint32_t x = 0; x < 9; ++x)
        EXPECT_EQ(OfflineFilmSourceCoordinate(x, 9, 3), expected[x]);
    EXPECT_EQ(OfflineFilmSourceCoordinate(0, 1281, 321), 0u);
    EXPECT_EQ(OfflineFilmSourceCoordinate(1280, 1281, 321), 320u);
    constexpr uint32_t maxDisplay = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(OfflineFilmSourceCoordinate(maxDisplay - 1, maxDisplay, 1073741824),
        1073741823u);
}

TEST(OfflineDisplayData, MapsEighthAndSixteenthPreviewPixelCenters)
{
    const struct { uint32_t display; uint32_t film; } extents[] = {
        {1280, 80}, {720, 45}, {1279, 80}, {719, 45}, {1281, 81}, {721, 46},
        {1280, 160}, {720, 90}, {1279, 160}, {719, 90}, {1281, 161}, {721, 91},
        {1, 1}, {15, 1}, {17, 2}};
    for (const auto& extent : extents)
    {
        SCOPED_TRACE(testing::Message() << extent.display << " -> " << extent.film);
        for (uint32_t pixel = 0; pixel < extent.display; ++pixel)
        {
            // 小尺寸用直接中心公式独立校验现有商余数映射，不更改显示采样方式。
            const uint32_t expected = static_cast<uint32_t>(
                ((2 * uint64_t(pixel) + 1) * extent.film) / (2 * uint64_t(extent.display)));
            const uint32_t source = OfflineFilmSourceCoordinate(pixel, extent.display, extent.film);
            EXPECT_EQ(source, expected);
            EXPECT_LT(source, extent.film);
        }
    }
}

TEST(OfflineDisplayData, MapsLargeEighthAndSixteenthPreviewPixelCenters)
{
    constexpr uint32_t maxDisplay = std::numeric_limits<uint32_t>::max();
    for (const uint32_t filmExtent : {536870912u, 268435456u})
    {
        SCOPED_TRACE(filmExtent);
        EXPECT_EQ(OfflineFilmSourceCoordinate(0, maxDisplay, filmExtent), 0u);
        EXPECT_EQ(OfflineFilmSourceCoordinate(maxDisplay / 2, maxDisplay, filmExtent),
            filmExtent / 2);
        EXPECT_EQ(OfflineFilmSourceCoordinate(maxDisplay - 1, maxDisplay, filmExtent),
            filmExtent - 1);
    }
}

TEST(OfflineDisplayData, QuarterPreviewUploadKeepsCompleteFilmAndIdentityRules)
{
    auto film = CompleteFilm(321, 181);
    const OfflineDisplayUploadState empty;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, empty));
    film.direct.pop_back();
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, empty));
    film.direct.emplace_back();
    const OfflineDisplayUploadState uploaded{7, 1, OfflineAov::Combined, true};
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, uploaded));
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Indirect, 7, uploaded));
    film.generation = 8;
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 9, uploaded));
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 8, uploaded));
}

TEST(OfflineDisplayData, EighthAndSixteenthPreviewUploadKeepsGenerationAndAovRules)
{
    const struct { int width; int height; uint32_t displayWidth; uint32_t displayHeight; } extents[] = {
        {160, 90, 1280, 720}, {161, 91, 1281, 721},
        {80, 45, 1280, 720}, {80, 45, 1279, 719}, {81, 46, 1281, 721}};
    for (const auto& extent : extents)
    {
        SCOPED_TRACE(testing::Message() << extent.displayWidth << "x" << extent.displayHeight
            << " -> " << extent.width << "x" << extent.height);
        auto film = CompleteFilm(extent.width, extent.height);
        const OfflineDisplayUploadState empty;
        EXPECT_TRUE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Combined, 7, empty));
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Combined, 8, empty));
        const OfflineDisplayUploadState uploaded{7, 1, OfflineAov::Combined, true};
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Combined, 7, uploaded));
        EXPECT_TRUE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Direct, 7, uploaded));
        EXPECT_TRUE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Indirect, 7, uploaded));
        const OfflineDisplayUploadState newerSample{7, 2, OfflineAov::Combined, true};
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Direct, 7, newerSample));
        film.generation = 8;
        EXPECT_TRUE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Combined, 8, uploaded));
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, extent.displayWidth, extent.displayHeight,
            OfflineAov::Combined, 9, uploaded));
    }
}

TEST(OfflineDisplayData, EighthAndSixteenthPreviewUploadRequiresCompleteSample)
{
    const struct { int width; int height; } extents[] = {{161, 91}, {81, 46}};
    for (const auto& extent : extents)
    {
        SCOPED_TRACE(testing::Message() << extent.width << "x" << extent.height);
        auto film = CompleteFilm(extent.width, extent.height);
        const OfflineDisplayUploadState empty;
        film.direct.pop_back();
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721, OfflineAov::Combined, 7, empty));
        film.direct.emplace_back();
        film.indirect.pop_back();
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721, OfflineAov::Combined, 7, empty));
        film.indirect.emplace_back();
        film.completedSamples = 0;
        EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721, OfflineAov::Combined, 7, empty));
        film.completedSamples = 1;
        EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721, OfflineAov::Combined, 7, empty));
    }
}

TEST(OfflineDisplayData, UploadRequiresCompleteFilmAndPreservesGenerationAndAovRules)
{
    auto film = CompleteFilm(640, 360);
    const OfflineDisplayUploadState empty;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, empty));
    film.indirect.pop_back();
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, empty));
    film.indirect.emplace_back();
    film.completedSamples = 0;
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, empty));
    film.completedSamples = 1;
    const OfflineDisplayUploadState uploaded{7, 1, OfflineAov::Combined, true};
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, uploaded));
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Direct, 7, uploaded));
    film.generation = 8;
    EXPECT_TRUE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 7, uploaded));
    EXPECT_FALSE(NeedsOfflineFilmUpload(film, 1281, 721,
        OfflineAov::Combined, 9, uploaded));
}
} // namespace VL
