#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "editorJsonAssetTypes.h"

namespace VL::Editor
{

// 所有上限在 JSON 输入边界执行；展示截断不影响余下语法校验，data 也受输入大小/深度约束。
struct EditorJsonAssetLimits
{
    static constexpr size_t MaxFileBytes = 4 * 1024 * 1024;
    static constexpr size_t MaxSourceLines = 100000;
    static constexpr size_t MaxDepth = 64;
    static constexpr size_t MaxFields = 4096;
    static constexpr size_t MaxTextBytes = 1024;
    static constexpr size_t MaxFieldPathBytes = 4096;
    static constexpr size_t MaxCatalogEntries = 20000;
    static constexpr size_t MaxScanEntries = 100000;
};

// 纯解析入口，不读取引用路径。true 表示支持的候选（包括有诊断的损坏 JSON），
// false 表示路径不合法或非支持类型；输出 diagnostic 区分失败原因。
bool ParseEditorJsonAsset(std::string_view assetPath, std::string_view jsonText,
    EditorUi::EditorJsonDocumentSnapshot& document, bool includeFields = true);

// 仅检查相对路径的词法约束；实际文件的 canonical 越界检查另由 worker 在读取前执行。
bool ValidateEditorJsonAssetPath(std::string_view assetPath, std::string& error);

// GT facade 接收 UI 查看/刷新意图，独立 worker 只读取资源根内的 JSON 元数据。
// 所有公开方法由同一 GT 调用；只有 Poll 发布不可变快照，不加载 World/data/GPU、不写资产。
class EditorJsonAssetLibrary
{
public:
    EditorJsonAssetLibrary();
    ~EditorJsonAssetLibrary();
    EditorJsonAssetLibrary(const EditorJsonAssetLibrary&) = delete;
    EditorJsonAssetLibrary& operator=(const EditorJsonAssetLibrary&) = delete;

    bool Initialize(std::filesystem::path resourceRoot, std::string& error);
    void Shutdown() noexcept;
    void RequestRefresh();
    void RequestOpen(const std::string& path);
    void Poll();
    std::shared_ptr<const EditorUi::EditorJsonAssetCatalogSnapshot> GetCatalog() const;
    std::shared_ptr<const EditorUi::EditorJsonDocumentSnapshot> GetDocument() const;
    bool IsCatalogLoading() const;
    bool IsDocumentLoading() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL::Editor
