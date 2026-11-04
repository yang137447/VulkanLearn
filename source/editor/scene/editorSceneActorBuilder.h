#pragma once

#include <vector>

#include "editor/ui/editorSceneActorTypes.h"

namespace VL
{

class World;

// 把 active World 的稳定帧状态拍成一个只读 actor 列表：网格与地形（按场景对象
// 聚合 section）、四类灯光、相机、环境与场景级风对象。SpeedTree 每物种响应曲线
// 属于网格资产，不进入 actor 列表。返回值全部是值语义副本，UI 侧不持有
// World、SceneNode 或 GPU 对象；派生规则（RuntimeId、灯光参数）与渲染快照同源。
//
// 边界：terrain 在 World 里仍是带同一 sceneObjectIdentity 的 chunk mesh 对象；
// 独立 Terrain actor 类型只属于编辑器身份/展示，不引入第二套 World 地形类型。
// 编辑器观察相机也不作为第二个场景 actor。
std::vector<EditorUi::EditorSceneActorSnapshot> BuildEditorSceneActors(const World& world);

} // namespace VL
