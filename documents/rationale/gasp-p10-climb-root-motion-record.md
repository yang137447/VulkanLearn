# GASP P10 2.5 m Climb 根运动记录

状态：执行记录，非当前契约。记录 2.5 m Climb 的独立根运动验收；
不改变 P8 1 m Mantle/Vault 子集和 P9 独立包结论。

## 范围

把 UE 的 `Climb_Start_2_5` stand/walk/run 导出为 VL clip，用真实根运动驱动 2.5 m 上攀：

- 新 FBX：`M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot.fbx`、
  `M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot.fbx`、`M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot.fbx`
- `SM_mannequin.json` 新增三个 clip，clip 总数 21。
- 新场景 `SC_gasp_traversal_climb.json`、`_run.json`、`_walk.json`：
  障碍顶面 250 cm，probe 使用 `TraversalAction=Climb`、`TraversalGait=Stand/Walk/Run`。
- 检测球从局部 `y=120` 出发，覆盖 2.5 m 障碍的高度范围；
  `TraversalAction=Climb` 的高度窗口为 70–300 cm。

根轨迹（FBX root）：

```text
first = (0, 75, 0)
last  = (0, -50, 250)
Y min/max = -50 / 75
Z min/max = 0 / 250
```

导入后根运动表现为向上约 250 cm、向前约 50–90 cm，正是上攀落顶所需的
“上-落”路径。

## 实现

- `SkeletalPlayback::RootMotionDelta` 从原始根轨道逐播放 tick 求差；
- `RootMotionYawDelta` 从根旋转四元数求 yaw 增量，Climb 在位移后单独加到
  Actor yaw；不把旋转混入速度；
- `Character.ConsumeRootMotion` 把 `delta/dt` 作为本帧速度覆盖；
- 原生 `CharacterVirtual` 的 `rootMotionActive` 分支用根速度替换地面/空中速度，
  再叠加一次重力，所有碰撞仍由 Jolt 执行；
- Climb 不调用 `Character.Jump`，动作结束/中断清空 root override；
- Mantle/Vault 仍保留已验证的 jump + target distance 逻辑，不混用根运动。

## 验收

仓库场景：

```text
P8_TRAVERSAL_START height=250 action=M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot
P8_TRAVERSAL_END pos=0,340,-169 grounded=True
```

`y=340 = 障碍顶 250 + 胶囊中心约 90`，角色落在顶面；`z` 在顶面范围内。

run：

```text
P8_TRAVERSAL_START height=250 action=M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot
P8_TRAVERSAL_END pos=0,340,-176 grounded=True
```

独立包 `artifacts/gasp-p10-climb-package-05`（1721 文件，约 1.59 GB）：

```text
stand: END pos=0,340,-173 grounded=True yaw=0.0
run:   END pos=0,340,-151…-196 grounded=True yaw=0.0（10/10）
walk:  END pos=0,340,-193 grounded=True yaw=0.0
```

`.err` 为空，退出正常。回归：

```text
Mantle: END pos=0,190,-202 grounded=True
Vault:  END pos=0,90,-290 grounded=True
```

## 证据

- `artifacts/gasp-p10/climb-export-03/traversal-audit.json`
- `artifacts/gasp-p10/climb-fbx-inspect.json`
- `artifacts/gasp-climb2.log/.err`
- `artifacts/gasp-climb-run2.log/.err`
- `artifacts/gasp-climb-walk4.log/.err`
- `artifacts/gasp-p10-climb-package-05.log`
- `artifacts/gasp-p10-pkg5-stand.log/.err`
- `artifacts/gasp-p10-pkg5-walk.log/.err`
- `artifacts/gasp-p10-pkg5-run-1…10.log/.err`

## 限制

- stand/run 前半段用根竖直，后半段及 walk 全程用目标高度闭环校正竖直
  （上限 ±200 cm/s），水平仍由根轨迹驱动；动画身体仍是原 clip。
- 该闭环修正把 package-04 中 Climb run 的 3/10 airborne abort 降到
  package-05 的 0/10。
- 已消费根 yaw，但 Climb 三段实测 yaw 均约 0；pitch/roll 仍未消费。
- 2.5 m Climb 之外的 med/high Mantle/Vault 仍未接入。
- 1 m Vault 没有地面型根轨迹，仍使用 jump + target distance；
  详见 [根运动 UE 对照](gasp-root-motion-ue-analysis.md)。
