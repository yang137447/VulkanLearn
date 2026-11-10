#pragma once

// 编辑器分节载荷。只有编辑器兼容聚合/编辑器代码包含本头；运行时聚合
// （ui/viewModel/runtimeUiSections.h）不依赖这里的任何类型。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/logStore.h"
#include "editor/assets/editorJsonAssetTypes.h"
#include "editor/ui/editorSceneActorTypes.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "ui/viewModel/runtimeUiSections.h"

namespace VL
{

// 大纲节。归属：UiSubsystem（它同时拥有 actor 列表、MI 模型列表与选中态）。
// 版本源：UiSubsystem 的 outlinerRevision，只在指针替换或选中态变化时推进。
struct UiSceneOutlinerSection
{
    std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>> actors;
    std::shared_ptr<const std::vector<EditorUi::MaterialInstanceModelSnapshot>> models;
    std::string selectedActorIdentity;
    std::optional<EditorUi::MaterialInstanceModelSnapshot> selectedModel;
    std::optional<uint32_t> selectedMaterialSlotIndex;
};

// 资产节。归属：UiSubsystem（日志存储、JSON 资产库、缩略图库都由 GT 轮询）。
// 载荷本身就是各库已经发布好的不可变快照；同一 revision 重复 Poll 得到同一个
// shared_ptr，因此“N 次构建无变化”只发布一次。
struct UiAssetsSection
{
    std::shared_ptr<const LogSnapshot> logs;
    std::shared_ptr<const EditorUi::EditorJsonAssetCatalogSnapshot> assetCatalog;
    std::shared_ptr<const EditorUi::EditorJsonDocumentSnapshot> jsonDocument;
    std::shared_ptr<const EditorUi::EditorThumbnailSnapshot> thumbnails;
    bool assetCatalogLoading = false;
    bool jsonDocumentLoading = false;

    bool operator==(const UiAssetsSection& other) const
    {
        return logs == other.logs && assetCatalog == other.assetCatalog &&
            jsonDocument == other.jsonDocument && thumbnails == other.thumbnails &&
            assetCatalogLoading == other.assetCatalogLoading &&
            jsonDocumentLoading == other.jsonDocumentLoading;
    }
};

// 兼容聚合：运行时节 + 编辑器节。UiSubsystem 的 store 使用它，因此现有
// 面板/测试仍可按名字访问全部九节。
struct UiViewModelSections : UiRuntimeViewModelSections
{
    UiSnapshotSection<UiSceneOutlinerSection> sceneOutliner;
    UiSnapshotSection<UiAssetsSection> assets;
};

inline UiSectionVersions VersionsOf(const UiViewModelSections& sections)
{
    UiSectionVersions versions = VersionsOf(
        static_cast<const UiRuntimeViewModelSections&>(sections));
    versions.versions[static_cast<size_t>(UiSectionId::SceneOutliner)] =
        sections.sceneOutliner.version;
    versions.versions[static_cast<size_t>(UiSectionId::Assets)] = sections.assets.version;
    return versions;
}

// 两个 owner 各发布自己的节：EngineLoop 拥有运行时六节，UiSubsystem 拥有
// 大纲/资产/状态。脏集判定读取合并后的同一份版本。
inline UiSectionVersions MergeSectionVersions(
    const UiRuntimeViewModelSections& engineSections,
    const UiViewModelSections& uiSections)
{
    UiSectionVersions versions = VersionsOf(engineSections);
    versions.versions[static_cast<size_t>(UiSectionId::SceneOutliner)] =
        uiSections.sceneOutliner.version;
    versions.versions[static_cast<size_t>(UiSectionId::Assets)] = uiSections.assets.version;
    versions.versions[static_cast<size_t>(UiSectionId::Status)] = uiSections.status.version;
    return versions;
}

// 编辑器侧完整 store：运行时发布器 + 资产/大纲节。
class UiViewModelSectionStore : public UiSectionStoreBase<UiViewModelSections>
{
public:
    bool PublishSceneOutliner(UiSceneOutlinerSection value, uint64_t inputRevision)
    {
        const bool published = sceneOutliner.Publish(std::move(value), inputRevision);
        if (published) view.sceneOutliner = sceneOutliner.Section();
        return published;
    }
    bool PublishAssets(UiAssetsSection value)
    {
        const bool published = assets.PublishIfChanged(std::move(value));
        if (published) view.assets = assets.Section();
        return published;
    }

    UiSectionVersions Versions() const noexcept { return VL::VersionsOf(view); }

private:
    UiSectionSlot<UiSceneOutlinerSection> sceneOutliner;
    UiSectionSlot<UiAssetsSection> assets;
};

} // namespace VL
