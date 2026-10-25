#pragma once

#include <cstddef>
#include <string>

// RenderableObject 的缓存键。普通网格按“模型缓存键 + section”共享，因此同一
// SM 的多个场景 placement 共用一份 GPU 几何；GPU 发丝模拟持有可变的 current/
// previous/prevPrev 与参数 UBO，不能跨 placement 共享，所以 TressFX GPU section
// 追加场景实例身份，让每个 placement 拥有独立的 RenderableObject 与模拟状态。
// 该函数是纯字符串规则，供 loader 与模块测试共用，不依赖 Vulkan/World。
inline std::string MakeRenderableObjectKey(
    const std::string& modelCacheKey,
    size_t sectionIndex,
    const std::string& sectionName,
    const std::string& instanceIdentity,
    bool perPlacementState)
{
    std::string key = modelCacheKey + "|" + std::to_string(sectionIndex) + "|" +
        sectionName;
    if (perPlacementState)
        key += "|instance=" + instanceIdentity;
    return key;
}
