#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VL
{

// 大纲 actor 的类型。Mesh..Environment 与 SceneWind 和场景 JSON 的对象 type 一一对应；
// ParticleSystem 与 Terrain 同样是场景对象；Script 只表达 World 里确实存在的蓝图
// 实例状态。SpeedTree 每物种的响应曲线数据属于网格资产，不作为场景 actor 出现在大纲里。
enum class EditorSceneActorType : uint8_t
{
    Mesh,
    DirectionalLight,
    PointLight,
    SpotLight,
    SquareAreaLight,
    Camera,
    Environment,
    ParticleSystem,
    Terrain,
    SceneWind,
    Script
};

// 类型规范表，不决定 actor 排列顺序，不生成大纲分组。
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
const char* SceneActorGroupTitle(EditorSceneActorType type);
const char* SceneActorGroupIcon(EditorSceneActorType type);

// 稳定 actor 身份：mesh 用场景对象名，其余类型用各自的 World 名字（灯光名、风剖面 key）。
// 场景风全场景至多一个且没有名字，用固定名字 "scene"（身份即 "wind|scene"）。
std::string SceneActorIdentity(EditorSceneActorType type, const std::string& name);

// 稳定身份的逆解析：分隔符之前是分组键（= 场景对象 type），之后是对象名。
// 执行边界用它把 UI 传来的身份还原成类型与名字，再交给 World 的可见性写入端；
// 名字本身可以含 '|'，因此只按第一个分隔符切分。
struct EditorSceneActorIdentity
{
    EditorSceneActorType type = EditorSceneActorType::Mesh;
    std::string name;
};

bool ParseSceneActorIdentity(const std::string& identity, EditorSceneActorIdentity& parsed);

// 可见性开关只覆盖渲染路径真正持有的对象：网格/地形、四类灯与粒子系统。
// 相机、环境、场景风与 Blueprint 没有逐 actor 的渲染侧可见性状态，因此不为它们
// 显示一个点了没有效果的眼睛。
bool SceneActorHasVisibilityToggle(EditorSceneActorType type);

} // namespace VL
