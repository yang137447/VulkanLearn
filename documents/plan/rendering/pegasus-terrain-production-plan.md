# 飞马地形生产计划（Pegasus Terrain Production Plan）

## 文档状态

- 性质：**计划**（非契约）。当前行为以 `documents/contracts/terrain.md` 与
  `documents/contracts/texture-asset-json-v1.md` 为准。
- 范围：从 4 km 高度场生成到引擎内可加载的 16 块地形资产；不含散布、植被风与
  远景 HLOD（它们是后续轨道）。
- 相关文档：`houdini-pegasus-integration-feasibility.md`（接入可行性）、
  `terrain-openworld-demo-plan.md`（引擎侧 M0–M5）、
  `terrain-vt-streaming-design.md`（VT P0–P4）。
- 工具链文档：`tool/blender-terrain-import/HF_LIBRARY.md`。

## 0. 结论摘要

地形生产拆成 **8 个数据流阶段**（S1–S8）与 **6 个里程碑**（M-A…M-F）。
当前 S1–S5 已完成并可复现：造型在几何节点、水文在 CPU 求解器、资产 4×4 切片；
参数迭代约 3 s。剩余工作集中在引擎内验收（M-B/M-C）、派生掩膜（M-D）与
内容（M-E）。

2026-11-09 的高度场对标修正了这个结论的“造型已足够”部分：当前资产的
**绝对高差只有 Houdini 参考的约 1/4，坡度 p95 只有约 1/10，曲率 p95 只有约
1/18**。归一化后的形状统计接近会掩盖这个问题。证据、参数扫描和第一版调参
见 [Pegasus 高度场对标记录](../../rationale/pegasus-heightfield-comparison-record.md)。

> **2026-11-09 更正：** 上面的 1/4 是相对 **2 km bg43**
> （`HoudiniPegasus`，502.5 m 高差）得出的，不是 4 km 生产基准。
> 4 km 基准是 `D:\YYBWorkSpace\Pegasus\decoded\_ref4km`：147.7 m 高差、
> std 26.8 m、slope p95 11.7°。老 v1 为 123.7 m / 27.1 m / 12.0°，与 4 km
> 基准基本一致；v2 为 524.4 m / 144.1 m / 63.2°，是相对 4 km 基准的过冲。
> 继续做 4 km 生产时应以 `_ref4km` 为准，除非明确选择 2 km bg43 观感作为
> 另一个目标。

### 0.1 路线切换（2026-11-09）：改为 Houdini 导出直连

GN 复刻已经做到统计量对齐（relief −3.0%、std +2.6%、slope +7%、
curvature −14%），但 `anisotropy` 只有 **1.03**（Houdini 1.28），说明山脊/
谷线缺少主方向；继续逼近等于在 Blender 里重写 Houdini 的 worleyFB 骨架、
10 轮河道路径搜索和 `masknoise`/`distort` 中段。

因此 4 km 地形改为：

```text
Houdini 导出（height + water/debris/sediment）
  -> tool/houdini-terrain-import/prepare_pegasus_4km.py
  -> tool/houdini-terrain-import/prepare_pegasus_import.py
  -> split_pegasus_tiles.py -> assemble_pegasus_tiles.py
```

切片/LOD/VT/场景合并全部复用原链路。当前出货资产
`Generated/HoudiniPegasus4km/`（16 tile、5 层）。决定与数据映射见
[路线切换记录](../../rationale/pegasus-pipeline-pivot-houdini.md)。

下表 S2/S3/S3b 的“完成度”从此描述的是 **GN 对照实现**；出货地形的高度直接
来自 Houdini，不再有复刻误差。

> **2026-11-10 追加更正：** 当前 `_ref4km` 并不是飞马岛，而是
> `tool/houdini-terrain-import/export_pegasus_4km.py` 把
> `HFErodeExamples.hip` 的 1 km 侵蚀示例放大到 4 km 的结果（无海岸线、
> 147.7 m 低起伏）。岛的真实数据在 UE `Pegasus_Demo.uproject` 的 Landscape
> 和 `Pegasus_Landscape_Layers.bgeo.sc`。要先从 UE/Houdini 导出一份岛的
> height + 层，才能做海岛形态；证据见对标记录 §0.1。

## 1. 目标与边界

- 目标：一张 4 km × 4 km、1024² 源高度场，切成 4×4 个 1 km tile，
  在 VL 中可加载、可流式驻留、1080p 可交互。
- 不做：运行时地形编辑/重算、运行时侵蚀、VDB/云、HLOD actor streaming。
- 与 Houdini 的关系：Houdini 只作为统计口径与形态参考，**不作为逐像素基准**。
  算法按主流 LEM 论文实现（见 §3）。

## 2. 数据流阶段

| 阶段 | 内容 | 产物 | 状态 |
|---|---|---|---|
| S1 参考 | 解码 Houdini HDA 参数；导出 4 km 基准（erosion before/after + 4 层） | `decoded/_ref4km/global`、参数表 | ✅ 冻结 |
| **S0 Houdini 直连** | `prepare_pegasus_4km.py` 把高度 + water/debris/sediment 归一化成 5 层 | `Generated/HoudiniPegasus4km/` | ✅ **当前出货路径** |
| S2 造型（备用） | heightfield → noise×2 → distort | `heightmap_shape.png` | 🅿️ GN 对照实现，停止对齐 |
| S3 水文（备用） | 填洼 → 汇流 → 隐式 stream power → 坡面松弛 | `heightmap_gn.png` | 🅿️ GN 对照实现 |
| S3b 河流（备用） | 投影蜿蜒河道 → 切河床 → 双层 mask | `mask_gn_river*.png` | 🅿️ 无 Houdini 数据时的 fallback |
| S4 派生层 | water / debris / sediment / bedrock 掩膜 | `Layer_g_*.png` | ✅ Houdini 层已接入（5 层） |
| S5 打包 | 4×4 切片（1 px margin）、TR/MI/SC、层纹理数组 | 16 × `tile_XX/`、`SC_terrain_pegasus_4x4.json` | ✅ v2 一键链已接 |
| S6 引擎验收 | 加载、接缝、VT 驻留、LOD、碰撞 | 可运行场景 + 截图/统计 | ⚠️ 加载/VT/LOD 通过，碰撞与 1080p Release 未验 |
| S7 散布 | 散布点 → `VEG_*.json` → 实例化 | 树/石/草 | ⬜ |
| S8 表现 | 层混合调色、风、远景 | 最终画面 | ⬜ |

### S2 造型

- 节点组 `VL_HF_PegasusTerrain`：`Heightfield → Noise ×2 → Distort`。
- 输出密度（`Samples`，默认 1024）与工作密度（`Work Samples`，默认 512）分离；
  噪声/扭曲在输出密度评估，侵蚀在独立网络求解后把增量加回。
- 2026-11-09 复核：相对 **4 km `_ref4km`**，老 v1 为 123.7 m / 27.1 m /
  12.0° / 0.0180，Houdini 为 147.7 m / 26.8 m / 11.7° / 0.0169，量级基本
  对齐；相对 **2 km bg43**，当前 v1 只有 1/4 高差、1/10 坡度，但这不应直接
  作为 4 km 生产目标。
- v2 的 524.4 m / 144.1 m / 63.2° 匹配 2 km bg43，相对 4 km `_ref4km`
  高差 3.5×、坡度约 5.3×，是过冲；不能直接作为 4 km 出货资产。
- 2026-11-09 还修了一个 seed 接线问题：`VL_HF_Noise` 的 `Seed` 没进
  preset，两个 noise 实际是 seed=0，导致中心固定放射亮脊；现在已接
  `Noise Seed` / `Noise Distortion`，detail 使用 `seed+37`。候选
  `Generated/BlenderPegasusGN_v1_seed17`（seed 17、distortion 0、
  v1 高度量级）已经没有中心固定放射特征，下一步只需要补大尺度振幅。
- 最终 4 km 标定：`noise 450/2200 + detail 65/400`、seed 17、distortion 0、
  无 macro island、`K=3e-6`；shape 149.1 m / 29.1 m / 12.5° / 0.0176，
  eroded 149.1 m / 29.2 m / 12.6° / 0.0190，参考 `_ref4km` 147.7 / 26.8 /
  11.7° / 0.0169。资产
  `Generated/BlenderPegasusGN_v1_calibrated/`，`config.json` 已切到它。
- 疏密节奏第一版：`VL_HF_Noise` 加 `Mask`，preset 加低频 `Rhythm` mask；
  候选 `Generated/BlenderPegasusGN_v1_rhythm`（noise 420/2200 + detail
  110/400 + rhythm 1.0/1200）得到 shape 144.7 / 27.5 / 12.9° / 0.0186，
  eroded 144.7 / 27.5 / 13.0° / 0.0200。对比图
  `artifacts/rhythm/rhythm_final_compare.png`。`config.json` 已切到它。
  这只是低频 mask 近似；真正的 Houdini 节奏还需要多段侵蚀链路。
- 四段侵蚀第一版：在 rhythm 形状上跑 `--chain pegasus`，`K=5e-6`；
  候选 `Generated/BlenderPegasusGN_v1_rhythm_chain` 得到
  144.7 / 27.5 / 13.4° / 0.0227，参考 after 147.7 / 26.9 / 11.9° /
  0.0206。`1e-4` 条纹过强、`3e-6` 不可见，5e-6 为当前平衡点。
  `config.json` 已切到它；下一步在侵蚀间插入 `resample + masknoise +
  distort2/3`。
- 分辨率链已接入：`erode1/2 @512 → erode3/4 @256 + domain warp →
  upsample 1024`（`--chain-resample --chain-warp`）。候选
  `Generated/BlenderPegasusGN_v1_rhythm_resampled` 得到
  144.7 / 28.0 / 12.1° / 0.0094，参考 after 147.7 / 26.9 / 11.9° /
  0.0206。大尺度疏密/宽谷更接近 Houdini，但 256→1024 升采样让 curvature
  偏低；下一步补 detail restore 或 masknoise 的高频回填。
  `config.json` 已切到它。
- S4 派生层已按 Houdini 语义修正：`bedrock` 是侵蚀前 height 层，
  `debris/sediment` 是绝对米制后 0..1 clamp。对比图
  `artifacts/stage_compare/S4_blender_rhythm_resampled_masks_vs_houdini.png`。
  下一步把这些 mask 接进材质层，并按参考调整 water/debris/sediment 阈值。
- S4 材质层已接入：`Scree Debris` + `River Silt` 追加到 TR/MI，最终 6 层；
  本地 Megascans 只有 UE `.uasset`，先用 KiteDemo layer_05/layer_01 占位。
  detail restore 1.0 把 resampled 链 curvature 从 0.0094 拉到 0.0171；
  最终 144.7 / 27.8 / 12.7° / 0.0171。`config.json` 指向
  `Generated/BlenderPegasusGN_v1_rhythm_resampled/`。
  下一步：从 UE 导出 Megascans 源图替换占位、接 water 材质。
- 当前 HF producer 缺少旧 `build_pegasus_gn.py` 的 `_island_envelope` /
  `_island_spine`；没有它们时，噪声只能生成均匀碎脊，不能生成参考里的
  主山体和岛屿轮廓。

#### 关键节点与效果

| 节点 | 作用 | 主要视觉结果 |
|---|---|---|
| `heightfield` + `heightfield_project` + `maskbyobject` / `maskshrink` / `maskblur` | 投影道路/河流曲线并生成走廊掩膜 | 主河谷、道路和谷底走廊，后续侵蚀不会抹掉 |
| `heightfield_noise1`（worleyFB/hmfT，amp 200 / element 900） | 大尺度山体/山脊骨架 | 主山体、宽谷和岛屿的大形 |
| `heightfield_noise2`（sparse，amp 90 / element 280） | 中尺度细节 | 次级山脊、支脉和中等起伏 |
| `heightfield_distort1`（curl，amp 140 / element 800） | 扭曲采样坐标 | S 形山脊/谷线，去掉噪声团块感 |
| `heightfield_remap1/2` + `heightfield_erode1/2` | 高度重映射 + 水力侵蚀 | 主沟谷、树枝状水系、V 形谷、沉积 |
| `heightfield_resample1` + `masknoise1` + `distort2` | 改变分辨率、掩膜扰动、二次扭曲 | 打断尺度重复，增加中小尺度细节 |
| `heightfield_remap3` + `heightfield_erode3` | 继续侵蚀 | 细沟、支沟、冲积扇和碎屑坡 |
| `heightfield_resample2` + `masknoise2` + `distort3` + `heightfield_erode4` | 最后一轮细节/侵蚀 | 高频冲沟和最终表面粗糙度 |
| `remap_bedrock` / `debris` / `water` / `sediment` | 派生掩膜 | 岩石露头、碎石坡、河床、沉积色 |
| `blast_mask` + `attrib_unreal_material` | 导出层/属性 | 进入材质混合和散布 |

结论：**大山体来自 `noise1` + macro island/spine，主河谷来自
`heightfield_project` 和多轮侵蚀，冲沟/碎石坡来自后段 erode 与派生掩膜**。
只调噪声振幅只能让统计量接近，不能补齐这些结构。

### S3 水文

| 环节 | 方法 |
|---|---|
| 填洼 | priority-flood + ε（Barnes et al. 2014） |
| 汇流 | D8（O'Callaghan & Mark 1984）；MFD 可选（Quinn 1991） |
| 下切 | 隐式 stream power `E = K·A^m·S^n`（Braun & Willett 2013），`m=0.5, n=1` |
| 坡面 | 临界坡松弛（Roering et al. 1999 的离散形式） |

参数是物理量：`K [1/yr]`、`dt [yr]`、`steps`、`U [mm/kyr]`。当前默认
`K=3e-6`、`dt=1000`、`steps=12`，得到 rms 0.080 m、max 2.02 m 的高度差。
这个量级相对 500 m 级的参考地形几乎不可见；新造型烘焙后需要重新评估侵蚀量。

### S3b 河流（PE_heightfield_paths）

`water` 层是水力侵蚀的水深层，**不是**飞马的 authored 河流。河流在
`sop_PE_heightfield_paths.2.0`（Linescape）里独立生成：

```text
waypoints -> foreach × 10 (findshortestpath each-to-each)
          -> smooth / polypath / fuse -> sweep -> heightfield_project
          -> heightfield_blur -> remap1(河岸环) / remap2(河道核心)
          -> heightfield_cutoutbyobject 切河床
          -> copylayer 写回 mask / road / slope 层
```

Blender 侧实现 `tool/blender-terrain-import/river_paths.py`，位置在**侵蚀之后**
（Houdini 的 paths HDA 消费侵蚀输出，且自身只输出 mask；在侵蚀后切河床才
不会把河道剖面抹平）：

```text
heightmap_shape.png -> erode_pegasus_global.py -> heightmap_gn.png
  -> 正弦蜿蜒中心线 + 法线 ±45 m 谷底投影（authored waypoints 时退回 Dijkstra）
  -> corridor(110 m + 160 m 模糊, 归一化) / channel(60.5 m, 归一化)
  -> bank = corridor × (1 − channel)
  -> heightmap_river.png / mask_gn_river.png / mask_gn_river_water.png
  -> split -> assemble
```

当前结果（`Generated/BlenderPegasusGN_v1_rhythm_river/`）：

| 项 | 值 |
| --- | --- |
| 河网 | 主河 + 3 条支流（Houdini 是 10 轮 each-to-each，密度更高） |
| 切深 / 河宽 / 河岸 | 6 m / 110 m（水面核心 60.5 m）/ 160 m |
| 覆盖率 | 河道核心 3.62%，河岸环 7.71% |
| 材质层 | `River Bank` + `River Water`，TR/MI 共 **8 层** |
| 高度统计 | 143.2 m / 27.5 m / 12.3° / 0.0153（参考 after 147.7 / 26.9 / 11.9° / 0.0206） |

验收：`globalLayers=8`、`visibleLayers=8`、`starvedFrames=0`、
`groups=256 chunks=256`；截图 `artifacts/river_stage/river_far.png` /
`river_close.png`；对比图 `artifacts/stage_compare/S5_*.png`。

已知差距：河网密度低于参考；`River Water` 只是 Opaque 材质层，没有透明/折射/
反射；Megascans 仍是 KiteDemo 占位；河床是解析 V 形剖面而非几何切割。

### S4 派生层

需要补：`water`（河道水深）、`debris`（沉积物）、`sediment`（悬移质）、
`bedrock`（基岩出露）。掩膜是否参与材质分层见 §6 开放问题。

### S5 打包

- `split_pegasus_tiles.py`：4×4、每块 256 + 1 px margin，写出
  `tile_layout.json` 与每块的 `Source/heightmap_gn_tile.png` / masks。
- 资产身份：`TR_*.json` 的 `placementId = terrain|<name>`；layer key 跨 tile 一致。
- 2026-11-09 复核：`build_pegasus_tiled.py` 已改为
  HF producer `--no-erosion` →（可选）`river_paths.py` →
  `erode_pegasus_global.py` → split → assemble，一键脚本与实际出货链路一致。
  河流阶段的开关是 `--river/--no-river`、`--river-width-m`、
  `--river-depth-m`、`--river-bank-m`、`--river-branches`、
  `--river-waypoints`、`--water-texture-root`。

### S2/S3 第一版调参（2026-11-09）

```text
NOISE_AMPLITUDE_M   = 1050
NOISE_ELEMENT_M     = 900
DETAIL_AMPLITUDE_M  = 600
DETAIL_ELEMENT_M    = 200
```

在同一 1024² 造型阶段得到：高差 524.4 m、std 79.8 m、slope p95 67.6°、
curvature p95 0.285（参考 502.5 m / 100.5 m / 64.4° / 0.298）。
该组参数只解决“统计量”，还需要 macro island/spine 与后续多轮侵蚀解决形态。
**注意：这组参数对齐的是 2 km bg43，不是 4 km `_ref4km`（147.7 m /
26.8 m / 11.7°）；4 km 生产不能直接采用。**

第一版 v2 资产已经落到 `Generated/BlenderPegasusGN_v2/`，一键链为
`build_pegasus_tiled.py` → HF producer → `erode_pegasus_global.py` →
split → assemble。当前使用 `--erodibility 1e-4` 做视觉标定（LEM 默认
`3e-6` 在新山体上几乎不可见）。引擎证据见
[Pegasus 高度场对标记录](../../rationale/pegasus-heightfield-comparison-record.md) §8。
当前仍未解决：外围 0 高度方板没有水体/海面材质、山体内部缺少主山体/主河谷
结构、Pegasus 4×4 尚未做碰撞与 1080p Release 性能验收。

## 3. 里程碑与验收

| 里程碑 | 范围 | 验收口径 | 预估 |
|---|---|---|---|
| M-A 资产可复现 | S1–S5 | 同参数重跑产物一致；单次参数迭代 < 10 s；统计基线有记录 | ✅ 已完成 |
| M-B 资产可加载 | S6 前半 | 场景可加载；16 块边界高度一致、法线连续；`terrainlayerstats` 可解释 | 2–3 d |
| M-C 性能达标 | S6 后半 | 1080p ≥ 200 fps；VT 无持续抖动；常驻页 ≤ 预算 | 3–5 d |
| M-D 地表正确 | S4 + S8 前半 | 层过渡无接缝；水/河床/碎屑掩膜与高度一致；无 NaN/空洞 | 3–5 d |
| M-E 内容完整 | S7 | 同 seed 实例确定；树竖直、石贴地；draw call 可观测 | 1–2 周 |
| M-F 规模化 | 16 km（4×4 个 4 km） | 页表/atlas/预算在 16 km 下成立；加载时间可控 | 1–2 周 |

依赖关系：

```text
M-A(✅) ──► M-B ──► M-C ──► M-F
   │          │
   └─► M-D ───┴──► M-E
```

M-D 可与 M-B 并行；M-F 必须在 M-C 之后，否则规模问题会掩盖性能问题。

## 4. 入口与参数位置

```powershell
# 造型（改噪声参数时才需要重跑）
blender.exe --background --python tool\blender-terrain-import\build_pegasus_library_global.py -- ^
  --output-dir  <res>\Generated\BlenderPegasusGN\Global\Source ^
  --manifest    <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --layer-texture-root <res>\Generated\KiteDemo\Textures\Source ^
  --resolution 1024 --no-erosion

# 水文（改侵蚀参数只跑这一步）
python.exe tool\blender-terrain-import\erode_pegasus_global.py -- ^
  --manifest <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --cache-dir <res>\Generated\BlenderPegasusGN\Global\Cache ^
  --erodibility 3e-6 --dt-years 1000 --steps 12

# 切片
split_pegasus_tiles.py --source-manifest <...>\blender_gn_import.json ...
```

几何节点里已标注「造型层，水文在 `terrain_erosion.py`」；`Erosion*` 参数带
`[GN 实验侵蚀]` 前缀，不参与烘焙。

## 5. 验证口径

- 造型：std、slope_p95、curv_norm；对比参考时**先把参考块平均到候选格数**。
- 水文：高度差 rms / max / 分位分布；解算器 `_self_test`（洼地违例、热力质量）。
- 资产：切片边界逐样本相等；`terrainlayerstats` 的 `exactPages`/`requested`/`uploads`。
- 性能：`--perf-jsonl` 时间序列 + `gpustats`；mailbox 默认。
- **每做一个阶段都必须交付「产出效果 vs Houdini 对应阶段」的对比图**
  （不是节点面板 vs 产出）。2026-11-09 之后产出直接来自 Houdini，对比对象
  变成「引擎渲染 vs Houdini 导出」。规则：
  1. 两边用同一套 hillshade（`terrain_metrics.hillshade`）或同一套 Cycles
     设置渲染，差异只能来自数据；
  2. Houdini 侧必须写清用的是哪个 reference（4 km 用
     `decoded/_ref4km/global/*`；2 km bg43 只能作观感参考）；
  3. 每张图标注阶段名、参数和统计量（relief/std/slope p95/curvature p95）。
  当前示例：`artifacts/stage_compare/S2_blender_rhythm_vs_houdini.png`、
  `S3_blender_final_rhythm_vs_houdini.png`、
  `S4_blender_rhythm_resampled_masks_vs_houdini.png`、
  `S5_blender_river_vs_houdini.png`（以上为 GN 对照期）、
  `artifacts/houdini_ingest/houdini_ingest_compare.png`（切换后：Houdini
  高度 / Houdini 水系 / 引擎同一数据）。

## 6. 开放问题

1. 输出密度：1024（3.91 m/texel）是否足够，还是需要 2048。
2. S4 的掩膜是否参与 `M_terrain` 分层，还是只用于散布/植被剔除。
3. 16 km 规模：是 4×4 个 4 km tile（16×16 = 256 个 1 km tile），
   还是先做单一 4 km。
4. 碰撞：沿用 Jolt heightfield 的整块地形，还是按 chunk 分块。

## 7. 后续接入路线（2026-11-10）

4 km 地形已经走通「Houdini 导出 → 4×4 tile → 引擎」，VT mip 也按 page UV
分层。剩下的是内容、性能和规模，按依赖顺序分四段。

### A. 资产补全

| 项 | 现状 | 下一步 | 验收 |
| --- | --- | --- | --- |
| 材质贴图 | `Scree Debris` / `River Silt` / `River Bank` / `River Water` 仍是 KiteDemo + 占位水面 | 从 UE 导出 Megascans PNG，替换 `prepare_pegasus_4km.py` 里的 `layer_XX` / `water_*` 路径后重跑 split | MI 贴图路径指向 Megascans，`terrainlayer stats` 无变化 |
| 源分辨率 | Houdini 1024² / 4 km（3.91 m/texel） | 重新导出 2048² 或 4096²，`--tile-resolution 512/1024` | 近景网格间距 ≤ 2 m；顶点内存从 27.9 MB 增至 ~112/450 MB |
| 水面 | `River Water` 是 Opaque 材质层 | 二选一：湿润度调制（低 roughness）或独立透明 water pass | 河面有反射/颜色，且不破坏地形深度 |
| 河岸/基岩 | `River Bank` 用 gravel 占位 | Megascans 碎石/岩壁；`bedrock` 高度层可再导出一张露头 mask | 层过渡无接缝 |

### B. 散布接入（`PE_Unreal_Instances`）

Houdini 的散布 HDA 输出点属性与我们的 `VEG_*.json` 是 1:1 的：

| Houdini 属性 | VEG 字段 | 说明 |
| --- | --- | --- |
| `P` | `position` | 米 → cm |
| `orient` | `rotation` | `[x,y,z,w]`，需归一化 |
| `scale` / `pscale` | `scale` | 统一缩放 |
| `unreal_instance` | `variant` | 资产路径列表的索引 |
| `N` | — | 可用于贴地校正 |
| `unreal_foliage_attachment_*` | — | 暂不支持，先忽略 |

步骤：

1. Houdini 导出 `points.json`（`P/orient/scale/unreal_instance`）；
2. 新增 `tool/houdini-terrain-import/build_houdini_vegetation.py`，把每个
   `unreal_instance` 映射到 `SM_*.json` → `variant`，生成 `VEG_*.json`；
3. 场景里挂 `mesh` 对象消费 VEG（现有 `CypressGrove` 路径已验证）；
4. 校验：实例数、`meshDraws`、树贴地/尺寸、风相位。

### C. 引擎验收

| 项 | 命令/证据 | 通过标准 |
| --- | --- | --- |
| Play 碰撞 | `terraincollision stats` | Play 后 16 个 placement 都有 body，玩家不穿地 |
| 1080p Release 性能 | `--perf-jsonl` + `gpustats` | 1080p ≥ 60 fps，`pageBudget` 不持续超限 |
| VT 驻留 | `terrainlayer stats` | `targetMips` 分层、`fallbackSlots=0`、无持续 `pending` |
| 远景 | 截图 + `terrainchunk stats` | 4 km 边界外无空洞，LOD 切换无跳变 |

当前 1280×720 development 实测：远景 `targetMips=3789/11082/5387/657`、
`resident=425/512`、GPU ~0.9 ms。1080p Release 需要把页预算与上传队列一起
复测。

### D. 规模化（16 km）

当前 32×32 页表正好铺满 4 km（125 m/页），16 tile 已经是这张表的上限。
16 km 需要：

1. 多 Region 各自持有页表/atlas（或把页表扩到 128×128）；
2. tile 数 16 → 256，`pageTilesPerSide` 相应下调；
3. LOD 距离与 chunk residency 距离按 4× 重新标定。

在这之前不建议先做 16 km——会把页表和流式的问题掩盖在规模里。
