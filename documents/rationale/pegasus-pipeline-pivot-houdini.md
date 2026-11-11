# 路线切换：GN 复刻停止，改为 Houdini 导出直连（2026-11-09）

> 状态：**决策记录，不是契约**。当前地形资产的行为仍以
> `documents/contracts/terrain.md` 为准；本文只解释为什么停止在 Blender
> 几何节点里逐项对齐 Houdini。

## 1. 决定

Pegasus 4 km 地形的高度、侵蚀和地表派生层**改为以 Houdini 导出为唯一
事实来源**。Blender 几何节点（`tool/blender-terrain-import/*`）保留为历史
参考和备用实现，但不再投入时间做“参数对齐”。

替换后的数据流：

```text
Houdini 导出（height + water/debris/sediment/bedrock）
  -> tool/houdini-terrain-import/prepare_pegasus_4km.py   # 单位换算 + 权重归一化
  -> tool/houdini-terrain-import/prepare_pegasus_import.py # 校验 + manifest
  -> tool/blender-terrain-import/split_pegasus_tiles.py    # 4x4 切片（复用）
  -> tool/blender-terrain-import/assemble_pegasus_tiles.py # 16 tile 合并场景（复用）
  -> TR / MI / SC + 32x32 VT 页表
```

切片、LOD、页表、场景合并和运行时路径**完全复用**原 GN 路线已经验证过的
代码，只有“谁产生高度和权重”这一层换了。

## 2. 为什么停

GN 路线（`Generated/BlenderPegasusGN_v1_rhythm_river/`）已经做到：

| 指标 | Blender | Houdini `_ref4km` after | 差 |
| --- | ---: | ---: | ---: |
| relief | 143.2 m | 147.7 m | −3.0% |
| std | 27.5 m | 26.8 m | +2.6% |
| slope p95 | 12.3° | 11.5° | +7% |
| curvature p95 | 0.0153 | 0.0177 | −14% |
| **anisotropy** | **1.03** | **1.28** | 结构差异 |

统计量已经对齐，但 `anisotropy` 说明山脊/谷线缺少主方向；`ridge_density ≈
valley_density` 说明谷不是水系切出来的。要继续逼近，需要重建的不是参数，
而是：

1. `heightfield_noise1` 的 worleyFB 各向异性骨架；
2. 10 轮 `findshortestpath` 的枝状河网；
3. `masknoise1/2 + distort2/3` 的中段扰动；
4. 四段 hydro erode 的水体/崩积行为。

这四项本质上是在 Blender 里重写一遍 Houdini。相比之下，Houdini 导出已经
包含最终高度和真实水系层，直接接入的成本低于继续对齐。

## 3. 输入数据

`D:\YYBWorkSpace\Pegasus\decoded\_ref4km\`（仓库外，不入库）：

```text
manifest.json                     sizeM=4000, samples=1024, tileGrid=4
global/height_after_height.png    最终高度（-62.61..84.78 m）
global/height_after_water.png     水深（0..1.535 m，22.9% texel 非零）
global/height_after_debris.png    松散碎屑厚度（0..1.048 m）
global/height_after_sediment.png  沉积厚度（0..0.920 m）
global/height_after_bedrock.png   基岩面高度（-62.96..84.80 m）
```

`height_after_<name>` 是 Houdini heightfield 的层名，不全是高度：`water`/
`debris`/`sediment` 是米制厚度，只有 `height`/`bedrock` 是高度。

## 4. remap 规则（本管线的唯一“创作”部分）

Houdini 的 `remap_water/debris/sediment` 曲线没有导出，所以这里用分位数
定标；这是当前最需要 Houdini 侧补数据的地方。

| 层 | 原始范围 | 归一化 |
| --- | --- | --- |
| `River Water` | 0..1.535 m | `clamp(depth / 0.35, 0, 1)`（p99≈0.364） |
| `Scree Debris` | 0..1.048 m | `clamp(depth / 0.25, 0, 1)`（p99≈0.225） |
| `River Silt` | 0..0.920 m | `clamp(depth / 0.15, 0, 1)`（p99≈0.129） |
| `Rock Face` | 坡度 | `smoothstep(p90, p99)`（4 km 缓地形上绝对角度不触发） |
| `Gravel Ground` | — | `1 − (rock+water+debris+sediment)`，再整体归一化 |

归一化后逐 texel 求和为 1，`prepare_pegasus_import.py` 的
“mask sum ≈ 255”门禁通过（实测 253.6）。

覆盖率（权重 > 0.5）：rock 3.5%、water 4.5%、debris 2.8%、sediment 3.6%、
base 83.7%。

## 5. 验证

资产：`Generated/HoudiniPegasus4km/`（16 tile、5 层、`SC_terrain_pegasus_4x4.json`）。

引擎（1280×720，Debug，`--frame-limit 60`）：

```text
[TerrainLayer] globalLayers=5 sourceSets=1 dedupedLayers=75
[TerrainLayer] visibleLayers=5 exactPages=5 starvedFrames=0
[Terrain]     resident=776/1024 feedbackHits=760 missPages=0 pending=0
[TerrainChunk] groups=256 chunks=338 meshDraws=256
              drawnByLevel=1:63,2:193
```

截图：`artifacts/houdini_ingest/houdini_far.png`（默认机位）、
`houdini_top.png`（3 km / 45°）。

与 GN 路线对比：地形不再有“复刻误差”，因为高度就是 Houdini 导出的同一份
数据；剩余差异只可能来自 PNG 量化（16-bit，约 0.002 m）和切片重采样
（1024 → 1027 → 259）。

## 6. 影响

保留：

- `split_pegasus_tiles.py` / `assemble_pegasus_tiles.py` / VT / LOD / 场景合并；
- GN producer 代码与文档（作为对照实现，不再作为出货路径）；
- `documents/rationale/pegasus-heightfield-comparison-record.md` 的全部对标记录。

停止：

- 继续调 GN 噪声/侵蚀参数去追 Houdini；
- `river_paths.py` 作为出货河流（保留为无 Houdini 数据时的 fallback）。

接下来：

1. 需要改地形时，在 Houdini 里改并重新导出，而不是改 Blender 参数；
2. `remap_water/debris/sediment` 的官方曲线需要从 Houdini 导出后替换分位数定标；
3. Megascans 贴图导出后替换 KiteDemo 占位；
4. Play 碰撞与 1080p Release 性能仍未验。

owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-09
