#pragma once

#include <cstddef>
#include <memory>
#include <string>
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

// 树行：分组行只带标题与该组当前可见数量；actor 行带 sceneActors 里的下标。
struct EditorSceneActorRow
{
    bool isGroup = false;
    EditorSceneActorType type = EditorSceneActorType::Mesh;
    size_t actorIndex = 0;
    std::string label;
    size_t actorCount = 0;
};

// 排序：先按显示名的 ASCII 大小写不敏感比较，再用 identity 打破平局，
// 与迁移前的模型树排序同样是确定性的（不依赖 unordered_map 的遍历顺序）。
bool SceneActorNameLess(const EditorSceneActorSnapshot& left, const EditorSceneActorSnapshot& right);

// 搜索：query 已折叠（FoldAsciiCase）；空 query 匹配全部。
bool SceneActorMatchesQuery(const EditorSceneActorSnapshot& actor, const std::string& foldedQuery);

// 过滤器取值 "all" 或分组 key；未知 key 视为 "all"（视图不做隐式空结果）。
bool SceneActorMatchesTypeFilter(const EditorSceneActorSnapshot& actor, const std::string& typeFilter);

// 按固定分组顺序输出可见行：组内有序、空组不输出。query/typeFilter 只影响可见性，
// 不改变 actor 下标，因此事件地址在本次快照内稳定。
std::vector<EditorSceneActorRow> BuildSceneActorRows(
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& query,
    const std::string& typeFilter);
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
