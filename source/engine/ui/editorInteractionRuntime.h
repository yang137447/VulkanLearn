#pragma once

#pragma once

class Controller;

namespace VL
{

class SubsystemCollection;
class UiSubsystem;

// 编辑器场景交互的唯一判定：Play/Pause 期间场景归蓝图运行时所有，选择、gizmo、
// 观察相机、参考网格和场景编辑命令都必须关闭；离线渲染同样只消费作者场景。
// UI 侧再叠加“开发者 UI 是否可见”，命令执行端只使用脚本状态这一权威部分。
bool IsEditorSceneInteractionEnabled(
    const SubsystemCollection& subsystems,
    const UiSubsystem* uiSubsystem);

// 同步世界观察相机、编辑器辅助几何与选择描边。返回本次判定结果，并把上一次
// 状态写回 wasEnabled；下降沿清掉编辑器选中态，避免游戏画面残留选择描边。
bool SynchronizeEditorInteractionState(
    const SubsystemCollection& subsystems,
    UiSubsystem* uiSubsystem,
    Controller* controller,
    bool& wasEnabled);

} // namespace VL
