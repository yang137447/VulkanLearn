# Full Pegasus 标准配置与验证入口

标准生产用户入口固定为：

`Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json`

路径保留 `Validation` 仅为稳定引用；此 Full16 场景是默认标准地形配置，
不是临时 probe。主线程负责 config/initScene 切换、运行时预算与材质兼容，
不得以旧2×2成品替代完整岛屿验收。

## 生成

```powershell
py -3 tool/validation/terrain/build_composited_fixture.py --full-island
py -3 tool/validation/terrain/check_composited_fixture_camera.py --scene Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json
```

16块8192²地形，每块192页槽位，独立base/detail MI与height/Paint副本；
原海面、环境、灯光保留，无C#角色。生成器所有模式仅产生新VT场景，
不再生成legacy scene/TR或旧layer-VT参数/atlas绑定。
退休distanceFade字段不写入作者MI；静态全视距是当前唯一生产语义。
coverage/source/height消费者仍保留。已有编辑副本不默认覆盖。

## 现有验证边界

- `editor_s4_prepare.py` 私有闭包清理退休作者字段，不修改源资源。
- `editor_s4_main.cpp` 不再读退休参数，checker/reopen/reload不再检查该字段。
- `editor_s4_test.py` 与 `runtime_stress.py --self-test` 是CPU检查，不启动renderer。
- `prepare_composited_s1_comparison.py` / `run_composited_s1_comparison.py`
  已退役，任何实际调用明确拒绝；旧收据仅供历史，不再生成/执行旧生产路径。
- 独立 `validate_composited_bake_gpu.py` / `composited_bake_gpu_check.cpp`
  保留作GPU数值oracle，不是第二套生产运行路径。
- `accept_game_delivery.py` 默认Full16/8192/192，资源观测使用terrainvt stats。

## 后续窗口参数（仅准备，当前不执行）

私有stage工具默认只解析引用/输出计划，不拷贝或启动任何进程：

```powershell
py -3 tool/validation/terrain/prepare_full_delivery_stage.py
```

已预检作者/海面/环境/图材质闭包147文件、207,926,891 bytes，
16份Cook待生成。输出目录计划为同topic `fs` / `fr`，
均要求新建且不得覆盖既有目录。当前最长作者资源路径197字符。
工具不从共享shader/spv复制cache。
最终Cook完整发布及Release冻结后，才显式复制：

```powershell
py -3 tool/validation/terrain/prepare_full_delivery_stage.py `
  --include-cook --copy-inputs --binary-directory build-vt-release/bin
```

Cook闭包重定位到私有资源的 `VT/<16-id>/`，仅修改私有TR packagePath；
manifest、依赖/coverage/height sidecar、payload文件名/bytes保持原样。
该工具不做Cook/hash验证通过宣称，实际协议验证由最终加载/导出/verify执行。
若目录已存在须检查来源并选择新短目录，禁止自动删除或覆盖。

源码/shader、材质schema、Full16资源预算和binary全部冻结后，
主线程先交出独占renderer窗口，再依次执行：

1. Full16预算负向载入，确认明确拒绝、无部分World发布。
2. 正式资源根的Full16新身份Cook，16个manifest/height/coverage闭包完整，
   确认静态材质参数兼容。预计材质payload约19.4GB，不把布局预算当实测成本。
3. 16地块冷启动、正常LOD/TES、全岛/近景/海面图及目标PID内存采样。
4. 独立stage资源根、shader/config/UI/game binary与marker，
   只在私有cache增量编译并warm，禁止写共享SPV。
5. 现包先核对身份，归档到同topic未占用的短目录；不删未知目录。
6. 正式包参数示例：

```powershell
py -3 tool/validation/terrain/accept_game_delivery.py `
  --scene Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json `
  --expected-surfaces 16 --expected-resolution 8192 --expected-resident-pages 192 `
  --packagepath D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/terrain-openworld/vt-game `
  --runid full16-standard-delivery --timeout 1800 --execute
```

工具本身不执行Cook或覆盖已有包；`--execute`只能在独占窗口使用。
filter/gen/encoding参数必须与最终冻结契约一致；容器版本不随采样策略臆改。
未获得最终窗口前只允许help/CPU checks，不启动renderer或Cook。

## Full16运行退出条件

capture_plan保留全岛far；mid=tile05，near=tile15，
额外tile00_near与cross_tile_corner（05/06/09/10共同角点）。
正常LOD/TES机位来自真实各tile高度中心，不以固定LOD0替代。
每项保留camera实际回显、terrainchunk的实际LOD及terrainvt全部16surface数据。

worker2移动采用有限步camera pose插值，另做远距离teleport；
再执行完整场景loadworld并核对新World generation、16surface与共享资源生存期。
每段停稳后检查队列/读入/CPU页缓存/过期页/读错误归零，
预算selected需求有界，无静止镜头持续IO循环。
运行时cache账本应满足冻结的GPU1GiB/CPU512MiB cap；
source数组另记，不把cache cap当整个进程峰值。
独立预算负向测试必须在部分World发布前失败；未测试不得声称通过。
全岛成本绑定该run自己的PID采CPU/WDDM Dedicated/Shared，
使用已有runtime_stress sampler；不得复用旧2×2成本数字冒充Full16。

## 冻结前CPU admission预检

```powershell
py -3 tool/validation/terrain/preflight_full_cook.py
```

当前作者数据检查32 MI无退休字段、与新schema参数/纹理槽匹配、
config默认Full、graph含terrainCompositedFeedback0..3，Cook为0/16。
预检按service逐surface admission、实际PNG源高度mip2 atlas布局、
coverage与513²normal/tangent帧、3-slot staging假设计算：

| 账本 | bytes |
| --- | ---: |
| 16面Cook GPU累计payload | 977,320,192 |
| 最高GPU预捕获admission | 984,968,592 |
| CPU surface/staging累计预留 | 268,435,456 |
| 最高CPU Cook工作区admission | 325,373,024 |
| GPU/CPU cap | 1,073,741,824 / 536,870,912 |

预检不推断所有TR都使用heightBudget的最大8MiB；
它由真实源PNG尺寸、有效height slices、全局layer key数计算常驻高度atlas。
纹理source共享、图像分配对齐、旧World退休重叠和driver资源不属于该payload cap，
必须另外采样，不把CPU估算称为实机加载通过。

## 最终build成功信号后的后台Cook命令

以下仅是预备命令，目前没有启动。先取得主线程最终build成功确认，
用 `prepare_full_delivery_stage.py --copy-inputs --binary-directory build-vt-release/bin`
复制完整冻结二进制和输入（首次Cook不加--include-cook）。
报告将保存在新stage根full-delivery-stage.json；校验其中resourcePath与Full场景。
输出必须使用新证据目录，禁止覆盖旧收据。

```powershell
$topic = 'D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/terrain-openworld'
$stage = Join-Path $topic 'fs'
$logs = Join-Path $topic 'VulkanLearn/artifacts/terrain-composited-vt/full16-standard'
if (Test-Path -LiteralPath $logs) { throw 'Evidence directory exists; inspect it before continuing' }
New-Item -ItemType Directory -Path $logs | Out-Null
$cook = Start-Process -FilePath (Join-Path $stage 'bin/main_editor.exe') `
  -WorkingDirectory $stage -WindowStyle Hidden -PassThru `
  -ArgumentList '--terrain-vt-cook','--initial-scene',`
    'Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json',`
    '--worker-thread-count','1','--no-dev-ui','--validation','--exit-after-frames','2' `
  -RedirectStandardOutput (Join-Path $logs 'cook.stdout.log') `
  -RedirectStandardError (Join-Path $logs 'cook.stderr.log')
$cook.Id | Set-Content (Join-Path $logs 'cook.pid')
```

通过持有的process检查退出并记录exit；逐tile必须5468页、16个manifest全部原子发布、
validationclean、clean teardown。任何错误先报具体日志，不绕过材质/schema/预算。
Cook在私有fr中写真实Full16派生产物，不复用2×2包；
私有TR的packagePath在首次Cook前就展平为VT/<16-id>/manifest.json，
防止Full深路径加64字符sidecar超出Windows路径上限。
sourceLogicalManifest→privateManifest映射在stage报告cookOutputLocations中，
仅改变私有输出路径，依赖身份按既有契约排除此输出字段。
后续导出器保留该安全短闭包。
退出0后立即在同stage运行w2全岛far机位的截图/terrainvt stats/terrainchunk stats，
先交用户全岛画面，再做跨tile压力与包流程；不在Cook过程中启动第二个renderer。
