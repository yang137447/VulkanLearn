# Houdini / Pegasus 地形离线导入

这套工具按 Project Pegasus 的离线烘焙思路接入 Houdini 地形，不把
Houdini Engine/HAPI 作为运行时依赖：

```text
Houdini heightfield + masks
  -> 16-bit PNG + 8-bit VLP1 权重
  -> TR / SC / MI + texture arrays
  -> VulkanLearn 地形编辑器
```

> **注意（2026-11-05 起）。** 下文"数据来源"一节描述的 G66 导出 fixture
> **已停用**：那条路只是拿别人的烘焙结果，不是 Pegasus 的实现。当前的复刻
> 路线是解出 Pegasus 原始 Houdini 工程的节点实现，在 Blender 几何节点里
> 重建；见本文末尾的 **"Project Pegasus 原始工程解码"**。

> **2026-11-09 更新。** 几何节点复刻已停止（`anisotropy` 1.03 vs Houdini
> 1.28，属于结构差异而非参数差异）。当前出货路径是**直接接入 Houdini
> 4 km 导出**，见下面「Pegasus 4 km 导出直连」。GN 代码保留为对照实现。

## Pegasus 4 km 导出直连

输入目录（仓库外，不入库）：

```text
D:\YYBWorkSpace\Pegasus\decoded\_ref4km\
  manifest.json                     sizeM=4000, samples=1024, tileGrid=4
  global\height_after_height.png    最终高度 -62.61..84.78 m
  global\height_after_water.png     水深 0..1.535 m（22.9% texel 非零）
  global\height_after_debris.png    碎屑厚度 0..1.048 m
  global\height_after_sediment.png  沉积厚度 0..0.920 m
  global\height_after_bedrock.png   基岩面高度
```

注意 `height_after_<name>` 是 Houdini heightfield 的**层名**：`water` /
`debris` / `sediment` 是米制厚度，只有 `height` / `bedrock` 是高度。

一键生成 4×4 资产：

```powershell
$py = "D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\5.2\python\bin\python.exe"
$res = "D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources"

# 1) 单位换算 + 权重归一化 + manifest
& $py tool\houdini-terrain-import\prepare_pegasus_4km.py `
  --export-root D:\YYBWorkSpace\Pegasus\decoded\_ref4km `
  --resource-root $res `
  --output-relative Generated/HoudiniPegasus4km

# 2) 4x4 切片（复用 GN 路线已验证的脚本）
& $py tool\blender-terrain-import\split_pegasus_tiles.py `
  --source-manifest "$res\Generated\HoudiniPegasus4km\houdini_pegasus_import.json" `
  --resource-root $res --output-root "$res\Generated\HoudiniPegasus4km" `
  --tile-grid 4 --tile-resolution 256 --heightmap-margin 1 `
  --lod-levels 3 --lod-distances-m 1250 2500 --tile-size-m 1000 `
  --page-tiles-per-side 8 --asset-prefix houdini_pegasus_tile

# 3) 16 tile 合并成场景
& $py tool\blender-terrain-import\assemble_pegasus_tiles.py `
  --resource-root $res `
  --tile-layout "$res\Generated\HoudiniPegasus4km\tile_layout.json" --verbose
```

输出：

```text
Generated\HoudiniPegasus4km\
  Source\heightmap.png              # 归一化高度
  Source\Layer_g_{base,rock,water,debris,sediment}.png
  houdini_pegasus_4km_source.json   # 描述符（含原始导出来源）
  houdini_pegasus_import.json       # prepare_pegasus_import 产出
  tile_00..tile_15\ TR/MI/SC
  SC_terrain_pegasus_4x4.json       # 合并场景
```

remap 规则（Houdini 官方曲线未导出，暂用分位数定标）：

| 层 | 归一化 |
| --- | --- |
| `River Water` | `clamp(depth / 0.35, 0, 1)` |
| `Scree Debris` | `clamp(depth / 0.25, 0, 1)` |
| `River Silt` | `clamp(depth / 0.15, 0, 1)` |
| `Rock Face` | 坡度的 `smoothstep(p90, p99)`（缓地形上绝对角度不触发） |
| `Gravel Ground` | `1 − 其余四项`，再整体归一化 |

`prepare_pegasus_import.py` 会检查掩膜逐 texel 求和 ≈ 255；实测 253.6。

引擎验收（1280×720，Debug）：`globalLayers=5`、`visibleLayers=5`、
`resident=776/1024`、`missPages=0`、`groups=256 chunks=338 meshDraws=256`。
截图 `artifacts/houdini_ingest/houdini_far.png` 与 `houdini_top.png`。

## Project Pegasus 原始工程解码

官方 Pegasus 工程（`$PEGASUSPROJECT` 指向的那套）已下载解包到仓库外的
`D:\YYBWorkSpace\Pegasus\`，**不入库**（SideFX 教程资产有独立授权条款，
只能本地使用）。它交付的是 `.hip`/`.hda`，不是 FBX/glTF，所以第一步是
把二进制容器解成可读配方。

### 容器格式（实测，`houdini_archive.py`）

`.hda` 与 `.hip` 都是 Houdini 私有二进制，但结构一致：

```text
[INDX 索引段]               .hip 没有这一段，直接以 ".start" 开始
070707 + 70 位十进制 + 名字 + NUL + 内容      <- 一个"子记录"
（成员体可能被 gzip 包一层；TRAILER!!! 结束）
```

* 名字从第 76 字节开始、以 NUL 结束。索引段里的 `070707` 只有 16 位数字且
  没有名字，不会被当成子记录。
* 成员体有两种排布：明文（Pegasus 的 `.hda`）与 gzip 压缩（.hip 的
  `.OPs`、部分 `.def/.parm`），所以脚本对整文件"直接切"和"解压后切"两种
  都试，取成员更多的那一种。
* `.hda` 的节点名形如 `hdaroot/<node>.<init|def|parm|order|inp>`，
  子网络是 `hdaroot/a/b/<node>`；`.hip` 的节点名是 `obj/geo1/...`。

### 解码

```powershell
py -3 tool\houdini-terrain-import\decode_pegasus.py --check   # 只看统计
py -3 tool\houdini-terrain-import\decode_pegasus.py           # 写配方
py -3 tool\houdini-terrain-import\decode_pegasus.py --json    # 机器可读
```

输出 `D:\YYBWorkSpace\Pegasus\decoded\<asset>\`：

```text
network.txt   节点顺序、连线、每个节点的参数、VEX snippet
network.json 同内容的机器可读版本
```

当前解码结果（2026-11-05）：

| 资产 | 节点 | 连线 | VEX |
|---|---:|---:|---:|
| `sop_pe_landscape.1.0.hda` | 48 | 56 | 0 |
| `ProjectFiles_LandscapeCreation_HFErodeExamples.hip` | 404 | 439 | 0 |
| `Landscape_Workflow_01_Overview.hip` | 61 | 60 | 4 |
| `sop_PE_heightfield_paths.2.0.hda`（Linescape） | 82 | 96 | 4 |
| `sop_PE_Load_Landscape.2.hda` | 71 | — | 2 |
| `sop_PE_Unreal_Instances.2.0.hda` | 23 | — | 4 |

### 已知的 Pegasus 地形主链（`sop_pe_landscape.1.0.hda`）

```text
heightfield -> heightfield_project <- merge3(road/river 曲线 → polyextrude)
            -> heightfield_maskbyobject -> maskshrink -> maskblur  (mask)
heightfield_noise1 (worleyFB/hmfT, amp 200, element 900)  -> noise2 (sparse, amp 90, 280)
            -> heightfield_distort1 (curl, amp 140, element 800)
            -> heightfield_remap1 -> heightfield_erode1   (Hydro, freeze frame 5)
            -> heightfield_remap2 -> heightfield_erode2   (再侵蚀一轮)
            -> heightfield_resample1 -> masknoise1 -> heightfield_distort2
            -> heightfield_remap3 -> heightfield_erode3
            -> heightfield_resample2 -> masknoise2 -> heightfield_distort3
            -> heightfield_erode4
            -> remap_bedrock -> remap_debris -> remap_water -> remap_sediment
            -> blast_mask -> attrib_unreal_material -> output
```

Erode 2.0 的关键参数（两轮一致）：`hydro_erosionrate 1`、`hydro_bankangle 20`、
`water_spreaditers 140`、`rain_amount 0.9`、`rain_density 0.05`、
`rain_evaporation 0.04`、`debris_quant 0.25`、`debris_reposeangle 15`、
`thermal_cutangle 50`、`thermal_erosionrate 0.03`、`seed 4278`、
`dofreeze on` / `freezeframe 5`。

这套链的"四段 侵蚀 → resample → distort"就是 Pegasus 地表特征（冲沟 +
崩积锥 + 后期扰动）的来源，也是几何节点复刻时要逐段对应实现的部分。

## 数据来源（历史，已停用）

以下 G66 路径只保留为历史记录，不再作为复刻数据来源。

Pegasus 本身是 SideFX 的 Houdini + UE5 教程工程，不是可安装的运行时 SDK。
官方 `.hip`/`.hda` 需要 Houdini 商业或 Indie 授权；本机 Houdini 19.5
当前没有可用 license（`localhost:1715` 无授权服务），所以本工具只处理
**已经导出的真实数据**，不负责 cook `.hip`。

## 数据来源

当前 fixture 来自已有的 G66 工程，不是 Pegasus 官方资产：

```text
D:\YYBWorkSpace\G66\res\scene_bw\bigworld\export\
  Heightmap.png              4065x4065, 16-bit grayscale
  Layer_g_layer0..3.png      4065x4065, 16-bit grayscale, 归一化地表权重
```

世界尺寸和高度范围来自同目录的
`terrain_swap\ue_to_houdini\bg43.bgeo.sc`。`gstat` 可读到：

```text
height volume: [4065, 4065, 1]
Size:   2048.5, 512.707, 2048.5   (m)
Bounds: -2464.25, -462.525, 6815.75
        -415.748, 50.182, 8864.25
```

这些值写在 `houdini_pegasus_source.json`。脚本会先核对 PNG 分辨率与
`bgeoResolution` 一致，再生成导入清单，避免把不同 tile 的高度和尺寸拼错。

**许可提醒：** G66 数据只用于本地管线验证，不要随发行包分发。官方
Pegasus 教程资产有独立授权条款，不能因为“能下载”就当作可再分发资产。

## 生成资产

```powershell
py -3 tool\houdini-terrain-import\prepare_pegasus_import.py `
  --check-only

py -3 tool\houdini-terrain-import\prepare_pegasus_import.py

py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py `
  D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources\Generated\HoudiniPegasus\houdini_pegasus_import.json `
  --resource-root D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources `
  --verbose
```

输出位于资源 worktree：

```text
Generated\HoudiniPegasus\
  TR_terrain_houdini_pegasus_bg43.json
  SC_terrain_houdini_pegasus_bg43.json
  MI_terrain_houdini_pegasus_bg43.json
  Source\heightmap.png
  Source\terrain_houdini_pegasus_bg43_paint.vlpaint
  Textures\...
```

`Source\heightmap.png` 是 16-bit PNG；`paint.vlpaint` 是 VLP1，把 Houdini
的 `g_layer0..3` 直接烘成 2048² 的权重覆盖层。运行时不需要新增外部 mask
格式，现有 Paint 覆盖层路径就能消费这套权重。

## 当前限制

- 地面材质使用 `Generated\KiteDemo\Textures\Source\layer_XX_*` 的真实
  PBR 纹理作为占位。G66 XML 引用的原始 `.tga` 不在本机资源中；在找到可
  授权纹理前，不把占位纹理说成 Houdini/Pegasus 原始材质。
- fixture 只用了 4 张归一化 `g_layer` mask。`Layer_water`、`Layer_sediment`、
  `Layer_curvature` 等仍是导出备用数据，尚未映射成运行时层。
- 2026-11-04 的 Houdini 安装没有 license，因此没有从 `.hip` 重新 cook；
  世界尺寸与高度范围来自 bgeo 缓存元数据，而不是脚本内猜值。
- 这是离线烘焙管线，不是运行时 HAPI 接入。

## 验证

```powershell
.\build\bin\terrain_tests.exe
.\build\bin\editor_ui_tests.exe --gtest_filter=*Terrain*
py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py --self-test
py -3 tool\validate-docs.py
```
