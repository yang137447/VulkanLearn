# GASP P8 障碍交互起步记录

状态：执行记录，非当前契约。P8 的 **1 m stand 固定子集**已完成技术验收；
walk/run、med/high、根运动逐帧消费、碰撞 stall、F 输入实窗与独立包列为后续增强，
不代表 P8 全阶段或 M2 完成。
接续 [P7 记录](gasp-p7-timeline-record.md)；
计划见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 锁定范围

P0 只登记了 Traversal 资产、未承诺动作。本轮从锁定 UE 5.8 工程导出两个
**1.0 m stand** 动作，其他高度/速度变体暂不承诺：

| 动作 | 资源 | 时长 | 根运动 |
| --- | --- | --- | --- |
| Mantle | `M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot` | 2.000 s | UE 根运动开启 |
| Vault | `M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot` | 2.567 s | UE 根运动开启 |

P9 后续扩展又接入同一 1 m 高度的 walk/run 变体：
`M_Neutral_Traversal_Mantle_1_0_walk_F_Lfoot`（3.400 s）、
`..._run_F_Lfoot`（2.233 s）、
`M_Neutral_Traversal_Vault_1_0_walk_F_Rfoot`（3.167 s）、
`..._run_F_Lfoot`（3.033 s），由 `TraversalGait` 选择。

导出工具 `tool/animation/audit_gasp_traversal.py` 只读盘点 78 个 Traversal
AnimSequence，并按名称导出选定 FBX；未保存/升级原 UE 工程。
两个 FBX 已放入 `Maps/SC_gasp/Source/`，作为 `SM_mannequin.json` 的第 12/13
个片段；运行时按 `inPlace=true` 播放，竖直位移由 Jolt 跳跃/碰撞承担。

## 实现

- `Gasp.CharacterBlueprint` 增加默认开启的 `TraversalEnabled`、默认关闭的
  `TraversalProbe` 和 `TraversalAction`（`Mantle`/`Vault`）。
- 按 `Interact`（F）或探针触发：
  - `Physics.SphereCastLocal` 从角色腰/胸高度向前 140 cm、半径 20 cm 找障碍；
  - 命中后向障碍内部偏移 30 cm，从头部高度向下找顶面；
  - 高度按脚底计算，只接受 70–130 cm；
  - 顶面上方 180 cm 向上射线检查净空，有顶棚则拒绝；
  - 按 `-hit.Normal` 的水平方向设置 Actor yaw，播放对应 in-place 片段。
- 动作期间跳过正式 Locomotion 状态机；Jolt 跳跃提供竖直位移，
  Mantle 前 0.7 s 给 180 cm/s 落到顶面，Vault 前 1.2 s 给 200 cm/s
  越过并落到障碍另一侧；动作结束前未落地则 `AbortTraversal` 交回
  正常状态机，动作结束后恢复 Idle 与正式 MM 状态机。

## 场景矩阵

| 场景 | 障碍 | 预期 | 实际日志 |
| --- | --- | --- | --- |
| `SC_gasp_traversal.json` | 1.0 m | Mantle 成功落到顶面 | `P8_TRAVERSAL_START height=100` → `...END pos=0,190,-202 grounded=True` |
| `SC_gasp_traversal_vault.json` | 1.0 m, Vault | Vault 越过并落地 | `END pos=0,90,-290 grounded=True` |
| `SC_gasp_traversal_abort_void.json` | 1.0 m, 后方无地面 | 动作结束未落地则中断 | `P8_TRAVERSAL_ABORT reason=airborne` |
| `SC_gasp_traversal_abort_stall.json` | 1.0 m + 前方高墙 | 水平进度停滞则中断 | `P8_TRAVERSAL_ABORT reason=stalled progress=10` |
| `SC_gasp_traversal_reject_high.json` | 1.6 m | 高度拒绝 | `P8_TRAVERSAL_REJECT height=160` |
| `SC_gasp_traversal_reject_low.json` | 0.5 m | 高度拒绝 | `P8_TRAVERSAL_REJECT height=50` |
| `SC_gasp_traversal_reject_clearance.json` | 1.0 m + 顶棚 | 净空拒绝 | `P8_TRAVERSAL_REJECT clearance` |
| `SC_gasp_traversal_reject_nohit.json` | 前方无目标 | 目标失效拒绝 | `P8_TRAVERSAL_REJECT no-hit` |
| `SC_gasp_traversal_input.json` | 1.0 m, probe 关闭 | F 键正式路径 | 本轮未取得键鼠焦点，未登记通过 |

Mantle 关键轨迹：`(0,90,-80)` → 跳跃 `(0,223,-166)`，`grounded=False`
→ `(0,190,-202)`，`grounded=True`，最后 `P8_TRAVERSAL_END`。
Vault 越过障碍后落到 `(0,90,-290)`；无地面场景在动作结束前仍处于空中，
记录 `ABORT reason=airborne` 并把控制交回正常状态机。
场景由 42 个增至 61 个（含 P9 walk/run、stall、foot debug 与 P10 Climb 场景），schema 检查通过（`gasp-p8-scene-schema.log`）。

## 限制

- 只用 1 m stand/walk/run；med/high、Catch/Climb 未接入。
- 竖直位移由跳跃近似，不是 UE 根运动/动作位移的逐帧消费；视觉动作与胶囊
  轨迹可能不完全对齐。
- 已覆盖 no-hit、高度/净空拒绝、airborne abort 和 stalled abort；
  动作期间根位移只消费一次、低/高帧率矩阵尚未做。
- F 正式键鼠路径未取得稳定前台焦点证据；探针自动触发走同一
  `RequestTraversal`，但不是输入验收。
- 未导出独立包，未接入 P9 的完整依赖闭包/回归。

因此 **P8 的 1 m stand 固定子集技术验收完成**；P8 全阶段、walk/run/根运动
与 M2/P9 仍未完成。

## 证据

- UE 盘点：`artifacts/gasp-p8/traversal-audit-01/traversal-audit.json`。
- UE 导出：`artifacts/gasp-p8/traversal-export-01/`（2 个 FBX + 审计）。
- 场景日志：`artifacts/gasp-p8-traversal-probe8.log`（最终 Mantle）、
  `gasp-p8-traversal-probe7.log`、`gasp-p8-traversal-vault2.log`、
  `gasp-p8-traversal-abort-void2.log`、
  `gasp-p8-traversal-reject-high2.log`、
  `gasp-p8-traversal-reject-low2.log`、
  `gasp-p8-traversal-reject-clearance2.log`、
  `gasp-p8-traversal-reject-nohit.log`。
- 构建：`artifacts/gasp-p8-traversal-build4.log`、`gasp-p8-traversal-build5.log`、
  `gasp-p8-traversal-build6.log`。
- 资源源：`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs`、
  `BP_gasp_traversal_probe.json`、`SC_gasp_traversal*.json`、
  `Meshes/SM_mannequin.json`、`Source/M_Neutral_Traversal_*.fbx`。

契约见 [动画运行时契约](../contracts/animation-runtime-skinning.md)，
测试登记见 [测试契约](../contracts/testing.md)。
