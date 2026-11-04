# Blender Geometry Nodes 地形 producer

离线 producer：Blender 生成高度场、推导掩码、保存可检查的 `.blend`，输出仍由
`tool/terrain-ue-import/build_vulkanlearn_assets.py` 转成 VulkanLearn 的 `TR/SC/MI`
和 VLP1。运行时不依赖 Blender。

## 组成

| 文件 | 作用 |
|---|---|
| `pegasus_gn_core.py` | 核心节点：域扭曲、海岸、fBm / billow / ridged / cellular / dune、岩层阶地 |
| `build_pegasus_gn.py` | 主入口：建图、求值、侵蚀、重标定、写 PNG16/mask/manifest/scatter |
| `terrain_erosion.py` | 纯 numpy 侵蚀：priority-flood 填洼 + D8 汇流 + Braun-Willett 隐式 stream power + 热力 talus |
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

侵蚀层（`terrain_erosion.py`）默认是 **Braun & Willett (2013) 隐式 stream power**：

1. `fill_depressions` — Barnes 2014 priority-flood + ε。
2. `_d8_receivers` — 每格的 D8 最陡下降接收者与流向长度。
3. `_topological_levels` — 到出口的流级（按填充后高度降序一次遍历）。
4. `_solve_implicit_network` — 按流级从出口往上解
   `dh/dt = U - K A^m S^n`：`n=1` 是线性方程，每级一次向量化求解；其他指数用
   6 步 Newton。每级只依赖已经解出的下游节点，一遍拓扑扫描即可，
   **不再有旧显式解法的 `0.5 × 下游高差` 上限**。
5. `thermal_erode` — 8 邻域休止角，欧氏距离权重，同步转移、质量守恒。

`pre_smooth_iterations` + `detail_restore` 先在低频面上侵蚀（让 D8 汇流连成
长树枝），再把被平滑掉的高频细节按比例贴回。

旧显式路径保留为 `method="explicit"`（`flow_accumulation` MFD + 封顶下切），
仅用于 A/B 对比。只依赖 numpy，可脱离 Blender 单独运行；实测 512² × 10 轮约
11s、1024² 约 45–55s，最大下切从显式的 ~37m 提升到 ~100m。

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
* `--erosion-pre-smooth 6`、`--erosion-detail-restore 0.4`
* `--thermal-iterations 6`
* `--camera-position x y z`、`--camera-rotation pitch yaw roll`（写入 manifest，单位 cm/度）

## 验收

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

## 已知边界

* 隐式解已去掉下切上限，但没有沉积项与抬升耦合，长期地貌平衡不成立；
  `uplift_m` 只是预留参数。
* D8 在 8m 网格上仍会被高频细节干扰，`pre_smooth_iterations` 是经验补偿，
  不是真正的大尺度汇流模型。
* 没有 VDB 云、HLOD、路网、河流水面；`dune` 之外没有风沙输运。
* 导出 scatter 点来自侵蚀后的高度场；`.blend` 里的 GN scatter 只作预览。
* 引擎侧 chunk LOD 仍用裙边遮缝；正解是边界缝合或 CDLOD geomorph。
