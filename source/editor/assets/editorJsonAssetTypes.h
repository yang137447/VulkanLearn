#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VL::EditorUi
{

enum class EditorAssetKind : uint8_t { MaterialInstance, Mesh, Scene, Texture, Terrain };
enum class EditorJsonValueKind : uint8_t { Object, Array, String, Number, Boolean, Null };

// 资源 worker 发布的不可变 JSON 元数据；UI 只借用共享快照，不加载引用的 data 或 GPU 资源。
struct EditorJsonAssetEntry
{
    std::string assetPath;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    std::string name;
    std::string description;
    std::string diagnostic;
};

struct EditorJsonAssetCatalogSnapshot
{
    uint64_t revision = 0;
    std::vector<EditorJsonAssetEntry> assets;
    std::string diagnostic;
};

// JSON 属性预先扁平化；parent 为前序遍历父行，-1 表示根节点，容器展开由 UI 自己保存。
struct EditorJsonField
{
    int parent = -1;
    uint32_t depth = 0;
    std::string path;
    std::string label;
    std::string value;
    EditorJsonValueKind kind = EditorJsonValueKind::Null;
};

struct EditorJsonDocumentSnapshot
{
    uint64_t revision = 0;
    std::string assetPath;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    std::string name;
    std::string description;
    std::vector<EditorJsonField> fields;
    std::string diagnostic;
};

// RmlUi 回调仅发出查看/刷新意图；文件读取和发布由 GT facade 与独立 worker 承担。
class IEditorJsonAssetRequestSink
{
public:
    virtual ~IEditorJsonAssetRequestSink() = default;
    virtual void RequestOpenJsonAsset(const std::string& assetPath) = 0;
    virtual void RequestRefreshAssetCatalog() = 0;
};

const char* EditorAssetKindName(EditorAssetKind kind);
const char* EditorAssetKindIcon(EditorAssetKind kind);

} // namespace VL::EditorUi
