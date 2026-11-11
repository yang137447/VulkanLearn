# 粒子离线渲染与缩略图预览边界

2026-11-01。背景：离线渲染需要覆盖 PS_*.json，但现有路径追踪器只消费三角网格；
粒子缩略图也只推进过一次 prewarm 固定步。本记录解释实现选择与当前边界，
**不是契约**；粒子运行时的现行行为仍以
[粒子系统契约](../contracts/particle-system.md)为准。

## 离线路径

- `source/render/offline/offlineParticleSimulation.*` 是 `particleSimulate.comp`
  的 CPU 镜像：PCG 随机流、spawn 形状、重力/drag、风/curl 噪声/涡旋/吸引子、
  寿命倍率曲线、burst/prewarm 与解析碰撞公式逐项对应 shader。
- 离线没有帧循环，因此先按固定步长推进到确定时刻
  （`OfflineParticlePreviewDefaults()`：1/60s × 30 步 = 0.5s，上限 32 步），
  再交给 `offlineParticleCompositor.*` 做软件合成。
- 合成层是“预乘颜色 + 背景可见比例”：alpha 图层累乘 transmittance，
  additive 图层只累加颜色。`OfflineRenderCoordinator` 在 prepare worker 内
  为冻结快照构建该层，`OfflinePublishedResult::particles` 与 film 同代发布；
  `ExportOfflineResult` 用它合成 `combined.pfm`/`combined.ppm`，并写出
  `particles.pfm`，同时保留 `direct.pfm`/`indirect.pfm` 的几何 AOV 原值。

## 确定性

- WorldSnapshot 里的粒子系统来自 unordered_map；离线按系统名排序，发射器之间
  走既有的“远 → 近 + 稳定 key”顺序，单发射器仅在作者开启 `sortParticles`
  时按深度降序。
- 离线种子混入每帧随机状态。`seed=0` 时与 GPU shader 的默认随机流一致；
  非零 seed 只服务离线测试/可重复扰动，GPU 运行时没有对应的离线输入。
- 模拟只使用 float 算术、固定步长与有序容器，不读取 wall clock、线程调度或
  活动 World 状态。

## 当前边界

- `renderMode=mesh` 在 CPU 合成中退化为 billboard：离线包没有粒子网格实例化
  数据，且不加载 Vulkan/资源根纹理。
- flipbook 纹理、逐帧 UV/pivot、软粒子 sceneDepth 与场景遮挡不进入软件合成。
  有纹理的资产会使用程序化圆盘/条带形状，颜色仍来自作者曲线。
- plane/box/capsule/SDF 碰撞都复用 `particles/particleCollision.h` 的 CPU
  辅助函数；粒子事件（spawn/death/collision 回传 C#）不进入离线层，GPU 的
  `fixedTimeStep` 子步也由离线自己的固定预览策略替代。GPU 的
  `deterministicRandom`/`randomSeed` 相位同样不进入离线层，离线使用自己的
  稳定 seed 推进随机流。
- 交互式离线视口的显示 shader 目前仍只上传几何 film；粒子层随
  `OfflinePublishedResult` 发布并由导出合成。预览候选（`interactivePreview`）
  不构建粒子层，避免 30 步模拟挤占 30ms 预览预算；显示侧接入需要同时扩展
  上传身份与合成 shader，不在这次改动范围内。

## 缩略图预览

- `EditorThumbnailRenderSession` 让粒子缩略图推进
  `ParticleThumbnailStepCount()`（30）个 1/60s 固定步，而不是一次 prewarm。
- 每一步单独提交一次 compute：粒子 UBO 是持久映射的，把多个
  `RecordSimulation` 录进同一 command buffer 会让所有 dispatch 读到最后一次
  写入的 dt/prewarm。模型/材质缩略图不经过这条路径。

## 证据

- `tool/offline-integration-tests/offline_particle_test.cpp`：
  ParticleAssetLoader 解析、固定步确定性、30 步 burst 演化、合成边界/
  alpha/additive、64×48 像素 golden（8×6 线性 RGB 均值，容差 2e-4）、
  coordinator 发布与导出。
- `tool/editor-ui-tests/particleThumbnailTimingTests.cpp`：缩略图步数与离线
  步长定义一致、预览时长受 maxSteps 约束。
- `tool/offline-integration-tests/fixtures/PS_offline_particle_golden.json`
  与 `tool/offline-integration-tests/goldens/particle_billboard_64x48.txt`：
  固定的单粒子 burst 资产与 golden 数据。
