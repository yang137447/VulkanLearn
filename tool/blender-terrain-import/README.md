# Blender Geometry Nodes 地形 producer

离线 producer：Blender 生成高度场、推导掩码、保存可检查的 `.blend`，输出仍由
`tool/terrain-ue-import/build_vulkanlearn_assets.py` 转成 VulkanLearn 的 `TR/SC/MI`
和 VLP1。运行时不依赖 Blender。

## 组成

| 文件 | 作用 |
|---|---|
| `pegasus_gn_core.py` | 核心节点：域扭曲、海岸、fBm / billow / ridged / cellular / dune、岩层阶地 |
| `build_pegasus_gn.py` | 主入口：建图、求值、侵蚀、重标定、写 PNG16/mask/manifest/scatter |
| `terrain_erosion.py` | 纯 numpy 侵蚀：priority-flood 填洼 + MFD 汇流 + Braun-Willett 隐式 stream power + 沉积/抬升 + 热力 talus |
| `terrain_metrics.py` | 客观指标：径向对称度、各向异性、脊/谷密度、汇流长度、坡度分布 |
| `terrain_preview.py` | 无头预览：hillshade / slope / Cycles 3D / metrics.json |
| `png16_io.py` | 无 Pillow 的 16-bit PNG 读写 |
| `export_terrain.py` | 兼容路径：读外部 16-bit 高度图 |

## 算法

形状层使用主流地形合成原语，而不是单一噪声：

* **fBm** — Mandelbrot / Musgrave 分形布朗运动，5 个八度，gain 0.5。
* **billow** — `|2n-1|` 八度求和，圆润云团。
* **ridged multifractal** — `(1-|2n-1|)^2` 八度求和（Musgrave 1988），锐利山脊。
* **cellular** — Worley / Voronoi F1，喀斯特塔与火山锥。
* **dune** — 沿风向做各向异性拉伸后的 ridged 噪声。
* **domain warp** — Perlin 1985 / Quilez 式坐标扭曲，消除各向同性的“布纹感”。
* **coast** — 低频大陆噪声 + 岸向倾角 + 海湾噪声，海岸线不是圆盘。
* **strata** — 按高度相位加噪的岩层带，只在陡坡上生效。

侵蚀层（`terrain_erosion.py`）默认是 **Braun & Willett (2013) 隐式 stream power**，
汇流默认用 **Quinn (1991) MFD**：

1. `fill_depressions` — Barnes 2014 priority-flood + ε。
2. `_flow_weights` — MFD 按坡度的 `p=1.1` 次方分配流向；`flow_method="d8"` 仅保留作 A/B。
3. `_csr_flow_edges` / `_accumulate_from_weights` — 把汇流图压成按源点排序的 CSR，
   用高度从高到低的单遍扫描累积汇水面积。
4. `_topological_levels_multi` — 到出口的最长流级；每个接收者都严格更低，
   因此高度从低到高单遍扫描即可得到正确的拓扑层。
5. `_solve_implicit_network_multi` — 按流级从出口往上解
   `dh/dt = U - K A^m S^n`：`n=1` 是线性方程，每级一次向量化求解；其他指数用
   6 步 Newton。 incision 按 MFD 权重分摊到多个下游接收者，每级只依赖已经解出的
   下游节点，一遍拓扑扫描即可，
   **不再有旧显式解法的 `0.5 × 下游高差` 上限**。
6. `_deposit_sediment`（可选）— 把本步产生的泥沙沿同一张汇流图送到下游；
   平坦格按 `1 - exp(-cell / deposition_length_m)` 和坡度权重沉积，出口的剩余泥沙
   离开网格。`ErosionDiagnostics` 给出 produced / deposited / exported / uplifted
   体积，自检要求质量账平衡。
7. `thermal_erode` — 8 邻域休止角，欧氏距离权重，同步转移、质量守恒。

`uplift_m` 在每步求解前加到所有非出口格；出口保持为基准面。`pre_smooth_iterations`
+ `detail_restore` 仍保留为可选的“低频面侵蚀 + 高频贴回”，但 MFD 默认
`pre_smooth_iterations=0`，不再依赖它把 D8 的碎沟连起来。

侵蚀后 producer 再用物理宽度 `--boundary-width-m` 做一次海平面 taper，
把每条边的最后一段平滑压到 0；只做侵蚀前的 taper 会让边界格继续参与汇流，
导出后留下锯齿状竖墙。最终高度场重新计算 MFD 汇流面积，并按
`--river-percentile` 阈出 0..1 的河网 mask；`River Bed` 层使用 0..1 高度窗口
和 0..45° 坡度窗口，河道跨海拔仍然保留。

旧显式路径保留为 `method="explicit"`（`flow_accumulation` MFD + 封顶下切），
仅用于 A/B 对比。只依赖 numpy，可脱离 Blender 单独运行；512² × 10 轮
MFD + 沉积实测约 20s，纯 D8 约 11s；1024² 完整 producer 约 90s。
MFD 的汇流更分散，最大下切会低于 D8
（512² alpine 基线约 550m vs 576m），但不再出现单点尖沟。

## 地貌预设

`--terrain-type` 选择预设，权重决定哪些原语进入图：

| 预设 | 特征 |
|---|---|
| `alpine` | 内陆高山，锐脊 + 山体块，无海 |
| `coastal` | 海岸山脉、宽滩、海湾 |
| `canyon` | 高原 + 深切曲流，岩层带明显 |
| `dune` | 风成沙丘，强各向异性平行脊 |
| `karst` | 热带喀斯特塔、洼地 |
| `volcanic` | 火山锥群 + 熔岩台地 |

预设定义在 `pegasus_gn_core.py` 的 `PRESETS` 中，改权重即可派生新地貌。

## 运行

```powershell
$blender = "D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\blender.exe"
$res = "D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources"

& $blender --background --python tool\blender-terrain-import\build_pegasus_gn.py -- `
  --output-dir "$res\Generated\BlenderPegasusGN\Source" `
  --manifest "$res\Generated\BlenderPegasusGN\blender_gn_import.json" `
  --layer-texture-root "$res\Generated\KiteDemo\Textures\Source" `
  --resolution 1024 --size-m 4000 --height-scale-m 800 `
  --seed 17 --scatter-density 0.0024 --terrain-type alpine

py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py `
  "$res\Generated\BlenderPegasusGN\blender_gn_import.json" --resource-root $res
```

常用开关：

* `--no-erode` 跳过侵蚀（用于对比基础形状）
* `--erosion-iterations 10`、`--erosion-k 0.2`（隐式系数）
* `--erosion-flow-method mfd|d8`（默认 `mfd`）
* `--erosion-pre-smooth 0`（默认；MFD 通常不需要）、`--erosion-detail-restore 0.4`
* `--erosion-deposition 1.0`（0 关闭）、`--erosion-deposition-length-m 1000`
* `--erosion-uplift 0.0`（每步抬升米数）
* `--thermal-iterations 6`
* `--boundary-width-m 100`（最终海平面 taper 的物理宽度）
* `--river-percentile 98.5`（汇流面积进入河网 mask 的百分位）
* `--no-river-mask`（生成 4 层地表，不生成 River Bed 层）
* `--camera-position x y z`、`--camera-rotation pitch yaw roll`（写入 manifest，单位 cm/度）

## 验收

数值回归（MFD vs D8、沉积质量账、抬升、沉积长度尺度）：

```powershell
& 'D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\5.2\python\bin\python.exe' `
  tool\blender-terrain-import\terrain_erosion_tests.py -v
```

如果把该解释器路径传给 CMake 的 `VULKANLEARN_TERRAIN_EROSION_PYTHON`，
同一脚本也会注册成 CTest 项 `terrain_erosion_tests`。

### Pegasus 植被切片

资源里已有的 Bistro `FillOut_BushTree.glb` 含一棵完整的 Italian Cypress。
先用 Blender 抽出独立树原型：

```powershell
& $blender --background --python tool\blender-terrain-import\extract_pegasus_vegetation.py -- `
  --source "$res\Maps\SC_bistro_exterior_modular\Source\Models\bistro_modular\FillOut\FillOut_BushTree.glb" `
  --output "$res\Generated\BlenderPegasusGN\Vegetation\cypress.glb" `
  --metadata "$res\Generated\BlenderPegasusGN\Vegetation\cypress.metadata.json"
```

再按 GN scatter 点生成确定性场景（默认 122 棵、独立 TR/MI 的 `u_uvScale=128`）：

```powershell
& 'D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\5.2\python\bin\python.exe' `
  tool\blender-terrain-import\build_pegasus_vegetation_scene.py `
  --resource-root "$res"
```

生成：

```text
Generated\BlenderPegasusGN\Vegetation\SM_pegasus_cypress.json
Generated\BlenderPegasusGN\Vegetation\VEG_pegasus_cypress.json
Generated\BlenderPegasusGN\SC_terrain_pegasus_cypress_grove.json
Generated\BlenderPegasusGN\TR_terrain_blender_gn_pegasus_grove.json
Generated\BlenderPegasusGN\TR_terrain_blender_gn_pegasus_grove_mi.json
```

运行：

```powershell
.\build\bin\main.exe --no-dev-ui `
  --initial-scene Generated/BlenderPegasusGN/SC_terrain_pegasus_cypress_grove.json
```

当前切片只有 Italian Cypress；122 棵树由一个 `CypressGrove` mesh 对象消费
`VEG_pegasus_cypress.json`，叶/干两个 section 各产生一个 instanced draw。
`InstanceVertex.params` 已携带 `windPhase`/`seed`，但运行时还没有接入风；
草地、每实例 LOD 和逐实例剔除仍是后续工作。

快速核对 draw 数：

```powershell
.\build\bin\main.exe --no-dev-ui --worker-thread-count 1 `
  --initial-scene Generated/BlenderPegasusGN/SC_terrain_pegasus_cypress_grove.json `
  --console-command "terrainchunk stats" --console-command-delay 120
```

应在 `terrainchunk stats` 看到 16 个 terrain chunk 加 2 个 cypress section，
合计 `meshDraws=18`。

可视化验收：

```powershell
& $blender --background --python tool\blender-terrain-import\terrain_preview.py -- `
  --heightmap "$res\Generated\BlenderPegasusGN\Source\heightmap_gn.png" `
  --size-m 4000 --height-m 800 `
  --out-dir artifacts\gn_research\preview --name alpine
```

输出 `*_hillshade.png`、`*_slope.png`、`*_3d.png`、`*_metrics.json`。关键指标：

* `radial_symmetry` — 1 表示完美同心圆盘；改造前基线 0.91，当前约 0.77
* `anisotropy` — 山脊主方向性，越大越像山脉；基线 1.02，当前约 1.27
* `ridge_density` / `valley_density` — 两者接近说明是各向同性噪声，不是脊谷网络
* `drainage_length_km` — 连续谷线总长，衡量树枝状水系

### 2026-11-05 六地貌 1024² 默认参数（河网批次之前）

下表是加入 `River Bed` 层和侵蚀后边界 taper 之前、4 层地表的基线；
当前默认输出为 5 层，不能把下表当作带河网资产的验收数据。

| 预设 | anisotropy | drainage_km | slope_p50° | deposited / exported (M m³) |
|---|---:|---:|---:|---:|
| alpine | 1.88 | 1.96 | 35.2 | 174 / 195 |
| coastal | 1.15 | 1.43 | 22.2 | 112 / 200 |
| canyon | 1.14 | 3.84 | 16.6 | 91 / 263 |
| dune | 14.43 | 3.67 | 53.9 | 381 / 464 |
| karst | 1.08 | 1.17 | 27.6 | 116 / 211 |
| volcanic | 1.13 | 1.10 | 26.8 | 112 / 214 |

六个预设的侵蚀/沉积/输出质量账误差都 < 1e-15。对照图：
`artifacts/gn_biomes/contact_sheet.png`。coastal 东侧边缘仍有条带，后续应优先
检查海岸权重与边界 taper，而不是直接调侵蚀系数。

### 2026-11-05 河网与边界二次收敛

512² alpine 冒烟（`--boundary-width-m 400 --river-percentile 98.5`）：

* `mask_gn_river.png` 出现连续枝状主河道，不再是均匀噪声斑点；
* 侵蚀后边界重新 taper，hillshade 不再有侵蚀造成的单格锯齿竖墙；
* `flow_accumulation.png`、`mask_gn_river.png` 与 `River Bed` 层写入 manifest；
* 4 个地表层加 River Bed 共 5 层，转换器仍按现有 VLP1/top-4 路径消费。

1024² alpine 默认切片：河网 mask 覆盖 1.35% texel，预览指标
`radial_symmetry=0.7785`、`anisotropy=1.5659`、`ridge_density=0.0990`、
`drainage_length_km=2.577`；转换器输出 5 层 TR/MI/VLP1，引擎加载截图在
`VukanLearnResources/Generated/Screenshots/pegasus_gn_river_final.bmp`。

边界 taper 是矩形海平面过渡，不是海岸线建模；需要真正不规则海岸时应继续改
GN 的 `coast` 场，而不是把 taper 宽度无限调大。

## 已知边界

* 沉积是 transport-limitation 代理：按坡度和沉积长度尺度在平坦格回填，
  不是完整的泥沙输运/挟沙力求解；`deposition_rate` 仍是艺术参数。调大
  `deposition_length_m` 会把更多泥沙送出网格、保留更多河道；调小则填谷更明显。
* 抬升是均匀的 `uplift_m`，出口固定为基准面；没有空间变化的抬升场或岩性差异。
* MFD 比 D8 慢约 2×；1024² 资产需要更长侵蚀时间。
* 河网是汇流面积阈值 mask，不是水力求解、水面 mesh 或流量动画。
* 边界 taper 是矩形海平面过渡，不是海岸线建模；多 tile 拼接前需要改成周期性
  海岸/侵蚀，不能直接依赖单 tile 的边框 taper。
* 没有 VDB 云、HLOD、路网或水面；`dune` 之外没有风沙输运。
* 导出 scatter 点来自侵蚀后的高度场；`.blend` 里的 GN scatter 只作预览。
* 引擎侧 chunk LOD 已改为边界缝合，不再使用裙边；粗 LOD 的边界环固定用
  step=1 法线，内侧粗顶点按到边界的距离混向细法线，避免过渡带明暗线。
