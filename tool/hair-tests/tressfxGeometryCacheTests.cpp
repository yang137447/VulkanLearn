#include "render/resource/tressfxGeometryCache.h"

#include "mesh/loader/common/meshAssetLoader.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

std::shared_ptr<const MeshAssetImportResult> NewGeometry()
{
    return std::make_shared<const MeshAssetImportResult>();
}

TEST(TressfxGeometryCache, RepeatedGroupEditsReuseImportedGeometry)
{
    TressfxGeometryCache cache(2);
    // 同一 .tfx + 同一几何身份：组编辑不改变缓存键，只换 binding。
    const std::string key = MakeTressfxGeometryCacheKey(
        "model|tressfx-geometry-v2", "tfx-blake3");
    int importCalls = 0;
    const auto loader = [&importCalls]()
    {
        ++importCalls;
        return NewGeometry();
    };
    const auto first = cache.GetOrLoad(key, loader);
    const auto second = cache.GetOrLoad(key, loader);
    EXPECT_EQ(importCalls, 1);
    EXPECT_EQ(first.get(), second.get());

    // 纯 Find 命中不重新导入。
    EXPECT_EQ(cache.Find(key).get(), first.get());
    EXPECT_EQ(importCalls, 1);
}

TEST(TressfxGeometryCache, OnlyGeometryOrTfxChangeMisses)
{
    TressfxGeometryCache cache(4);
    const std::string key = MakeTressfxGeometryCacheKey("model", "digest-a");
    cache.Store(key, NewGeometry());
    EXPECT_NE(cache.Find(key), nullptr);
    EXPECT_EQ(cache.Find(MakeTressfxGeometryCacheKey("model", "digest-b")),
        nullptr);
    // 描述符里的 groups/simulation/materialSlots 不属于几何身份，不能参与键。
    EXPECT_EQ(cache.Find(MakeTressfxGeometryCacheKey("model", "digest-a")),
        cache.Find(key));
    EXPECT_EQ(cache.Find(MakeTressfxGeometryCacheKey("model-v2", "digest-a")),
        nullptr);
}

TEST(TressfxGeometryCache, StoreReplacesEntryWithoutGrowing)
{
    TressfxGeometryCache cache(2);
    cache.Store("k", NewGeometry());
    const auto replaced = NewGeometry();
    cache.Store("k", replaced);
    EXPECT_EQ(cache.Size(), 1u);
    EXPECT_EQ(cache.Find("k").get(), replaced.get());
}

TEST(TressfxGeometryCache, LeastRecentlyUsedEntryIsEvicted)
{
    TressfxGeometryCache cache(2);
    cache.Store("a", NewGeometry());
    cache.Store("b", NewGeometry());
    // 命中 a 后 b 变成最久未使用。
    ASSERT_NE(cache.Find("a"), nullptr);
    cache.Store("c", NewGeometry());
    EXPECT_NE(cache.Find("a"), nullptr);
    EXPECT_EQ(cache.Find("b"), nullptr);
    EXPECT_NE(cache.Find("c"), nullptr);
    EXPECT_EQ(cache.Size(), 2u);
}

TEST(TressfxGeometryCache, ClearDropsAllGeometry)
{
    TressfxGeometryCache cache(2);
    cache.Store("a", NewGeometry());
    cache.Clear();
    EXPECT_EQ(cache.Size(), 0u);
    EXPECT_EQ(cache.Find("a"), nullptr);
}

} // namespace
