# 2026-11-14 帧率与转动卡顿排查记录

状态：执行记录（rationale），**非当前契约**。记录当天在
`SC_terrain_pegasus_4x4`（16 tile）+ Echo 角色上的测量与修复；数值只对应当天的
构建与机器，不应被当作长期基线。

## 1. 环境与工具

- 机器：Windows，1280×720，前台窗口，`--no-dev-ui` 或编辑器 UI 可见两种口径。
- 构建：MinGW Release。`build-merge` 编译期开启 Vulkan validation；`build-noval`
  关闭 validation 并启用 Tracy。
- 采样：标题栏 `FpsTool`（1 s 窗口）、`--perf-jsonl`（每秒一行）、Tracy
  （`tracy-capture` 15 s + `tracy-csvexport -u` 逐事件）、`VL_HITCH_LOG=1`（≥20 ms 单帧）。

## 2. 修复前基线

| 配置 | FPS |
| --- | --- |
| validation ON，16 tile，编辑器 UI | 35–41 |
| validation OFF，同上 | 88–94，长时间稳定 88 |
| validation OFF，去掉全部地形 | 137–144 |
| validation ON，`chunkQuads=256` | 70–80 |

同一场景改 LOD 距离、关材质后处理、heightBlend、VT/数组切换都不改变帧率；
draw 数与 chunk 数线性相关。编辑器 UI 再吃约 5 ms。

## 3. 五个根因

### 3.1 骨骼物理每个 section 步进一次

`World::TickScriptAnimations` 原先在 `meshObjects` 循环里对每个 section 调
`BonePhysicsSolver::Solve`。Echo 的一个 SKM 展开为 20 个 `WorldMeshObject`，
它们共享同一个 `SkeletalPlayback` 与同一个 `sceneObjectIdentity`，于是同一个
Jolt `PhysicsSystem` 每帧被 `Update` 20 次：物理时间被放大 20 倍，CPU 也从
0.18 ms 变成 2.70 ms。

证据：Tracy unwrap 中 `worldAnimation.cpp:825`（`Anim.BonePhysics`）在 1227 帧里
出现 24524 次（≈20/帧），mean 135 µs；`TickScriptAnimations` 合计 3.45 ms，
其中 `Anim.PlayerTick` 0.44 ms、`Anim.Grounding` 0.26 ms。

修复后同一项为 1227 次、mean 183 µs，整段 0.91 ms。

### 3.2 particleSoftDepth 每帧重建 descriptor

`ParticleRenderer::RecordSoftDepth` 的资源签名只哈希了**当前 swapchain image** 的
`sceneDepth` view/sampler 与软深度 view；所有 per-image descriptor set 与 UBO
却是整组一次性创建的。image 轮换让签名每帧变化，于是每帧
`DestroySoftDepthResources()` + 重建 pool/set/N 个 UBO。

证据：新增的 per-pass CPU 录制表显示 `particleSoftDepth` 1.27–1.35 ms/帧，
而 `geometry` 只有 0.32–0.34 ms。

### 3.3 UI 热重载在 GT 上做同步目录扫描

`UiSubsystem::BuildAssetFingerprint` 对 `ui.assetRoot` 下 46 个文件逐个
`last_write_time`/`file_size`（Windows 上每次 stat 约 0.4 ms）。500 ms 节流下
每 0.5 s 产生一次约 19 ms 的顿挫，表现为 p10 掉到 89 fps。

证据：Tracy 捕获中 `TryHotReload`（`uiSubsystemDocuments.cpp:479`）在约 38 s 里
74 次、mean 19.2 ms。

### 3.4 shader FileMonitor 每 250 ms 同步扫描

`ShaderFileMonitor::Poll` 在 GT 上递归遍历 `shader/glsl` 的 220 个源文件并逐文件
stat，Windows 下约 100 ms。表现为 4–5 次/秒、每次约 105 ms 的硬卡顿；与转动无关，
但转动时最容易被察觉。

证据：`VL_HITCH_LOG` 行形如
`[Hitch] frame 106.2 ms (pre-tick 104.0, clock dt 108.1): scripts 0.01 worldSnapshot 0.94 …`；
10 s 内 40 条，修复后同一观察窗口 0 条。

### 3.5 校验层的固定成本

同一场景、同一二进制，`--no-validation` 与默认（validation ON）相差约 3 倍：
62 fps vs 196–207 fps。校验层本身是本地帧率差异的主要来源，不应作为渲染路径的
性能结论。

## 4. 修复后

| 配置 | 修复前 | 修复后 |
| --- | --- | --- |
| 无 validation + 编辑器 UI | 88–94 | p50 206.6 / p90 233.5 / max 240 |
| 无 validation + 无 UI | 约 160（不稳） | p50 199 / max 240 |
| 带 validation + 编辑器 UI | 35–41 | 约 62 |

240 是前台 present/DWM 的上限；`gpu-pass-timing.md` 里 2026-11-06 的 9-tile 记录
同样是 240。修复后 `scripts` 约 2.0 ms（Play，含 0.9 ms 动画、0.18 ms 骨骼物理）、
`worldSnapshot` 约 1.1 ms、RT 侧 `Terrain.ExactLayerFeedback` 约 0.7 ms、
`BeginFrame` 约 0.7 ms。

## 5. 方法陷阱

- `FpsTool` 用 `RuntimeClock::TickDeltaSeconds()` 的帧间隔算 FPS；该值在
  `TickEngineFrame` 中段才采样，因此 pre-tick 的 100 ms 不会体现在标题上。
  判断真实帧率要看 `VL_HITCH_LOG` 的 `clock dt` 或 Tracy 的帧数。
- `--gpu-stats` 会为每个 pass 写 `BOTTOM_OF_PIPE` timestamp，串行化 GPU，
  同一场景可把帧率从 200+ 压到 46；不能用它做帧率结论。
- 用 CUA 注入的合成鼠标拖拽会一次灌入多条输入事件，制造 17 ms 级 `input`
  单帧；判断“转动是否卡”应看稳态日志或真实鼠标。
- 测量窗口里的 `[Hitch]` 首次命中也可能是启动/切 Play 的一次性开销
  （Echo Play 进入时 scripts 70 ms + UI 10 ms）。
