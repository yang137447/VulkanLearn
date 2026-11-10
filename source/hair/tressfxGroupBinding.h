#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hair/tressfxGroupRanges.h"

namespace VL::Hair
{

// 预览 GPU 颜色表/参数表按上限预留，组数变化只原地重写。
// 4096 与 groups[].guideRanges 的总区间上限一致：每个组至少一个 range，
// 因此合法资产不会超过这个命名组数量；超出时运行时返回明确错误。
inline constexpr uint32_t TressfxGroupPreviewNamedGroupCapacity = 4096u;
inline constexpr uint32_t TressfxGroupPreviewColorCapacity =
    TressfxGroupPreviewNamedGroupCapacity + 1u;

// 分组声明的最小值语义表示：与 MeshAssetBuildPlan 解耦，hair 层不依赖 mesh 头。
struct TressfxGroupDeclaration
{
    std::string name;
    std::vector<TressfxGuideRange> ranges;
};

// 当前预览/资产的分组绑定：每根 guide 的组号（0 = Default，k = 第 k 个命名组）
// 与组名声明顺序。ribbon 顶点/索引、发根点云等几何数据不在其中，因此组编辑
// 只替换这一份绑定，不需要重新解析/展开 .tfx。
struct TressfxGroupBinding
{
    std::vector<uint32_t> guideGroupIds;
    std::vector<std::string> groupNames;
    // 描述符层算出的“名称 + 区间”身份；用于判断两个绑定是否等价。
    // 不参与几何 modelCacheKey。
    std::string key;

    uint32_t GuideCount() const
    {
        return static_cast<uint32_t>(guideGroupIds.size());
    }
    size_t NamedGroupCount() const { return groupNames.size(); }
};

// 只做区间上界与跨组重叠校验，不展开 40k 数组。导入器在 .tfx 解析后用它
// 拒绝越界声明；描述符层无法独立完成，因为 guideCount 来自文件头。
void ValidateTressfxGuideGroups(
    const std::vector<TressfxGroupDeclaration>& groups,
    uint32_t guideCount,
    const std::string& where);

// 把 groups[] 展开成每 guide 组号。空声明返回长度 guideCount 的全 0 表。
// 越界/重叠在这里再次拒绝（描述符层已检查重叠，导入期检查上界）。
std::shared_ptr<const TressfxGroupBinding> BuildTressfxGroupBinding(
    const std::vector<TressfxGroupDeclaration>& groups,
    uint32_t guideCount,
    std::string key,
    const std::string& where);

// 草稿 binding 的预览同步请求：40k guide 数组只在草稿结构变化时进入队列，
// 不随鼠标 hover 或选择变化重复发送。
struct TressfxGroupBindingPreviewRequest
{
    std::string objectName;
    uint64_t worldGeneration = 0;
    uint64_t draftRevision = 0;
    std::vector<uint32_t> guideGroupIds;
    std::vector<std::string> groupNames;
    std::string bindingKey;
};

// 值语义校验：UI 只能提交有界数据，渲染线程不解释未知字段。
bool ValidateTressfxGroupBindingPreviewRequest(
    const TressfxGroupBindingPreviewRequest& request, std::string& error);

// 把请求转成 RenderableObject 消费的只读绑定；调用方应先通过上述校验。
TressfxGroupBinding MakeTressfxGroupBinding(
    const TressfxGroupBindingPreviewRequest& request);

// GPU 表更新前的纯逻辑校验：guide 数必须匹配，命名组数不能超过预留容量，
// 每个 guide 的组号必须落在“Default + 已声明命名组”范围内。
bool ValidateTressfxGroupPreviewBinding(
    const TressfxGroupBinding& binding, uint32_t guideCount, std::string& error);

} // namespace VL::Hair
