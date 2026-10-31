#include "render/resource/tressfxGeometryCache.h"

#include <algorithm>

TressfxGeometryCache::TressfxGeometryCache(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity))
{
}

std::shared_ptr<const MeshAssetImportResult> TressfxGeometryCache::Find(
    std::string_view key)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end())
        return nullptr;
    // 命中即提升为最近使用：组编辑后的重复加载不应把仍在用的几何挤掉。
    order_.splice(order_.begin(), order_, found->second.order);
    return found->second.geometry;
}

void TressfxGeometryCache::Store(
    std::string key, std::shared_ptr<const MeshAssetImportResult> geometry)
{
    if (!geometry)
        return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end())
    {
        found->second.geometry = std::move(geometry);
        order_.splice(order_.begin(), order_, found->second.order);
        return;
    }
    order_.push_front(key);
    entries_.emplace(std::move(key), Entry{std::move(geometry), order_.begin()});
    while (entries_.size() > capacity_)
    {
        entries_.erase(order_.back());
        order_.pop_back();
    }
}

size_t TressfxGeometryCache::Size() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return entries_.size();
}

void TressfxGeometryCache::Clear()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    order_.clear();
    entries_.clear();
}
