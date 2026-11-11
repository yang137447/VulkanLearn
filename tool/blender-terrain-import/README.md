# Blender Geometry Nodes 地形 producer

离线 producer：Blender 生成高度场、推导掩码、保存可检查的 `.blend`，输出仍由
`tool/terrain-ue-import/build_vulkanlearn_assets.py` 转成 VulkanLearn 的 `TR/SC/MI`
和 VLP1。运行时不依赖 Blender。

## Pegasus 阶段 01：高度场基座 + 一轮侵蚀

这是当前复刻工作的进行中阶段（2026-11-05）。对应 Houdini
`sop_pe_landscape.1.0.hda` 的前半段。**算法全部落在几何节点里**，Python 只负责
建图与烘焙，不参与求值，所以所有参数都能在 Blender 界面里直接拖。

```text
Mesh Grid → Noise 1（大尺度山体）→ Noise 2（中尺度山脊）
          → Coast（径向 + 噪声扰动，岛外落回海面）
          → Distort（两个去相关噪声位移 UV）
          → Remap → Capture Attribute（把字段锁成数据）
          → Erode 一轮（8 邻域坡度 → 下坡流量 → stream-power 下切 → 热力塌落）
          → Set Position（归一化高度 × Height Scale）
```

```powershell
$blender = "D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\blender.exe"

# 生成 .blend（直接在 Blender 里打开就能看/改节点图）
& $blender --background --python tool\blender-terrain-import\build_pegasus_stage01.py -- `
  --blend artifacts\pegasus\stage01\stage01.blend

# 数值验收 + 预览图（hillshade / height / flow）
& $blender --background --python tool\blender-terrain-import\check_pegasus_stage01.py -- `
  --blend artifacts\pegasus\stage01\stage01.blend --preview-dir artifacts\pegasus\stage01
```

| 文件 | 作用 |
|---|---|
| `pegasus_stage01_gn.py` | 节点库：基座、侵蚀、socket 定位与连接工具 |
| `build_pegasus_stage01.py` | 建整张图、写 `.blend`；`--cut base/erosion/full` 逐级调试 |
| `check_pegasus_stage01.py` | 求值后打印高度/流量/海岸指标，输出预览 PNG |

打开 `stage01.blend`，对象的 Geometry Nodes 修改器上就是
`VL_Pegasus_Stage01_Heightfield_Erode`，15 个输入全部可调：

| 输入 | 默认 | 对应 Houdini 参数 |
|---|---:|---|
| Height Scale | 800 m | heightfield Size.y |
| Sea Level | 0.0 | — |
| Noise 1 Amplitude / Element | 0.62 / 2400 m | `heightfield_noise1` amp 200 / element 900 |
| Noise 2 Amplitude / Element | 0.16 / 280 m | `heightfield_noise2` amp 90 / element 280 |
| Coast Wobble | 0.10 | 海岸线扰动 |
| Distort Element | 800 m | `heightfield_distort` element_size |
| Flow Merge | 0.85 | 汇水并入比例 |
| Flow Exponent / Slope Exponent | 1.0 / 1.0 | stream-power `A^m S^n` |
| Erosion Strength | 0.35 | `hydro_erosionrate` 的相对比例 |
| Max Incision | 120 m | 单步下切上限 |
| Talus Angle / Thermal Factor | 50° / 0.5 | `thermal_cutangle` / `thermal_removalrate` |

验收读数（256²、8 km）：高度 -0.25..0.60（≈ -322..961 m）、海岸覆盖 16.5%、
`erode_flow` 单轮 1.0、岛外角区高度标准差 0。

### 已知偏差

* **侵蚀是近似**：Houdini 的 `heightfield_erode` 是 GPU 体积算例，带降水、水体
  输运与 `freezeframe` 迭代；这里用"下坡邻居流量并流 + stream-power 下切 +
  一次热力塌落"近似，保留树枝状沟谷，但不是逐位复现。`erode_flow` 单轮上限约
  `1/(1-Merge)`，长程汇水要靠多轮 Repeat（阶段 02）。
* **Distort 不是 curl**：Blender 4D 噪声没有 curl，用两个去相关噪声当偏移场。
* **海岸由程序生成**：原工程的海平面与岛屿轮廓来自 heightfield 底图。
* **单位**：属性存归一化高度层（≈0..HeightScale 米），坡度在下切前换算成米/米；
  早期直接用"归一化高度 / 米"得到 1e-4 量级坡度，下切恒为 0。

### Blender 5.x 节点图的三个坑（已在代码里处理）

1. `Set Position` 的 `Position`（绝对）与 `Offset`（相对）都是 VECTOR，按类型取
   socket 会取错，导致整张网格被覆盖成偏移量。
2. **字段在 `SetPosition` 之后会重新求值**：`coast`、噪声等依赖 `Position` 的
   字段必须在位移前用 `Capture Attribute` 锁成数据，否则岛外海面退化成噪声。
3. 海岸掩膜必须用 lerp 而不是"乘常数"，乘法会保留噪声幅度。

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

### Pegasus 4×4 PCG tile 场景

一键生成 4km 全局场、切成 16 个 1km tile、转换资产并合并场景：

```powershell
py -3 tool\blender-terrain-import\build_pegasus_tiled.py `
  --resource-root "$res" `
  --output-relative Generated/BlenderPegasusGN_v2 `
  --erodibility 1e-4 --verbose
```

该脚本现在按实际出货链执行：

```text
build_pegasus_library_global.py --no-erosion
  -> erode_pegasus_global.py
  -> split_pegasus_tiles.py
  -> assemble_pegasus_tiles.py
```

造型默认参数为 `noise 1050/900 + detail 600/200`，默认开启
`--macro-island`（岛屿轮廓 + 山脊）。`--erodibility 1e-4` 是当前第一版
可见侵蚀的视觉标定，也是 `build_pegasus_tiled.py` 的默认值；
`erode_pegasus_global.py` 自身的 LEM 默认仍是 `3e-6`（在 500 m 级新山体上
几乎不可见）。

`--seed` 现在会写入两个 noise 实例（detail 自动用 `seed+37`）；旧资产在
`VL_HF_Noise` 没接 `Seed` 时会固定使用 seed=0，并在中心留下 Perlin 放射
特征。需要接近 4 km `_ref4km` 的低起伏基准时，用：

```powershell
py -3 tool\blender-terrain-import\build_pegasus_tiled.py `
  --resource-root "$res" `
  --output-relative Generated/BlenderPegasusGN_v1_rhythm_resampled `
  --noise-amplitude 420 --noise-element 2200 `
  --detail-amplitude 110 --detail-element 400 `
  --rhythm-strength 1.0 --rhythm-element 1200 `
  --noise-distortion 0 --seed 17 --no-macro-island `
  --erodibility 5e-6 --chain pegasus --chain-k 1.0 `
  --chain-resample --chain-warp --chain-detail-restore 1.0 `
  --warp-seed 17 --verbose
```

`--rhythm-strength/--rhythm-element` 用低频 mask 控制 detail 的疏密带；
`--chain-resample` 让 erode1/2 在 512、erode3/4 在 256 上跑，再升回 1024；
`--chain-warp` 在两次分辨率切换处插入 domain warp。该组参数得到
144.7 m / 27.8 m / 12.7° / 0.0171（含 detail restore 1.0），参考
`_ref4km` after 为 147.7 m / 26.9 m / 11.9° / 0.0206。
派生层按 Houdini remap 语义输出：`bedrock` 是侵蚀前 height 层，
`debris/sediment` 是绝对米制后 0..1 clamp（不按峰值归一化）。
`build_pegasus_tiled.py` 会把 `Scree Debris`/`River Silt` 追加到全局
manifest，最终 TR/MI 为 6 层；本地 Megascans 只有 UE `.uasset`，本轮
使用 KiteDemo `layer_05/layer_01` 占位。

#### 河流阶段（`PE_heightfield_paths`）

`water` 掩膜是水力侵蚀的水深层，**不是** authored 河流。飞马的河流在
`sop_PE_heightfield_paths.2.0`（Linescape）里独立生成：waypoints →
foreach × 10 的 `findshortestpath`（each-to-each）→
smooth/polypath/fuse → sweep → `heightfield_project` →
`heightfield_blur` → `remap1`（河岸环）/ `remap2`（河道核心）→
`cutoutbyobject` 切河床。

对应实现 `river_paths.py`，在**侵蚀之后**执行（paths HDA 消费的是侵蚀网络
的输出；侵蚀前切河床会被下一轮侵蚀填平）：

```powershell
py -3 tool\blender-terrain-import\build_pegasus_tiled.py `
  --resource-root "$res" `
  --output-relative Generated/BlenderPegasusGN_v1_rhythm_river `
  --noise-amplitude 420 --noise-element 2200 `
  --detail-amplitude 110 --detail-element 400 `
  --rhythm-strength 1.0 --rhythm-element 1200 `
  --noise-distortion 0 --seed 17 --no-macro-island `
  --erodibility 5e-6 --chain pegasus --chain-k 1.0 `
  --chain-resample --chain-warp --chain-detail-restore 1.0 `
  --river --river-width-m 110 --river-depth-m 6 --river-bank-m 160 `
  --river-branches 3 --river-smooth 2 --verbose
```

流程变成：

```text
build_pegasus_library_global.py --no-erosion
  -> erode_pegasus_global.py   # 四段侵蚀，输出 heightmap_gn.png
  -> river_paths.py            # 投影 + 切河床 + 双层 mask
  -> split_pegasus_tiles.py
  -> assemble_pegasus_tiles.py
```

河流相关开关：

| 开关 | 默认 | 作用 |
|---|---:|---|
| `--river/--no-river` | on | 是否执行河流阶段 |
| `--river-width-m` | 110 | 谷底走廊宽度（切深按它分布） |
| `--river-water-width-m` | width × 0.55 | 水面核心宽度 |
| `--river-depth-m` | 6 | 河道中心切深 |
| `--river-bank-m` | 160 | 高斯河岸过渡宽度 |
| `--river-branches` | 3 | 自动支流数（Houdini 是 foreach × 10） |
| `--river-smooth` | 2 | Chaikin 平滑次数 |
| `--river-waypoints` | — | 自定义 `[[x,z],...]`（米）；省略时自动生成 |
| `--water-texture-root` | `Generated/PegasusWater/Textures/Source` | 水面贴图目录 |

输出三张关键图：

- `Global/Source/heightmap_river.png`：切过河床的最终高度（送进切片）；
- `Global/Source/mask_gn_river.png`：**河岸环** `corridor × (1 − channel)`；
- `Global/Source/mask_gn_river_water.png`：**河道核心**，给水面材质。

`_append_derived_layers` 会据此追加 `River Bank` 和 `River Water` 两层
（TR/MI 从 6 层变 8 层）。水面贴图由
`make_water_textures.py --output <res>\Generated\PegasusWater\Textures\Source`
生成；本地没有可用的 Megascans 源图时，这是占位。

注意：`M_terrain` 是 Opaque，`River Water` 只是低 roughness 的材质层，
不是透明水体；要真正的水面反射/折射需要独立的 pass。

输出：

```text
Generated\BlenderPegasusGN_v2\
  Global\Source\heightmap_gn.png
  Global\Source\mask_gn_*.png
  tile_00..tile_15\Source\heightmap_gn_tile.png
  tile_00..tile_15\TR_*.json / MI_*.json
  tile_layout.json
  SC_terrain_pegasus_4x4.json
```

`--reuse-global` 跳过全局造型与侵蚀；`--skip-erode` 只跳过侵蚀；
`--split-only` 只生成 tile 输入与 manifest，不跑 16 次资产转换。

切分规则：

* 全局高度/权重先按 `--heightmap-margin` 扩一圈再重采样；默认 margin=1，
  每个 tile 高度图为 259²（`gridResolution + 2*margin + 1`），相邻 tile
  共享边界高度。mesh 边界采样落在内圈，法线梯度可以读到另一侧 overscan。
* 每个 tile 为 1km、`gridResolution=256`、`chunkQuads=64`；默认
  `lodLevels=3`、LOD 阈值 1.25km/2.5km。粗 LOD 的边界缝合环仍按 LOD0
  间距生成，所以相邻 tile 即使选择不同 LOD，边界顶点和法线仍一致。
* `pageTilesPerSide=8`，16 个 tile 正好铺满 32×32 全局 VT 页表。
* 所有 tile 共享同名 layer key、第一份 texture arrays 和同一套 camera/environment；
  当前 v2 资产为 4 层，`terrainlayer stats` 显示
  `globalLayers=4 sourceSets=1 dedupedLayers=60`。
* 合并场景默认相机为 3 km 高、6.4 km 远、`far_clip=12 km`；否则 500 m
  级山体会把旧的 350 m 机位埋进地里。

当前限制：跨 tile 的 LOD 选择各自独立，依赖边界缝合环保证水密；没有统一的
邻接优先级或 HLOD。边界法线使用 heightmap overscan（`heightmapMargin=1`）。
v2 画面仍有岛屿外围 0 高度方板（water 未接入材质）和山体内部均匀碎脊；
下一步需要 macro landform/valley mask，而不是继续加噪声。

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
