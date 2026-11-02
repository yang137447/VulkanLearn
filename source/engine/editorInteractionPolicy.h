#pragma once

namespace VL::EditorInteraction
{
// 编辑器场景交互的唯一判定：Play/Pause 期间场景归蓝图运行时所有，选择、gizmo、
// 观察相机、参考网格和场景编辑命令都必须关闭；离线渲染同样只消费作者场景。
// UI 侧再叠加“开发者 UI 是否可见”，命令执行端只使用脚本状态这一权威部分。
inline bool SceneEditingEnabled(
    bool scriptsEditing,
    bool developerUiVisible,
    bool offlineMode) noexcept
{
    return scriptsEditing && developerUiVisible && !offlineMode;
}
} // namespace VL::EditorInteraction
