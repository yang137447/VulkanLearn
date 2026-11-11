#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// RenderableObject 的缓存键。普通网格按“模型缓存键 + section”共享，因此同一
// SM 的多个场景 placement 共用一份 GPU 几何；GPU 发丝模拟持有可变的 current/
// previous/prevPrev 与参数 UBO，不能跨 placement 共享，所以 TressFX GPU section
// 追加场景实例身份，让每个 placement 拥有独立的 RenderableObject 与模拟状态。
// 该函数是纯字符串规则，供 loader 与模块测试共用，不依赖 Vulkan/World。
//
// 作用域：Shared 表示可跨 placement 共享；Placement 表示键带实例身份、每个放置
// 一份可变 GPU 状态。运行时 spawn 只对 Placement 作用域的对象克隆独立实例。
enum class RenderableObjectScope
{
    Shared,
    Placement
};

inline std::string MakeRenderableObjectKey(
    const std::string& modelCacheKey,
    size_t sectionIndex,
    const std::string& sectionName,
    const std::string& instanceIdentity,
    RenderableObjectScope scope)
{
    std::string key = modelCacheKey + "|" + std::to_string(sectionIndex) + "|" +
        sectionName;
    if (scope == RenderableObjectScope::Placement)
        key += "|instance=" + instanceIdentity;
    return key;
}

inline std::string MakeRenderableObjectKey(
    const std::string& modelCacheKey,
    size_t sectionIndex,
    const std::string& sectionName,
    const std::string& instanceIdentity,
    bool perPlacementState)
{
    return MakeRenderableObjectKey(modelCacheKey, sectionIndex, sectionName,
        instanceIdentity,
        perPlacementState ? RenderableObjectScope::Placement : RenderableObjectScope::Shared);
}

// 键是否带实例作用域（Placement）。克隆/spawn 与测试共用这一条规则，不再各自
// 搜索 "|instance=" 字面量。
inline bool IsPlacementScopedRenderableKey(std::string_view key)
{
    return key.find("|instance=") != std::string_view::npos;
}

// 运行时 spawn 克隆 per-placement 毛发时，键尾的 |instance=<模板身份> 必须换成
// 新演员的组件身份，否则新实例会借用模板的 RenderableObject（绑定、模拟历史、
// params UBO 全是模板的）。只替换精确匹配的尾部；共享网格的键没有 instance 段，
// 原样返回。旧身份为空时不做替换，避免把 "|instance=" 误当匹配。
inline std::string RebaseRenderableObjectKeyInstance(
    const std::string& key,
    const std::string& previousIdentity,
    const std::string& newIdentity)
{
    if (previousIdentity.empty() || newIdentity.empty())
        return key;
    const std::string suffix = "|instance=" + previousIdentity;
    if (key.size() <= suffix.size() ||
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
        return key;
    return key.substr(0, key.size() - suffix.size()) + "|instance=" + newIdentity;
}
