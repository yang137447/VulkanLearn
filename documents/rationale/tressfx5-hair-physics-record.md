# TressFX 5 发丝物理交互执行记录

状态：2026-10-24 执行记录与验收清单，不是实现契约。用户确认先关注物理交互，
渲染暂缓；主线程规划管理，子任务分别实施复位、审计空间/时间与外部验收。
当前实现事实以 [TressFX 导入契约](../contracts/tressfx-asset-import.md) 为准。

## 后续对照方式决策（2026-10-24）

用户随后明确 UE 侧主要看代码与物理分析，不准备、构建或启动 UE，
不采集 UE 截图或运行轨迹。后续主要证据为固定 TressFX revision 的 host/shader 公式，
以及单位、仿真空间、位置历史和实际调度的源码审计，配合纯 CPU oracle 与独立数值分析。
VL 场景、实际运行和数值验证保留，不依赖 UE 可运行工程；以同输入源码语义与可测物理
响应评估接近程度，不宣称 UE 实测一致。Groom 仅作能力参考，缺源码的实现保持未核实。
共同 Actor 世界历史、源步长/循环、VSP 单位与作者曲线仍为优先差距，
详见 [物理效果对照决策](../plan/rendering/tressfx5-hair-port-plan.md#ue5-物理效果对照决策2026-10-24)。
下文既有采集与测试结果按实际范围保留，不因后续路线变化改写。

## 证据口径

- GTest 验证纯数学、历史/时钟状态与契约；不冒充 GPU dispatch 输出逐点读回。
- 外部工具使用生产启动、Console animation/camera/screenshot、perf JSONL 与事件日志，
  不新增 runtime test 标志、不注册 CTest。完整 BMP 落盘后才读取 PNG。
- 动画 pause/seek 只冻结骨骼；发丝仍受风不能被当作游戏 Pause 失效。
- 动作、get、screenshot 通过同一次注入依次提交；RT 调度仍有延迟，early 图片不保证精确第一帧。
- 图片不能证明 finite positions、段长上界、零穿透或十分钟无 NaN；这些没有数据时明确保留。
- 30/60/120 是启动帧率上限，实际 CPU/GPU 时间单列；变量一步 `maxDt=0.016`
  不保证各帧率下等物理时间，不能把三档烟测称为帧率无关性验证。
- 所有 Vulkan 会话串行，结束自有进程；强制结束不构成正常 teardown 证据。
- 不改共享 config、正式 SM 或原作者资源；记录其 SHA256 及实际参数。

## 本轮清单

| 项目 | 实施/验证目标 | 当前结果 |
| --- | --- | --- |
| 复位 | current/previous/prevPrev 同步到当前蒙皮 rest、清旧速度；覆盖首次姿态/seek/clip | 已实现，10 项状态/参数测试通过；三 buffer 赋值尚无 GPU 逐点读回 |
| 空间 | source→hair relative model；根点/胶囊/代理共用一致空间 | 已实现，3 项相对空间/冻结身份测试通过 |
| 时间 | 冻结游戏步长；暂停/time-scale 与动画暂停分开 | 已实现，3 项帧时钟测试通过；实际 ScriptRuntime 时钟由对应套件验证 |
| 基线 | 原 Release 60 Hz 连续 Idle/Walk、seek/clip/接触多视角 | 首轮已采集；Walk 离开肖像机位，视觉项不算通过，见下文 |
| 最终短烟测 | 新实现 30/60/120 Hz，实际 GPU/SDF 日志、PNG、perf 与复位事件 | 修正 +Z 机位后三档已完成，27 张图人工复核可见角色/完整头发；原失败证据保留 |
| 长烟测 | 可复用 `--duration 600 --rates 60`；只声称实际日志/图像/进程存活范围 | 本轮未执行 |
| 专门快速根运动 | 快速转头与 placement 传送独立夹具 | 未做，不以 Idle/Walk 代替 |
| 碰撞响应 | 摩擦、碰撞速度、胶囊/SDF 重叠冲突 | 保持源位置投影语义；先取接触缺陷证据，不因源 previous 写回已注释而擅改 solver |
| 多实例 | 每个 placement 独立状态与世界空间动力学 | 未纳入本轮完成声明 |

## 工具

`tool/hair/capture_hair_physics.py` 是外部烟测入口。默认新输出目录、30/60/120 Hz、
连续 Walk 4 秒，先加载正式 Idle，依次采集 Walk、动画 pause、seek 0.25/2.75、
clip 切回 Idle、侧面与背面接触图。报告保留实际播放状态、shader dispatch/SDF 日志、
事件式 reset 日志、GPU hairSimulation 时间、输入/二进制 SHA256 与进程清理结果。
普通 mesh Walk 保留原根位移，所以连续 Walk/seek 2.75 使用沿行程观察的机位；
seek 0.25/2.75 肖像各跟随实际根位移，切回 Idle 后恢复原肖像，
侧/背接触另设完整头顶取景。
`--startup-delay 5 --startup-only --rates 60` 可只采集首次 Idle，供 Debug 验证层短检查；
此时 `--require-reset` 只要求 first-pose，完整序列另外要求至少四个不连续事件。
`completed` 只表示采集完成，`visualReviewRequired` 明确要求人工复核。

```powershell
& 'C:\Program Files\Python314\python.exe' tool/hair/capture_hair_physics.py `
    --output artifacts/hair-physics-final --build-directory build-rel `
    --rates 30 60 120 --require-reset
```

长烟测加 `--duration 600 --rates 60`；它仍不是数值稳定性的完整验收。
`artifacts/` 是 gitignored；本记录在采集完成后登记结论，失败证据与局限不删除。

## 本地输入核对

正式 `SM_female_hair_skinned.json` 的本次实际风为 `[2000,0,0]`；
旧契约末段的 `[200000,0,0]` 与该作者资产不符。测试读取资产原值并记 SHA256，
不为对齐文档修改作者资产。GPU guide 为 40,334 根、每根 32 点，
胶囊为头/颈/肩/上背四个，SDF 代理为 7,391 三角形。

## 首轮基线

`artifacts/hair-physics-baseline-20261024/`，旧 `build-rel/bin/main.exe`：
60 Hz 一轮、9 张 1280×720 完整截图；stderr 0 B、VUID 0；
dispatch 为 40,334 guides / 1,290,688 points / SDF objects 1。
40 行 perf 中 39 行 GPU 有效，hairSimulation 中位 1.545 ms，CPU 中位 16.683 ms。
config/正式 SC/正式 SM 的 SHA256 未变，采集自有进程已终止，无残留 main。

图像复核发现连续 Walk 与 seek 2.75 时角色已离开肖像画面；这些图仅有背景，
不能作为 Walk 交互效果通过。seek 0.25 early 可见横向拉片，later 发丝收回，
形成旧复位缺口的图片证据；未量化段长或穿透。侧/背相机首轮过近裁头顶，
最终脚本调整为沿行程观察，再逐图确认角色可见；首轮失败图保留。

## 模块结果

2026-10-24 `hair_asset_tests`：177 tests / 16 suites，failures 0、errors 0，
结果为 `artifacts/hair-reset-tests.xml`。其中新增 10 项
`TressfxSimulationHistory` 用例检查首次姿态、延迟首姿态、静态初始化、连续/暂停、
seek 一次消费、播放器更换、缺失恢复与当时的 224B 参数（后续扩到 288B）；6 项
`TressfxFrameContext` / `TressfxFrameClock` 用例检查不同 placement 的根点世界坐标、
根点/胶囊/代理共用完整相对 palette、legacy pose 冻结值、重复快照、暂停恢复及换 World。
这些用例不调用 Vulkan，不读取三份 GPU 位置；Reset 分支的三 buffer 赋值还需结合源码、
新 shader 编译与实际事件/截图审阅。

`vl_renderer_backend_tests` 104 项 / 13 suites 通过，日志为
`artifacts/tressfx-physics-motion-backend-tests.log`；编辑器快照过滤 10 项 / 2 suites
通过，日志为 `artifacts/tressfx-physics-motion-snapshot-tests.log`。
这两个结果用于冻结快照/发布链回归，不代替发丝 GPU 位置数值验收。

ScriptRuntime 时钟/快照过滤 16 项 / 3 suites 通过，日志与 XML 为
`artifacts/tressfx-physics-motion-script-tests.log` / `.xml`；
其中新增两例直接检查普通动画与 scaled 游戏步长分别冻结、游戏 Pause 保持普通动画时钟。
这不是全量 `script_tests` 执行结果。

Debug/C# ON `cmake --build build -j 8` 全目标构建成功，exit 0；
日志为 `artifacts/tressfx-physics-motion-build.log`。
Release/C# ON `cmake --build build-rel --target main -j 8` 也已构建成功，exit 0。
Debug 首轮启动日志 `artifacts/tressfx-physics-gates-20261024/shader-reload.stdout.log`
显示 `hair/tressfxSimIntegration.comp` 等 9 个 guide shader 参与 cache miss 后重编译，
总计 `compiled=28, shaderc=39, failed=0`；这确认本轮 shader 可编译，不是 GPU 数值读回。

## 既有 Vulkan 门禁

证据为 `artifacts/tressfx-physics-gates-20261024/` 的 stdout/stderr/result JSON；
各进程串行并走正常 teardown：

| 入口 | 实际结果 |
| --- | --- |
| `--shader-reload-test` 默认 UI | exit 0，stderr 空，无 VUID |
| `--shader-compute-reload-test` 默认 UI | exit 0，无 VUID；stderr 包含故意把 `skySHGenerate` local_size 16 改 8 后的 ABI 拒绝 |
| `--world-graph-transaction-test` 默认 UI | exit 2；high-light transaction 的 `Scene.DraftSourceChanged` traversal 失败，无 VUID |
| `--world-graph-transaction-test --no-dev-ui` | 额外隔离诊断 exit 0，stderr 空，无 VUID；不覆盖默认 UI 的失败 |

默认 UI World 失败与 [测试契约](../contracts/testing.md) 2026-10-18 已记录的
`Generated/Validation` 绝对路径遍历问题相同，本批未扩范围修编辑器资产路径。
首轮另有 `--shader-reload-test --no-dev-ui` exit 0；不能将这些结果简化成“三门禁全部通过”。

## 新实现首轮采集与取景失败

`artifacts/hair-physics-final-20261024/` 完成 30/60/120 三档，各 9 张完整图，
config/SC/SM SHA256 均未变；每档 first-pose 1 次、pose-discontinuity 5 次，
stderr 空，实际 guide/SDF 路径和 GPU perf 均有记录。Release 没有启用验证层，
日志 VUID 0 不作为开启验证层的证明。

| 帧率上限 | perf 行 / GPU 有效 | CPU 中位 ms | hairSimulation GPU 中位 ms |
| --- | --- | --- | --- |
| 30 | 30 / 29 | 33.348 | 1.517 |
| 60 | 29 / 28 | 16.755 | 1.510 |
| 120 | 28 / 27 | 8.346 | 1.560 |

人工复核发现该目录的 walk-continuous、walk-paused、seek275 仍仅背景，
因此 Walk 视觉验收失败；`completed=true` 不改变这一结论。seek025 early 和 clip-idle
可见复位到当前 rest，未出现旧基线的横向拉片，但图片不保证精确复位第一帧。
侧/背图包含完整头顶与发梢。seek025 later 仍可见头发落到脸前，
不能据此声称零穿脸/零穿透；碰撞数值质量仍未验收。

随后通过现有 `vl_animation_probe --fbx` 只读采样确认方向：
原 FBX root 在 0.25/2.75/4 s 为 `(0,-50.00045,0)` /
`(0,-550.00494,0)` / `(0,-800.00720,0)`，导入器 `(x,z,-y)` 映射为 VL **+Z**。
正式 retarget 的 root→FemaleRoot 复制位移差，不改方向或按身高缩放；
证据为 `artifacts/tressfx-physics-motion-walk-inspection.json`。
初轮误按 -Z 取景，角色实际走到了相机后方。补采使用连续 Walk
`(0,151,950)→(0,151,400)`，seek 0.25/2.75 肖像分别对准 +50/+550 cm；
不修改正式 scene/SM/config。

## Debug 发丝短检查

`artifacts/hair-physics-debug-20261024/` 使用当前 Debug `build/bin/main.exe`、
正式 GPU hair 场景、`--startup-delay 5 --startup-only --rates 60 --require-reset`。
first-pose 1 次、实际 SDF domain 日志、后续正常帧图与 3 行 perf（2 行 GPU 有效）
证明录制链越过初始 Reset 继续工作，hairSimulation GPU 中位 1.537 ms，
stderr 0 B、VUID 0。新 shader 启动编译 `compiled=28, failed=0`。
完整 Idle 图已人工复核。自有进程由采集器终止，不作为正常 teardown 证据，
也没有 GPU 位置/穿透深度读回。

## 修正机位补采结果

`artifacts/hair-physics-corrected-20261024/` 是本轮有效的取景补采，不能与上面的失败
`hair-physics-final-20261024/` 目录混称。Release 二进制 SHA256 为
`9141c8f6cc50b1f236300679f64ecffb2c5e6ae6066c4b6f0b87c4d5399e1231`。
三档各 9 张 1280×720 完整图，`contact-sheet.png` 均已人工逐图复核：
连续 Walk、动画 pause、两个 seek、切回 Idle、侧/背接触中的角色与完整头发可见。
连续 Walk 实际采样 time 约 1.69..1.72 s，不据此宣称全轨迹所有时刻可见。

各档 first-pose 1 次、pose-discontinuity 5 次，stderr 0 B；实际 guide/SDF 路径、
播放状态和 GPU perf 已记录。Release 不启用验证层，其 VUID 0 只作日志事实。
config/SC/SM SHA256 均保持原值，自有进程已终止，无残留 main。

| 帧率上限 | perf 行 / GPU 有效 | CPU 中位 ms | hairSimulation GPU 中位 ms |
| --- | --- | --- | --- |
| 30 | 30 / 29 | 33.432 | 1.528 |
| 60 | 28 / 27 | 16.705 | 1.510 |
| 120 | 28 / 27 | 8.365 | 1.555 |

两个 seek early 近景未出现旧基线的横向拉片；随后仍会受风，动画 pause 后发丝继续动
符合本场景普通 mesh 时钟语义。later 图片会看到头发落到脸前；不把图像烟测解释为
无穿透/无接触抖动。专门快速转头、placement 传送、十分钟长测、GPU 数值读回、
摩擦/速度响应、多实例与世界空间动力学仍未纳入完成声明。

## 第三批：世界历史、源调度与 VSP/碰撞核对（2026-10-24）

本批按用户指定顺序执行：① 世界空间历史与角色运动惯性；② 源步长、循环、阻尼与
实际物理时间；③ VSP 单位、刚度曲线与碰撞响应。主线程规划/审阅，三个子任务分别
负责世界历史、时间/参数、验证准备；UE 侧只读固定 revision 源码，不构建/启动/截图，
渲染继续延后。详细证据分别在
[世界历史记录](tressfx5-hair-world-history-record.md)、
[时间/参数记录](tressfx5-hair-timing-params-record.md)、
[验证记录](tressfx5-hair-verification-record.md)。

### 1. 世界空间历史与运动惯性

- 源 `ApplyVertexBoneSkinning` 在骨骼混合后乘 `PrimitiveData.LocalToWorld`
  （patch 8303..8308），Integration 再 `*=0.01`（8424..8425）；current/previous/
  prevPrev 三份都是**世界空间米**，`w` 只是 movable/inverse-mass 标记。SDF 代理
  同样世界化（9763..9766）。solver 不使用 `PreviousLocalToWorld`，也没有显式速度
  buffer；速度就是 `current - previous`。
- VL SSBO 是 hair 局部厘米。placement 变化时，Integration 用
  `H = inverse(currentModel) * lastModel` 把三份历史换基，根目标用
  `inverse(hairModel) * sourceModel * bindToPose`；`motion.z` 用于 dt=0 的只换基。
  推导表明：对刚体平移、绕任意 pivot 旋转及组合，世界位置、世界速度以及
  gravity/wind/global shape 的点级结果与源世界 solver 等价；非均匀缩放/剪切只
  保证点换基，rest length、胶囊半径、SDF cell/margin 等局部度量不等价。
- 审计未发现功能性缺陷：Commit 在 Integration 后且先于 Reset/rebase 的 continue；
  rebaseOnly 不跑碰撞/SDF，恢复首帧先重建 collider/SDF；丢 pose 时缓存矩阵与位置
  用同一个 `H`；首帧不把 actor 初始 placement 误当移动。边界（source/hair model
  不一致、同 identity 不同 model、CPU backend 无历史）已写入世界历史记录 §4/§6。
- 新增 5 项测试：首帧、0dt 换基、pivot 旋转、快速平移的根/梢惯性，以及 90 帧
  “世界 Verlet oracle vs 局部+rebase”逐帧对照（位置容差 2e-2 cm、速度 5e-2 cm）。
- **未覆盖**：正式 Female 场景的 hair/source model 都静止，运行时不会触发换基；
  当前没有 placement 平移/转身/teleport 夹具，换基代码只有模块 oracle 证据。
- 附带发现并修正：源 reset 帧的 host 仍会录制 VSP/LocalShape/LengthWind/SDF
  （`TressFXCoreSimulation.cpp:1137-1214`），之后才清 reset 标志；VL 的 reset 帧
  只录制 Integration，所以 reset 帧最终姿态与源不同（VL 停在纯 rest）。三份历史
  的写入本身仍等价。

### 2. 源步长、循环、阻尼与实际物理时间

闭式：`n(Δ)=0`（Δ≤0）、`1`（0<Δ≤0.0167）、`2`（Δ>0.0167，硬上限 2）；
每个 substep 的 dt 恒为 **0.0125 s**，`0.0167` 只用于循环扣除；每帧物理时间
`n*0.0125`。`Guides.Frame` 每渲染帧 +1，不随 substep 增加；源的每次
`SetWind` 还乘 `(0.3+FRand())`，不可复现。

| 实测帧率中位 | 对应 Δ | 源 n | 源每帧物理时间 | 源倍率 | VL 当前 dt | VL 倍率 |
| --- | --- | --- | --- | --- | --- | --- |
| 29.984 | 0.03335 | 2 | 0.025 | 0.75× | 0.016 | 0.48× |
| 59.618 | 0.01677 | 2 | 0.025 | 1.49× | 0.016 | 0.95× |
| 119.035 | 0.00840 | 1 | 0.0125 | 1.49× | 0.0084 | 1.00× |

60 Hz 的 1/60 严格小于 0.0167，所以 60.0 Hz 是 1 步；实测 59.6 Hz 已跨到 2 步。
源的物理时间倍率因此非单调（48 Hz 为 1.2×、60 Hz 为 0.75×），且 60 Hz 附近会
在 0.75×/1.5× 之间跳变。阻尼 `exp(-D*0.0125*60)` 每步 0.974092，源每秒保留
0.207（60/30 Hz，60 步/秒）或 0.0429（120 Hz，120 步/秒）；VL 60 Hz 每秒约
0.133，指数 2.016 对源 1.575。

本轮**没有改运行时调度**：源路径要求固定 0.0125、按 Δ 决定 1/2 次循环，改动会
同时影响 pass 录制次数、物理时间、阻尼、VSP 触发和既有资产校准，必须单独排期。
当时仍是 `dt = clamp(frameDelta, 0, 0.016)`、一帧一步；`tressfxSourceSchedule.h`
与 17 项测试只固定推导，不参与运行时。
（2026-10-24 后续子任务已实施原生调度：GPU/CPU 均按 n=0/1/2 × 0.0125 s，
`maxDt` 只保留给旧 `Step`，详见
[时间/参数记录](tressfx5-hair-timing-params-record.md) §6。）

### 3. VSP 单位、刚度曲线与碰撞响应

- VSP 阈值默认已从 5000 改为 **140 cm**，即源默认 1.4 world m 的同量纲换算；
  `length(new-2*old+oldOld)` 未除 dt²，是长度而非加速度。140 cm 的等效加速度在
  12.5 ms 步长下 896000 cm/s²、16 ms 下 546875；因此同一阈值在不同 step 下
  触发难度不同。正式 Female 资产 `vspCoefficient=0`，本轮运行没有 VSP dispatch。
- 源 VSP 与 LocalShape 都是逐顶点作者曲线，按 `i/N` 采样（梢部只到 `(N-1)/N`），
  LocalShape 再 clamp 到 0.95；VL 仍是 VSP 线性近似 + 整条统一 `bendStiffness`。
  这是资产/schema/UBO 级改动，未在本轮迁移。
- 碰撞：源只有 SDF 一条路径，只写 current（previous 写回在 patch 8051 已注释），
  无摩擦/碰撞速度/多次迭代；胶囊是 VL 新增，顺序为胶囊→SDF clear/splat/collide，
  SDF 可把点推回胶囊且没有第二次投影。两边都只写 current、都无摩擦，不能把
  “新增摩擦”说成恢复源行为。

### 验证

- `hair_asset_tests`：**217 tests / 22 suites 全部通过**，XML 见
  `artifacts/hair-source-align-tests.xml`（Debug）。
- Release 30/60/120 Hz 外部采集：`artifacts/hair-physics-source-align-20261024/`，
  三档各 9 张完整图（共 27），每档 first-pose 1 次 + pose-discontinuity 5 次
  （共 18 个 reset 事件），stderr 0 B；perf 行 31/28/28，GPU 有效 30/27/27，
  hairSimulation GPU 中位 1.669/1.616/1.637 ms，CPU 中位
  33.351/16.774/8.401 ms。`verify_hair_physics.py --profile capture` 0 失败，
  `accepted=false`（未做数值读回）。人工复核 contact sheet：角色与完整头发可见。
- Debug 60 Hz `--startup-only` 短检查：`artifacts/hair-physics-source-align-debug-20261024/`，
  shader `compiled=28, failed=0`，first-pose reset 1 次、SDF domain 日志存在，
  stderr 0 B、VUID 0（validation 开启）。
- `py -3 tool\validate-docs.py`：`documents layering OK`。
- 契约同步修正：VSP 默认 140、UBO 288 bytes/offset 224、world-history 与
  per-asset/多 placement 边界、`hair_asset_tests` 当时 217 项（第四批最终 243）。

### 仍未完成

- 源调度的运行时对齐（固定 0.0125 / 1–2 次循环）未实施；先要决定“忠实源”还是
  “固定步长 accumulator”，并重标定阻尼/VSP/长度约束。
- 无 placement 平移/转身/teleport 与快速转头夹具；无 GPU 逐点读回，因此根点滞后、
  发梢轨迹、段长、穿透、有限值仍不能验收。
- VSP/LocalShape 作者曲线、多骨 `.tfxbone`、摩擦/碰撞速度、多实例独立状态未实现。
- 本批只改测试、纯函数头、工具与文档；运行时 solver/shader 仅修了
  `hairSimulationPass.cpp` 的命名空间编译错误，没有改物理行为。

## 第四批：原生调度与 placement 夹具（2026-10-24）

用户决策：运行时采用**源原生行为**（不是固定步长 accumulator）；补齐
placement 运行时夹具；同意一次性 GPU hair trace 读回；其余 VSP/LocalShape 曲线、
多骨 `.tfxbone`、源碰撞语义与多实例需要继续对齐。本批先落地前两项。

### 1. 原生调度（已实施）

`HairSimulationPass` 与 `GuideSimulation::StepFrame` 共用
`TressfxSourceScheduleForDelta`：

```text
n = 0 (delta <= 0), 1 (0 < delta <= 0.0167), 2 (delta > 0.0167)
for substep in 0..n-1: solver step 0.0125 s
simulationFrame += 1                      // 每渲染帧一次
```

- `n == 0`：不录 pass，不消费 ResetPositions/pose/时钟/历史；reset 留到下一个
  正 delta 帧。
- Reset：首 substep Integration 用独立 resetParams（`motion.y=1`、dt=0），
  同轮后续 StrandLevel/VSP/LocalShape/LengthWind/胶囊/SdfCollide 继续；n=2 时
  第二轮正常。reset 日志增加 `substeps=N`。
- placement 换基：非 reset 帧的首 substep Integration 用独立参数块携带
  `historyToCurrentLocal`；后续 substep 用单位矩阵。`motion.z` 宿主路径删除。
- SDF：SdfClear/SdfSplat 每帧一次（循环前），SdfCollide 每 substep 一次。
- `maxDt` 仅旧 `Step(dt)` 使用；GPU 原生路径与 `StepFrame` 不再读它。
- 仍与源不同：源每 substep 的 `SetWind` 有 `(0.3+FRand())` 随机幅值，VL 确定性；
  n==0 帧 VL 不递增 `Guide SimulationFrame`，源会无条件 `++Guides.Frame`（首帧
  相位固定差 1）。

测试：243 tests / 23 suites 全通过（含 4a 的 12 项曲线与阶段 3 的 5 项 trace；
本批新增 9 项覆盖 `PlanGuideSubsteps`、`StepFrame`
固定步长/风帧、零 delta 延迟 reset。Release 30/60/120 Hz 采集
`artifacts/hair-native-schedule-final-20261024/`：27 图、18 reset（每档
first-pose 1 + pose-discontinuity 5）、stderr 0、VUID 0；
hair GPU 中位 2.945/2.042/1.864 ms（30 Hz n=2，较旧 1.669 ms 明显上升；
60 Hz 实测 59.6 Hz 也跨到 n=2）。Debug 60 Hz 启动 `compiled=28, failed=0`、
first-pose reset 1、VUID 0、stderr 0。

### 2. placement 夹具（已实施）

正常 Console 命令（不是 runtime test flag）：

```text
mesh transform <identity> <px> <py> <pz> [rx ry rz]   // 绝对 TRS，cm/度，scale=1
mesh offset <identity> <dx> <dy> <dz>                 // 当前根位置叠加
mesh sweep <identityA> <identityB> <dx> <dy> <dz> <seconds>  // GT 每帧线性推进
```

`RuntimeCommandExecutor` 在 GT 每帧按 `steady_clock` 推进 sweep，复用
`World::SetMeshSceneObjectTransform`，不依赖 sceneDocuments；`loadworld` 会取消
sweep。采集脚本新增默认关闭的 `--placement-probes`；窗口内先 `animation pause`
冻结根运动，再同时平移/绕 Y 旋转/teleport/sweep 两个 identity。
`artifacts/hair-placement-final-20261024/`：7 图、命令 0 拒绝、placement 窗口
`pose-discontinuity=0`（整次仅 first-pose 1）、stderr 0、VUID 0；图像可见发丝相对
头部滞后/拖尾。

**新发现**：该 Female Idle 是带根运动的循环 clip；不冻结姿态时，每约 10 s 的
loop wrap 会自己递增 `SkinPose::discontinuity` 并触发 `pose-discontinuity` reset
（实测 551 帧 @60 Hz）。所以 placement 窗口必须冻结动画，才能把 reset 唯一归因
到 placement。这也意味着“播放中的 placement”会被周期 reset 掩盖，二者的
证据必须分开记账；是否应把“原地循环的 loop wrap”从 discontinuity 中排除，
留待源动画/复位语义对齐时决定。

### 3. GPU hair trace（已实施）

正常 Console 命令 `hair trace <path> <frames> <guide-stride>`：只读第一个 GPU
hair 对象，选中 `0,stride,...`（上限 256 根），每个 frame-in-flight 槽一份
host-visible readback buffer；所有 dispatch 后 compute→transfer barrier，逐
guide copy current/previous/prevPrev，再 transfer→compute barrier；下一次同槽
`Record` 开头（BeginFrame 已 wait fence）map 写盘。格式为 `VLHT` v1：header
含 guide/points/stride/选中索引/rest length，每帧含 frame/world/substeps/
timeStep/resetReason/dispatchCount + guide→point→三份 float4。

最终 Release 60 Hz 证据 `artifacts/hair-final-trace-20261024/`：

- 120 帧 / 40 guides / 32 点，7,381,952 B；finiteCount=0，根/梢非有限=0。
- 段长比 p50=1.046、p99=3.009、max=9.248；根点逐帧最大位移 0.071 cm，
  发梢 3.224 cm。
- `substeps`：1 步 103 帧、2 步 17 帧（实测 58.4 Hz，部分帧跨过 0.0167）；
  dispatch 8（n=1）/14（n=2，含 SdfClear/Splat 与每 substep 的 SdfCollide）。
- stderr 0、VUID 0；trace 开关时 dispatch 与关闭时一致；hair GPU 中位
  1.635 ms（trace 窗口），未测到求解开销。`verify_hair_physics.py` 的
  acceptance 仍因快速甩头/穿透/抖动/逐帧 dt/VSP A/B/四档 rate 等缺口退出 2，
  不宣称完整物理验收。

限制：只读子集；最后 1–2 个 frame slot 要等下次复用或 Shutdown 的
`FinishTrace(true)`/WaitIdle 才落盘；世界重建会停止 trace。

### 4. VSP/LocalShape 作者曲线（已实施）

- `TressfxSimulationConfig` 增加可选 `vspStiffness` / `localShapeStiffness`
  逐顶点数组；schema 接受 1..64 项、每项 [0,1]；长度必须等于该 `.tfx` 的
  `pointsPerStrand`，否则导入期硬错误。LocalShape >0.95 在加载期 clamp 并记录。
- UBO 追加两个 `vec4[16]`（各 256 B）：vsp offset 288、localShape 544，
  `sizeof(GuideSimulationParams)==800`；`motion.w` bit0/bit1 表示数组存在。
- VSP 的逐顶点乘法按源语义放在 VSP pass（`VspCoeff < 0.9` 才乘）；
  LocalShape 每点用数组值（数组存在时标量为 0 仍运行），仍乘 0.5；CPU
  `SimulationConfig`/`VspPass`/`LocalShapePass` 同步，`renderableObject`
  构造时把 section 曲线传给 CPU solver。
- 正式 Female 资产没有曲线数组，继续走 VSP 线性 / LocalShape 单标量回退；
  真实 Maya 曲线数据尚未导出，当前只对齐数据路径与数学。

### 5. 契约与计数同步

`tressfx-asset-import.md` 已改为 n=0/1/2 × 0.0125 s、reset 首轮继续、SDF
clear/splat 每帧一次而 collide 每 substep 一次、maxDt 仅兼容旧 Step、reset 日志
带 `substeps`、UBO 800 B/曲线字段；`testing.md` 的 hair 计数改为 **244**，
`tressfx-asset-import.md` / `testing.md` 的 VSP 默认 140/旧 5000 冲突已修。

### 6. 多实例独立状态（已实施）

GPU 发丝 section 的 renderable key 现在追加场景实例身份
（`source/render/resource/renderableObjectKey.h`）：同一 `SM_*` 的多个
placement 各建一个 `RenderableObject`，因此 current/previous/prevPrev、
params UBO、胶囊、SDF 代理、history/clock 全部按 placement 隔离；CPU 导入几何
仍共享，GPU 几何 buffer 各一份。加载期的“多 placement 硬拒绝”已删除，
`TressfxSimulationOwner` 只作为运行期安全网。

用临时双发丝场景（两个 `Female_hair_skinned` 使用同一 SM，第二个实例名
`Female_hair_skinned_b`，模型空间与源一致；采集后已删除临时场景）做 Release
烟测：`GPU guide simulation dispatch: objects=2 guides=80668 points=2581376
passes=9 sdfObjects=2`，两个 SDF collision buffer 的 identity 分别为
`...|instance=Female_hair_skinned` 与 `...|instance=Female_hair_skinned_b`，
`mesh offset Female_hair_skinned_b 150 0 0` 被接受并截图，stderr 0。证据在
`artifacts/hair-multi-instance-smoke-20261024/`。

源碰撞语义仍保持“只写 current、无摩擦/碰撞速度”的 SDF-only 路径；胶囊是 VL
新增且只投影位置。测试 `TressfxCapsuleCollision.ProjectionDoesNotRewriteVelocityHistory`
固定该行为。

### 7. 多骨 .tfxbone（CPU 数据路径 + GPU 接入）

新增 `source/hair/tfxBoneLoader.{h,cpp}`，按源 `TressFXAsset.cpp:417-501` 读取
`.tfxbone`：骨名表 + 每 strand 的 `(boneIndex, weight)`。`ParseTfxBone` 校验
截断/骨数/名字长度/影响数/索引/权重；`MapTfxBoneToSkeleton` 把骨名映射到目标
骨架、跳过 `-1`/零权重（源行为）、记录未知骨、按 joint 合并并归一化权重。
缺失 strand 返回空权重，调用方回退根骨/rest。

资产字段 `simulation.skinning.boneWeights` 由 importer 读入，加载期映射后进入
`TressfxSkinningBinding::strandBones`；`RenderableObject` 上传每 strand 4 个
`(joint,weight)` 与每槽全骨架 palette，Integration/StrandLevel 用
`params.motion.w` bit2 选择加权 `InitialPos`/`SkinningQuat`。正式 Female 没有
`.tfxbone`，仍走单根骨。临时 75/25 Head/Neck 与 100% Chest 场景（采集后删除）
验证了加载、dispatch、无 VUID 与“根点确实被加权矩阵移动”；60 帧 trace
finite=151680/bad=0。模块测试 254 项。记录见
[多骨数据路径记录](tressfx5-hair-multibone-record.md)。

### 8. placement 数值惯性 + 穿透 trace（v3）

`hairTraceFormat` 升到 v3：每帧增加 `hairModel`（列主序 mat4）与碰撞前
SDF/胶囊最大穿透统计；新增 `tool/hair/analyze_placement_trace.py` 对 sweep/control
两段 trace 比较根点/发梢。2026-10-24 用 `animation pause` 固定姿态、
`mesh sweep FemaleCharacter Female_hair_skinned 200 0 0 2`、600 帧 trace
（40 guides）得到：

- 无 reset；
- 根点最大差 **0.0205 cm**（根点严格跟随源骨骼/placement）；
- 发梢最大差 **36.89 cm**、均值 **9.83 cm**、最后 30 帧均值 **7.57 cm**；
- 碰撞前最大穿透：SDF **4.40 cm**、胶囊 **3.30 cm**（同一 trace；
  正式 120 帧无 sweep 时为 2.35/3.29 cm）。

未暂停的 600 帧版本在第 479 帧出现 `reason=4`（pose-discontinuity，loop-wrap），
之后发梢差被重置拉回约 0.4 cm；这说明 loop-wrap reset 会把 placement 惯性证据
截断，也确认它仍是待决策项。证据在
`artifacts/hair-placement-trace-20261024/`（sweep/control 600 v3、summary JSON）。

### 9. VSP/LocalShape 运行时曲线烟测

用临时 SM 写入 32 项 `vspStiffness`（1→0）与 `localShapeStiffness`（0.95→0.45）、
`vspCoefficient=0.5`，并在同一场景跑 120 帧 trace；日志确认
`vspcurve=32`、`localshapecurve=32`、`vsp=0.5`，trace
`finiteCount=0`、segment max **9.75**、SDF/capsule 最大穿透
**1.98/3.29 cm**。临时 SM/场景已删除；正式 Female 仍为空数组回退。

### 下一步（本目标继续）

1. 做绕 Y 旋转与 teleport 的 placement trace，并把过冲/衰减归因到具体
   Verlet/历史换基项；平移已有数值结果，旋转/teleport 尚未做。
2. 用真实 `.tfxbone` 资产验收多骨路径；本地无该资产，当前只有合成夹具。
   CPU backend 的世界历史/独立 placement 状态仍缺。
3. 决定 loop-wrap reset：原地循环的 wrap 目前会被 `!inPlace` 规则当成
   pose discontinuity；需要与源动画/复位语义对齐后再改，避免用 placement
   夹具误读每 ~10 s 的周期 reset。
