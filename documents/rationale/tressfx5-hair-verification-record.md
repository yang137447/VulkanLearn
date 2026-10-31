# TressFX 5 发丝物理验证记录

状态：2026-10-24 的验证准备记录，不是实现契约。内容覆盖验证矩阵、现有能力盘点、
采集/检查工具用法和「当前不能验收」的最小改动；记录前半段没有构建或启动 UE，也没有
构建、启动 VulkanLearn 或跑渲染。**同日补记**：随后新增了正常 Console `mesh` 命令
（§2.2）并跑通一次 `--placement-probes` 短采集（§4.4），因此 §2.2 的能力表、§2.3 的
placement 缺口、§3.1 的「VL 当前差距」和 §3.4 的目标 1 状态已按实际更新。当前实现事实
以 [TressFX 导入契约](../contracts/tressfx-asset-import.md) 和
[测试契约](../contracts/testing.md) 为准；两者与本文冲突时以契约为准。

固定源基线仍是 `GPUOpenSoftware/UnrealEngine` 的 `TressFX5-5.4.4`，
revision `61f8dd9b51b88d41937ef8d73618aac778642f41`。源行为来自该 revision 的
host/shader 公式、单位与调度审计，以及
[物理效果对照决策](../plan/rendering/tressfx5-hair-port-plan.md#ue5-物理效果对照决策2026-10-24)
中已记录的源码行号；不声称与 UE 实测一致，也没有 UE 截图/轨迹。

## 1. 证据口径

四类证据分开记账，任何一类都不能替换另一类：

| 类别 | 能证明 | 不能证明 |
| --- | --- | --- |
| 独立数学 oracle / CPU 参考 solver | 公式、单位换算、约束顺序、固定输入下的数值期望 | GPU 实际 dispatch、真实资产蒙皮、世界空间 placement |
| GTest 模块测试 | 纯函数、历史/时钟状态、资产解析和参数布局 | GPU 三份位置缓冲、实际帧调度、穿透深度 |
| 外部 Console 烟测（本仓库工具） | 进程真实启动、Console 路径可执行、日志事件、GPU pass 时间、图像与 SHA256 | guide 逐点位置、段长、速度、穿透、NaN 上界 |
| GPU / CPU 数值导出 | 上述逐点量及其时间序列 | 不自动证明视觉质量；仍需有限值/阈值判定和人工复核 |

硬约束：

- 图片只能证明「某一时刻的可见性」，不能证明有限值、段长上界、零穿透或十分钟无 NaN。
- `completed=true` 只表示采集流程结束；`visualReviewRequired=true` 表示必须人工看图。
- 30/60/120/144 Hz 是启动帧率上限，不等于帧率无关性通过；实际 dt 和每真实秒物理时间
  必须单独测量。
- 动画 pause 只冻结骨骼；发丝仍受风，不能把「pause 后发丝还在动」解释成游戏时钟失效。
- 外部采集串行执行；强制结束自有进程不构成正常 teardown 证据。
- 不改正式 `config/`、`SC_*`、`SM_*` 资产；证据写 gitignored 的 `artifacts/`。

## 2. 现有能力盘点

### 2.1 现有遥测能证明什么

| 通道 | 当前内容 | 能证明 | 不能证明 |
| --- | --- | --- | --- |
| `report.json` | 运行目录、帧率上限、截图元数据、播放状态、reset/dispatch/SDF 日志、perf 摘要、输入/二进制 SHA256、进程清理字段 | 采集流程、身份与输入未被替换、每个报告字段可复查 | 发丝位置、速度、接触几何 |
| `perf.jsonl` | 每秒一行 `fps`、`cpuMs`、`gpuTotalMs`、`valid`、`passes`、`cpuStages` | 聚合 CPU/GPU 时间、`hairSimulation` pass 中位时间、GPU 采样是否有效 | 每帧 dt、每帧 dispatch 次数、逐点位置 |
| `stdout.log` 的 `[TressFX] GPU guide simulation dispatch:` | 只在首个有工作帧 print 一次：objects/guides/points/passes/sdfObjects | 实际走到 GPU guide 路径，资产规模与 pass 表大小 | `passes=9` 不是本帧 dispatch 数；不能证明后续帧都在步进 |
| `stdout.log` 的 `[TressFX] ResetPositions:` | `reason`、`playbackIdentity`、`discontinuity`、`frame` | 首次姿态、seek/clip、pause 恢复等复位事件及次数 | 复位后三份 GPU 位置是否真的相等 |
| `stdout.log` 的 SDF 行 | `SDF collision buffer:` / `SDF domain:` | 碰撞代理被加载、SDF 域被建立 | 任何点的穿透深度 |
| BMP/PNG + SHA256 | 交换链截图、尺寸、完整载荷、摘要 | 图像可读、未截断、可人工复核 | 物理数值；截图不能替代读回 |
| `report.json` 的进程字段 | `pid`、`ownedProcessExited`、`processExitCode`、stderr 字节数 | 自有进程是否退出、是否有 stderr 诊断 | 正常 teardown 语义（强制结束不是优雅退出） |
| `--console-command "perf json"` | 命令执行时刻的单行快照 | 该时刻 FPS/CPU/GPU total/pass 表 | 逐帧分布；注入延迟不受控 |
| GTest `hair_asset_tests` | 解析、纯数学、历史/时钟、参数布局 | CPU 侧公式与状态契约 | GPU buffer 内容和实际调度 |

现有工具（`tool/hair/capture_hair_physics.py`）的默认序列：正式 Idle 场景启动、
`startup.bmp`、Idle、Walk、动画 pause、seek 0.25、seek 2.75、切回 Idle、
侧面/背面接触图；报告保留播放状态、dispatch/SDF 日志、reset 事件、GPU
`hairSimulation` 时间、输入/二进制 SHA256 和进程终止结果。

### 2.2 Console 命令能力表

本轮先做源码盘点，随后补记了新增的 `mesh` 命令并实际运行引擎（§4.4）。
以下命令来自 `source/debugConsole.cpp::ProcessCommand` 的实际分支，不臆造命令。

| 命令 | 现有用途 | 能否做 placement 平移/传送 | 能否做快速转头 | 能否做固定机位跟随 | 结论 |
| --- | --- | --- | --- | --- | --- |
| `animation`（play / pause / get / seek / clip） | 播放/暂停/查询/跳转/切 clip | 否，只改变骨骼动画 | 否，正式场景只有 Idle/Walk 两个 clip | 否 | 能做 seek/clip 复位和 pause 探针 |
| `camera`（get / position / lookat / pose） | 设置/读取稳定机位 | 否，只改相机 | 否 | 只能固定机位或分段改机位；没有 attach/follow/插值 | 接触侧/背图可用；连续跟随不可用 |
| `screenshot [file.bmp]` | 交换链截图 | 否 | 否 | 否 | 只产出图像 |
| `perf json` | 单行性能快照 | 否 | 否 | 否 | 采样快照，不是逐帧遥测 |
| `gpustats` | 最近一次逐 pass GPU 表 | 否 | 否 | 否 | 聚合表 |
| `loadworld` / `loadscene` | 整 World 重载 | **否**：会换 worldGeneration，全部历史重建 | 否 | 否 | 不能当作 placement teleport；它测的是重载而不是换基 |
| `mesh transform` / `mesh offset` / `mesh sweep` | 显式改变 mesh 场景对象根 TRS：绝对 TRS、当前位移叠加、有限时长线性推进 | **是**：位置 cm、rotation 度（Y 为 yaw）；sweep 在 GT 每帧推进 | 否，只改 actor 根变换，不改骨骼 pose | 可配合固定机位 | 正常 Console 命令（`debugConsole.cpp:289`），复用 `World::SetMeshSceneObjectTransform`；`transform` 的 scale 固定为 1（非单位缩放 actor 会被重标定），`offset` 保留当前 rotation/scale；图片只证明可见性 |
| `previewmodel` | 把模型注入预览场景后重载 | 否 | 否 | 否 | 不改变当前 World 的 actor 轨迹 |
| `injectinput`（action / down / up） | 注入 gameplay 动作 | 否 | 否 | 否 | 没有 actor transform 通道 |
| `editor <JSON>` | 编辑器资产命令 | 否（资产文档，不是运行时 actor 路径） | 否 | 否 | `--no-dev-ui` 时 editor host 不可用 |
| `windstrength` / `windgust` | SpeedTree 风 | 否 | 否 | 否 | 不控制发丝作者风向量 |
| `motiondebug` / `debugdraw` / `debugview` | 调试显示 | 否 | 否 | 否 | 不影响 solver 输入 |

关键发现（补记后）：

- 底层已有 `RuntimeCommandType::SetActorProperty` 和 `ApplyEditorActorProperty`，其中 `Mesh`
  分支支持 Position/Rotation/Scale。但它依赖 `sceneDocuments->PrepareActorEdit`，面向编辑器
  文档，`--no-dev-ui` 下不可用，因此**没有**复用它做 placement。
- 新增的 `RuntimeCommandType::SetMeshPlacement`（`source/engine/runtimeCommand.h:43`）只带
  场景对象身份和 TRS/位移/时长，由 `RuntimeCommandExecutor::ApplyMeshPlacement`
  （`source/engine/runtimeCommandExecutor.cpp:893`）在 GT 上直接调用
  `World::SetMeshSceneObjectTransform`，不触碰编辑器文档，也不写资产。
- `mesh offset` 需要读当前根 TRS，因此 `World` 新增只读
  `GetMeshSceneObjectTransform`（`source/world/world.h:174`、`source/world/world.cpp:509`）；
  scene TRS 是 actor 级的，读第一个 section 即可。
- `mesh sweep`（`runtimeCommandExecutor.cpp:1032`）由执行器在每个 GT 帧推进一次，
  用 `steady_clock` 而不是帧计数算比例，因此 30/60/120 Hz 下位移速度一致；没有命令时
  没有每帧默认开销。同一 identity 的新 sweep 会替换旧 sweep，`loadworld` 会取消它。
- `loadworld` 仍是整 World 替换，会重置 hair 历史/时钟，不能用于验证「同一 actor
  连续移动时的惯性/换基」。

### 2.3 能力缺口清单

| 缺口 | 证据 | 影响 |
| --- | --- | --- |
| 有 placement 命令但无逐点轨迹读回 | `mesh` 只写 World 根 TRS 并打印回执；guide 位置仍是 device-local SSBO | 目标 1 的连续运动/传送只能采到图像与事件，不能算惯性/过冲/段长数值 |
| 无快速转头夹具或运行时姿态命令 | 正式源 `SM_gasp_motion_source.json` 只有 `M_Neutral_Stand_Idle_Loop`、`M_Neutral_Walk_Loop_F` | 快速甩头只能由新夹具提供，不能用 Walk 代替 |
| 无相机跟随/挂接命令 | `camera` 只有 get/position/lookat/pose，且 `--no-dev-ui` 下无 attach 路径 | 连续跟随只能人工分段设机位，不能证明跟随误差 |
| 无逐帧 hair dt/timeStep/dispatch 计数 | `perf.jsonl` 每秒一行；dispatch 日志只打一次 | 目标 2 的 dt 分布、n 次 dispatch、物理时间比不能验收 |
| 无 guide 逐点读回 | `hairSimulationPass` 的 current/previous/prevPrev 是 device-local SSBO，只被 compute/graphics 消费 | 根点延迟、发梢滞后、过冲、衰减、段长、穿透、抖动都不能量化 |
| 无 VSP/capsule/SDF 运行时开关 | VSP 系数、capsules、`collision.mesh` 全来自 SM 资产 | 目标 3 的 A/B 对照不能只靠现有 Console |
| 无零风静置夹具 | 发丝风来自作者资产 `wind: [2000,0,0]`；Console 的 `windstrength` 只作用于 SpeedTree | 接触抖动与风致运动混在一起 |

## 3. 验证矩阵

阈值是初版工程判定，量纲与场景相关；每个阈值都必须由一个独立 oracle
（解析特例、CPU 参考 solver 或手算轨迹）校准后才可作为验收线。`[现有]` 表示
本轮工具已能产出证据，`[新增]` 表示需要读回/导出/夹具。

### 3.1 目标 1：世界空间历史与角色运动惯性

| ID | 场景 | 源行为 | 物理含义 | VL 当前差距 | 指标 | 数据来源 | 采集方式 | 阈值 / 判定 | 失败证据保留 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M1-PLACE-T | 共同 Actor 连续平移：静止→匀速→停止（+200 cm，0.2 s 起、1.0 s 停） | 源 patch 在 `ApplyVertexBoneSkinning` 组合 `LocalToWorld`，Integration 把 world cm 转 world m；位置历史在世界空间推进 | 发梢因惯性滞后，停止后过冲并衰减；根点应随 actor 无稳态误差 | VL 用 `sourceToHair` 相对 palette，`TressfxModelHistory` 只在 model 变化时换基；**已有**正常 Console `mesh offset/transform/sweep` 驱动共同 Actor；缺逐点读回 | 根点跟随延迟（帧）；发梢世界位移/滞后；过冲比；5% 衰减时间；与解析刚体轨迹偏差 | `[新增]` GPU readback + 解析轨迹；`[现有]` Console 回执、图像、reset 事件 | 正常 Console `mesh offset/transform`（非 runtime test flag）或 `mesh sweep` 固定时长推进；逐点读回尚未实现 | 根点 95% 响应 ≤1 帧；发梢滞后 ≤2 帧；过冲 ≤15%；5% 衰减 ≤0.5 s；轨迹偏差 ≤max(0.5 cm, 1% 位移) | 原始 readback `.bin`/CSV、帧号、dt、reset 事件、阈值计算 JSON、失败样本索引 |
| M1-PLACE-R | 绕 Y 轴 0→90°/180° 转身（阶跃或 180°/s） | 源世界变换参与蒙皮与历史；整 actor 旋转不是骨骼 pose 变化 | 发梢相对头部的角滞后、世界轨迹偏转；不能把转身当 seek 复位 | 同上；`mesh transform <id> <x> <y> <z> 0 90 0` 已能做 actor 旋转阶跃，连续转速仍靠多次命令 | 根点角误差；发梢世界位移滞后；段长伸长比；过冲 | `[新增]` GPU readback + 解析旋转 oracle；`[现有]` Console 回执 + 图像 | 同 M1-PLACE-T，用 `mesh transform ... rx ry rz` 的 Y 分量 | 角误差 ≤1°；发梢滞后 ≤2 帧；段长 p99 ≤1.10；过冲 ≤15% | 同上，另存旋转矩阵序列 |
| M1-TELEPORT | 同一 actor 瞬移 +200 cm 或 +90° | 源 teleport/大跳变通常触发历史复位；复位后不能把旧速度当惯性 | 传送帧应一次 reset，之后从当前蒙皮 rest 重建，不应拉长/穿透 | VL 有 `pose-discontinuity`/`playback-changed` reset；**已有**正常 Console `mesh offset` 大位移可做传送，实测 placement 本身不触发 reset（见 §4.4）；`loadworld` 仍是换 world 而非 teleport | 复位事件帧号/原因；复位后首帧位置误差；段长；NaN | `[现有]` Console 回执 + reset 日志；`[新增]` readback | `mesh offset` 前后各采若干帧；冻结姿态以排除 Idle loop wrap 的 reset | teleport 后 1 帧内出现 1 次 reset；复位后 2 帧内无 >1.5× rest 段长；无 NaN | 复位前后 3 帧 readback、reset 日志、图像仅辅助 |
| M1-HEAD | 快速甩头：0.1–0.2 s 内 yaw 60–90° | 源根点/胶囊/SDF 共用世界/相对变换；头部快速转动是骨骼 pose 变化，不是 actor placement | 发根跟随头部；发梢滞后、过冲和接触响应 | 正式 clip 只有 Idle/Walk；没有快速转头夹具 | 根点跟随误差；发梢滞后；最大段长伸长；接触穿透 | `[新增]` 夹具 + GPU readback | 新增测试 clip 或正常 Console 姿态命令；固定机位取侧/背 | 根点误差 ≤0.5 cm；发梢滞后 ≤2 帧；段长 p99 ≤1.10；无 NaN | 完整姿态/位置序列与失败帧截图 |
| M1-ROOT | 根点跟随 | 源不可移动点写回 `InitialPos`（蒙皮 rest） | 根点应等于当前蒙皮 rest，不能漂移 | `rootFixedPoints=2` 已有 CPU 测试；缺 GPU 逐点读回 | 根点世界位置 vs 解析蒙皮位置 | `[新增]` GPU readback | 读回每根 guide 前 2 点，和 CPU palette 解析值比较 | 误差 ≤0.5 cm 或 1% 段长 | 根点逐帧 CSV + palette 副本 |
| M1-TIP | 发梢世界位移/滞后 | 源世界空间历史传播速度和惯性 | 发梢应滞后于根点，但不应持续拉长 | 无发梢轨迹 | 发梢相对头部位移、互相关滞后 | `[新增]` GPU readback | 每帧读回 guide 末点，和头骨矩阵比较 | 互相关滞后 ≤1 帧；稳态幅值误差 ≤10% | 轨迹曲线与互相关原始数组 |
| M1-OVERSHOOT | 停止后的过冲/衰减 | 源阻尼系数直接进入 `exp(-DampingCoeff*dt*60)` | 过冲/衰减由阻尼与约束共同决定 | 无时间序列 | 过冲比、5% 衰减时间、残差 | `[新增]` GPU readback + CPU oracle | 停止后连续读回 1 s | 过冲 ≤15%；5% 衰减 ≤0.5 s；与 CPU oracle 同 dt 偏差 ≤0.5 cm | 停止后每帧轨迹与 oracle 对照 |
| M1-RIGID | 与解析刚体运动偏差 | 源 world cm→m 后积分；无外部刚体插值 | 纯平移/旋转下，若没有惯性，轨迹应与刚体变换重合；有惯性则偏差应可解释 | 相对空间 solver 的偏差未量化 | 点位置误差、角度误差 | `[新增]` readback + 解析变换 | 对同一 model 轨迹分别算蒙皮刚体点和 solver 点 | ≤max(0.5 cm, 1%)（静止/准静态）；动态项单独记账 | 误差时间序列与公式参数 |

### 3.2 目标 2：源步长/循环/阻尼/实际物理时间

| ID | 场景 | 源行为 | 物理含义 | VL 当前差距 | 指标 | 数据来源 | 采集方式 | 阈值 / 判定 | 失败证据保留 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M2-RATES | 30/60/120/144 Hz 各一次 | 源主路径固定 `TimeStep=0.0125`、最多 2 次循环，再从局部 dt 扣 `0.0167`（按计划记录的源码行号） | 源每真实秒的物理推进与帧率不是简单相等；需要按同输入 dt 序列推演 | VL 已按源原生调度：每帧 n=0/1/2 个固定 0.0125 s substep，`maxDt` 不再限制 GPU/`StepFrame`；逐帧 dispatch/timeStep 统计仍缺 | 实际 fps；每帧 dispatch；每真实秒物理时间；reset 次数 | `[现有]` perf + 日志；`[新增]` 逐帧 stats | 现有 `--frame-limit`；新增 stats 通道 | 实际 fps 在目标 85%–110%；每档至少 1 个有效 GPU 采样；四档均有 dispatch/SDF 证据 | 每档 report/perf/log/readback |
| M2-DT | 每帧实际 dt 分布（含变量 dt） | 源上层把组件 dt 保存后进入固定步长循环；局部 clamp 不代表上层实际调度 | 必须知道每帧传给 solver 的 dt，而不是只看启动帧率 | 当前没有逐帧 dt；perf 每秒一行；`fps` 倒数只是聚合代理 | dt min/p50/p95/p99/max；每帧 timeStep | `[新增]` 正常 Console `hair stats` 或等价统计 | 帧末累计直方图，Console 读取；不新增 runtime test flag | 所有 dt 有限且 ≥0；每帧物理时间只能是 0/0.0125/0.025 s；每帧输入的 frameDelta p50 才对应 33.3/16.7/8.3/6.9 ms | stats JSON + 原始直方图；不能只留截图 |
| M2-DISPATCH | 每帧 n 次 dispatch | 源主路径最多 2 轮，每轮按 pass 录制；VL pass 表固定 9 项 | 必须区分「pass 表大小」和「本帧实际 dispatch 数」 | 日志 `passes=9` 是表大小；只在首帧打印一次 | 正常帧/复位帧/暂停帧/跳过帧的 dispatch 数 | `[新增]` 逐帧统计 | 在 `HairSimulationPass::Record` 累计 `recordedPasses`/`dispatchGroups` | 本资产正常帧每 substep 8 个 dispatch（VSP 关闭；SDF clear/splat 每帧 2 个、collide 每 substep 1 个）；reset 帧同样跑完整链（首轮 reset + 后续 pass，n=2 再一轮），不是仅 Integration=1；n=0/暂停/重复快照=0 | 每帧 CSV：frame、dt、mask、dispatch count、reset reason |
| M2-PHYS | 每真实秒物理时间 | 源固定步长/循环决定累计物理时间；`0.0167` 扣除是源调度的一部分 | 30 Hz 时 VL 一帧最多 0.016 s，物理时间会慢于真实时间；60/120/144 接近 1:1 | 没有逐帧 timeStep，无法证明；现有 fps 只能算代理 | `Nsteps * timeStep / wallSeconds` | `[新增]` stats | 用 M2-DT 的 timeStep 直方图求和，除以采集墙钟 | 原生调度下每帧物理时间 = n*0.0125，每秒 = fps*n*0.0125：30 Hz≈0.75，60/120/144≈0.75/1.5/1.8；按实测 report 记录，不写死成功率 | 物理时间累计表、帧数、墙钟、公式 |
| M2-RESET | 首次姿态、seek、clip、pause/resume | 源 reset 由姿态/播放身份/不连续触发；复位清旧速度并同步三份历史 | 复位不能把旧惯性带入新姿态；pause/resume 不应误复位 | 日志已有 first-pose/playback-changed/pose-discontinuity/pose-recovered | 每类 reset 次数、帧号、复位后首帧位置 | `[现有]` 日志 + `[新增]` readback | 现有 seek/clip/pause 序列；新增 teleport 场景 | first-pose ≥1；每次 seek/clip 1 次；pause/resume 不产生 reset；teleport 1 次；稳态无 reset；reset 帧日志带 `substeps=` 且同帧继续录后续 pass | 原始 reset 行与前后帧 readback |
| M2-PAUSE | 动画 pause 后是否继续步进 | 源有独立的游戏/动画时钟；动画 pause 只冻结骨骼 | pause 后发丝可以继续受风步进，但必须有 dt/step 证据；不能只看图 | 现有只有 pause 前后图片和 reset 日志 | 每帧 step 计数、timeStep、reset | `[新增]` stats + `--pause-probes` | 现有 pause 探针 + 新增逐帧统计 | pause 后 timeStep>0 的帧数 >0 且 reset 不增加；若期望冻结，必须给出源语义依据 | 逐帧 stats、reset 日志、pause 前后图 |
| M2-VAR | 不限速变量 dt | 源按组件 dt 调度；不保证帧率固定 | 变量 dt 下不能假定固定步长；要验证 dt 序列与物理时间 | `--rates 0` 可启动不限速，但没有逐帧 dt | dt 分布、p99、掉帧/长帧 | `[新增]` stats | `--rates 0 --duration N`，记录实际 dt | 所有 dt 有限；p99 ≤maxDt（被 clamp）；长帧必须显式出现在直方图 | 原始 dt 直方图与长帧索引 |

### 3.3 目标 3：VSP 单位、刚度曲线、碰撞响应

| ID | 场景 | 源行为 | 物理含义 | VL 当前差距 | 指标 | 数据来源 | 采集方式 | 阈值 / 判定 | 失败证据保留 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M3-VSP | VSP on/off（同轨迹同 dt） | 源 VSP 的 `length(new-2*old+oldOld)` 未除 dt²，是二阶位置差幅值；默认 1.4 world m≈140 cm；作者曲线按 `i/N` 采样 | VSP 传播根部速度、抑制拉长；系数与阈值必须与单位匹配 | VL `vspCoefficient` 来自资产（本 Female 资产为 0，即关闭）；代码默认阈值已由 5000 改为 140 cm，但仍是线性 VSP 近似、统一 LocalShape 标量；本轮未做运行时 A/B | 段长伸长比 p99/max；VSP on/off 差值；系数扫描 | `[新增]` GPU readback 或 CPU 参考导出 | 正常 Console `simulation vsp` 或 CPU 导出；同输入 dt 序列 | 静止 p99 ≤1.02；运动 p99 ≤1.10，max ≤1.5；VSP on 相对 off 的 p99 降低 ≥20%（高加速度段） | 每个系数的段长时间序列、参数、CPU oracle |
| M3-UNIT | 单位/刚度曲线对照 | 源 world cm→m、rest×0.01、VSP 1.4 m；LocalShape/VSP 作者曲线 64 项 | 单位错会把阈值放大 100 倍；曲线形状影响根部/梢部刚度 | VL cm 不缩放；线性 VSP、统一 stiffness；作者曲线未迁移 | 单位换算误差；曲线采样误差；段长/角度响应 | `[新增]` CPU 导出 + 公式 | 同一组 rest/曲线分别按源与 VL 公式计算 | 单位换算 0 误差；曲线在 `i/N` 采样点相对误差 ≤1%；响应曲线在容差内 | 公式表、采样表、误差图 |
| M3-CAP | 胶囊接触（头/颈/肩/上背） | 源没有 guide 胶囊 pass；VL 用四根骨骼胶囊按顺序投影 | 投影后点应在半径+margin 外；重叠顺序决定最终位置 | 已有 CPU 胶囊数学测试；无运行时逐点距离 | 穿透深度代理 `max(0, r-d)`；接触抖动；段长 | `[新增]` GPU readback | 从 current 位置和当前胶囊参数计算 | p99 穿透 ≤0.1 cm；max ≤0.5 cm；静止接触抖动 ≤0.1 cm/帧；无 NaN | 胶囊参数、距离、投影前后点、失败帧 |
| M3-SDF | SDF 身体代理接触 | 源 SDF 对 previous 的写回在 patch 8051 已注释；VL 保留位置投影语义并每帧重建 SDF | 负距离是穿透；梯度投影后应在 margin 外；不能把投影当速度响应 | 有 CPU SDF 镜像测试；无 GPU 逐点距离 | SDF 有符号距离、穿透深度、接触抖动、段长 | `[新增]` GPU readback + SDF 参数 | 读回 current 与 SDF grid/域，按同一三线性采样算距离 | p99 ≤0.1 cm；max ≤0.5 cm；无 NaN；接触点不振荡 | SDF 域、采样距离、梯度、失败帧 |
| M3-JIT | 静置接触抖动 | 源接触响应受风/阻尼/约束影响；没有零风时抖动与风混在一起 | 静置抖动量级决定是否可见抖动 | 无零风夹具、无位置序列 | 末梢位置标准差、速度、接触状态翻转 | `[新增]` stats + readback | 固定姿态、记录 2 s；风不可关时另记风输入 | 位置标准差 ≤0.1 cm/帧；接触状态不每帧翻转 | 2 s 逐帧位置、风速/参数 |
| M3-FIN | 有限值/段长上界 | 源 solver 约束段长；数值爆炸必须先被检测 | NaN/Inf 或超大段长会污染后续所有帧 | 没有 GPU 位置读回 | 非有限计数；段长/rest 比 p99/max | `[新增]` GPU readback | 逐帧扫描 current/previous/prevPrev | 非有限计数=0；段长 >0；p99 ≤1.10；max ≤2.0（硬失败）；任何 NaN 立即失败 | 首个失败帧索引、点索引、原始三份位置 |

### 3.4 当前状态摘要

| 目标 | 当前可采集 | 当前不可验收 | 结论 |
| --- | --- | --- | --- |
| 目标 1：世界历史/actor 运动 | 共同 Actor 平移/绕 Y/teleport/sweep 已能由正常 Console `mesh` 命令驱动并采图，窗口内 reset 计数与 Console 回执入报告；逐点轨迹/过冲/衰减仍未采 | 平移/转身/teleport/快速甩头的根点与发梢轨迹、过冲、衰减、刚体偏差 | 部分可采集（partial）；数值仍未验收 |
| 目标 2：步长/循环/实际时间 | 30/60/120/144 帧率上限、perf 聚合、reset 事件、pause 探针、变量 dt 启动 | 逐帧 dt 分布、每帧 dispatch 数、物理时间累计、pause 步进计数 | 未验收；只能做 smoke |
| 目标 3：VSP/刚度/碰撞 | 胶囊/SDF 路径日志、侧/背接触图、CPU 纯数学测试 | VSP A/B、段长伸长比、穿透深度、接触抖动、有限值 | 未验收 |

三个目标都必须等数值导出/夹具落地后才能从「smoke」升级为「验收」；图片和
`completed=true` 不改变本结论。

## 4. 工具与报告

### 4.1 `capture_hair_physics.py`（追加扩展）

默认行为保持：不传新参数时仍是 30/60/120、4 s Walk、原有 9 张图序列。
新增参数：

| 参数 | 作用 | 备注 |
| --- | --- | --- |
| `--rates 0` | 不限速变量 dt 运行（不传 `--frame-limit`） | 0 只用于变量 dt；目录名 `00-uncapped` |
| `--pause-probes N` | `animation pause` 后每 `--pause-probe-interval` 秒再拍 N 张 | 默认 0，不改变旧序列 |
| `--settle-probes N` | Walk 前每 `--settle-interval` 秒拍 N 张 Idle | 用于静置/接触观察，不是数值验收 |
| `--perf-probes` | 每张截图同次注入 `perf json` | 记录采样快照；明确不是逐帧 dt |
| `--placement-probes` | 走共同 Actor placement 序列：Idle → +100 cm 平移 → 绕 Y 90° → teleport → 1 s `mesh sweep` | 默认关闭；开启后替换 Walk 交互序列，默认 30/60/120 与 9 图序列不变 |

新增报告字段（全部追加）：

- 顶层：`schemaVersion`、`tool`、`startedAt`/`finishedAt`/`elapsedSeconds`、
  `captureArguments`、`scenarioCoverage`。
- 每 run：`runDirectory`、`configuredFrameLimit`、`frameLimitMode`、
  `requireReset`、`pauseProbesRequested`、`settleProbesRequested`、
  `placementProbesRequested`、`dispatch`、`resetEvents`、`stdoutBytes`、`timing`。
- placement run 额外：`placement`（`mode`、相机、位移/时长、`evidence`（Console 回执原文）、
  `rejections`、`missingEvidence`、`resetEvents`/`resetReasonCounts`、`dispatchEvidence`、
  `poseFrozenDuringPlacement`、`visualDifferenceOnly`）。
- 每 capture：`pngSha256`、`bmpBytes`/`pngBytes`、`bmpHeader`、`resetEvents`、
  `resetReasonCounts`、`dispatchEvidence`、`sdfEvidence`、可选 `perfSnapshot`。
- `scenarioCoverage` 不再是纯静态表：`motion.placement.*` 三项按本次是否启用
  `--placement-probes` 在 `captured` / `partial` 之间切换，默认运行不会谎称已采集。
- `timing.source` 当前固定为 `not-collected`，并在 `reason` 中说明
  「perf JSONL 每秒一行，没有逐帧 dt/timeStep/dispatch」。这是明确的缺口，
  不是通过标记。

示例：

```powershell
py -3 tool/hair/capture_hair_physics.py --output artifacts/hair-physics-matrix `
    --rates 30 60 120 144 --require-reset
py -3 tool/hair/capture_hair_physics.py --output artifacts/hair-physics-vardt `
    --rates 0 --duration 10 --pause-probes 3 --settle-probes 3 --perf-probes
py -3 tool/hair/capture_hair_physics.py --output artifacts/hair-placement-probes-20261024 `
    --rates 60 --placement-probes --perf-probes
```

### 4.2 `verify_hair_physics.py`（新增纯 Python 检查器）

读取一次采集目录的 `report.json`、每个 run 的 `perf.jsonl`、`stdout.log`、
`stderr.log`、BMP/PNG，以及报告记录的输入/可执行文件摘要。默认 `--profile acceptance`；
只想检查采集完整性时用 `--profile capture`。

```powershell
py -3 tool/hair/verify_hair_physics.py artifacts/hair-physics-matrix `
    --profile capture --json-out artifacts/hair-physics-matrix/verify.json
py -3 tool/hair/verify_hair_physics.py artifacts/hair-physics-matrix `
    --profile acceptance --json-out artifacts/hair-physics-matrix/acceptance.json
```

检查项：

- 报告 schema、完成标志、人工复核要求、`physicsNumericalValidation` 一致性；
- `exeSha256` 格式与当前二进制差异（只标记历史证据，不伪造当前实现）；
- config/SC/SM 输入 SHA256 覆盖、捕获前后不变、当前文件可重算；
- 每个 run 的 `rateHz`、`completed`、`sequenceCompleted`、进程 PID/退出/残留；
- 每张图的 BMP 头（24-bit、宽高、完整载荷）、BMP/PNG 摘要、播放状态；
- stderr 为空、VUID=0、Console 没有被拒命令；
- dispatch 行、SDF 行、reset 事件与报告计数一致；
- `perf.jsonl` 每行 schema、有效 GPU 行、`hairSimulation` pass、中位值、FPS 与
  `--frame-limit` 的 85%–110% 容差、perf 时间覆盖；
- `timing` 是否来自逐帧 runtime 源；`timeStep.max ≤ maxDt`、排序、物理时间比；
- 若报告带 `trace`：重新解析 VLHT 文件，校验 SHA256/字节数/帧数/header 字段，
  计算 `finiteCount`、段长比 p50/p99/max、根点/发梢逐帧最大位移与
  reset/substep/dispatch 分布；缺失 trace 在 acceptance profile 记为证据缺口；
- 验收 profile 额外要求 30/60/120/144 四档 run（可用 `--expect-rate` 覆盖，变量 dt
  用 `0`）、逐帧 dt/dispatch/readback/numericEvidence 和矩阵状态。

退出码：

| 退出码 | 语义 |
| --- | --- |
| 0 | 所选 profile 的所有必需检查通过；`capture` 通过不代表物理验收 |
| 1 | 用法、路径或 `report.json` 读取/解析失败 |
| 2 | 必需证据缺失、损坏、摘要不符、进程未清理或矩阵项未采集 |
| 3 | 证据存在但阈值/一致性失败（FPS、GPU 覆盖、dt、物理时间比等） |

`capture` profile 把 FPS/GPU 覆盖率/时间跨度列为验收缺口而不是采集损坏；
`acceptance` 才把同样的低帧率或覆盖不足按 exit 3 失败。`accepted=true` 只在
`--profile acceptance` 且所有失败/缺口为零时为真。

机器可读输出顶层字段：`profile`、`passed`、`accepted`、`exitCode`、`summary`、
`acceptance.missingOrUnproven`、`matrixCoverage`、`trace`、`checks`、`failures`。
`accepted=true` 只在 `--profile acceptance` 且所有失败/缺口为零时为真。

本轮对既有采集 `artifacts/hair-physics-corrected-20261024/` 做了文件级验证：
`--profile capture` 通过（0 失败，报告列出多个明确缺口），`--profile acceptance`
以 exit 2 失败。另对 `hair-physics-baseline-20261024`、
`hair-physics-debug-20261024`、`hair-physics-final-20261024` 跑同一 capture
profile，均 0 失败；旧报告的缺失字段只记为缺口，不自动改写成通过。这证明
检查器能区分「采集过程完整」与「物理可验收」，不是物理通过。

### 4.3 检查器自测

本轮用合成夹具验证了报告解析、证据缺口和阈值分支：

- 缺少 `report.json`：exit 1；
- 缺 `timing`/矩阵项：exit 2；
- 证据齐全但 FPS 30 对 `--frame-limit 60`：exit 3。

完整 `--profile acceptance` 通过还要求四档 rate（默认 30/60/120/144）、逐帧
timing 和 numericEvidence；§4.4 的短采集也缺这些项，因此本轮仍不宣称
acceptance 通过。合成夹具只用于测试检查器逻辑，不是引擎证据，已从 `artifacts/` 删除。

### 4.4 补记：`--placement-probes` 短采集（2026-10-24，实际执行）

命令与产物（`artifacts/` 被 gitignore，结论以本节为准）：

```powershell
cmake --build build-rel --target main -j 8
py -3 tool/hair/capture_hair_physics.py `
    --output artifacts/hair-placement-probes-20261024 --rates 60 `
    --placement-probes --perf-probes
py -3 tool/hair/verify_hair_physics.py artifacts/hair-placement-probes-20261024 `
    --profile capture
```

采集序列（全部是普通 Console 命令，没有 runtime test flag）：

| 步骤 | 命令 | 结果 |
| --- | --- | --- |
| 机位 | `camera pose 0 151 900 0 151 0` | 固定机位；场景 fov=38（水平）、far_clip=1000，位移因此保持在画面与远裁剪内 |
| idle | （截图 `placement-idle`） | `playing=1`、Idle clip |
| 冻结姿态 | `animation pause` | 只为把 placement 窗口内的 reset 归因到 placement；发丝仍按风/约束步进 |
| 共同平移 | `mesh offset FemaleCharacter 100 0 0` + `mesh offset Female_hair_skinned 100 0 0`（同次注入） | 两条回执 `position=(100,0,0)` |
| 绕 Y 转身 | `mesh transform <两个 identity> 100 0 0 0 90 0` | 两条回执 `rotation=(0,90,0)` |
| teleport | `mesh offset <两个 identity> 150 0 0` | 两条回执 `position=(250,0,0)`（比首次平移更大） |
| sweep | `mesh sweep FemaleCharacter Female_hair_skinned -300 0 0 2` | `Mesh sweep started` + `Mesh sweep finished ... position=(-50,0,0)` |
| 恢复 | `animation play` | 进程随后被采集器终止 |

同一序列的命令行形式（可直接粘进 Console；`scale` 固定为 1，rotation 单位为度）：

```text
mesh offset FemaleCharacter 100 0 0
mesh offset Female_hair_skinned 100 0 0
mesh transform FemaleCharacter 100 0 0 0 90 0
mesh transform Female_hair_skinned 100 0 0 0 90 0
mesh offset FemaleCharacter 150 0 0
mesh offset Female_hair_skinned 150 0 0
mesh sweep FemaleCharacter Female_hair_skinned -300 0 0 2
```

采集器给引擎传的参数保持 `--no-dev-ui --worker-thread-count 2`，本次实测这条链在
无开发 UI、双线程下都可用。

报告实测（`00-60hz`，`report.json`）：

- `completed=true`、`sequenceCompleted=true`、`inputsUnchanged=true`；7 张截图
  （`idle-continuous` + `placement-idle/translated/rotated/teleport/sweep-mid/sweep-end`），
  BMP 头都是 1280×720/24-bit，全部图有 `perfSnapshot`。
- placement 段：`evidence` 8 条 Console 回执、`rejections=[]`、`missingEvidence=[]`、
  `resetEvents=[]`（窗口内 0 次）；整次运行 reset 只有启动的 `first-pose=1`，
  `pose-discontinuity=0`。
- `sweepMidCapturedDuringMotion=true`：`placement-sweep-mid` 的 `Screenshot saved`
  早于 `Mesh sweep finished`，说明中间图拍在推进过程中，而不是终点。
- 运行健康度：`stderrBytes=0`、`vuidCount=0`、无 `Unknown command`；
  dispatch 行 `objects=1 guides=40334 points=1290688 passes=9 sdfObjects=1`；
  `perf.jsonl` 24 行、23 行有效 GPU 采样，中位 FPS 59.17（`--frame-limit 60`），
  `hairSimulation` 中位 2.08 ms、GPU total 中位 5.87 ms；报告 `exeSha256` 与当次
  `build-rel/bin/main.exe` 一致。
- `verify_hair_physics.py --profile capture` 退出码 0（0 failures），
  `scenarioCoverage` 的 `motion.placement.*` 三项随 `--placement-probes` 变为 `captured`；
  `--profile acceptance` 仍缺逐帧 timing/readback，不算物理验收。

图像观察（仅可见性）：每次瞬移后发丝云仍停在旧世界位置、被拉向新的根点，
sweep 中间帧的发丝朝运动反方向拖尾，停下的终点帧继续收敛；这与
[TressFX 世界历史记录](tressfx5-hair-world-history-record.md) §4 的换基推导方向一致，
但不是逐点数值证据。

归因注意：该资产 Idle 是带根运动的循环 clip，**不冻结姿态时**每 ~10 s 的 loop wrap
会自己推进 discontinuity 并触发一次 `pose-discontinuity` reset（首次实测两次相隔
551 帧 @60 Hz）。第一次实现没有 pause，报告里因此出现 2 次 reset；它们与 placement
命令时间上接近但因果无关。默认 Walk 序列本来就会产生这类 reset，读 reset 计数时
必须区分 loop wrap / seek / placement。

## 5. 当前不能验收的项与最小改动

当前只能把「采集/日志/SHA/进程/GPU pass 时间」作为 smoke 证据；以下矩阵项
**不能**给出通过结论。

| 不能验收的项 | 原因 | 本轮进展 / 后续最小改动 |
| --- | --- | --- |
| placement 平移/绕 Y/teleport 的数值惯性 | 命令已落地，但 Console 回执只证明 World 根 TRS 写入 | `hair trace` v2 + `analyze_placement_trace.py` 已做 200 cm/2 s 平移 sweep 与静态 control：根点差 0.0016 cm、发梢最大 32.97/均值 9.68/尾段 7.62 cm；绕 Y/teleport 的过冲归因尚未做 |
| 快速甩头 | 正式资产没有转头 clip | 增加测试夹具 clip（放在测试资源/artifacts，不改正式 SM），或增加正常 Console 姿态命令；必须固定 dt 序列 |
| 固定机位跟随 | `camera` 只有静态 pose | 增加 `camera follow <identity> <offset...>` 正常命令，或外部按已知根运动分段设机位；分段机位不能证明跟随误差 |
| 逐帧 dt/timeStep/dispatch | 只有每秒聚合 perf 和首帧 dispatch 行 | 在 `HairSimulationPass` 累计每帧 `timeStep`、`recordedPasses`、`resetReason`，由正常 Console `hair stats` 输出 JSON 直方图；不加 runtime test flag |
| guide 逐点位置/段长/穿透/抖动 | device-local SSBO 原先无读回 | `hair trace` v3 已读回逐点位置/段长与碰撞前 SDF/胶囊最大穿透（正式 2.35/3.29 cm，sweep 4.40/3.30 cm），并有 finite/segment 阈值；接触抖动仍需逐帧接触/速度序列另行归因 |
| VSP/capsule/SDF A/B | 参数只来自 SM 资产 | 运行时可加正常 `simulation vsp` / `simulation collision` 命令；或先用 CPU 参考导出做公式/单位 A/B。CPU 导出不证明 GPU 路径 |
| 零风静置抖动 | 发丝风是作者资产常量 | 增加测试夹具把 `wind` 设为 0（只改测试资产），或运行时风覆盖命令；否则抖动结论必须带风输入 |

### 5.1 GPU readback 最小方案（已实现，2026-10-24）

状态：已实现并通过 Release `main` 的正式 Female 场景短 trace。入口仍是普通 Console
诊断命令，没有新增 runtime test flag，也没有注册 CTest。

命令：

```text
hair trace <path> <frames> <guide-stride>
```

- 参数校验：`path` 非空、`frames > 0`、`guide-stride > 0`；Console 侧与 GT 执行端
  各校验一次（`source/debugConsole.cpp:403`、`source/engine/runtimeCommandExecutor.cpp:890`）。
- 只 trace 场景里第一个 GPU hair 对象；guide 取 `0, stride, 2*stride, ...`，最多
  `256` 根。样本只覆盖这个子集，不是全部约 4 万根 guide。
- 请求路径：Console → `RuntimeCommandType::RequestHairTrace`
  （`source/engine/runtimeCommand.h:77`）→ GT 的 `RuntimeCommandExecutor`
  → `RenderSystem::RequestHairTrace`（`source/renderSystem.cpp:1254`，互斥量保护）
  → 渲染线程 `RecordHairSimulation` 转发给 `HairSimulationPass::BeginTrace`
  （`source/renderSystem.cpp:3938`）。Console/GT 线程不访问 GPU、World 或文件句柄。

数据流与同步：

```text
HairSimulationPass::Record
  -> current/previous/prevPrev SSBO（device-local，已有）
  -> 本帧所有 dispatch（含 SdfCollide）录完后
  -> compute -> transfer barrier
  -> 每个选中 guide 分别 copy 三段 P*16 字节到当前 frame-slot readback buffer
  -> transfer -> compute barrier
  -> pending 元数据（frameIndex/worldGeneration/substeps/timeStep/resetReason/dispatchCount）
  -> 下一次同 slot 的 Record 开头（BeginFrame 已 WaitForFence）map + 写文件 + 清 pending
```

- 每个 frame-in-flight 槽一份 host-visible、host-coherent readback buffer
  （`eTransferDst`），大小 `selectedGuides * pointsPerGuide * 3 * 16`；另有一次性
  rest-length buffer。只在 trace 激活时分配/拷贝；未激活时 Record 只有一次
  `trace.active` 分支（`source/hair/hairSimulationPass.cpp:985` 附近）。
- `vkCmdCopyBuffer` 需要源 buffer 带 `eTransferSrc`；该 usage 位加在
  `CreateUploadedDeviceLocalStorageBuffer`（`source/renderableObject.cpp:24`），
  未激活 trace 时不产生拷贝或同步开销。
- barrier 与拷贝集中在 `RecordTraceCopies`（`source/hair/hairSimulationPass.cpp:520`），
  文件落盘在 `FlushTraceSlot`（`:472`），结束/汇总在 `FinishTraceInternal`（`:644`）。
- 目标帧数达到后停止新拷贝；最后 1–2 个仍 pending 的 slot 在它们下一次复用或
  `RenderSystem::ShutdownRenderObject` 的 `FinishTrace(true)` 中冲刷
  （`source/renderSystem.cpp:3625`，一次 queue idle）。正常 trace 只延迟到后面
  1–2 帧，不回读时没有额外等待。

文件格式（小端，`source/hair/hairTraceFormat.h:61`）：

| 段 | 字段 | 类型/字节 | 说明 |
| --- | --- | --- | --- |
| header | magic | `char[4]` = `VLHT` | 坏 magic 直接拒绝 |
| header | version | u32 | 当前 3（v3 增加 frame 穿透统计） |
| header | totalGuideCount | u32 | 对象总 guide 数（不是选中数） |
| header | pointsPerGuide | u32 | 每根点数 |
| header | stride | u32 | guide 选择步长 |
| header | selectedCount | u32 | 1..256 |
| header | startFrame | u64 | 首帧 `RenderScene::frameIndex` |
| header | selectedGuides | u32 × selectedCount | 升序、必须小于 totalGuideCount |
| header | restLengths | f32 × selectedCount × pointsPerGuide | 末点为 0；用于段长比 |
| frame | frameIndex / worldGeneration | u64 / u64 | 渲染帧号与世界代 |
| frame | substeps / timeStep | u32 / f32 | 源调度 substep 数与 0.0125 s |
| frame | resetReason / dispatchCount | u32 / u32 | reset 枚举与该对象本帧实际 dispatch 数 |
| frame | hairModel | f32 × 16 | 该帧 hair model（列主序）；世界 = model × local，供 placement 轨迹/惯性分析 |
| frame | sdfPenetrationMaxCm / capsulePenetrationMaxCm | f32 / f32 | 碰撞前点进入 SDF/胶囊内部的最大深度（cm，0=无）；collision/SDF shader atomicMax |
| frame | samples | 48 B × selectedCount × pointsPerGuide | 顺序固定为 guide → point → (current, previous, prevPrevious) 各 float4 |

- 文件实际布局与偏移由 `HairTraceWriter`/`HairTraceReader`
  （`source/hair/hairTraceFormat.h:108`、`:141`）统一负责；纯 CPU 单元测试在
  `tool/hair-tests/hairTraceTests.cpp`，覆盖合成数据往返、坏 magic、截断
  header/帧和非法选择索引。
- 采样顺序是 `guide -> point -> buffer`；GPU readback buffer 本身是
  `state -> guide -> point`（`vkCmdCopyBuffer` 只能拷连续区间），Writer 在 CPU
  侧做一次 48-byte 交错，不改变 GPU 拷贝粒度。
- v2 的 `hairModel` 让离线脚本可把 hair-local 点变到世界；`tool/hair/
  analyze_placement_trace.py` 对 sweep/control 两段 trace 逐帧比较局部根/发梢。
  2026-10-24 的 200 cm / 2 s sweep + `animation pause`（避免 loop-wrap reset）：
  600 帧、40 guides、无 reset；根点最大差 **0.0205 cm**（根点严格跟随），
  发梢最大差 **36.89 cm**、均值 **9.83 cm**、最后 30 帧均值 **7.57 cm**。
  这说明根点刚性跟随正确，发梢在大位移后有持续滞后/不同下垂平衡；不是
  穿透/接触质量结论。
- v3 增加碰撞前穿透统计：正式 120 帧 trace 的 SDF 最大 **2.35 cm**、胶囊
  **3.29 cm**；200 cm sweep 的 600 帧 trace 为 SDF **4.40 cm**、胶囊
  **3.30 cm**（均低于 6 cm 阈值）。这些是**投影前进入内部**的深度，不是 margin
  缓冲层，也不是碰撞后的接触抖动。
- 曲线运行时烟测：临时 SM 作者 32 项 VSP/LocalShape 曲线（`vspCoefficient=0.5`），
  日志出现 `vspcurve=32`、`localshapecurve=32`，120 帧 trace finite=0、
  segment max 9.75、SDF/胶囊穿透 1.98/3.29 cm；临时资产已删除。

外部校验与指标：

- `tool/hair/capture_hair_physics.py` 新增 `--trace`、`--trace-frames`
  （默认 120）、`--trace-stride`（默认 1024）；默认不启用，旧采集序列不变。
  它在 startup 截图后注入一次 `hair trace`，等待
  `[TressFX] Hair trace complete` 摘要，记录文件 SHA256/字节数/帧数，并把
  `physicsNumericalValidation`、`numericEvidence`（kind=`gpu-readback`）写进报告。
- `tool/hair/verify_hair_physics.py` 的 `read_hair_trace`（`:162`）重新解析 VLHT
  文件并计算 `finiteCount`、`segmentRatioP50/P99/Max`（current 相邻点距离 / rest
  长度）、根点/发梢逐帧最大位移、substeps/timeStep/resetReason/dispatchCount
  分布；`check_trace`（`:1023`）校验文件摘要、帧数、字段一致性，并把非有限值
  记为硬失败、段长量级记为 acceptance 阈值。缺失 trace 时 acceptance profile
  仍报 `run[0].trace` 缺失并失败。
- 实测（`artifacts/hair-trace-final2-20261024`，Release，60 Hz，正式 Female）：
  120 帧、40 根选中 guide、32 点/根、文件 7,381,952 B；`finiteCount=0`、
  根/发梢非有限数均为 0；段长比 p50=1.046、p99=3.048、max=9.492；根点逐帧
  最大位移 0.077 cm、发梢 3.122 cm；stderr 0 B、VUID 0；首帧 dispatch 行与
  trace 关闭时相同（`objects=1 guides=40334 points=1290688 passes=9
  sdfObjects=1`）。`hairSimulation` GPU 中位时间 1.832 ms，关闭 trace 的
  基线为 1.837 ms，未测到求解开销。
- 验证器：带 trace 的 capture profile 退出码 0；acceptance 仍因快速甩头、穿透/抖动、
  逐帧 dt/dispatch、VSP A/B 与四档 rate 等矩阵缺口退出 2，不能把本次 trace 当作
  完整物理验收。无 trace 的 baseline acceptance 同样失败（退出码 3，含
  `run[0].trace` 缺口）。
- 段长比是诊断值，不是几何正确性结论：当前正式资产根部 rest 段只有约
  0.15 cm，trace 实测 p99/max 明显高于 1；seek 触发的 reset 帧上
  `prevPrev` 段长比精确为 1.0，说明 rest 数据与三份历史拷贝本身对齐，
  高比值来自当前求解/资产根部采样，需要后续单独归因，不能拿本 trace 宣称
  “段长已验收”。

限制：

- trace 是只读 copy，不写任何模拟 SSBO、不改求解顺序、不替换参数。
- 只记录第一个 GPU hair 对象和最多 256 根选中 guide；未选中的 4 万根 guide
  不在数值证据范围内。
- 最后一帧不是同步回读：目标帧数达到后，最后 1–2 个 slot 要等下一次复用或
  退出时的 queue idle 才落盘；若进程被强制终止且未走到 `FinishTrace`，这几帧
  可能缺失。
- 选中对象在 World/graph 重建后消失时 trace 停止并只写出已录制帧；跨重建的
  长时间 trace 不支持。
- 不替代逐帧 dt/dispatch 遥测、穿透深度或 VSP A/B；这些仍是未完成项。

### 5.2 CPU 参考导出最小方案

若只需要验证 VSP/单位/段长/胶囊公式，可先不碰 GPU：

- 新增 `tool/hair/hair_reference_export.cpp`（或扩展现有 `tool/hair/main.cpp`），
  链接现有 `GuideSimulation` / `tfxLoader` / `StrandGeometry`；
- 输入：`.tfx` 的 rest positions、inverse mass、SM 的 simulation 参数、
  固定 dt 序列、VSP/胶囊 A/B 配置；
- 输出：JSONL/CSV，每行 frame、timeStep、point index、current/previous/prevPrev、
  rest length、胶囊距离；
- 不启动引擎、不新增 runtime flag、不注册 CTest；只证明 CPU 公式和单位，
  不冒充 GPU dispatch 结果。

## 6. 与测试契约的一致性

- 不新增 runtime test flag，不注册 CTest；采集仍是
  `tool/hair/capture_hair_physics.py` 的外部生产 Console 路径。
- 本文自身不改契约；`documents/plan/rendering/tressfx5-hair-port-plan.md` 的状态表
  按本轮结果更新（plan 层只是计划，不是契约）。契约里的验证入口、串行 Vulkan 会话、
  runtime gate 与 GTest 分工保持不变，见 [测试契约](../contracts/testing.md)。
- `--frame-limit` 是普通启动参数，不是 runtime test；`perf json`/`gpustats`/
  `screenshot`/`animation`/`camera`/`mesh`/`hair trace` 仍是生产 Console 命令。
- `mesh` 不是测试夹具开关：它只写活动 World 的 mesh 根 TRS，不写资产、不注册
  CTest、不改变启动参数语义；没有命令时不会每帧自动运行（`mesh sweep` 也只在
  显式命令后的有限时长内推进）。
- `hair trace` 同样是普通 Console 诊断命令，不是 runtime test flag，也不注册
  CTest：它只读拷贝 device-local 位置 buffer，不改求解，未激活时零开销。
- 图片仍只作可见性/取景辅助；有限值、段长、穿透、速度必须由 GTest 或数值
  导出证明，不能以 `completed=true` 或截图替代。§5.1 的 trace 只提供选中 guide
  子集的 GPU 读回，仍不是全量 guide 或穿透深度验收。
- 现有 `hair_asset_tests`（本轮最终重跑为 **243 项通过**，2026-10-24；
  含原生 substep/reset 计划 9 项、4a 曲线/导入 12 项、trace 格式/往返 5 项）继续
  负责纯数学/状态/参数布局；本记录不把它当作 GPU 逐点验收。
- 文档改动后运行 `py -3 tool\validate-docs.py`；本轮工具脚本用
  `py -3 -m py_compile` 验证，并实际运行 Release `main.exe` 完成 120 帧
  `hair trace` 短采集与 `verify_hair_physics.py --profile capture`（退出码 0）。
