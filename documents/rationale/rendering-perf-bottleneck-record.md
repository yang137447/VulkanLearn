# 渲染性能瓶颈与优化记录（2026-10-23）

状态：**证据归档**（rationale 层，不是契约）。记录本轮实测数字、已落地的优化、
以及尚未实施但已评估的方案。任何结论都能在文中对应的提交或 `artifacts/` 日志里复核。

## 1. 测量手段

- GPU：引擎内 timestamp 查询（`documents/contracts/gpu-pass-timing.md`），
  `--gpu-stats` 周期打表、console `gpustats`、`--perf-jsonl <path>` 逐秒 JSONL。
- CPU：进程 `TotalProcessorTime` 采样 + 帧时间（`perf json` 的 `cpuMs`）。
- 画面：固定帧 `screenshot` + 像素差；冻结姿态用 `animation seek` + `animation pause`。

## 2. 本轮定位到的三个问题与结果

| 问题 | 根因 | 修复 | 效果 |
| --- | --- | --- | --- |
| 动画 CPU 成本 ~12.4 ms/帧 | `BuildSkinPose` 每帧对所有顶点做 CPU LBS 只为求包围盒 | 预计算逐骨骼绑定空间 AABB，姿势时只变换 8 角点（`skeletalPlayback.cpp`） | 12.4 ms → ~0.5 ms；skinned 场景 54 → ~83 FPS |
| 4 级阴影各画一遍全部 caster | `DrawShadowScene` 没有逐级联过滤，draw list 原样复用 | `shadowCasterCull`：级联 light view-proj 阴影盒 vs 当前世界 AABB，加方向光 receiver-XY 预筛 | GPU 12.09 → 4.71 ms；79 → 187 FPS；只切剔除时冻结帧逐像素相同 |
| 每级联重扫全部 draw packet 收紧 Z 范围 | 投影只依赖光源朝向，四级完全相同 | 每帧缓存一次光源空间 AABB，逐级联只做 XY 过滤 + Z 合并 | 微秒级，当前规模测不出；移除随级联数增长的 N× 循环 |

另外一个已删除的历史坑：`IntersectsSplitFrustumFast`（相机 slice 剔除）曾被
`#if VULKANLEARN_SHADOW_TIGHT_Z_BOUNDS`（默认 0、全仓未定义）挡在编译外，
判据体积也不对（相机 slice ≠ 光源阴影盒），跨相机平面时还会退化为零宽。
`7cf2274` 修 CSM Z 界时删掉了调用，于是退化成"完全不剔除"。该函数与
`ComputeViewAabbFromWorldCorners` 已在 `b7f6d68` 删除。

## 3. 当前剩余预算（200 m 阴影距离、剔除开启、skinned 场景）

GPU 总计约 4.7–5.2 ms：

- `shadowCascade1` 2.3–2.4 ms（角色蒙皮绘制；c0/c2/c3 已降到 ~0.006 ms）
- `geometry` 2.2–2.6 ms（同一次角色绘制）
- 其余所有 pass 合计 < 0.1 ms

CPU 帧时间波动很大：空载 ~6.5 ms（约 153 FPS），有其他进程时 ~11.8 ms（约 85 FPS）。
**多数时候 CPU 先到瓶颈**，此时削减 GPU 时间不会提高帧率。

## 4. 已评估、未实施的方案：每帧一次预蒙皮（pre-skinning）

现状：同一批 23.5 万顶点在一帧内被蒙皮两次——`geometry` 与 `shadowCascade1`
各跑一遍骨骼混合的顶点着色器。判据：同一网格静态版每级阴影 0.05 ms，蒙皮版
2.4 ms，差 40 倍来自 vertex fetch + 混合，而不是光栅化。

做法：加一个 compute pass，每帧把顶点蒙皮一次写入世界空间缓冲（位置，必要时
法线/切线），`geometry` 与 shadow pass 直接读该缓冲。

- 收益：一次蒙皮替代两次，预期省 ~2 ms GPU；角色跨多个级联时不再随级联数线性增长；
  同一结果可复用于运动矢量与逐帧精确包围盒。
- 代价：显存 3–12 MB；需要"已蒙皮"顶点布局与对应 pipeline 变体（阴影只需位置）；
  compute→vertex barrier 与帧内顺序；若将来接入 blend shape / SpeedTree 风，
  必须在同一 pass 内计算，否则出现"预蒙皮没算风、几何里算了"的不一致。
- 结论：**只有在 GPU 受限时才有意义**；当前多数帧是 CPU 受限，优先级低于定位 CPU 瓶颈。

## 5. 下一步（写入时的计划；第 1、2 项已由 §6 完成）

1. 增加 CPU 分阶段计时（与 GPU pass 表对称）：输入/脚本、动画、头发 GT 模拟、
   世界快照发布、UI 视图模型、渲染录制与提交、present/等待。——已落地，见 §6。
2. 用该表定位 CPU 瓶颈，再决定优化项（预蒙皮、提交合并、UI 绑定频率等）。——已定位，见 §6。
3. 若之后要做 200 m 大世界演示，还需要把相机 `far_clip`（当前 1000–2000 cm）
   与阴影距离对齐，否则级联 2/3 覆盖范围内没有可见接收者。

## 6. CPU 分阶段计时实测（2026-10-23）

实现见 `documents/contracts/gpu-pass-timing.md` 的 CPU 阶段一节。测量方式：Release
`build-rel/bin/main.exe`，1280×720，`--perf-jsonl` 运行约 20 秒，取最后 15 行求中位数；
CPU 表对账用**逐帧 sum 的中位数**（各行中位数之和不可加，且 `other` 单帧钳制会丢掉超出的部分）。

### 场景与阶段表（中位数，ms）

| 阶段 | skinned 不限速 | skinned `--frame-limit 30` | hair 不限速 | hair `--frame-limit 30` |
| --- | ---: | ---: | ---: | ---: |
| `input` | 0.028 | 0.036 | 0.031 | 0.031 |
| `scripts` | 0.008 | 0.010 | 0.009 | 0.009 |
| `animation` | 0.089 | 0.089 | 0.085 | 0.092 |
| `hairSimulation` | 0.001 | 0.001 | 0.001 | 0.002 |
| `worldSnapshot` | 0.022 | 0.025 | 0.029 | 0.029 |
| `uiViewModel` | 1.317 | 1.473 | 1.394 | 1.548 |
| `renderRecord` | 0.005 | 0.006 | 0.005 | 0.006 |
| `submitPresent` | 3.492 | 0.930 | 7.267 | 1.229 |
| `other` | 0.243 | 30.832 | 0.905 | 30.650 |
| **逐帧 sum** | **5.085** | **33.388** | **9.490** | **33.435** |
| `cpuMs` | 5.084 | 33.387 | 9.490 | 33.434 |
| `gpuTotalMs` | 4.780 | 5.492 | 8.883 | 9.043 |
| 中位 FPS | 196.7 | 30.0 | 105.4 | 29.9 |

### 结论

- **不限速时最大的可归属项是 `submitPresent`，但它主要不是 CPU 工作**。skinned 3.49 ms、
  hair 7.27 ms；把 GPU 留出余量（`--frame-limit 30`）后分别降到 0.93/1.23 ms，降幅正好是
  等待 GPU/swapchain 的部分。RT 模式下这一阶段吸收渲染线程的消费、录制、提交、present 和
  帧槽 fence 等待，因此“CPU 6–12 ms”里的大头是 GT 在等渲染线程，而不是 GT 在算。
- **真正的 GT CPU 大头是 `uiViewModel`，1.3–1.5 ms/帧**，且几乎不随帧率上限变化；
  其余 GT 阶段合计约 0.15 ms（input/scripts/animation/hairSimulation/worldSnapshot）。
  下一项 CPU 优化应在这里：编辑器视图模型当前每帧收集场景 actor、debug draw、motion debug 等
  快照，即使 Performance/大纲页没有刷新或 developer UI 不可见；按可见性/脏标记跳过大块收集
  比继续拆 GPU pass 更直接地降低 GT CPU。
- **不限速的帧仍由 GPU 定速**：两场景 `cpuMs` 中位数 ≈ `gpuTotalMs` + 0.1–0.6 ms，
  GPU 分别是 4.78/8.88 ms。因此 pre-skinning 这类削减 GPU 工作的方案在 GPU 受限时仍有意义；
  但它不是当前 CPU 数字的来源。
- **均值不可用于定位**：skinned 的 `cpuMs` 均值 11.86 ms，而中位数 5.08 ms；`other` 均值
  6.96 ms 全由少数宿主调度尖峰贡献。均值与中位数的差距说明这台宿主有噪声；排名以中位数为准，
  不应把均值里的 `other` 当成某个引擎阶段。
- 单线程补充运行（`--worker-thread-count 1`）把同一段渲染工作记到 `renderRecord`
  （中位 3.470 ms），`submitPresent` 为 `n/a`；这与 RT 模式不限速的 `submitPresent` 3.492 ms
  对得上，进一步说明后者是不限速时的渲染/GPU 节拍，而不是 GT 计算。

## 7. `uiViewModel` 归因与可见性/节流修复（2026-10-23）

### 7.1 先归因：1.3 ms 到底花在哪里

在 `EngineLoop::UpdateUiViewModel()` 与 `UiSubsystem::Update()` 内临时插入
`steady_clock` 分段计时（每 120 帧打印，测量后删除），skinned 场景
（`SC_tressfx_female_skinned.json`）、Release、`--gpu-stats`。结果先纠一个前提：
**本 worktree 里 `--no-dev-ui` 时该阶段只有约 0.35 ms**；能复现 1.28–1.35 ms 的是
“开发者 UI 已初始化但不可见”（`developerUiVisible=false`，例如沉浸模式或配置隐藏、
且 `developerUiEnabled=true`）的状态。此时编辑器面板仍在逐帧重建，却一帧都不可见。

该状态下逐帧中位数（ms，29 个 120 帧窗口的中位数）：

| 分段 | 中位 | 说明 |
| --- | ---: | --- |
| `CanUndo` + `CanRedo` | 0.427 | 每次内部 `weakly_canonical`，两遍/帧 |
| `materialInstanceEditorPanel->Build()` | 0.378 | 整份面板 DOM/绑定重建 |
| `panel->UpdateRuntimeView()` | 0.159 | 内容浏览器/日志/motion 控件刷新 |
| `rmlContext->Update()` | 0.102 | 含隐藏的编辑器文档 |
| `ScriptRuntime::View()` | 0.051 | 复制类/组件类/性能 JSON |
| `BuildEditorSceneActors()` | 0.023 | 场景 actor 值副本 |
| 绘制快照构建 | 0.020 | RmlUi render + 纹理快照 |
| `GetMaterialInstanceModelContexts()` | 0.014 | 材质预览模型列表 |
| `CaptureDebugDraw()` | 0.003 | 本场景几乎为空 |
| `CaptureMotionDebug()` | 0.001 | motion debug 关闭 |
| 渲染器 getter / 其他 | ~0.01 | 不是瓶颈 |
| **合计** | **~1.28–1.35** | 与 GPU 阶段表 1.317/1.394 一致 |

结论与题面列的嫌疑不同：`CaptureMotionDebug`、`CaptureDebugDraw`、动画预览块在本场景
都是微秒级；真正的大头是**隐藏时仍逐帧重建编辑器面板**（0.55 ms）、
**每帧两次 `CanUndo` 的路径规范化**（0.43 ms）和 **RmlUi 上下文更新**（0.10 ms）。

### 7.2 修复内容

- 新增 `source/ui/uiViewModelRefresh.h`：`UiViewModelBuildMode`（None / RuntimeHud /
  FullEditor）与 100 ms `UiViewModelRefreshSchedule`；隐藏状态即使有脏标记也不构建完整快照。
- `EngineLoop` 按可见性决定档位；完整编辑器字段（脚本视图/undo、渲染器状态、
  环境/CSM/曝光、动画特征、scene actors、MI 模型、性能快照）只在 10 Hz 到期或事件
  强制时构建。逐帧只保留世界路径、离线标记、DrawDebug/相机冻结和运行时 HUD 文本。
- `UiSubsystem::Update` 增加档位/到期参数：隐藏时跳过面板构建与 RmlUi 主更新，
  只发布不含 UI 文档、仅 DrawDebug 的绘制快照；非到期帧保留上一份脚本视图。
- 显式强制刷新事件：可见性/模式/语言动作、World 代数或路径变化、视口/大纲/MI 选中、
  任意 `UiAction`、编辑器命令执行（保存/撤销/参数）、脚本编译开始/完成、脚本播放状态、
  离线模式及其命令回馈。
- 动画预览的 clip 列表、`FeatureSchema`、特征通道名按“世界代数 + 播放器身份”缓存；
  只有特征值随播放时间在刷新帧重算。
- `ScriptRuntimeSubsystem::HudView()` 提供只含 `hudLines` 的轻量视图，HUD 仍逐帧更新。

### 7.3 修复后实测（同一 Release、同一场景、`--perf-jsonl`，中位数）

| 状态 | `uiViewModel` 修复前 | 修复后 | `cpuMs` 修复前 | 修复后 |
| --- | ---: | ---: | ---: | ---: |
| 编辑器已初始化但隐藏（原 1.317 的状态） | 1.346 | **0.153** | 6.180 | **4.889** |
| `--no-dev-ui` | 0.355 | **0.038** | 25.701* | **5.175** |
| 开发者工作区可见 | 2.087 | **0.761** | 5.800 | **5.479** |
| hair 场景、`--no-dev-ui` | 0.407 | **0.087** | 26.028* | **8.6** |

\* 修复前的两次隐藏运行恰好遇到宿主调度噪声，`cpuMs`/`other` 明显偏高；`uiViewModel`
本身是每帧自计时、不受帧率上限影响，因此前后对比以该阶段中位数为准。同一状态多次复测
会在上述中位数附近波动（隐藏编辑器 0.15–0.20、`--no-dev-ui` 0.04–0.08、可见 0.72–0.80）。
可见状态修复后仍有约 0.76 ms，主要是**逐帧必须执行的工作区 RmlUi 渲染**（约 0.5 ms）+
上下文更新（约 0.1 ms）；被节流的面板数据构建只剩约 0.15 ms/帧的均摊。

刷新延迟：编辑器面板数据 ≤100 ms；选中、模式切换、World 换代、编辑器命令等强制事件
在下一 GT 帧生效（≤1 个渲染帧）；HUD 文本每帧更新；输入/hover/滚动/相机指示仍逐帧。
Performance 页仍按自己的 5 Hz 读取同一份冻结快照；`perf json`/`--perf-jsonl` 直接读
`GpuPassTimer`，与本修复无关，实测数值无变化。

### 7.4 复现入口

```
build-rel/bin/main.exe --dev-ui --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_skinned.json \
  --perf-jsonl artifacts/ui-after-immersive.jsonl --gpu-stats
build-rel/bin/main.exe --no-dev-ui --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_skinned.json \
  --perf-jsonl artifacts/ui-after-nodevui.jsonl --gpu-stats
```
隐藏状态截图见 `artifacts/ui-hidden-after.png`（只剩 HUD 场景名），可见状态见
`artifacts/ui-editor-after.png`（Outliner/Details/Viewport/Content Browser/Log/Performance）。

## 8. UI 分节化（第 2 层）

第 1 层解决了"隐藏态仍在重建"，但可见态每次刷新仍构建整棵快照。第 2 层把视图模型拆成
分节：source/ui/uiViewModelSections.h 定义 `UiSnapshotSection<T> { shared_ptr<const T> data; version }`，
节为 scripts / animation / sceneOutliner / assets / performance / viewportCamera /
environmentLighting / offline / status。

- 每节只有一个 owner 知道何时变化：脚本构建态、世界换代、选中、资产库 revision、
  性能窗口计数、离线状态等；不变则版本不推进。
- UiSubsystem 按 `ChangedFrom(appliedVersions)` 只对变化节重脏绑定；面板
  `BuildDeveloperPanels` 只在面板相关节变化时执行，不再每帧重建。
- 日志、资产目录、缩略图改为"按 revision 发布的不可变快照"，同一 revision 返回同一个
  `shared_ptr`，不再每帧 `Freeze()/GetCatalog()/GetSnapshot()` 拷贝。
- 17 项分节/版本/刷新计划单测覆盖：同一 revision 只发布一次、在飞消费者保留旧不可变节、
  选中只重脏 outliner、空闲窗口只重建节奏节、面板掩码排除 performance/animation。

实测（Release，skinned 场景，`--perf-jsonl`，5 次采样中位数）：

| 状态 | 第 1 层后 uiViewModel | 第 2 层后 |
| --- | ---: | ---: |
| 编辑器可见 | 0.761 ms | **0.53 ms** |
| `--no-dev-ui` | 0.038 ms | 0.04 ms |

可见态剩下的约 0.5 ms 是 RmlUi 每帧渲染与 context update（必要开销），不是快照构建；
要再降需要第 3 层"空闲不重绘工作区"。
