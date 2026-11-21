# MotionBricks UE5 Manny 接入记录

日期：2026-11-20。代码与资源分别位于 `codex/motionbricks` /
`codex/motionbricks-assets`。

## 来源与资产

来源为本机 UE5.8.4 官方模板 `SKM_Manny_Simple`，从隔离的
`artifacts/motionbricks/manny-export/VLMannyExport.uproject` 导出，
不保存原 UE 工程或修改引擎模板。

`export_motionbricks_manny.py` 在 UE Python commandlet 导出 FBX 与出处报告。
`vl_animation_probe --fbx` 检查得到两个网格分区、89 个物理骨骼，无动画。
FBX SHA-256 为
`7fc4b4376d5b51db5877c6797febcfdcae56586ca7231b5c98e071b778e0ab67`。
Epic 模板资产许可保持不变，不随工具代码转换许可。

`build_motionbricks_manny.py` 复用正式 FBX 基底/骨架导出检查，生成：

- `Maps/SC_motionbricks/Source/SKM_Manny_Simple.fbx`
- `Skeletons/SK_Manny.json`
- `Meshes/SKM_manny.json`
- `Materials/MI_manny.json`
- `Retarget/RT_Manny_From_SOMA.json`
- `Blueprints/BP_motionbricks_manny.json`
- `SC_motionbricks_manny.json`
- `SC_motionbricks_manny_comparison.json` 及其固定相机蓝图

上述路径除首项外均相对 `Maps/SC_motionbricks/`。
`calibrate_motionbricks_retarget.py` 支持指定 skeleton/profile/pose-name，
Manny 的校准独立生成，不修改 UEFN 的 profile。
银灰色 VL PBR 材质用于观察动作，不宣称复现 UE 原材质图。

双角色对照中左侧为 Manny、右侧为原 UEFN，两者接受相同按键，
但各自有独立的模型生成实例；不作为同一模型输出的逐帧重定向对照。
后者仍属于官方演示对齐计划的后续阶段。

## 实机验证

运行 `capture_motionbricks.py` 时指定 Manny 的 scene/blueprint，
完整走过真实 ONNX → external pose → C# → Jolt → Vulkan 链路。
七个阶段全部接地，没有 DisableBlueprint、MotionBricks error 或 VUID。

| 阶段 | 水平实际速度均值 cm/s |
| --- | ---: |
| Idle | 约 0 |
| Walk | Z=-155.33 |
| Run | Z=-313.09 |
| Right | X=155.36 |
| Left | X=-142.89 |
| Back | Z=149.89 |
| Stop | 漂移约 6.32 |

Walk 的 Pause/Resume/Stop 后重新 Play 检查通过。
截图检查了 Idle、Run 和 Right，角色为银色 Manny，
手臂没有先前 UEFN 未校准时的交叉问题。
证据位于 gitignored `artifacts/motionbricks/manny/`，
主汇总为 `runtime-summary.json`。
验证工具退出后删除自己的临时蓝图/场景并恢复正式 Gameplay 程序集。

`start_motionbricks.ps1` 默认启动 Manny 并进入 Play，
支持 `-Character uefn` / `comparison`。
依赖路径优先读取环境变量或显式参数，本机可回退到现有 motionbricks-ref。

## 范围

本次是角色资产、独立校准、展示场景与验证工具接入，没有改引擎生成算法。
SOMA 原生基准显示、官方目标轨迹完整对齐、Idle 状态机与接触处理
继续依照 [官方演示对齐计划](../plan/animation/motionbricks-official-demo-alignment.md) 分阶段实施。
