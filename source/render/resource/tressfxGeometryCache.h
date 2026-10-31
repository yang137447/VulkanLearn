#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

struct MeshAssetImportResult;

// .tfx 几何导入缓存键：几何身份 + 文件内容摘要 + 描述符字节摘要。
// groups[] 刻意不进入键：组编辑只替换 TressfxGroupBinding，几何必须复用。
inline std::string MakeTressfxGeometryCacheKey(
    const std::string& modelCacheKey,
    const std::string& tfxDigest)
{
    // 不能把整份 TF 描述符哈希放进几何键：groups/simulationDefaults 与材质槽
    // 也在同一份 JSON 里，组编辑或材质槽修改会错误地让 ribbon 缓存失效。
    // 几何身份已由 modelCacheKey 覆盖；材质槽在命中后按当前 plan 重算 sectionPlans。
    return modelCacheKey + "|tfx=" + tfxDigest;
}

// 进程级只读几何缓存。条目是不可变的 MeshAssetImportResult（ribbon 顶点、
// 索引、发根点云），不含 Vulkan 资源，因此不参与 World/GPU epoch 退休。
// 容量按 LRU 限制：正式 40k guide 资产单份约 300 MB 顶点/索引，默认只保留
// 2 份（当前资产 + 最近一次），避免多份大资产同时常驻。
class TressfxGeometryCache
{
public:
    explicit TressfxGeometryCache(size_t capacity = 2);

    std::shared_ptr<const MeshAssetImportResult> Find(std::string_view key);
    void Store(std::string key,
        std::shared_ptr<const MeshAssetImportResult> geometry);
    // Loader 返回 shared_ptr<const MeshAssetImportResult>；命中时不调用。
    template <class Loader>
    std::shared_ptr<const MeshAssetImportResult> GetOrLoad(
        const std::string& key, Loader&& loader)
    {
        // 串行化 loader，避免两个线程同时 miss 后重复展开同一份 .tfx。
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (std::shared_ptr<const MeshAssetImportResult> hit = Find(key))
            return hit;
        std::shared_ptr<const MeshAssetImportResult> loaded = loader();
        if (loaded)
            Store(key, loaded);
        return loaded;
    }

    size_t Capacity() const { return capacity_; }
    size_t Size() const;
    void Clear();

private:
    struct Entry
    {
        std::shared_ptr<const MeshAssetImportResult> geometry;
        std::list<std::string>::iterator order;
    };

    size_t capacity_ = 2;
    mutable std::recursive_mutex mutex_;
    // front = 最近使用。键在 map 里只存一份，LRU 链表持有它的副本。
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> entries_;
};
