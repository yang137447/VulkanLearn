# S4 实际 EditorHost 自动验收入口

此工具新增独立测试入口，不改 EditorHost、Service、编辑算法、生产 console
或根 CMake。`editor_s4_main.cpp` 用 `RunApplication` 启动正常 Editor，
包装 `IEditorHost`，所有虚函数转发给真实 `EditorHost`。唯一新增行为是
GT `Tick` 向真实 `CommandBus::Queue(UiAction)` 排队；下一帧
`EngineLoop::ApplyUiActions` 经 `BeginUiActionBatch / ApplyUiAction /
EndUiActionBatch` 处理。没有直接调用 Session 写数据，也没有调用
`RefreshPlacement` 伪造更新。

现有 JSON Editor command 处理文档命令，未提供 Paint/Sculpt 投递协议；
当前无需新增生产 `terrainvt edit` 接口，因而没有生产接口确认依赖。
现有 `editor_ui_tests` 链接的是 CPU/UI 切片，不编译真实 EditorHost /
CompositedService，单独增加一个 UI 测试不能证明 GPU 编辑闭环。

## 不占用 renderer 的检查

从本 worktree 根目录执行：

```powershell
py -3 tool/validation/terrain/editor_s4_test.py
py -3 tool/validation/terrain/editor_s4.py build --build-dir build-vt-release `
  --output artifacts/terrain-editor-s4-release-frozen
```

`build` 默认使用 `build-vt-release`，兼容 `--build-dir` / `--build`。
读取对应 MinGW `main_editor.dir` 的 compiler、defines、优化选项、include 和 link
response files；仅编译新增入口并链接现有静态库。不会运行 renderer，
也不会重新配置或修改现有构建目标。输出目录必须不存在；重建使用新目录。
编译与链接每条命令上限 240 秒。日志和 source/exe 摘要保存在输出目录。
**先等主线程通知 Release `main_editor` 链接完成，再链接验收器**；已有静态库能链接
不代表它们包含正在被其他 agent 修改的源码。

## 主线程取得独占 renderer 窗口后运行

```powershell
py -3 tool/validation/terrain/editor_s4_prepare.py `
  --resource-root ../VukanLearnResources --output D:/s4/fix

py -3 tool/validation/terrain/editor_s4.py run `
  --build-dir build-vt-release `
  --exe artifacts/terrain-editor-s4-release-frozen/editor_s4.exe `
  --resource-root D:/s4/fix --staging-root D:/s4 `
  --scene Generated/Validation/TerrainCompositedVT/Variants/Near100m_R1024_Default/SC_composited_vt_100m.json `
  --output artifacts/terrain-editor-s4-run-worker2 `
  --frames 120 --frame-limit 1000 --minimum-stage-ms 2000 --worker 2 --timeout 600
```

默认 scene 是真实存在的 Near100m 变体；prepare 读取其 capture_plan 的 near
相机，并在复制的 MI 上显式设置 `u_heightBlendDistanceFade=0`。原始 2×2
fixture 的 surface00 中心海平面也可指定，但本入口默认使用更明显的 100m
近景。需要自选场景时，fixture 要求：

单 terrain scene 的 placement 默认从实际 object name 推导，不改写作者身份；
显式 `--placement` 会严格核对。最终解析身份写入 `invocation.json`，离线 analyze
使用该身份。当前工作树 Default scene 的 name 是 `Landscape_Near100m`；
主线程的隔离 fixture 如已改为 `Landscape_00`，则以该 fixture 的实际 JSON 为准。

- 目标 TR 已启用 compositedVT，有有效 Cook 包、至少两个视觉不同的 layer，
  且 `paintPath` 非空；允许 Paint 文件尚不存在。
- 相机固定，能看清目标 placement 的 UV `(0.5, 0.5)`，该区域没有高度饱和。
  小笔刷半径为 `0.025`，Paint 固定选择 layer 1；中心恰好落在常见偶数页网格边界。
  虚拟分辨率应足以区分局部 dirty 与整幅失效。
- 使用 authoring 场景，移除会运动的角色、粒子、动画、动态曝光等干扰。
  Undo/Redo 截图平均 RGB 误差阈值为 1/255；动态画面会造成明确失败。
- `editor_s4_prepare.py` 从 scene 出发遍历 TR、MI、texture descriptor、
  image/array payload、Paint 和 meta；已有 Cook 包则包括其二进制、
  dependency/coverage/height sidecar 和各自 payload。缺失的 Cook 输出留待初始
  Cook，缺失必需作者输入直接失败。不会扫描或复制其他 topic 的资源目录。
- renderGraph 配置是另一组 closure 根：包括 Common shadow / pass 材质及其资源。
  创建空 `Maps/` 供关闭开发 UI 后仍执行的 sceneRoot 初始化使用；不会因此复制
  全部 Maps。首次真实启动发现的这两项缺失已修入 prepare，并保留失败日志。
- 复制使用 `copy2`，不使用 hardlink；prepare 不修改输入资源。当前默认变体
  在无 Cook 输出时的实测 closure 是 55 个文件、65,959,451 bytes。

run 再按 closure 复制资源，把 config/shader/ui/script 放到短临时 staging，
在复制的 config 中
设置绝对 resourcePath，并在 staged project 中运行专用 exe。Save 写入
复制的 heightmap / Paint / `.meta.json`。不使用 hardlink，不覆盖原资源，
不临时修改原 config。默认 staging 为 `%TEMP%/vls4/e4-<id>`，也可指定
`--staging-root D:/s4`；复制前拒绝长度达到 240 字符的资源目标路径。
`staging.json` 记录路径并保留现场；运行证据仍写入 `--output`。
超时终止该次进程树，保留现场和证据。
运行 worker 1、worker 2 使用不同输出目录，串行执行。

## 实际动作与判据

首先用独立子进程 Cook 准备后的 all-VT fixture。然后依次等待 baseline、
Paint、Paint Undo、Paint Redo、Sculpt、Sculpt Undo、Sculpt Redo、连续 24 次
交替 Erase/Paint、material、material_revert、material_reject、conflict、
conflict_recover、Save。每阶段至少执行 `--frames` 个 producer 步骤，并在截屏前
满足 `--minimum-stage-ms` 的实际时间；期间仍由正常 EngineLoop Tick 推进，
不阻塞 renderer，不在 producer 中 sleep，
采集真实服务的逐帧 stats、World 高度/Paint BLAKE3 摘要、GPU 截图。
没有后台轮询或自动重试 renderer。Save 完成后退出编辑进程，再启动独立
Cook 子进程消费保存到磁盘的快照，最后以新进程 reopen，再投递真实
`LoadWorld` 命令 reload。所有 renderer 子进程串行。

验收器检查：

- World generation/placement 不换代；Paint/Sculpt 的 World 数据发生变化，
  Undo 回原值、Redo 回编辑值；Paint 不改变高度，Sculpt 不改变 Paint。
  无覆盖的空 Paint 快照归一化，避免 Undo 到空对象被误判。
- 每次编辑的服务 revision 增长；Save 保持版本和 World 内容。
  每阶段结束 queued/reading/stale、preview readback 和 pending update 收敛。
  Editor 的 dirty 集合包含尚未请求的屏外虚拟页，按需重建允许其保留；
  只有 resident stale 和请求队列均为零时才按非驻留失效页记录，不能把 stale
  当作 dirty 一起忽略。冷启动 Runtime 的全零 dirty 检查仍保留在原驱动。
  报告字段明确为 `dirtyNonresident`，`residentWorkingSet` 单独列出 stale、
  queued、reading、previewReadbacks。这些计数描述 CPU 驻留/任务状态，
  不表示 GPU 所有虚拟页都是最新内容。
- 小笔刷阶段实际采样到非零 dirty，且峰值小于该 surface 全 mip 总页数。
  这是脏页数量证据，不替代 dirty 矩形逐页空间覆盖测试。
- Paint/Sculpt 前后**实际 BMP RGB 像素**发生变化；忽略 header / alpha，
  Undo/Redo 对照图平均每通道误差不超过一个 8-bit 级别。
  还以实际编辑前后差异至少 2 RGB 级的像素建立 mask，在该区域单独检查
  恢复误差 <= 1，避免小笔刷残留被整屏平均稀释。
  这是 GPU 最终输出变化与可逆性检查，不证明每个合成材质通道正确。
- Save 后 heightmap 和 Paint 文件摘要改变；Paint v2 manifest 的
  `contentSha256` 必须认证真实保存的 Paint 字节。
- material 使用 `RenderSystem::CreateMaterialInstancePreviewAdapter`：
  Connect → Apply `u_uvScale × 1.75`（static UV）→ RestoreBaseline。
  在现有 GT safe-point idle hook 后调用，记录实际 Completed / liveSwapCommitted
  及 live MI 参数；检查服务 revision、输出变化和恢复。不会直接调用
  `NotifyMaterialChanged`。material_reject 向同一 API 投递错误 World generation，
  要求 Prepare 拒绝、不提交、不推进 VT revision，保留最近有效状态。
- conflict 在未保存 Paint/Sculpt 之后，仅修改复制的 MI 的 `u_uvScale` 字节，
  等待**真实 EditorHost** 的 `unsaved edits conflict` feedback。要求未保存
  World 高度/Paint/live UV 和画面保持不变，恢复步骤还原原始 MI 字节。
  不创建第二份 Monitor 来代替真实 Host 的冲突信号。
  untouched baseline 若已报告 source changed，明确失败，不把后来的既存冲突
  误算成 harness 外部修改触发的有效冲突证据。
  每阶段首尾及每次反馈变化均记录 `afterHostTick-beforeProducer` 观测、
  phase / Host Tick 序号、单调时间、stageElapsedMs 和 feedbackChanged。
  外部写入/还原另有 `EDITOR_S4_EXTERNAL` 事件时间。冲突必须来自注入后的
  后续 Host Tick，反馈必须新变化；阶段观测不足 1500 ms 时保持缺证据。
- saved Cook 前后作者文件摘要相同，输出依赖身份必须改变；随后实际 reopen /
  reload 的 Paint、持久化高度、live UV、MI 磁盘身份和 GPU 输出匹配 Save。
  reload 必须推进 World generation。PNG 的高度按 R16_UNORM 量化摘要比较
  （匹配现有 PNG writer 的精度）；编辑期间 Undo/Redo 仍用完整浮点样本摘要。
- rapid 编辑期间必须看到新增 staleBakeResults 或 discarded 计数；
  没观察到时返回证据不足，不能把“最终队列为零”算作旧任务拒绝通过。
- rapid 保存动作前 baseline 加每次实际处理后的 24 个 WorldHash；每个相邻 hash
  必须改变，避免把饱和后的 no-op 当作新 revision。1000 Hz 是正常 launch pacing
  配置，用来增加自然 GPU 重叠，不注入或伪造拒绝计数。未发生竞争的单线程运行
  仍保留 missing，不因另一轮已见拒绝就改写该轮结论。
- 非零进程退出码、Vulkan validation / service 错误均拒绝通过。

每次运行生成 bootstrap/saved Cook 日志、`editor.log`、`reopen.log`、每阶段截图、
`invocation.json`、`process.json`、`save-files.json`、`lifecycle.json`、
`staging.json` 和 `editor-s4-report.json`。可离线复核：

```powershell
py -3 tool/validation/terrain/editor_s4.py analyze `
  --output artifacts/terrain-editor-s4-run-worker2 `
  --placement 'terrain|Landscape_Near100m'
```

退出码：0 本入口判据通过；1 工具/配置失败；2 验收失败；3 证据不足。
固定帧数不足时，保留失败证据并由主线程决定是否以更大 `--frames` 新开运行。

## 覆盖边界

本入口覆盖 S4-01/02/06 的实际编辑、版本推进、收敛和输出变化，S4-03 的数值
preview/revert，以及 S4-04 的真实 Save/Cook/reopen/reload。S4-05 覆盖真实
Host 冲突诊断和保留未保存的 World 状态。没有注入确定的旧 GPU 任务延迟；rapid 是竞争窗口，
计数不增长就缺证据。没有第二套测试编辑算法，没有 GPU 通道真值 oracle。
未保存 Cook 的私有 `CheckBeforeRecook` 没有公开 Host 入口，因而不声称测试了
该拒绝路径；冲突期间不尝试 Save 覆盖或 Reload 丢弃。写失败、跨 placement/tile、
任意纹理/规则替换和下次 Play 碰撞检查仍是独立门禁。不能把本入口返回 0
解释为整个 S4 计划全部完成。

`editor_s4_test.py` 只检查验收器不会把缺阶段、未改变数据、整幅 dirty、
未收敛 stale、无旧任务拒绝证据、仅 alpha 变化、Undo GPU 不可逆、
Save 未落盘/manifest 缺失、伪造 preview 成功、冲突覆盖 live 数据、
Cook 修改作者快照、reopen 不一致、reload 未推进 generation、
异常退出或 validation 错误判为通过。
其中完整证据样例是人工构造数据，绝不作为 renderer 实测证据。
