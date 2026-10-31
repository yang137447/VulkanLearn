#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "ui/uiViewModelSections.h"

namespace
{

using VL::EditorUi::EditorJsonAssetCatalogSnapshot;
using VL::EditorUi::EditorSceneActorSnapshot;
using VL::EditorUi::EditorThumbnailSnapshot;
using VL::LogStore;
using VL::UiAnimationPreviewSection;
using VL::UiAssetsSection;
using VL::UiEnvironmentLightingSection;
using VL::UiPerformanceSection;
using VL::UiSceneOutlinerSection;
using VL::UiSectionBit;
using VL::UiSectionId;
using VL::UiSectionRevision;
using VL::UiSectionSlot;
using VL::UiSectionVersions;
using VL::UiScriptsSection;
using VL::UiSnapshotSection;
using VL::UiStatusSection;
using VL::UiViewModelSectionStore;

// 分节版本的核心承诺：输入没变就不换载荷、不递增版本。
TEST(UiSnapshotSectionTest, UnchangedRevisionPublishesExactlyOnce)
{
    UiSectionSlot<UiScriptsSection> slot;
    UiScriptsSection first;
    first.scripts.feedback = "ready";
    EXPECT_TRUE(slot.Publish(first, 7));
    EXPECT_EQ(slot.Section().version, 1u);

    UiScriptsSection rebuilt;
    rebuilt.scripts.feedback = "ready";
    EXPECT_FALSE(slot.Publish(rebuilt, 7));
    EXPECT_EQ(slot.Section().version, 1u);
}

TEST(UiSnapshotSectionTest, ChangedRevisionBumpsVersionOncePerPublish)
{
    UiSectionSlot<UiScriptsSection> slot;
    UiScriptsSection first;
    EXPECT_TRUE(slot.Publish(first, 1));
    UiScriptsSection second;
    second.scripts.canUndo = true;
    EXPECT_TRUE(slot.Publish(second, 2));
    EXPECT_EQ(slot.Section().version, 2u);
    EXPECT_TRUE(slot.Section().data->scripts.canUndo);
}

TEST(UiSnapshotSectionTest, InFlightConsumerKeepsOldImmutableSection)
{
    UiSectionSlot<UiStatusSection> slot;
    UiStatusSection before;
    before.activeWorldPath = "Maps/SC_a/SC_a.json";
    ASSERT_TRUE(slot.PublishIfChanged(before));
    const std::shared_ptr<const UiStatusSection> inFlight = slot.Section().data;

    UiStatusSection after;
    after.activeWorldPath = "Maps/SC_b/SC_b.json";
    ASSERT_TRUE(slot.PublishIfChanged(after));
    EXPECT_EQ(slot.Section().version, 2u);
    // 已发布的旧载荷不会被新发布改写：在途消费者继续读取完整旧值。
    EXPECT_EQ(inFlight->activeWorldPath, "Maps/SC_a/SC_a.json");
    EXPECT_EQ(slot.Section().data->activeWorldPath, "Maps/SC_b/SC_b.json");
    EXPECT_NE(inFlight.get(), slot.Section().data.get());
}

TEST(UiSnapshotSectionTest, IdenticalValueDoesNotPublish)
{
    UiSectionSlot<UiEnvironmentLightingSection> slot;
    UiEnvironmentLightingSection environment;
    environment.bloomStrength = 0.08f;
    EXPECT_TRUE(slot.PublishIfChanged(environment));
    EXPECT_FALSE(slot.PublishIfChanged(environment));
    EXPECT_EQ(slot.Section().version, 1u);

    environment.bloomStrength = 0.12f;
    EXPECT_TRUE(slot.PublishIfChanged(environment));
    EXPECT_EQ(slot.Section().version, 2u);
}

TEST(UiSnapshotSectionTest, AnimationTimeOnlyBumpsWhenTimeChanges)
{
    UiSectionSlot<UiAnimationPreviewSection> slot;
    UiAnimationPreviewSection animation;
    animation.time = 1.0f;
    EXPECT_TRUE(slot.PublishIfChanged(animation));
    EXPECT_FALSE(slot.PublishIfChanged(animation));
    animation.time = 1.1f;
    EXPECT_TRUE(slot.PublishIfChanged(animation));
    EXPECT_EQ(slot.Section().version, 2u);
}

TEST(UiSnapshotSectionTest, PerformancePublishesEverySampledWindow)
{
    UiViewModelSectionStore store;
    UiPerformanceSection performance;
    performance.performance.fps = 60.0;
    EXPECT_TRUE(store.PublishPerformance(performance));
    EXPECT_TRUE(store.PublishPerformance(performance));
    EXPECT_EQ(store.Sections().performance.version, 2u);
}

TEST(UiSectionVersionTest, ChangedFromReportsOnlyChangedBits)
{
    UiViewModelSectionStore store;
    UiScriptsSection scripts;
    ASSERT_TRUE(store.PublishScripts(scripts, 1));
    UiEnvironmentLightingSection environment;
    ASSERT_TRUE(store.PublishEnvironmentLighting(environment));
    const UiSectionVersions before = store.Versions();

    environment.bloomStrength = 0.2f;
    ASSERT_TRUE(store.PublishEnvironmentLighting(environment));
    const UiSectionVersions after = store.Versions();
    const VL::UiSectionMask changed = after.ChangedFrom(before);

    EXPECT_EQ(changed,
        UiSectionBit(UiSectionId::EnvironmentLighting));
    EXPECT_EQ((changed & UiSectionBit(UiSectionId::Scripts)), 0u);
}

TEST(UiSectionVersionTest, FirstPublishCountsAsChange)
{
    const UiSectionVersions empty;
    UiViewModelSectionStore store;
    UiStatusSection status;
    status.locale = "en-US";
    ASSERT_TRUE(store.PublishStatus(status));
    const VL::UiSectionMask changed = store.Versions().ChangedFrom(empty);
    EXPECT_EQ(changed, UiSectionBit(UiSectionId::Status));
}

TEST(UiSectionVersionTest, RepeatedAssetsPollPublishesOnce)
{
    UiSectionSlot<UiAssetsSection> slot;
    LogStore logStore;
    const auto logs = logStore.Freeze();
    auto catalog = std::make_shared<const EditorJsonAssetCatalogSnapshot>();
    auto thumbnails = std::make_shared<const EditorThumbnailSnapshot>();
    UiAssetsSection assets;
    assets.logs = logs;
    assets.assetCatalog = catalog;
    assets.thumbnails = thumbnails;

    // 5 次构建、同一批 revision：只发布一次。
    for (int build = 0; build < 5; ++build)
    {
        UiAssetsSection candidate = assets;
        slot.PublishIfChanged(std::move(candidate));
    }
    EXPECT_EQ(slot.Section().version, 1u);

    // 日志追加 → LogStore::Freeze 产生新 revision 快照 → 只再发布一次。
    logStore.Append(VL::LogSeverity::Info, "test", "one line");
    UiAssetsSection next = assets;
    next.logs = logStore.Freeze();
    EXPECT_TRUE(slot.PublishIfChanged(std::move(next)));
    EXPECT_EQ(slot.Section().version, 2u);
}

TEST(UiSectionVersionTest, AssetCatalogRevisionChangeBumpsVersion)
{
    UiSectionSlot<UiAssetsSection> slot;
    auto first = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    first->revision = 1;
    UiAssetsSection assets;
    assets.assetCatalog = first;
    ASSERT_TRUE(slot.PublishIfChanged(assets));

    UiAssetsSection sameRevision = assets;
    sameRevision.assetCatalog = first;
    EXPECT_FALSE(slot.PublishIfChanged(std::move(sameRevision)));

    auto second = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    second->revision = 2;
    UiAssetsSection changed = assets;
    changed.assetCatalog = second;
    EXPECT_TRUE(slot.PublishIfChanged(std::move(changed)));
    EXPECT_EQ(slot.Section().version, 2u);
}

TEST(UiSectionVersionTest, MergeTakesUiOwnedSectionsFromUiStore)
{
    UiViewModelSectionStore engineStore;
    UiScriptsSection scripts;
    ASSERT_TRUE(engineStore.PublishScripts(scripts, 3));

    UiViewModelSectionStore uiStore;
    UiAssetsSection assets;
    ASSERT_TRUE(uiStore.PublishAssets(assets));
    UiSceneOutlinerSection outliner;
    ASSERT_TRUE(uiStore.PublishSceneOutliner(outliner, 1));
    UiStatusSection status;
    ASSERT_TRUE(uiStore.PublishStatus(status));

    const UiSectionVersions merged = VL::MergeSectionVersions(
        engineStore.Sections(), uiStore.Sections());
    EXPECT_EQ(merged.Version(UiSectionId::Scripts), 1u);
    EXPECT_EQ(merged.Version(UiSectionId::Assets), 1u);
    EXPECT_EQ(merged.Version(UiSectionId::SceneOutliner), 1u);
    EXPECT_EQ(merged.Version(UiSectionId::Status), 1u);
}

TEST(UiSectionRefreshPlanTest, IdleWindowRebuildsOnlyCadenceSections)
{
    const VL::UiSectionMask rebuild =
        VL::PlanUiEditorSectionRefresh(0, false, false);
    EXPECT_EQ((rebuild & UiSectionBit(UiSectionId::Scripts)), 0u);
    EXPECT_EQ((rebuild & UiSectionBit(UiSectionId::Assets)), 0u);
    EXPECT_EQ((rebuild & UiSectionBit(UiSectionId::SceneOutliner)), 0u);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::Animation)), 0u);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::Performance)), 0u);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::ViewportCamera)), 0u);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::EnvironmentLighting)), 0u);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::Offline)), 0u);
}

TEST(UiSectionRefreshPlanTest, SelectionEventRebuildsOnlyOutliner)
{
    const VL::UiSectionMask rebuild = VL::PlanUiEditorSectionRefresh(
        UiSectionBit(UiSectionId::SceneOutliner), false, false);
    EXPECT_NE((rebuild & UiSectionBit(UiSectionId::SceneOutliner)), 0u);
    EXPECT_EQ((rebuild & UiSectionBit(UiSectionId::Scripts)), 0u);
    EXPECT_EQ((rebuild & UiSectionBit(UiSectionId::Assets)), 0u);
}

TEST(UiSectionRefreshPlanTest, FirstRefreshAndWorldChangeRebuildEverything)
{
    EXPECT_EQ(VL::PlanUiEditorSectionRefresh(0, false, true), VL::UiSectionAll);
    EXPECT_EQ(VL::PlanUiEditorSectionRefresh(0, true, false), VL::UiSectionAll);
}

TEST(UiSectionRefreshPlanTest, PanelMaskExcludesPerformanceAndAnimation)
{
    // 性能/动画/相机/环境/离线节变化不触发整份面板 Build。
    EXPECT_EQ((VL::UiCadenceSectionMask & VL::UiDeveloperPanelSectionMask), 0u);
    EXPECT_EQ((UiSectionBit(UiSectionId::Status) & VL::UiDeveloperPanelSectionMask),
        UiSectionBit(UiSectionId::Status));
    EXPECT_NE((UiSectionBit(UiSectionId::Assets) & VL::UiDeveloperPanelSectionMask), 0u);
    EXPECT_NE((UiSectionBit(UiSectionId::SceneOutliner) &
        VL::UiDeveloperPanelSectionMask), 0u);
}

TEST(UiSectionRefreshPlanTest, RuntimeViewMaskIncludesAnimationButNotPerformance)
{
    EXPECT_NE((UiSectionBit(UiSectionId::Animation) & VL::UiRuntimeViewSectionMask), 0u);
    EXPECT_NE((UiSectionBit(UiSectionId::Offline) & VL::UiRuntimeViewSectionMask), 0u);
    EXPECT_EQ((UiSectionBit(UiSectionId::Performance) & VL::UiRuntimeViewSectionMask), 0u);
    EXPECT_EQ((UiSectionBit(UiSectionId::Assets) & VL::UiRuntimeViewSectionMask), 0u);
}

TEST(UiSectionRevisionTest, ExistingRevisionsComposeIntoStableKey)
{
    const uint64_t first = UiSectionRevision()
        .Mix(7)   // world generation
        .Mix(3)   // document revision
        .Mix(2)   // script play state
        .Value();
    const uint64_t second = UiSectionRevision()
        .Mix(7)
        .Mix(3)
        .Mix(2)
        .Value();
    const uint64_t changed = UiSectionRevision()
        .Mix(7)
        .Mix(4)
        .Mix(2)
        .Value();
    EXPECT_EQ(first, second);
    EXPECT_NE(first, changed);
}

} // namespace
