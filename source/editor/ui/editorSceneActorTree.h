#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "editor/ui/editorSceneActorTypes.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace VL::EditorUi
{

// 大纲分组规范：类型顺序固定（这里就是输出顺序），因此帧间不抖动。
// key 同时是类型过滤器的取值和详情分组的键后缀；icon 是资产相对路径，
// 只是类型图示，不代表缩略图。
struct EditorSceneActorGroupSpec
{
    EditorSceneActorType type = EditorSceneActorType::Mesh;
    const char* key = "";
    const char* title = "";
    const char* icon = "";
};

const std::vector<EditorSceneActorGroupSpec>& SceneActorGroupSpecs();
const EditorSceneActorGroupSpec* FindSceneActorGroupSpec(EditorSceneActorType type);
const EditorSceneActorGroupSpec* FindSceneActorGroupSpecByKey(const std::string& key);
// 分组下标 = 规范表下标（事件用下标寻址）。这里查表而不是依赖 enum 顺序，
// 规范顺序变化时下标寻址仍然正确。
size_t SceneActorGroupIndex(EditorSceneActorType type);
const char* SceneActorGroupTitle(EditorSceneActorType type);
const char* SceneActorGroupIcon(EditorSceneActorType type);

// 大纲列表以 shared_ptr<const ...> 进入面板快照（与资产目录同一约定）：
// 逐帧不深拷贝 actor 数组；列表尚未发布时返回共享的空列表。
using EditorSceneActorList = std::vector<EditorSceneActorSnapshot>;
const EditorSceneActorList& SceneActorList(const std::shared_ptr<const EditorSceneActorList>& actors);

// 稳定 actor 身份：mesh 用场景对象名，其余类型用各自的 World 名字（灯光名、风剖面 key）。
std::string SceneActorIdentity(EditorSceneActorType type, const std::string& name);

// 树行：三种行共用一套结构，因此窗口化只需要一个行下标空间。
// 分组行只带标题与该组当前可见数量；actor 行带 sceneActors 里的下标；
// 槽行（展开的 mesh actor 的材质槽）带动作分量下标与选中比较用的 World 槽号。
struct EditorSceneActorRow
{
    bool isGroup = false;
    bool isSlot = false;
    EditorSceneActorType type = EditorSceneActorType::Mesh;
    size_t actorIndex = 0;
    // 槽行在 MeshSceneActorModel::materials 里的下标（事件的 data-component）。
    size_t slotIndex = 0;
    // 槽行的 World 材质槽号：选中态按它比较，与动作下标不是同一个量。
    uint32_t materialSlotIndex = 0;
    std::string label;
    size_t actorCount = 0;
    // actor 行：有可打开的材质槽（决定 ± 标记是否出现）；分组/actor 行：当前是否展开。
    bool hasChildren = false;
    bool expanded = false;
};

// 分组内的排序顺序。分组顺序由 SceneActorGroupSpecs() 固定，排序控件只作用于组内，
// 因此这里的取值不会重排分组本身。
enum class SceneActorSortOrder
{
    NameAscending,
    NameDescending
};

// 排序控件的取值（"name-asc" / "name-desc"，与 Content Browser 的 sort 控件同一套键）。
const char* SceneActorSortOrderKey(SceneActorSortOrder order);
bool ParseSceneActorSortOrder(const std::string& key, SceneActorSortOrder& order);

// 排序：先按显示名的 ASCII 大小写不敏感比较，再用 identity 打破平局，
// 与迁移前的模型树排序同样是确定性的（不依赖 unordered_map 的遍历顺序）。
bool SceneActorNameLess(const EditorSceneActorSnapshot& left, const EditorSceneActorSnapshot& right);
// 按方向比较：降序是升序的严格逆序（平局方向也一起翻转），仍满足严格弱序。
bool SceneActorOrderLess(const EditorSceneActorSnapshot& left,
    const EditorSceneActorSnapshot& right, SceneActorSortOrder order);

// 搜索：query 已折叠（FoldAsciiCase）；空 query 匹配全部。
bool SceneActorMatchesQuery(const EditorSceneActorSnapshot& actor, const std::string& foldedQuery);

// 过滤器取值 "all" 或分组 key；未知 key 视为 "all"（视图不做隐式空结果）。
bool SceneActorMatchesTypeFilter(const EditorSceneActorSnapshot& actor, const std::string& typeFilter);

// 唯一的"影响行集合"的视图状态：折叠的分组键（spec key）与展开的 mesh actor 稳定身份。
// 选择态不在这里：它只改已挂载行的 class，不改变有哪些行。
struct EditorSceneActorTreeExpansion
{
    std::unordered_set<std::string> collapsedGroups;
    std::unordered_set<std::string> expandedActors;
};

bool SameSceneActorExpansion(
    const EditorSceneActorTreeExpansion& left, const EditorSceneActorTreeExpansion& right);

// 按固定分组顺序输出可见行：组内有序、空组不输出。query/typeFilter 只影响可见性，
// 不改变 actor 下标，因此事件地址在本次快照内稳定。
// 折叠的分组只输出表头（子行既不挂 DOM，也不占窗口槽位）；
// 展开的 mesh actor 追加槽行，因此"哪些行存在"完全由本函数的入参决定。
std::vector<EditorSceneActorRow> BuildSceneActorRows(
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& query,
    const std::string& typeFilter,
    SceneActorSortOrder order,
    const EditorSceneActorTreeExpansion& expansion);

// 行内容签名：只混合会改变行集合/行文本的字段（类型、显示名、身份、section 的材质槽），
// 阳光强度或相机位姿这类只进 Details 的字段不参与，因此静止帧不会重建行集合与 DOM。
uint64_t SceneActorRowsSignature(const std::vector<EditorSceneActorSnapshot>& actors);

// 行窗口预算：与 Content Browser 的 MaxVisibleItems（192 条目）同一量级，
// #scene-models 任一时刻挂载的行节点不超过它（另加最多两个 spacer）。
constexpr size_t SceneActorMaxMountedRows = 192;
// 滚动方向各预取的行数：整行滚动时不必每帧增删首尾节点。
constexpr size_t SceneActorOverscanRows = 2;
// 首帧还没有实测行高时的 RCSS 兜底（.tree-row 的 .list-item 行距 + 边距量级）。
constexpr float SceneActorRowHeightFallback = 30.0f;

struct EditorSceneActorWindow
{
    size_t begin = 0;
    size_t end = 0;
    float rowHeight = SceneActorRowHeightFallback;
    // 未挂载行折算出的上下 spacer 高度：两者保留完整滚动范围，
    // 因此窗口化不是"截断到前 N 行"。
    float top = 0.0f;
    float bottom = 0.0f;
};

// 窗口恒等比较：两次计算在输入相同时必须给出逐位相同的浮点值，宿主据此判断
// "是否需要重挂 DOM"，而不是比较 DOM 本身。
bool SameSceneActorWindow(
    const EditorSceneActorWindow& left, const EditorSceneActorWindow& right);

// 纯窗口计算：给定行数、可视高度、滚动偏移与行高，返回挂载区间与两个 spacer 高度。
// 行高必须由调用方实测（首帧用 SceneActorRowHeightFallback），本函数不做测量。
EditorSceneActorWindow ComputeSceneActorWindow(
    size_t rowCount, float clientHeight, float scrollOffset, float rowHeight);

// Details 的只读字段行：id 用于逐帧刷新单项值，因此字段集合只由 actor 类型决定。
struct EditorSceneActorField
{
    std::string id;
    std::string label;
    std::string value;
};

// 只读字段：mesh 的 section key、四类灯的完整参数、相机七个字段、环境与风剖面。
// 主方向光的 intensity 与 CSM castShadows 由面板的可编辑控件承载
// （SetSunIntensity / SetCsmCastShadows），因此不在只读字段里重复出现。
std::vector<EditorSceneActorField> SceneActorFields(const EditorSceneActorSnapshot& actor);

// 把 mesh actor 还原成既有的 MI 导航值语义：材质槽只包含带有效材质实例的 section，
// 下标与大纲里的槽行一一对应，选择仍走原来的 RequestMaterialSelection 路由。
MaterialInstanceModelSnapshot MeshSceneActorModel(const EditorSceneActorSnapshot& actor);

// 与面板共用的数值格式：9 位有效数字，保证同一值在只读行与输入控件里写法一致。
std::string SceneActorNumber(float value);

} // namespace VL::EditorUi
