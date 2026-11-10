# 地形贴图打包规范（Terrain Texture Packing）

## 文档状态

- **定案日期**：2026-11-10
- **定位**：`plan/`，记录已确定的打包口径与落地进度；不是契约。
  置换 pass、全局图绑定、16-bit 源补齐落地后，再把「现在是什么」抽成
  `documents/contracts/terrain-texture.md`。
- **适用范围**：`M_terrain` 的层纹理、VT 页图集、全局宏图，以及后续的
  height blend / 细分置换。

## 0. 结论

1. 地形分层沿用「ID/权重 + top-4」的既有方案，**不接 UE 的 `T_Terrain_IDMAP`**。
2. 每层四张：`albedo` / `normal` / `orm` / `height`。
3. 高度**统一 16-bit**；AO 由高度用 heightfield HBAO 烘焙；地形不存 metallic。
4. 另有两张全局图：`colour`（大尺度 albedo）与 `masks`（AO + wetness），
   分辨率固定 2048²，不参与 VT。

## 1. 每层四张图

| 文件 | 格式 | R | G | B | A | 色彩空间 |
| --- | --- | --- | --- | --- | --- | --- |
| `<key>_albedo` | RGBA8 | albedo.r | albedo.g | albedo.b | 255（预留覆盖率） | **sRGB** |
| `<key>_normal` | RGBA8 | N.x | N.y | 255 | 255 | linear |
| `<key>_orm` | RGBA8 | **AO** | **roughness** | metallic（恒 0） | 255（预留） | linear |
| `<key>_height` | **R16** | 16-bit 高度（均值 0.5） | | | | linear |

约定与理由：

- ORM 的通道顺序与 UE / Substance 的 `OcclusionRoughnessMetallic` 一致，
  美术可直接用现成导出预设，不需要记我们的特殊顺序。
- 地形是电介质，`metallic` 恒填 0；保留该通道是为了不破坏 ORM 语义。
- `normal` 只使用 RG；引擎侧仍按 RGB（`tex.xyz * 2 - 1`）读取，
  B/A 写 255。后续若要上 BC5，只需把 Z 改成重建。
- AI 通道（albedo.A / orm.A）全部预留，当前没有任何语义。

## 2. 高度：16-bit、均值 0.5、两种用途

### 2.1 数值约定

- 磁盘：16-bit 灰度 PNG（`I;16`）。
- 打包：先按层内 `(v - min) / (max - min)` 满对比归一化，再整体平移，
  使均值落在 **0.5**。
- GPU：`R16_UNORM`。

### 2.2 两种用途

| 用途 | 公式 | 说明 |
| --- | --- | --- |
| Height blend | 直接用 `h` | 替换线性权重插值，避免层交界发糊 |
| 位移 | `(h - 0.5) × displacementM` | `displacementM` 是**峰峰值**（米），写在 TR 层字段 |

两者共用同一张高度，不再单独存 8-bit blend height，避免两套数据互相矛盾。

### 2.3 没有真实高度时

- 写平坦高度（全 32768）；
- TR 写 `"hasHeight": false`、`"displacementM": 0`；
- **禁止**拿 albedo 亮度或其他通道冒充高度。旧管线用 albedo 亮度顶 height，
  已废弃。

## 3. AO：由高度烘焙

使用 heightfield **HBAO**（horizon-based），在 1024² 打包分辨率上离线烘焙：

```text
worldH = (h - 0.5) * displacementM
texel  = repeatSizeM / resolution

每个方位角：沿高度场步进，取该方向最高地平线斜率
occlusion += saturate(horizonSlope / tan(maxSlope))

ao = 1 - mean(occlusion)
```

参数（`build_megascans_layers.py` 默认值）：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| 方位数 | 16 | 再高收益很小 |
| 步进 | 12 | 覆盖半径 |
| 粗尺度半径 | 8 m | 大结构 |
| 细尺度半径 | 1 m | 近景沟纹 |
| 组合 | `sqrt(ao_coarse × ao_fine)` | 两尺度几何平均 |
| 最大遮蔽坡度 | 60° | 超过即视为完全遮蔽 |
| 边界 | wrap（`np.roll`） | 保证平铺无缝 |
| `--ao-height-scale` | 1.0 | 只作用于 AO，可放大贴图起伏 |

只有 `hasHeight = true` 的层才烘焙 AO；其余层 `orm.R = 255`。

> 现状：改用 Poly Haven 的 16-bit displacement 后，AO 开始有意义
> （rock `aoMean≈0.991`、`aoMin≈0.4`），但平缓材质（沙/草地）仍接近 1。
> `--ao-height-scale` 默认 6，只影响 AO，不影响位移。

## 4. 全局宏图

| 文件 | 尺寸 | 格式 | 通道 |
| --- | --- | --- | --- |
| `T_global_colour` | 2048² | RGBA8 **sRGB** | RGB = 大尺度 albedo，A=255 |
| `T_global_masks` | 2048² | RGBA8 **linear** | R=AO，G=wetness，B/A=255 |

- 2048² / 4032 m = **1.97 m/texel**，只做宏观调制，替代不了 tiling 细节。
- 与层 AO 相乘：远景靠全局 AO，近景靠层 AO。
- wetness 用于压 roughness 与 albedo。
- `T_Terrain_IDMAP` 不打包、不接入。

## 5. 分辨率与存储

| 项 | 规则 |
| --- | --- |
| tiling 源 | 4096²（Megascans），打包到 **1024²** |
| VT 页 | 128²，图集 4096²（32×32 = 1024 物理页） |
| 1 K 层 mip | 8×8 → mip0..3，共 64+16+4+1 = **85 页/层** |
| 6 层预算 | **510 页 ≈ 512**；物理容量 1024（实测 `resident=442/512`） |
| height 存储 | **不进 VT**，6 层 1 K R16 常驻 ≈ 16 MB（含 mip） |
| 压缩 | 维持未压缩 RGBA8（与现有 atlas 合同一致） |

若以后升 2 K：2 K 层 = 341 页/层，6 层 2046 页 > 1024，放不下；
要么扩图集、要么只给主角层升 2 K，不能全层升。

## 6. 源数据与红线

源改用 **Poly Haven（CC0）**，不再依赖本地 Megascans 库（其高度只有 8-bit）：

| 层 | Poly Haven 资产 |
| --- | --- |
| Gravel Ground | `aerial_grass_rock` |
| Rock Face | `rock_face_03` |
| Scree Debris | `aerial_rocks_02` |
| River Silt | `brown_mud_dry` |
| Sand | `aerial_beach_01` |

下载与暂存：`tool/houdini-terrain-import/fetch_polyhaven_layers.py`
（diffuse / nor_dx / rough / displacement 4K；displacement 是 EXR，用
`exr_to_png16.py` 转 16-bit PNG）。选 `nor_dx` 而不是 `nor_gl`，是为了
沿用现有 `invertNormalGreen` 的 UE 法线约定。

打包时用 `--no-tint`：Poly Haven 是自然配色，不再做沙漠素材的重染。

红线：

1. 高度必须来自真实 displacement/height，不能用 albedo 亮度；
2. 没有 16-bit 高度的层不开位移（`displacementM = 0`）；
3. 全局图与 tiling 图的色彩空间不能混（只有 albedo/colour 是 sRGB）。

## 7. 管线入口

打包：`tool/houdini-terrain-import/build_megascans_layers.py`

```powershell
& $py tool\houdini-terrain-import\build_megascans_layers.py `
  --source D:\YYBWorkSpace\Pegasus\decoded\megascans `
  --output $res\Generated\Megascans\Textures\Source `
  --size 1024 --repeat-size-m 252
```

输出 `layers.json` 记录每层 `hasHeight / heightSource / displacementM /
roughnessMean / aoMean / aoMin`，供后续脚本读取。

TR 字段（已通过校验与透传，置换 pass 尚未消费）：

```json
{ "displacementM": 0.2, "hasHeight": true }
```

## 8. 待办

1. ~~16-bit displacement~~（已完成：Poly Haven 4K EXR → 16-bit PNG）；
2. 绑定 `layerHeight`（R16）并实现 height blend；
3. 细分置换：近场 tessellation（2 m 网格 → 4×4/8×8），shadow pass 同步，
   factor 在 chunk 边界保持一致；碰撞仍用 CPU 高度图，细部不参与；
4. 全局图 `colour` / `masks` 绑定进 `M_terrain`；
5. 图集压缩（BC5 normal / BC7 albedo/orm）与 2 K 主角层。
