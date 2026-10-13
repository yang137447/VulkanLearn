#pragma once

#include <vector>

#include "editor/ui/editorSceneActorTypes.h"

namespace VL
{

class World;

// 把 active World 的稳定帧状态拍成一个只读 actor 列表：网格（按场景对象聚合 section）、
// 四类灯光、相机、环境与 SpeedTree 风剖面。返回值全部是值语义副本，UI 侧不持有
// World、SceneNode 或 GPU 对象；派生规则（RuntimeId、灯光参数）与渲染快照同源。
//
// 边界：World 没有 terrain 运行时表示，编辑器观察相机也不作为第二个场景 actor。
std::vector<EditorUi::EditorSceneActorSnapshot> BuildEditorSceneActors(const World& world);

} // namespace VL
