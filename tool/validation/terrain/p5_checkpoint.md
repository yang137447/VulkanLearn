# P5 exporter / authoring checkpoint — 2026-11-21

实施进行中，**P5 未通过**。代码工作树的既有 dirty 成果保留；未提交、未清理旧 V1、
未运行 GPU/Cook/大 stage。管理者授权后已生成两个模式 Full16 V2 作者资产，
新的 CPU/路径/成本/协议 handoff 见
`artifacts/terrain-rvt-production/p5/euclid-main-handoff.md`。
最新 gen3 五阶段 closure、Noether 固定 ABI decoder、CPU 收据及等待 Euclid
snapshot lock 的状态见 `artifacts/terrain-rvt-production/p5/gen3-quality-checkpoint.md`。
此前 gen2 与早期 ABI/测试计数段落保留为历史检查点，以该最新记录为准。
后续 World2 正式入口、Coverage 默认值根修、Paint sidecar 审计及只读 ACL 方法见
`artifacts/terrain-rvt-production/p5/source-defaults-sidecar-world2-response.md`；
World2 仍使用 gen3 规则但 source hash 已变，不能混用旧五阶段诊断快照。

## 已实现

- `build_composited_fixture.py`：显式 V2 TR；共享 World 配方及双向邻接；
  新 manifest 输出域为 `VT/worldV2/<namespace>/manifest.json`。
  `--author-scene` 从现存场景的 height/Paint（含已编辑内容）复制到新作者命名空间，
  不覆盖原副本；已有目标目录拒绝复用。`--dry-run` 只读并打印来源和新路径。
- `prepare_full_delivery_stage.py` / `preflight_full_cook.py`：读取显式完整 World 配方，
  移除每 TR 自定预算的前提；复制 stage 的整个 config，包含冻结 JSON/sidecar。
- `publish_full_cook.py`：单个 World V2 闭包，原生 exporter 验证已安装包，
  对照私有 Cook manifest；内容对象先复制，最后原子切换 manifest。
  不再要求 16 个单 surface 全页包或每块 5468 页；不改旧 V1 发布。
- `accept_game_delivery.py`：V2 版本拒绝负例、旧 TR budget 字段负例、生产对象高度闭包。
  安装 verify 对损坏 root/coarse 拒绝；runtime 则必须在完整生产输入下重建。
  root 恢复需要原始 loading→GPU publication→rootReady 事件与未初始化 draw=0，
  不再把 corrupt-page/missing-page-package 统一要求 runtime nonzero。
  生产输入损坏属于拒绝 World。可选 fine-cache 禁用只记 not-applicable 并列独立验收范围，
  不算已执行的缓存损坏测试，也不免除 fresh residency / 未访问生产实测。
  这仍是辅助验收工具，不能代表完整 P5 通过。
- `run_full_runtime.py`：两种 worker 显式选择，支持独立包可执行文件；
  53 个覆盖机位分批，避免 Windows 命令行上限。输出 collected，不冒充 P0 质量通过。
- UE importer：移除 residentPages/heightBudgetBytes，独立 Landscape_00 和作者配方。
  Blender assembler：依据已有 split topology 生成完整 World 配方，所有 TR 共享发布路径。
- `rvt_native_adapter.py`：核对原始 P0 固定 SHA256、27 份证据及原始分布；
  验证真实 Boole 审计文件摘要；调用原生 exporter 全闭包验证；
  按 raw native events / 实际进程日志计算 case 条件，不接受人工 gate=true。
  `geometry_including_tes` 为完整 geometry GPU timestamp，p99 阈值 **3.754 ms**；
  不宣称独立 TES 子阶段。冷启动不宣称清 OS cache。
  可选磁盘缓存禁用时，删除/损坏 case 必须明确 not-applicable，不能算已执行通过。
- exporter：P0Frozen JSON 与 sidecar 随 Game 包，固定原始 SHA256；
  game-export-manifest 写入 terrainRvtFrozenSha256。校验器拒绝两文件一起放宽。
- P1 审计修复：runtime base/detail MI 与 production inputs 均按 manifest 父目录解析。
  闭包先转换为资源根相对路径供资产扫描，再整体随 World relocation 复制；
  不要求 Service 迁就资源根。回归放置资源根同名错误 MI，验证两份真正 MI 被选中；
  缺失 manifest-local MI 即拒绝，禁止从资源根回退。

## P0 证据

冻结原 SHA256：
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`。
原 JSON 的 pending 状态与 bytes 未改。

review receipt 使用 `independent-subagent-audit`，审计者 Boole，
绑定 `documents/rationale/terrain-rvt-p0-independent-audit.md`，
10776 bytes，SHA256
`269f69baf314deb7e200bb0470a880a3766010f8602426d668f794ce815e1fa4`。
没有用户批准的主张。

复核输出：`artifacts/terrain-rvt-production/p5/p0-adapter-audit.json`。
CPU exporter 构建/回归：
`artifacts/terrain-rvt-production/p5/exporter-rebuild.log`、
`exporter-tests.log`、`exporter-tests.xml`。
exporter CPU：77 项中 76 passed、1 skipped（Windows manifest symlink 无创建权限）；
新增基础/detail 同名诱饵和禁止资源根回退回归均通过。
本轮 Python validation unittest **85 项全部通过**，日志为 `python-tests.log`；
Blender 5.2 Python authoring 4 项通过；
UE importer `--self-test` 通过。后续修改以最新 CPU 日志为准。

## 最小真实 Full16 V2 生成命令

以下代码在指定工作树 PowerShell 中运行。初始 Coverage dry-run 输出在
`artifacts/terrain-rvt-production/p5/full16-v2-author-dry-run.json`。
随后两个模式 CPU 预检与生成已实际执行，最终收据为
`artifacts/terrain-rvt-production/p5/full16-v2-generation.json`；
目标 namespace 已存在，**不能重复生成覆盖**。
去掉 `--dry-run` 才写新作者资产，仍不生成任何替代材质像素。

```powershell
Set-Location D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn
$authorScene = 'Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json'
py -3 tool/validation/terrain/build_composited_fixture.py --new-v2 --full-island --virtual-resolution 8192 --weight-source CoverageAtlas --author-scene $authorScene --dry-run
py -3 tool/validation/terrain/build_composited_fixture.py --new-v2 --full-island --virtual-resolution 8192 --weight-source LocalLayerArray --author-scene $authorScene --dry-run
```

收到继续并获 GPU 执行窗口后，生成两个互不覆盖的作者/发布命名空间：

```powershell
foreach ($weightMode in @('CoverageAtlas', 'LocalLayerArray')) {
    py -3 tool/validation/terrain/build_composited_fixture.py --new-v2 --full-island --virtual-resolution 8192 --weight-source $weightMode --author-scene $authorScene
    if ($LASTEXITCODE -ne 0) { throw "Authoring failed: $weightMode" }
}
```

仅在二进制已完成 P1/P3 整合后，以下是实际私有 stage/Cook/导出的命令链。
这是待执行的命令，不是已有成品收据。stage/private/package 目录必须不存在。
不能把 CPU-only exporter 当作 shader warm 的 Game 二进制。

```powershell
$deliveryRoot = 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\p5-worldV2'
$frozenBinaries = (Resolve-Path build-vt-release/bin).Path
foreach ($weightMode in @('CoverageAtlas', 'LocalLayerArray')) {
    $sceneName = "Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_$weightMode/SC_composited_vt_4x4.json"
    $stagePath = "$deliveryRoot/$weightMode/stage"
    $privatePath = "$deliveryRoot/$weightMode/resources"
    $packagePath = "$deliveryRoot/$weightMode/game"
    py -3 tool/validation/terrain/prepare_full_delivery_stage.py --scene $sceneName --stage $stagePath --private-resources $privatePath --binary-directory $frozenBinaries --copy-inputs
    if ($LASTEXITCODE -ne 0) { throw "Stage failed: $weightMode" }
    Push-Location $stagePath
    try {
        & ./bin/main_editor.exe --terrain-vt-cook --initial-scene $sceneName --exit-after-frames 2
        $cookResult = $LASTEXITCODE
        [IO.File]::WriteAllText("$stagePath/cook.exit.txt", [string]$cookResult)
        if ($cookResult -ne 0) { throw "Cook failed: $weightMode" }
        & ./bin/main_game.exe --initial-scene $sceneName --shader-force-rebuild --exit-after-frames 2 --no-dev-ui
        if ($LASTEXITCODE -ne 0) { throw "Shader warm failed: $weightMode" }
        & ./bin/vl_export_game.exe --project-root $stagePath --binary-directory "$stagePath/bin" --flavor game --initial-scene $sceneName --output $packagePath
        if ($LASTEXITCODE -ne 0) { throw "Export failed: $weightMode" }
        & ./bin/vl_export_game.exe --verify-package $packagePath
        if ($LASTEXITCODE -ne 0) { throw "Installation validation failed: $weightMode" }
    } finally { Pop-Location }
    $env:VULKANLEARN_RVT_VALIDATOR = "$stagePath/bin/vl_export_game.exe"
    py -3 tool/validation/terrain/publish_full_cook.py --stage $stagePath --cook-exit "$stagePath/cook.exit.txt" --verified-package $packagePath --weight-source $weightMode --output "$deliveryRoot/$weightMode/publish-preview.json"
}
```

发布预览通过后，在管理者放行的执行窗口，将发布命令增加 `--publish` 并使用新的输出收据名。
不要预先改默认入口，也不要清退 V1。首先执行两个独立 Game 包各 worker1/2，
保存 raw timing、PID/WDDM、真实页身份、TES、对照图像和所有故障注入原始证据。

## 尚未完成 / 继续时必须处理

1. adapter 的语义校验器是证据消费者；当前 runtime 尚未输出其全部
   `[RVT_EVIDENCE]` 事件，故障注入/观测/全矩阵 runner 尚未接齐。
   没有事件会明确 incomplete，不接受队列空或 CPU estimated histogram 冒充采样质量。
   两种权重、两线程、全部 case 的真实可执行闭环仍未交付。
2. P0 原始目标、内存采样 gap、图像 ROI、page identity 和有效 mip 的完整自动门禁
   仍需对真实 runtime 数据联调，不能只相信 adapter 的 event 字段定义。
3. 已生成新 Full16 两模式作者资源，未 Cook、未发布、未打包真实全岛 Game，
   未取得视觉/性能证据。CPU fixtures 和原生 Store 验证不能替代这些。
4. 当前默认入口仍为既有 `Generated/.../Full4x4_R8192_Default/...`。
   最终必须迁移该默认场景的 TR 引用/发布配置及 config 默认入口的实际闭包；
   新 RVTv2 fixture 通过不等于默认已经切换。
   旧 authorHeight/Paint 副本及旧 V1 发布在此之前保持原状。
5. P5 退出须独立离线 Game、两权重/两线程、完整视觉性能证据和独立审核全部成立；
   之后才能按精确旧发布清单清退 V1。当前不宣称 P5 通过。
