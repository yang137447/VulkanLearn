# GASP P9 独立包集成记录

状态：执行记录，非当前契约。P9 的 **1 m stand/walk/run 独立包矩阵、程序集/World/
shader 生命周期、依赖闭包抽查与帧率冒烟**已完成技术验收；
med/high、Catch/Climb、根运动逐帧消费、真实输入与长时 GPU 压力列为后续增强，
不代表完整 M2 或最终交付完成。
接续 [P8 记录](gasp-p8-traversal-record.md)；
计划见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 本轮范围

验证 P8 的 1 m Mantle/Vault stand/walk/run、高度/净空/无目标拒绝与 airborne abort
能否在**同一个独立包**内按场景切换运行；不扩展打包器。

- 首个包：`artifacts/gasp-p9-traversal-export-01`（1708 文件），
  初始场景 `SC_gasp_traversal.json`，验证 Mantle。
- 矩阵包：`artifacts/gasp-p9-traversal-matrix-01`（1714 文件），
  `--dynamic-assets` 额外列入六个 `SC_gasp_traversal*.json`；
  从仓库外以 `--initial-scene` 切换场景，复用同一份包。
- gait 矩阵包：`artifacts/gasp-p9-traversal-gait-matrix-01`（1722 文件），
  再加入四个 walk/run 场景；同样的切换/退出方式。
- 最终包：`artifacts/gasp-p9-final-package-01`（1725 文件），含全部
  stand/walk/run、拒绝/中断场景与 foot debug 场景，使用当前 target-distance
  对齐与脚 IK 调试代码重新导出；本记录最终读数以该包为准。
- 包内使用私有 .NET 8 runtime，开发 UI 关闭，`scripts.autoPlay=true`。

## 结果

首个 Mantle 包：

```text
P8_TRAVERSAL_START height=100 action=M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot
P8_TRAVERSAL_END pos=0,190,-199 grounded=True
```

同一矩阵包按 `--initial-scene` 切换：

| 场景 | 包内结果 |
| --- | --- |
| `SC_gasp_traversal_vault.json` | `END pos=0,90,-290 grounded=True` |
| `SC_gasp_traversal_reject_high.json` | `REJECT height=160` |
| `SC_gasp_traversal_reject_low.json` | `REJECT height=50` |
| `SC_gasp_traversal_reject_clearance.json` | `REJECT clearance` |
| `SC_gasp_traversal_reject_nohit.json` | `REJECT no-hit` |
| `SC_gasp_traversal_abort_void.json` | `ABORT reason=airborne progress=211` |
| `SC_gasp_traversal_abort_stall.json` | `ABORT reason=stalled progress=10` |

gait 矩阵包：

| 场景 | 包内结果 |
| --- | --- |
| `SC_gasp_traversal_mantle_walk.json` | `END pos=0,190,-201 grounded=True` |
| `SC_gasp_traversal_mantle_run.json` | `END pos=0,190,-201 grounded=True` |
| `SC_gasp_traversal_vault_walk.json` | `END pos=0,90,-290 grounded=True` |
| `SC_gasp_traversal_vault_run.json` | `END pos=0,90,-290 grounded=True` |

全部运行结束后均有 `Quit event received` / `EngineLoop teardown complete`，
对应 `.err` 均为空。首个 Mantle 包窗口约 605 FPS；矩阵包无测试负载采样
**589–639 FPS**（1280×720），未做逐 pass/GPU 压力分项。

依赖闭包：最终包 `game-export-manifest.json` 1725 项；矩阵包 1714、gait 包 1722。
抽查含 `main.exe`、`managed/Gameplay.dll`、Traversal 场景及六个 Traversal FBX。

World 换代：矩阵包以
`--console-command "loadworld Maps/SC_gasp/SC_gasp_traversal_vault.json"`
`--console-command-delay 120` 启动，日志出现
`generation=2`、`Vault` 探针与 `(0,90,-290)` 落地，随后正常 teardown；
说明独立包内 World replacement 会重建 C#/Jolt/动画控制器并重新运行场景。

GPU 生命周期：同一包以 `--console-command "shaderreload all"` 启动，
日志记录 `compiled=16, pipelinesCreated=13, committed=true,
retiredPipelines=18, retirePending=18`，随后正常 teardown，stderr 为空。
这验证了独立包内的 shader/pipeline 事务提交与延迟退休路径，不是完整 GPU
压力/长时间生命周期矩阵。

程序集重载：新增 Console `reloadassembly`（`RuntimeCommandType::ReloadAssembly`，
经 ScriptRuntime subsystem 执行）。包内 autoPlay=true 时正确拒绝：
`Assembly reload rejected: Live reload preflight: Stop gameplay before
reloading skeletal animation controllers`；临时把包内 `scripts.autoPlay` 置为
false 后重跑，得到 `Assembly reload committed`，随后正常 teardown。
这验证了包内 Editing 成功路径和 Playing 受控骨骼的拒绝保护。

## 限制

- 已覆盖 1 m stand/walk/run；med/high、Catch/Climb 未接入。
- 已做 World 换代（generation=2）、一次 `shaderreload all` 事务/退休，以及
  Editing 程序集重载和 Playing 受控骨骼拒绝保护；长时间 GPU/程序集压力未做。
- 未做真实键鼠输入、逐 pass/GPU 压力分项与长时间性能回归；
  589–639 FPS 只是无测试负载的帧率冒烟。
- 矩阵包约 1.58 GB；未做依赖闭包裁剪/安装器/签名。
- F 正式键鼠路径仍未取得稳定焦点证据。

因此 **P9 的 1 m stand/walk/run 独立包子集技术验收完成**；
上述限制保留为后续 M2/最终交付增强。

## 架构审查补记（2026-10-21）

本地 GASP 提交 `b1f7aea` 的架构复审发现并修复了三类问题：

- **Montage 时间轴语义不一致（P1）**：Motion 面板使用当前播放范围内的相对秒数，
  原生播放器的 `Time()/Seek()` 使用 clip-local 绝对时间。非零 section 起点下，
  UI 的 `0` 会错误地 seek 到片段开头。现在控制命令先把相对值换算为播放范围绝对值，
  调试快照也输出相对位置；notify、warp 和 section 链继续使用原生绝对时间。
- **LateTick 与 Foot IK 的物理快照顺序错误（P1）**：原顺序在动画/IK 求值后才执行
  `StepPhysics(0)`，因此 LateTick 移动的支撑体要到下一帧才对 Foot IK 可见。现在改为
  `LateTick → StepPhysics(0) → 动画/Foot IK`，零时步不推进物理时间，也不重复消费一次性指令。
- **CustomData 迁移测试依赖属性排序（P1，测试缺陷）**：迁移新增属性后，测试按
  `properties[0]` 读取会误读 `ControlRotation`。测试改为按属性名定位，生产迁移协议未改变。

 回归覆盖包括非零 Montage 起点、非法 seek 状态保持、同帧移动坡面 Foot IK、
 SpringArm/物理零步命令和 CustomData 迁移。旧物理顺序下，Foot IK 回归稳定偏离当前
 坡面约 `1.03 cm`，修复后通过；最终聚焦脚本回归集 `49/49`、动画预检 `255/255`、
 Motion/Preview UI `19/19` 通过，架构边界审计和文档校验通过。

 尚未修复的 **P2**：Motion Matching 首次请求在缓存未命中时，仍由 GT 同步执行
 `BuildMotionDatabaseReport` 的冷构建和磁盘写入（`source/animation/motionRuntimeDatabase.cpp`，
 经 `source/world/worldAnimation.cpp` 绑定）。后续异步化必须设计候选数据库的隔离、
 World 代际失效和提交时身份复核，不能简单把现有加载调用搬到后台线程。

## 证据

- 导出：`artifacts/gasp-p9-traversal-export-01.log`、
  `gasp-p9-traversal-matrix-01.log`。
- 运行：`artifacts/gasp-p9-traversal-package.log/.err`、
  `gasp-p9-vault.log`、`gasp-p9-reject-high.log`、
  `gasp-p9-reject-low.log`、`gasp-p9-reject-clearance.log`、
  `gasp-p9-reject-nohit.log`、`gasp-p9-abort-void.log`、
  `gasp-p9-world-replace2.log`、`gasp-p9-shader-reload.log`、
  `gasp-p9-assembly-reload.log`、`gasp-p9-assembly-reload-edit.log`、
  `gasp-p9-perf-run.log`、`gasp-p9-pkg-mantle-walk.log`、
  `gasp-p9-pkg-mantle-run.log`、`gasp-p9-pkg-vault-walk.log`、
  `gasp-p9-pkg-vault-run.log`、`gasp-p9-abort-stall.log`。
- 最终包：`artifacts/gasp-p9-final-package-01.log` 及
  `gasp-final-*.log`（12 个矩阵场景）。
- 包目录：`artifacts/gasp-p9-traversal-export-01/`、
  `artifacts/gasp-p9-traversal-matrix-01/`、
  `artifacts/gasp-p9-traversal-gait-matrix-01/`、
  `artifacts/gasp-p9-final-package-01/`。
- 场景/动作来源见 [P8 记录](gasp-p8-traversal-record.md)。
