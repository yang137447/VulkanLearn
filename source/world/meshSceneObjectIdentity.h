#pragma once

#include <string>

#include "world/world.h"

namespace VL
{

// 场景对象身份是"大纲一行 = 一个场景网格对象"的唯一键：多 section 模型在
// World::GetMeshObjects() 里是多个条目，它们共享 sceneObjectIdentity，单 section
// 对象的身份则回退到自己的对象名。
//
// 这条推导同时被读取端（scene/editorSceneActorBuilder 聚合 actor 行、
// worldSnapshotBuilder 的 MeshDrawSnapshot::sceneObjectIdentity）与写入端
// （World::SetMeshSceneObjectVisible）使用，因此放在这里一份，而不是各处再写一遍：
// 两处不一致时大纲的眼睛会静默改不到对象，正是这一轮修掉的缺陷。
// 参数 objectName 是 World::GetMeshObjects() 的键，不是 WorldMeshObject::debugName。
std::string MeshSceneObjectIdentity(const WorldMeshObject& object, const std::string& objectName);

} // namespace VL
