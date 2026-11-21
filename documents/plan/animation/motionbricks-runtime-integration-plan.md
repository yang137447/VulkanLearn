# MotionBricks 实时动作生成接入计划

> 规划文档，不是契约。基线为 2026-11-17 的 `master`。目标是在 VulkanLearn 内以
> C++ 同进程运行 MotionBricks，而不是把官方 Python demo 作为旁路进程交付。

## 0. 结论

可行，且不需要移植官方 demo 的 Warp/Newton 物理。MotionBricks 的推理面是单个
ONNX 模型，输入输出张量固定；官方 Python 运行时里的骨架采样、端点修正和历史
缓冲都是可逐行对照的纯数学。真正的工作量在引擎侧：外部姿态流、SOMA 骨架映射、
重定向、根运动、线程和资产打包。

长期方案采用 **C++ ONNX Runtime（可选 TensorRT/DirectML EP）+ 引擎原生姿态流**：

- 推理在独立工作线程完成，GT 只消费已完成的 generation；
- `SkeletalPlayback` 不理解 ONNX，只接收已经重定向到目标骨架的局部姿态；
- RT 仍只读冻结的 `SkinPose`，不新增 GT/RT 共享的可变推理状态。

## 1. 上游事实（2026-11-17）

- 仓库：<https://github.com/NVlabs/motionbricks>，v0.2.0，SIGGRAPH 2026。
- 模型：SOMA 30 关节人体、Unitree G1 机器人；30 Hz；最近 4 个 context pose
  + 最多 4 个 target pose。
- 单次生成 6..16 个 token，每 token 4 帧，原始输出 24..64 帧；去掉两端各 3 个
  边界帧后剩 18..58 个有效帧，再做端点修正。
- SOMA 输入：`global_root_values [N,8,5]`、`local_root_values [N,8,4]`、
  `local_poses [N,8,267]`、三个 `[N,8]` mask、`allowed_pred_num_tokens [N,11]`。
- SOMA 输出：`pred_global_motions [N,64,366]`、`pred_num_tokens [N]`。
- 267 = 29 个非根关节位置 87 + 30 个关节的 6D 旋转 180；366 再加 90 个关节速度
  和 4 个脚部 contact。
- 模型空间：米、Y-up、XZ 地面；VulkanLearn 为厘米、Y-up。桥接层负责 ×100 和
  左右手/前向轴校准。
- 模型体积：SOMA FP16 ONNX 约 380 MB，FP32 约 758 MB。RTX 4090 上 TensorRT
  单批 generation 约 1.28 ms/2.50 ms（FP16/FP32），不含输入准备和输出解码。
- 许可：代码 Apache-2.0；模型 NVIDIA Open Model Agreement（允许商用和修改后
  再分发，但须遵守条款）；示例 BVH/CSV 动作是 NVIDIA Sample Data Evaluation
  License，只能用于评估测试，不能随产品分发。

## 2. 目标架构

```text
C# MotionBricksComponent（目标姿态 / 参数 / 启停）
  -> World::MotionBricksRuntime（GT）
     -> MotionBricksModel（ONNX Runtime，工作线程）
     -> MotionBricksPlayer（历史、generation、30 Hz 采样与插值）
     -> SOMA -> SKM 重定向（CompiledRetarget）
     -> SkeletalPlayback::SubmitExternalPose
        -> 现有 post-process / foot IK / bone physics / GPU skinning
```

边界规则：

- 推理线程只拥有 ONNX session、输入/输出缓冲和 generation 结果，不触碰 World、
  Jolt、脚本运行时或渲染对象。
- GT 每帧最多取一个已完成 generation，按 30 Hz 时间轴采样并插值到引擎帧率。
- `SkeletalPlayback` 只接收目标骨架的 `LocalPose` 和显式根运动增量；它不采样
  clip、不推进 clip 时钟，也不读取 MotionBricks 类型。
- 根运动沿用现有 root-motion 消费路径交给 C#/Jolt；不把模型根位置直接写 Actor。
- 生成式姿态仍然经过现有 PostProcess、FootPlacement、SlopeWarping 和骨骼物理，
  这些阶段不需要知道来源。

## 3. 分阶段

### P0：外部姿态流接缝

实现状态（2026-11-17）：已完成。

- 代码：`source/animation/externalPose.h`、`source/animation/skeletalPlayback.*`
- 测试：`tool/animation-tests/externalPoseTests.cpp`

本阶段：

- `SkeletalPlayback` 增加显式的 external pose stream：GT 提交
  `ExternalPoseFrame`，Tick 消费；clip 时钟与外部流互斥。
- 外部流下仍允许 PostProcess/FootPlacement/SlopeWarping/骨骼物理。
- 硬切通过 `hardCut` 推进 `discontinuity`，避免把 teleport 当成连续速度。
- 新增模块测试覆盖替换采样、暂停、硬切、Clear 后恢复 clip、关节数校验。

验收：动画模块测试全绿；不改变现有 clip 路径的位级行为。

### P1：MotionBricks 纯数学核心

实现状态（2026-11-17）：已完成。编码、解码、边界帧裁剪、端点修正和 30 Hz 采样
已与官方 Warp kernel 的 golden 向量逐项对账。

- 代码：`source/animation/motionBricks/motionBricksContract.*`、
  `source/animation/motionBricks/motionBricksMath.*`
- 测试：`tool/animation-tests/motionBricksTests.cpp`
- 夹具：`tool/animation-tests/fixtures/motionbricks/`（manifest、golden、可复现脚本）

- `MotionBricksContract`：解析 `soma_n1.json` 的关节顺序、张量名/形状/dtype。
- SOMA 常量：30 关节、Hips 根、heading joints、foot contact joints。
- 编码：`LocalPose` → SOMA 模型空间 → `[8,5]`、`[8,4]`、`[8,267]`。
- 解码：`[64,366]` → 根位置/朝向、关节局部 TRS、脚部 contact。
- 端点修正、边界帧裁剪、世界放置恢复。
- 以官方 Python 实现生成 golden 向量；C++ 与 Python 在固定输入上逐项对比。

验收：synthetic tensor 单测 + golden 向量误差在 1e-5 量级；不依赖 GPU。

### P2：ONNX Runtime 绑定

实现状态（2026-11-17）：已完成。C++ 通过 `LoadLibrary` 动态加载
`onnxruntime.dll`（C API 22 / ONNX Runtime 1.22），CPU EP 跑通 SOMA FP32；
输出与 Python ONNX Runtime 的 golden 向量对齐（token=14，误差 < 1e-3）。

- 代码：`source/animation/motionBricks/onnxRuntimeApi.*`
- 头文件：`extern/onnxruntime/`（仅 C API header，运行时 DLL 动态加载）
- 测试：`tool/animation-tests/motionBricksOnnxTests.cpp`
- 推理夹具：`tool/animation-tests/fixtures/motionbricks/soma_inference_golden.json`

- 动态加载 `onnxruntime.dll`（`LoadLibrary`/函数指针），避免 MinGW 链接 ABI 风险；
  同进程、无 Python。
- 固定 N=1 输入/输出缓冲，tensor 名称和形状从 manifest 校验，不猜。
- CPU EP 先做正确性；CUDA/DirectML/TensorRT EP 作为可选后端。
- 缺少 DLL/模型时返回明确诊断，不静默降级。

验收：单请求 CPU EP 输出与 Python ONNX Runtime 对齐；模型缺失时测试显式 skip。

### P3：Player 与 generation 调度

实现状态（2026-11-18）：纯 GT 调度核心与异步 live 调度已完成。

- 代码：`source/animation/motionBricks/motionBricksPlayer.*`
- 测试：`tool/animation-tests/motionBricksPlayerTests.cpp`（13 项）
- `PoseHistory` 的 32 帧记录、delta-time shift 和 30 Hz context 采样对齐
  `MotionRecorder` / `history_sample_indices`
- `GeneratedMotionPlayer::Step()` 固定执行 sample → record，和官方 `BaseCharacter.step`
  的顺序一致；首帧 root delta 基准是 frame 0
- `CommitGeneration` 统一消费 `GenerationParams.transitionEarlyByFrames`，只接受
  18..58 帧，脚部 contact 必须 finite 且位于 [0,1]

P5 已补齐后台 worker、latest-wins 请求、provider 选择与 generation 遥测。
仍有待补充：10 分钟持续运行的累积漂移验收，以及更真实的目标姿态/转向输入。

- 32 帧姿态历史、context 刷新、target 选择、`num_poses_to_use`、
  `transition_early_by_frames`、allowed token counts。
- 30 Hz 采样与引擎帧率解耦；generation 边界按官方端点修正后连续拼接。
- 新 generation 的硬切/交叉淡入策略；迟到帧不阻塞 GT。
- 遥测：generation 耗时、有效帧数、token 数、重生成原因、丢帧数。

验收：连续 10 分钟行走/转向/停止无累积漂移；GT 不被推理阻塞。

### P4：骨架、重定向与根运动

实现状态（2026-11-18）：native baked 与 C#/Jolt 链路均已跑通。

- 代码：`source/animation/motionBricks/motionBricksSkeleton.*`、
  `source/animation/motionBricks/motionBricksBakedRuntime.*`、
  `source/world/motionBricksRuntime.*`、`World::TickMotionBricksAnimations`
- 资源：`Maps/SC_motionbricks/Skeletons/SK_soma.json`（77 关节）、
  `Retarget/RT_UEFN_From_SOMA.json`（23 映射）、
  `Generated/soma_probe_motion.json`、蓝图/C#/探针场景
- 原生探针：`SC_motionbricks_native_probe.json` 在 600 帧与 36000 帧
  （`--frame-limit 1000`）均 exit 0；日志确认
  `[MotionBricks] ... baked frames=50 target joints=91`
- C# 探针：`SC_motionbricks_probe.json` + `MotionBricks.CharacterBlueprint`
  1800 帧 exit 0、无蓝图校验错误；Jolt 胶囊位置/速度随根运动变化，
  反馈用 external pose 自带的 `deltaSeconds`，而不是消费姿态时的脚本 dt
- 校准：`soma_calibration_motion.json` 的 SOMA rest→UE rest 截图无扭曲
- 场景 mesh 组件允许 `motionBricks` 字段；runtime 同时扫描 script component
  和顶层 mesh 对象；MotionBricks 只在 `TickAnimations` 每帧推进一次，脚本阶段
  只读取上一帧已提交的姿态，两个入口不会重复步进

尚未完成：真实训练动作的转向/步态质量与目标姿态、foot contact 接入
FootPlacement；live ONNX 已由 P5 接入，GPU EP 仍只有回退路径验证。
当前 baked/live probe 的合成输入姿态本身不代表最终动作质量。

- 资源工作树新增 SOMA 骨架 `SK_*.json`（或等价的导入源）和目标 SKM 的
  `RetargetProfile`。
- 先用 SOMA→UE Mannequin 人形映射验证；G1 机器人不往人形 SKM 上强行重定向。
- 6D 旋转 → 四元数、模型米→厘米、前向轴/左右手校准。
- 根运动增量交给现有 C#/Jolt 路径；与 MotionWarping、RootOffset 的叠加顺序写清。

验收：`SC_*` 探针场景中角色走、跑、转向、停止；根位移和脚部接触目视一致。

### P5：桌面实时推理

实现状态（2026-11-18）：桌面 CPU live 路径已完成。本阶段只验收桌面实时推理
与动作连续性；移动端、安装包体积和打包性能预算不在本轮范围，也不作为门禁。

- 代码：`source/animation/motionBricks/motionBricksLiveRuntime.*`、
  `motionBricksRuntimeInterface.h`、`onnxRuntimeApi.*`、`source/world/motionBricksRuntime.*`
- 测试：`tool/animation-tests/motionBricksLiveTests.cpp`
- 资源：`SC_motionbricks_live_native_probe.json`、`SC_motionbricks_live_probe.json`、
  `BP_motionbricks_live.json`
- 后台单 worker 持有 ONNX session 与推理缓冲；GT 只消费已完成 generation。
  请求采用 latest-wins，旧请求不会阻塞下一帧；session 初始化失败会记录明确错误。
- `provider` 支持 `auto` / `cpu` / `directml` / `cuda`。`auto` 先探测实际
  可用的后端（DirectML 导出、CUDA provider DLL），再按 DirectML、CUDA、CPU
  顺序尝试，避免为未安装的后端制造 ORT 加载错误；实际选择的 provider 在
  session 就绪后输出。
- `AnimationPlayback.SetMotionBricksTarget()` 通过
  `motionBricksTarget` 命令把 WASD 方向与 Shift 步幅交给 live runtime；运行时
  在 worker 空闲时立即用新朝向/距离重建目标 generation。探针蓝图的相机
  `autoActivate` 提供第三人称跟随视角。
- live 目标姿态按官方 SmartPrimitive 语义使用 4 个 30 Hz 姿态：肢体来自最近
  4 帧生成姿态，只把根重写到目标轨迹上；单帧 1 米目标会被模型读成不可能速度，
  这是此前走跑 T-pose/夹腿滑步的原因。探针使用 `transitionEarlyByFrames=0`。
- 本机实测：CPU 稳态约 56–68 ms/代，单代 34–50 个有效帧；native live
  3600 帧 exit 0，连续 10 代；C# live 3000 帧 exit 0，连续 11 代，
  `Script play state: 1`；immersive 模式实测 W 前进、D 转向且跟随相机保持
  角色在画面内，走路可见交替迈步与摆臂。完整动画回归 400/400 通过。
- 当前 GPU 结论：DirectML 1.22 对该 SOMA FP32 模型初始化报 `E_INVALIDARG`；
  CUDA 缺少 `onnxruntime_providers_cuda.dll`。两者都会回退到 CPU，尚未验证
  GPU 加速收益。
- 模型文件约 758 MB，不进入 Git；运行前通过
  `VULKANLEARN_MOTIONBRICKS_SOMA_ONNX` 或场景 `motionBricks.model` 指定。
`VULKANLEARN_ONNXRUNTIME_DLL` 指定 ONNX Runtime DLL。模型许可与 NOTICE 仍需在
实际分发前单独核对。

- 后续可选优化：FP16 模型、按模型缓存 session、多角色批处理（N>1）。
- 发布包不得携带官方示例动作数据。

### P6：契约与门禁

- 行为稳定后新增 `documents/contracts/motionbricks-runtime.md`，只写已实现事实。
- 更新 `documents/routing.md`、`contracts/animation-runtime-skinning.md` 和
  `contracts/testing.md` 的验证映射。
- 运行 `py -3 tool\validate-docs.py` 和相关动画 CTest。

## 4. 关键决策

| 议题 | 选择 | 理由 |
| --- | --- | --- |
| 推理后端 | ONNX Runtime 优先，TensorRT 可选 | C API、DLL 动态加载、MinGW 友好；TensorRT 需要额外版本/构建约束 |
| 进程模型 | 同进程工作线程 | 长期方案要求可打包、低延迟；旁路 Python 只用于早期验证 |
| 姿态所有权 | GT 提交 `LocalPose` 拷贝 | 不把推理缓冲发布给 RT，沿用现有 `SkinPose` 冻结语义 |
| 骨架策略 | SOMA 为源、SKM 为目标 | 复用现有 `RetargetProfile`，目标网格拥有 skins/palette |
| 根运动 | 生成结果给显式 delta | 不把模型根位置直接写 Actor，避免和 Jolt/MotionWarping 双积分 |
| Smart Primitives | 引擎侧 JSON 目标姿态 | 不移植官方编辑器 GUI；目标姿态由场景/交互产生 |
| 帧率 | 模型 30 Hz，渲染插值 | 模型固定 30 Hz；引擎帧率解耦 |

## 5. 验证入口

- P0：`ctest --test-dir build -R "ExternalPose" --output-on-failure`
- P1：`ctest --test-dir build -R "MotionBricks" --output-on-failure`
- P2：设置 `VULKANLEARN_ONNXRUNTIME_DLL` 与
  `VULKANLEARN_MOTIONBRICKS_SOMA_ONNX` 后运行
  `ctest --test-dir build -R "MotionBricksOnnx" --output-on-failure`；
  缺失时显式 skip
- 动画回归：
  `ctest --test-dir build -R "^(AnimationSampling|AnimationDependency|AnimationRetarget|SkeletalBlend|SkinRenderHistory|MotionHistory|MotionRuntimeQuery|MotionMatching)" --output-on-failure`
- 文档：`py -3 tool\validate-docs.py`

## 6. 风险

- SOMA→UE Mannequin 的关节轴、6D 旋转展开和脚部 contact 语义若映射错，视觉会
  出现扭转/滑步；必须在 P1 用 golden 向量锁死数学。
- 端点修正和世界放置恢复容易与根运动双重积分；P3 必须只保留一条根位移来源。
- 30 Hz 生成 + 网络延迟会造成目标更新滞后；P3 需要迟到帧和重生成策略。
- 380 MB 模型和 GPU EP 依赖会影响安装包体积/目标机器；P5 单独处理。
- 模型许可允许商用，但示例动作数据不允许随包分发；资产提交前逐项核对。
- MinGW 对 `onnxruntime.dll` 的导入库支持不作为假设，P2 直接走动态加载。

## 7. 明确不做

- 不移植官方 demo 的 Warp/Newton 物理、渲染器和编辑器 GUI。
- 不做训练/微调，不重建第二个场景或材质作者路径。
- 不把 MotionBricks 类型泄漏进 `SkeletalPlayback`、`SkinPose` 或渲染线程。
