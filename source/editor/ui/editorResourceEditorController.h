#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "editor/assets/editorJsonAssetTypes.h"

namespace VL::EditorUi
{

struct EditorResourceEditorTab
{
    std::string path;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    std::shared_ptr<const EditorJsonDocumentSnapshot> snapshot;
    bool loading = false;
};

// 资源编辑器只保存 tab 的值状态；文件读取、UI 展示和资源加载由上层负责。
class EditorResourceEditorController
{
public:
    static constexpr size_t MaxTabs = 8;

    // 已打开的资源只激活，不重复创建 tab；达到上限时拒绝新的资源。
    bool Open(const std::string& path, EditorAssetKind kind);
    bool Close(const std::string& path);
    bool Activate(const std::string& path);

    const EditorResourceEditorTab* ActiveTab() const;
    const std::vector<EditorResourceEditorTab>& Tabs() const { return tabs; }

    // 异步结果只能写入当前活动且路径相同的 tab，避免旧请求覆盖新 tab。
    bool SetDocument(std::shared_ptr<const EditorJsonDocumentSnapshot> snapshot,
        bool loading);

    // 直接返回当前活动资源路径；没有活动资源时返回空字符串。
    std::string RequestReload();

    // 也支持一次性消费的重载请求，便于上层在 update 循环中轮询。
    bool ReloadRequested() const { return reloadRequested; }
    std::string ConsumeReloadRequest();

private:
    static constexpr size_t InvalidIndex = static_cast<size_t>(-1);

    std::vector<EditorResourceEditorTab> tabs;
    size_t activeIndex = InvalidIndex;
    bool reloadRequested = false;
};

} // namespace VL::EditorUi
