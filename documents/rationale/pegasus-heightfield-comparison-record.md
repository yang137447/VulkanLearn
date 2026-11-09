# Pegasus 高度场对标记录（2026-11-09）

> 状态：**证据归档，不是契约**。本文记录一次高度场对标、参数扫描和由此得到的
> 造型决策。当前运行时/资产合同仍以 `documents/contracts/terrain.md` 为准。

## 0. 重要更正：两个 Houdini 参考不能混用

本文最初把 `Generated/HoudiniPegasus/Source/heightmap.png`（G66 bg43）当作
Houdini 参考。它其实是 **2 km、约 502.5 m 高差**的观感/导入参考，不是 4 km
生产计划的阶段基准。4 km 生产基准是仓库外的
`D:\YYBWorkSpace\Pegasus\decoded\_ref4km\global\`：

| 基准 | 尺寸 | 高差 | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `_ref4km/height_before_height.png`（4 km） | 4000 m | 147.7 m | 26.8 m | 11.7° | 0.0169 |
| `HoudiniPegasus` bg43（2 km） | 2048.5 m | 502.5 m | 100.5 m | 64.4° | 0.298 |

老 v1（`Generated/BlenderPegasusGN`）为 123.7 m / 27.1 m / 12.0° / 0.0180，
与 **4 km `_ref4km`** 基本一致；新 v2（`Generated/BlenderPegasusGN_v2`）为
524.4 m / 144.1 m / 63.2° / 0.167，匹配的是 **2 km bg43**，相对 4 km 基准
高差 3.5×、坡度约 5.3×，属于过冲。

因此：

- 4 km 生产计划应以 `_ref4km` 为阶段基准；v1 的高度量级是正确的，v2 只是
  “2 km bg43 观感”标定；
- v2 的 macro island、multi-pass 思路可以保留，但高度/坡度必须重新标定到
  `_ref4km` 量级，否则不能作为 4 km 出货资产。

### 0.1 `_ref4km` 是侵蚀示例地形，不是飞马岛（2026-11-10）

`_ref4km` 由 `tool/houdini-terrain-import/export_pegasus_4km.py` 生成：脚本加载
的是 `HFErodeExamples.hip` 的 `/obj/Erosion_Examples/heightfield1`，把
`sizex/sizey` 从 1 km 放大到 4000 m、`gridsamples` 提到 1024，再跑同一条侵蚀
链导出。manifest 里的 `freezeFrames=5 / tileGrid=4 / samples=1024` 与该脚本
默认值一致。

因此：

- 它不是 Pegasus 的岛屿 landscape，**没有海岸线**，也不是 4 km 生产基准；
- 它的 147.7 m 高差和低起伏形态只代表 SideFX 的侵蚀示例；
- 真正的飞马岛在 UE 工程
  `D:\YYBWorkSpace\Pegasus\LandscapeWorkflow\ProjectFiles_LandscapeWorkflow\Unreal\Pegasus_Demo.uproject`
  的 Landscape，以及
  `Landscape Heightfield Layers\Pegasus_Landscape_Layers.bgeo.sc`（Houdini 层）；
- 要复刻岛，需要从 UE 导出 Landscape heightmap（或从 Houdini 重新导出
  height + 各层），再按 8 km（8×8 tile）或多 region 切分。

本文下面所有 `_ref4km` 对比结论仍然有效——它们是“侵蚀示例地形”的对比，
不是“飞马岛”的对比。

## 1. 输入

| 项 | 当前 GN 4 km | Houdini 参考 |
| --- | --- | --- |
| 高度图 | `Generated/BlenderPegasusGN/Global/Source/heightmap_gn.png` | `Generated/HoudiniPegasus/Source/heightmap.png` |
| 源分辨率 | 1024² | 4065² |
| 世界尺寸 | 4000 m × 4000 m | 2048.5 m × 2048.5 m |
| `heightScale` | 123.694 m | 512.707 m |
| 用途 | 当前 `SC_terrain_pegasus_4x4` 的造型/侵蚀结果 | 仓库内 Houdini bg43 高度场参考 |

参考图的原始来源是 G66 bigworld bg43 的 Houdini 导出，只用于本地管线验证；
它不是 Pegasus 官方资产，也不随发行包分发。

## 2. 方法

- 两边都用双线性重采样到 1024²；比较**绝对高度**（`normalized × heightScale`）
  和各自的 0..1 归一化形状。
- `slope` 用各自世界尺寸计算的中央差分梯度；`curvature` 用梯度的散度绝对值，
  作为局部粗糙度代理。
- 归一化形状用 256-bin CDF 比较；`cdf_max_abs_diff` / `cdf_mean_abs_diff`
  用来判断高度分布是否相似，不用于判断绝对起伏。
- 脚本：`artifacts/heightfield_compare.py`（本次运行结果
  `artifacts/heightfield_compare.json`）。

## 3. 对标结果（最初使用 2 km bg43 参考）

1024² 网格：

| 版本 | 高差 | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| 当前 GN 4 km | 123.7 m | 27.1 m | 12.0° | 0.0168 |
| Houdini 参考 2 km | 502.5 m | 100.5 m | 64.4° | 0.2980 |

关键比值：

| 指标 | 参考 / 当前 |
| --- | ---: |
| 绝对高差 | **4.06×** |
| 绝对 std | **3.71×** |
| 归一化 std | 0.91× |
| `tan(slope p95)` | **9.9×** |
| curvature p95 | **17.7×** |

结论：当前高度分布（归一化后）与参考接近，但**绝对高差只有约 1/4，坡度和
曲率低一个量级**。只看归一化 std/slope 会把这个问题判成“通过”，这是本次
对标最重要的发现。

## 4. 参数扫描

扫描通过临时 driver 覆盖 `build_pegasus_library_global.py` 的模块常量，
每次只重跑造型阶段（`--no-erosion`），输出在 `artifacts/sweep/<name>/`。

| 变体 | Noise amp/element | Detail amp/element | 高差 | std | slope p95 | curvature p95 |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| `base` | 360 / 2000 | 65 / 400 | 123.7 | 27.1 | 11.9° | 0.017 |
| `freq` | 360 / 900 | 65 / 280 | 143.3 | 24.5 | 21.6° | 0.035 |
| `amp2_detail` | 720 / 900 | 195 / 280 | 307.0 | 50.2 | 42.1° | 0.083 |
| `amp4_detail_small` | 1440 / 900 | 520 / 200 | 616.8 | 101.4 | 67.0° | 0.263 |
| `tune_g` | 1050 / 900 | 600 / 200 | 524.4 | 79.8 | 67.6° | 0.285 |
| `tune_g` + macro island | 1050 / 900 | 600 / 200 | 524.4 | 150.4 | 64.7° | 0.218 |

`tune_g` 的高差、slope p95、curvature p95 与参考接近（参考分别为
502.5 m / 64.4° / 0.298）；std 仍偏低，说明高度分布仍比参考集中。

## 5. 形态结论

参数扫描只能证明“统计量可以追平”，不能证明“形态像飞马”：

- `tune_g` 得到的是**全图均匀的碎脊**；
- Houdini 参考是**少数大山体 + 中间主河谷 + 低地**；
- macro island envelope 能恢复岛屿轮廓，但内部仍然均匀，说明还缺大山体/河谷
  的 macro mask。

根因：

1. 新 HF producer（`build_pegasus_library_global.py`）只有
   `Heightfield → Noise ×2 → Distort → Erode`，没有旧
   `build_pegasus_gn.py` 的 `_island_envelope` / `_island_spine`。
2. 旧 `build_pegasus_tiled.py` 仍调用 `build_pegasus_gn.py`，没有走新的
   HF producer + `erode_pegasus_global.py` 链，导致“文档写的是新链、实际
   一键脚本跑的是旧链”的不一致。
3. 当前出货侵蚀只有一次，rms 约 0.08 m、max 约 2 m；参考的四次侵蚀链
   才是主河谷/冲沟/碎屑坡的来源。

## 6. 决策

第一版造型参数：

```text
NOISE_AMPLITUDE_M   = 1050
NOISE_ELEMENT_M     = 900
DETAIL_AMPLITUDE_M  = 600
DETAIL_ELEMENT_M    = 200
```

并执行：

1. 把 `_island_envelope` / `_island_spine` 接进 HF producer，Pegasus 4×4
   默认开启；
2. 让 `build_pegasus_tiled.py` 调用 HF producer + `erode_pegasus_global.py`
   + split + assemble，而不是旧 `build_pegasus_gn.py`；
3. 重新烘焙后再次对标，并跑引擎加载/接缝/VT/截图验收；
4. 后续再补主河谷走廊、water/debris/bedrock 掩膜和植被。

## 7. 现场证据

- `artifacts/pegasus_height_compare.png`：当前 / 参考 / `tune_g` 三联图；
- `artifacts/pegasus_height_compare_island.png`：`tune_g + macro island`；
- `artifacts/pegasus_height_compare_erode_k1e4.png`：侵蚀前后与参考对比；
- `artifacts/sweep/tune_g/`：造型输出、manifest、对比 JSON；
- `artifacts/heightfield_compare.py`、`artifacts/render_height_compare.py`、
  `artifacts/apply_macro_island.py`：本次分析脚本（`artifacts/` 被 gitignore）。

## 8. 第一版落地（2026-11-09）

已把第一版参数写进 HF producer，并把 4×4 一键脚本改成实际出货链：

- `build_pegasus_library_global.py`：默认 `noise 1050/900 + detail 600/200`，
  默认开启 `_island_envelope` / `_island_spine`（`--macro-island`）；
- `build_pegasus_tiled.py`：改为调用 HF producer + `erode_pegasus_global.py`
  + split + assemble，不再调用旧 `build_pegasus_gn.py`；
- `assemble_pegasus_tiles.py`：4×4 合并场景的相机改为 3 km 高、6.4 km 远、
  `far_clip=12 km`，避免新山体把默认机位埋进地里；
- 新资产输出：`Generated/BlenderPegasusGN_v2/`。

第一版烘焙使用 `--erodibility 1e-4`（相对 LEM 默认 `3e-6` 是视觉标定，
不是物理默认值），得到：

```text
造型: 高差 524.4 m, std 79.8 m, slope p95 67.6°, curvature p95 0.285
侵蚀后: 高差 524.4 m, std 144.11 m
```

引擎验收（`SC_terrain_pegasus_4x4` v2，1280×720，Debug 构建）：

```text
[Terrain] resident=841/1024 virtualPages=1024 feedbackHits=825
          missPages=0 pending=0 preloadDeferred=0
[TerrainLayer] pageBudget=512 visibleLayers=4 starvedFrames=0
               exactSlots=5930 exactPages=4
[TerrainChunk] groups=256 chunks=256 drawnByLevel=2:256
```

截图：`artifacts/v2/pegasus_v2.bmp`。画面上已经是一个有主山体的岛屿，
但仍有两个明显差距：

1. 岛屿外围是 0 高度的方板，没有水体/海面材质，所以看起来像“山体放在
   一块平板上”；water 掩膜尚未接入 `M_terrain`；
2. 山体内部仍是较均匀的碎脊，缺少参考里的少数大山体和主河谷；
   下一步需要 macro landform / valley mask，而不是继续加噪声。

性能：当前 `build/` 是 Debug（`NDEBUG` 未定义），`--perf-jsonl` 显示
`fps≈10`、`gpuTotalMs≈0.52`、`submitPresent≈366–514 ms`，只能证明功能
路径，不能作为 Release/1080p 性能结论。碰撞也尚未在 Pegasus 4×4 上做
Play 验收。

## 9. 4 km 阶段对比（Blender vs Houdini）

以下对比使用 **4 km `_ref4km`** 作为 Houdini 基准；Houdini 高度图在 Blender
里用同一套 hillshade / Cycles 3D 设置渲染，因此差异来自数据本身，不是渲染器。

### S2 造型

- Blender v1：`Generated/BlenderPegasusGN/Global/Source/heightmap_shape.png`
- Houdini：`_ref4km/global/height_before_height.png`
- 高差/std/slope p95/curvature p95：Blender 123.7 / 27.1 / 12.0° / 0.0180，
  Houdini 147.7 / 26.8 / 11.7° / 0.0169
- 对比图：`artifacts/stage_compare/S2_blender_v1_vs_houdini.png`

v1 的高度量级和坡度已经对齐，但空间形态不同：Blender 有一条更集中的亮山脊，
Houdini 是更均匀的低起伏块状地形。

### S3 侵蚀

- Blender v1：`Generated/BlenderPegasusGN/Global/Source/heightmap_gn.png`
- Houdini：`_ref4km/global/height_after_height.png`
- 高差/std/slope p95/curvature p95：Blender 123.7 / 27.1 / 12.0° / 0.0180，
  Houdini 147.7 / 26.9 / 11.9° / 0.0206
- 对比图：`artifacts/stage_compare/S3_blender_v1_vs_houdini.png`

v1 的侵蚀量和 Houdini after 接近；但 v1 的侵蚀只改变很小的高度差，
Houdini 的 after 层也主要是重新分配，真正的形态差异需要看 water/debris/
sediment 派生层。

### v2 的过冲（对照）

- Blender v2：`Generated/BlenderPegasusGN_v2/Global/Source/heightmap_gn.png`
- Houdini：`_ref4km/global/height_after_height.png`
- Blender v2 524.4 / 144.1 / 63.2° / 0.167；高差 3.5×、坡度约 5.3×
- 对比图：`artifacts/stage_compare/S3_blender_v2_vs_houdini.png`

v2 匹配的是 2 km bg43，不是 4 km 基准；不能直接作为 4 km 出货资产。

### S4 派生层

- Blender v1：`mask_gn_water/debris/sediment/bedrock.png`
- Houdini：`_ref4km/global/height_after_water/debris/sediment/bedrock.png`
- 对比图：`artifacts/stage_compare/S4_blender_v1_masks_vs_houdini.png`

差异：

1. Blender v1 的 water/debris/sediment 覆盖率明显低于 Houdini，Houdini 的
   水系和碎屑网络更密；
2. Blender v1 的 `bedrock` 是常量 1 掩膜，Houdini 的 `bedrock` 是高度场
   （`-62.96..84.79` 量级），两者语义不一致；
3. 图里的每张 mask 都按自身 min/max 归一化用于观察，绝对范围不同；不能把
   归一化后的亮度当成物理量相等。

## 10. 中心放射伪影：noise seed 没有接线（已修复）

用户在 Blender v1 S2 高度图中心看到的固定放射亮脊不是 Houdini 参考里的东西。
定位结果：

1. `VL_HF_PegasusTerrain` 里的两个 `VL_HF_Noise` 实例，其 `Seed` 输入没有
   接到 preset；实际一直是 `seed=0`；
2. `build_pegasus_library_global.py --seed 17` 只影响 macro island 和层掩膜，
   没有影响噪声；
3. Blender 的 `ShaderNodeTexNoise` 是 Perlin 基函数，`Distortion=0.1405`
   在 seed=0 时会产生固定的中心旋涡/放射特征；Houdini 的 `noise1` 是
   `worleyFB`，不会出现同一个固定图案。

修复：

- `VL_HF_PegasusTerrain` 增加 `Noise Seed` / `Noise Distortion` 参数，分别
  接到两个 noise 实例；detail 使用 `seed + 37` 做去相关；
- `build_pegasus_library_global.py` 增加 `--noise-distortion`，并把 `--seed`
  写入 noise；
- 新候选 `Generated/BlenderPegasusGN_v1_seed17` 使用
  `noise 360/2000 + detail 65/400 + seed 17 + distortion 0`、
  `--no-macro-island`、`K=3e-6`。

结果：

- 中心固定放射特征消失；S2 对比图
  `artifacts/stage_compare/S2_blender_v1_seed17_vs_houdini.png`；
- 112.5 m / 21.0 m / 10.9° / 0.0173（参考 `_ref4km` 147.7 / 26.8 /
  11.7° / 0.0169）；
- 坡度/曲率对齐，但高差和 std 仍低约 24%；下一步需要只增加大尺度振幅，
  不能再用提高高频 roughness 的方式补高度。

## 11. 4 km 最终标定（2026-11-09）

在中心伪影修复后，只提高大尺度噪声，不动 detail：

```text
noise amplitude 450 / element 2200
detail amplitude 65 / element 400
seed 17 / distortion 0
no macro island
K = 3e-6, dt = 1000 yr, steps = 12
```

结果：

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| Blender shape | 149.1 m | 29.1 m | 12.5° | 0.0176 |
| Blender eroded | 149.1 m | 29.2 m | 12.6° | 0.0190 |
| Houdini `_ref4km` | 147.7 m | 26.8 m | 11.7° | 0.0169 |

高差 +0.9%、std +8.6%、slope +6.8%、curvature +4%（shape）；中心固定
放射特征消失。最终资产：
`Generated/BlenderPegasusGN_v1_calibrated/`，`config/config.json` 已指向它。
阶段对比：
`artifacts/stage_compare/S2_blender_calibrated_vs_houdini.png`、
`S3_blender_calibrated_vs_houdini.png`。

## 12. 疏密节奏（rhythm mask，2026-11-09）

Houdini 的 detail 不是全图平铺：第 1/2 次侵蚀在主河谷集中细节，后面的
`resample + masknoise + distort2/3` 再把细节打散到不同区域。Blender 当前
的全局 Perlin detail 没有这个空间分布，所以看起来“均匀铺满”。

第一版修复：

- `VL_HF_Noise` 新增 `Mask` 输入；`VL_HF_PegasusTerrain` 生成低频
  `Rhythm` mask（`Rhythm Strength` / `Rhythm Element Size`）；
- mask 只乘在 detail 上，保持大尺度 noise 的轮廓；
- producer / tiled wrapper 新增 `--rhythm-strength` / `--rhythm-element`。

最终候选 `Generated/BlenderPegasusGN_v1_rhythm`：

```text
noise 420/2200 + detail 110/400
rhythm strength 1.0 / element 1200
seed 17 / distortion 0 / no macro island / K=3e-6
```

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| Blender rhythm shape | 144.7 m | 27.5 m | 12.9° | 0.0186 |
| Blender rhythm eroded | 144.7 m | 27.5 m | 13.0° | 0.0200 |
| Houdini `_ref4km` | 147.7 m | 26.8 m | 11.7° | 0.0169 |

对比图：
`artifacts/rhythm/rhythm_final_compare.png`（baseline vs rhythm）、
`artifacts/stage_compare/S2_blender_rhythm_vs_houdini.png`、
`S3_blender_rhythm_vs_houdini.png`。
`config/config.json` 已指向 `v1_rhythm`。

这仍然只是低频 mask 的近似；要真正接近 Houdini 的节奏，还需要把
`erode1/2 → resample1 → masknoise1 → distort2 → erode3 → resample2 →
masknoise2 → distort3 → erode4` 的多段链路接进来。

## 13. 四段侵蚀链（2026-11-09）

在 rhythm 形状上跑已有的 `--chain pegasus` 四段侵蚀，并用 5e-6 的基准
K 收住强度（1e-4 会出现明显方向性条纹，3e-6 又几乎不可见）：

```text
noise 420/2200 + detail 110/400 + rhythm 1.0/1200
seed 17 / distortion 0 / no macro island
erodibility K=5e-6 / chain pegasus / chain-k 1.0
```

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| Blender rhythm shape | 144.7 m | 27.5 m | 12.9° | 0.0186 |
| Blender rhythm + chain | 144.7 m | 27.5 m | 13.4° | 0.0227 |
| Houdini `_ref4km` after | 147.7 m | 26.9 m | 11.9° | 0.0206 |

高差/std 对齐，slope/curvature 高约 10–12%；水系驱动的细沟和疏密带比
单次侵蚀明显。最终候选 `Generated/BlenderPegasusGN_v1_rhythm_chain/`，
`config/config.json` 已指向它。对比图
`artifacts/stage_compare/S3_blender_rhythm_chain_vs_houdini.png`。

当前 chain 还没有在两次侵蚀之间插入 Houdini 的 `resample + masknoise +
distort2/3`，所以细节尺度仍偏均匀；下一步应把这三段插进 erosion pipeline。

## 14. Houdini 分辨率链（512/256 + domain warp，2026-11-09）

在 `erode_pegasus_global.py` 增加 `--chain-resample --chain-warp`：

- `erode1/2` 在 512 网格上跑；
- 降采样到 256，用两个低频随机场做 domain warp（amp 60 m / element
  1000 m，mask 强度 0.75），再跑 `erode3`；
- 再 warp 一次（amp 6 m / element 80 m），跑 `erode4`；
- 最后升回输入 1024，并把 `flow/debris/sediment` 层同步升采样。

候选 `Generated/BlenderPegasusGN_v1_rhythm_resampled`：

```text
noise 420/2200 + detail 110/400 + rhythm 1.0/1200
seed 17 / distortion 0 / no macro island
K=5e-6 / chain pegasus / chain-resample / chain-warp
```

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| Blender rhythm shape | 144.7 m | 27.5 m | 12.9° | 0.0186 |
| Blender resampled chain | 144.7 m | 28.0 m | 12.1° | 0.0094 |
| Houdini `_ref4km` after | 147.7 m | 26.9 m | 11.9° | 0.0206 |

坡度已经对齐；curvature 偏低是因为 256→1024 升采样抹掉了高频。优点是
大尺度疏密/宽谷更接近 Houdini，不再出现 1024 单次链的方向性条纹。
对比图：
`artifacts/chain_erode/chain_resampled_compare.png`、
`artifacts/stage_compare/S3_blender_rhythm_resampled_vs_houdini.png`。
`config/config.json` 已指向该候选。

## 15. 派生层 remap 对齐（2026-11-09）

按 Houdini `remap_bedrock/debris/water/sediment` 的层语义修正了
`erode_pegasus_global.py`：

- `bedrock`：4 km 参考 `height_after_bedrock` 是高度层，不是常量 1；
  现在写侵蚀前 shape 的归一化高度，和 `-62.96..84.79` 的参考量级一致；
- `debris/sediment`：按 Houdini `remap_*` 的 0..1 clamp，不再按峰值
  归一化；当前 resampled 链 raw max 约 3.5/3.4 m，clamp 后材质 mask 的
  上限与参考的 1.05/0.92 m 同一量级；
- `water` 仍用汇水面积模型并归一化到 0..1（参考 max 1.535 m）。

对比图：
`artifacts/stage_compare/S4_blender_rhythm_resampled_masks_vs_houdini.png`。
bedrock 现在已经是高度层；water/debris/sediment 的通道分布仍比 Houdini
更稀疏/更饱和，原因是求解器和参考不是同一 hydro 实现。下一步在材质里把
这些 mask 接到 River Pebbles/Rock Face 等层，并按参考调整阈值。

## 16. S4 材质层接入与 detail restore（2026-11-09）

已把两个派生 mask 真正接进 `M_terrain` 层表：

- `Scree Debris`：weight=`mask_gn_debris.png`，texture=`layer_05_*`；
- `River Silt`：weight=`mask_gn_sediment.png`，texture=`layer_01_*`；
- 加上原有 4 层，TR/MI 现在是 **6 层**，tile scene 重新切分/转换/合并；
- 本地 Megascans 目录只有 UE `.uasset`，没有可直接导入的 PNG/JPG，
  因此这两层先用 KiteDemo 碎石/泥土贴图占位；后续从 UE 导出 Megascans
  源图后再替换。

detail restore：resampled 链在 256→1024 升采样后回填 shape 高频残差
（`--chain-detail-restore 1.0`）：

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| rhythm shape | 144.7 m | 27.5 m | 12.9° | 0.0186 |
| resampled chain | 144.7 m | 28.0 m | 12.1° | 0.0094 |
| + detail restore 1.0 | 144.7 m | 27.8 m | 12.7° | 0.0171 |
| Houdini `_ref4km` after | 147.7 m | 26.9 m | 11.9° | 0.0206 |

最终资产 `Generated/BlenderPegasusGN_v1_rhythm_resampled/`（6 层）；
`config/config.json` 已指向它。对比图：
`artifacts/chain_erode/restore_compare.png`、
`artifacts/stage_compare/S3_blender_final_rhythm_vs_houdini.png`。

仍未闭环：water 还没有水体材质；Megascans 源图需要从 UE 导出；1080p
Release 性能和 Play 碰撞未复验。

## 17. 河流阶段：PE_heightfield_paths（2026-11-09）

### 17.1 Houdini 侧到底做了什么

`water` 层是 hydro 侵蚀的水深层，**不是** authored 河流。飞马把水系单独放在
`sop_PE_heightfield_paths.2.0`（Linescape）里；网络结构见
`D:\YYBWorkSpace\Pegasus\decoded\sop_PE_heightfield_paths.2.0\network.txt`：

```text
Waypoints / Points (starts, ends)
-> foreach(iterations = 10)            # 每轮换一组 cost 属性
     attribwrangle4 (cost_mode/scale/power/invert/inname)
   -> findshortestpath1 (multiplicity=eachtoeach, outputpaths=on)
-> merge -> smooth (strength 1.3) -> polypath -> fuse
-> sweep (球半径 25 扫成带状面)
-> heightfield_project (把带投影到高度场, sample 3, hitfarthest)
-> heightfield_blur (radius 10, iterations 10)
-> heightfield_remap1 (0->0, 0.79->1, 1->0)   # 河岸环
-> heightfield_remap2 (0->0, 0.80->0, 1->1)   # 河道核心
-> heightfield_copylayer1 (mask -> road 层)
-> heightfield_maskbyfeature(slope) -> copylayer (mask -> slope 层)
   输出 = 原始高度 (mask/road/slope 层) + paths/swept 几何
```

要点：

1. 河网来自 **10 轮 each-to-each 最短路径**，所以参考的最终地貌是
   **枝状多河网**，不是一条主河加一条支流；
2. `remap1` 生成**河岸环**、`remap2` 生成**河道核心**，两者是互补的两张
   mask，分别给河岸材质和河道/水面用；
3. 输出高度是**原样接出**的：`Heightfield:0 <- switch2 <- object_merge1 <-
   IN_Heightfield`。`heightfield_cutoutbyobject` 在节点网里存在，但没有连到
   输出分支，所以这个 HDA 自己的产出是「mask 层 + 路径几何」，真正把河床压
   下去的是下游 landscape 图或材质高度；本管线选择在 HDA 之后解析地切 6 m；
4. `road` 层是这套 HDA 的副产物（路径 mask 被复制到 `road`），不是道路。

### 17.2 Blender 侧实现

`tool/blender-terrain-import/river_paths.py`，在
`build_pegasus_tiled.py` 里位于**侵蚀之后**：Houdini 的 paths HDA 消费的是
侵蚀网络的输出，而且它本身只输出 mask 层（`Heightfield` 分支把输入原样
接出）。放在侵蚀之后切河床，河道剖面才不会被下一轮侵蚀抹平：

```text
heightmap_shape.png -> erode_pegasus_global.py -> heightmap_gn.png
-> river_paths.py
     · 自动曲线：高点到低点的正弦蜿蜒中心线（可换成 authored waypoints）
     · _snap_to_valley：沿法线 ±45 m 把中心线拉向谷底（heightfield_project 近似）
     · authored waypoints 模式走 8 邻域 Dijkstra（对应 findshortestpath）
     · corridor = 110 m 宽 + 160 m 高斯模糊，按峰值归一化到 1.0
     · channel  = 60.5 m 宽 + 模糊，归一化到 1.0
     · bank     = corridor × (1 − channel)     <- 对应 remap1 的环
     · 支流在求解后的主河上找最近点汇入，并做线段相交检测，避免编织环
-> heightmap_river.png（最终高度）/ mask_gn_river.png / mask_gn_river_water.png
-> split -> assemble
```

关键坑（已写进 `documents/rationale/pitfalls.md`）：

- 不归一化时，110 m 宽的线被 160 m 高斯模糊后峰值只有约 0.35，
  河床切深和材质权重都会被静默缩小；
- 在 8 邻域栅格上直接找最短路，缓地形上会得到 90°/45° 折线，
  必须“先画蜿蜒曲线，再投影”；
- 支流必须从**离汇合点最近的边界**进入，否则会横穿主河形成不可能的编织环。

### 17.3 烘焙结果

参数：

```text
noise 420/2200 + detail 110/400 + rhythm 1.0/1200
seed 17 / distortion 0 / no macro island
K=5e-6 / chain pegasus / chain-resample / chain-warp / detail restore 1.0
river: width 110 m / water core 60.5 m / depth 6 m / bank 160 m /
       branches 3 / chaikin 2 / valley snap 45 m
```

| 阶段 | relief | std | slope p95 | curvature p95 |
| --- | ---: | ---: | ---: | ---: |
| Blender 4 km final（四段链 + river） | 143.2 m | 27.5 m | 12.3° | 0.0153 |
| Houdini `_ref4km` after | 147.7 m | 26.9 m | 11.9° | 0.0206 |

覆盖率：河道核心 mask > 0.5 的 texel 占全图 **3.62%**，河岸环
**7.71%**（16 块 tile 统计；tile 05 的 `mask_gn_River Water_07.png` 非空）。

对比图：

- `artifacts/stage_compare/S5_blender_river_vs_houdini.png`
  （Blender 河网 / Houdini after erode / Houdini 成品彩色地貌）
- `artifacts/stage_compare/S5_river_masks_vs_houdini.png`
  （Blender 河岸环 / 河道核心 / Houdini water 层）
- `artifacts/river_test3/river_preview.png`（路径 + 两张 mask 预览）

### 17.4 引擎接入

新增两层（共 8 层）：

| 层 | weight map | texture |
| --- | --- | --- |
| `River Bank` | `mask_gn_river.png`（环） | KiteDemo `layer_06_*` |
| `River Water` | `mask_gn_river_water.png`（核心） | `Generated/PegasusWater/Textures/Source/water_*` |

`_append_derived_layers` 只在 mask 存在时追加；水面贴图由
`make_water_textures.py` 生成（暗青蓝 albedo、roughness 20/255、metallic 0）。

引擎验收（1280×720，Debug，`--frame-limit 60`）：

```text
[TerrainLayer] globalLayers=8 sourceSets=1 dedupedLayers=120 capacity=256
[TerrainLayer] visibleLayers=8 exactSlots=28665 exactPages=8
[TerrainLayer] starvedFrames=0 pageBudget=680
[TerrainChunk] groups=256 chunks=256 meshDraws=256
              drawnByLevel=1:62,2:194
[Terrain]     resident=766/1024 feedbackHits=751 missPages=0 pending=0
```

截图：

- `artifacts/river_stage/river_far.png`（默认远景机位，河网可见）
- `artifacts/river_stage/river_close.png`（1.6 km 俯视汇流口，河床/河岸清晰）

### 17.5 仍与 Houdini 的差距

1. **河网密度**：Houdini 是 10 轮 each-to-each 的枝状河网；当前 4 km 只有
   主河 + 3 条支流。参考成品地貌里河网密度高一个量级；
2. **水面是材质层，不是水体**：`M_terrain` 是 Opaque，`River Water` 只是
   低 roughness 的贴图层，没有透明、折射、法线流动和反射；画面里更像深色
   湿痕；
3. **贴图是占位**：Megascans 本地只有 UE `.uasset`，`River Bank` /
   `River Silt` / `Scree Debris` 仍用 KiteDemo 贴图；
4. **切河床是解析剖面**：Houdini 的 HDA 只输出 mask，实际下切方式取决于下游
   图；当前是 mask × 6 m 的平滑下切，横截面更接近 V 形而非 U 形。

## 18. 整体完成度快照（几何，排除材质）

同一套 `terrain_metrics` 口径，4 km / 1024² / cell 3.91 m：

| 数据 | relief | std | slope p95 | curvature p95 | ridge / valley | anisotropy | radial symmetry |
| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| Blender shape | 144.7 m | 27.5 m | 12.6° | 0.0172 | 0.139 / 0.139 | **1.03** | 0.717 |
| Blender eroded | 142.6 m | 27.4 m | 12.2° | 0.0153 | 0.139 / 0.138 | 1.04 | 0.713 |
| Blender final（+river） | 143.2 m | 27.5 m | 12.3° | 0.0153 | 0.139 / 0.138 | 1.03 | 0.716 |
| Houdini before | 147.7 m | 26.8 m | 11.4° | 0.0156 | 0.132 / 0.144 | **1.29** | 0.746 |
| Houdini after | 147.7 m | 26.8 m | 11.5° | 0.0177 | 0.139 / 0.136 | **1.28** | 0.745 |

结论：

1. **统计量已经对齐**：高差差 -3.0%、std +2.6%、slope +7%、curvature -14%。
2. **结构没对齐**：`anisotropy` 是 1.03 对 1.28——我们的山脊/谷线几乎没有
   主方向，接近各向同性噪声；Houdini 有明确的长条脊谷走向。这是“细节平铺、
   没有节奏”的量化表现，也是当前最大的一项差距。
3. `ridge_density ≈ valley_density` 同样说明各向同性；Houdini 侵蚀前
   valley 明显多于 ridge（0.144 vs 0.132），是水系切出来的谷。
4. `drainage_length_km` 这个代理在 4 km 低起伏上失效（3×3 局部高差
   约 0.12 m，低于该指标 0.216 m 的谷线门槛），本轮不计入结论。

阶段完成度（估计，几何口径）：

| 阶段 | 完成度 | 主要缺口 |
| --- | ---: | --- |
| S1 参考解码 | 100% | — |
| S2 造型 | 85% | 各向同性；缺 macro 山体/脊线方向 |
| S3 四段侵蚀 | 80% | 缺 `masknoise1/2` 的真实高频回填 |
| S3b 河流 | 70% | 主河 + 3 支流 vs 10 轮枝状河网 |
| S4 派生层 | 75% | 分布比参考稀/饱和，阈值未细调 |
| S5 打包 | 95% | — |
| S6 引擎 | 85% | Play 碰撞、1080p Release 未验 |
| S7 散布 | 0% | 本场景无植被/石块 |
| S8 表现 | 0% | 本轮排除材质 |

owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-09
