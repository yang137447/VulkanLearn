# Houdini Project Pegasus（飞马计划）接入可行性

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 外部 DCC 管线接入的**可行性评估与实施计划**（未来工作，不是契约） |
| 状态 | 调研完成，未实现；本文只回答"接什么、怎么接、先做什么" |
| 修订 | 2026-10-31 第 1 版 |
| 结论 | 可行，但应接**离线烘焙管线**，不把 Houdini Engine 作为运行时依赖 |
| 关联 | [地形复刻计划](terrain-openworld-demo-plan.md)、[开放世界场景栈路线图](open-world-scene-stack-roadmap.md)、[地形合同](../../contracts/terrain.md)、[地形大世界层计划](terrain-large-world-layers-plan.md)、[天空与环境路线图](sky-pass-environment-roadmap.html) |
| 非目标 | 运行时 HAPI、VDB 体积云渲染、Nanite/meshlet、世界分区/HLOD 流式、运行时破碎 |

> 本文件是计划，不是契约。接入落地后，运行时可验证的行为写进
> `documents/contracts/*`；本文只保留来源、决策与尚未完成的工作。

## 0. 结论

1. **Pegasus 不是运行时 SDK**。它是 SideFX 用 Houdini + UE5 制作的 4km 开放世界岛屿技术演示，
   交付物是 `.hip`/`.hda`、教程和少量免费工具（Flora），以及一套"程序化生成 → 宿主引擎消费"的流程。
   直接"把 Pegasus 装进 VulkanLearn"不成立。
2. 可行的是**把 Pegasus 的产物离线烘焙成 VulkanLearn 已有/待补的资产格式**：
   heightfield + mask → `TR_*.json`；mesh → `SM_*.json`；材质 → `MI_*.json`；
   散布点 → 新的实例清单；贴图 → 现有 BC7/BC5 DDS/KTX2 管线。
3. 真正的卡点不在 Houdini，而在 VulkanLearn 侧：
   - 地形没有**外部 splat/weight map 导入**，当前权重由高度/坡度/noise 程序生成；
   - 没有 instancing/撒点，也没有植被承载类型；
   - 没有体积云/VDB 采样；现有天空是程序化渐变 + 太阳盘；
   - 没有 actor/HLOD streaming，地形只有材质页 VT 与加载期几何 chunk 驻留。
4. 最小可行切片（1km）：Houdini heightfield + 8 层 mask → TR + authored weight map；
   1k 棵树 + 5k 块石头（先 mesh 对象验证，再等 instancing）；一组 cliff 网格。
   VDB 云、Nanite 高模、运行时 RBD 不进入第一版。

## 1. Pegasus 实际交付物

官方页面把流程拆成两个阶段（来源：<https://www.sidefx.com/pegasus/>）：

| 阶段 | 内容 | 对 VulkanLearn 的可复用部分 |
| --- | --- | --- |
| 00 | 协作管线、Houdini Package、目录与版本控制 | 目录/版本约定；不需要代码 |
| 01–04 | Landscape Creation / Workflow / Material / Technical Deep Dive：Heightfield Erode、地形导入 UE、Megascans 材质、程序化路径与树木散布 | heightfield、mask/weight、layer 材质、散布点 |
| 05 | Cliff Generation：heightfield→geometry、切分 tile、置换、TOPs 批处理 | cliff/rock 网格与离线批处理 |
| 06 | Pile Tool：HDA + 刚体模拟生成堆叠/碎石 | 预烘静态堆叠网格；运行时破碎暂不做 |
| 07 | Nanite Props：PDG 把低模加工成高模变体 | 离线高模→LOD/减面，不使用 Nanite 运行时 |
| 08–09 | Architecture / Object Generation：建筑、石墙、DataTable 资产登记 | 建筑/道具网格与材质槽 |
| 10 | Clouds：Houdini 20 Cloud tools 生成 VDB 并导入 UE | 只能先烘 skybox/2D 云图；体积云另立计划 |
| 11 | Flora：原型植被建模工具（免费 `.hip`/`.hda`） | 树/草网格 + 后续 PP2/风数据 |
| Phase 2 | Landscape Color Map、Roads/Tracks/Rivers（Linescape）、Architecture 放置、Editor Utility Widgets | 颜色图/道路 mask 可烘进 weight；UE 编辑器工具不可直接复用 |

关键事实：

- Pegasus 教程工程的实际文件是 `.hip`/`.hda`/`.bgeo`/`.png`/`.tga`/`.exr` 与 UE 的
  `.uasset`/`.umap`；抽查的教程 ZIP 里**没有现成的 FBX/glTF/USD**。也就是说，
  即使走离线路线，也要自己用 Houdini 导出或写 bgeo/HAPI 读取器。
- Landscape 的主数据是 **ID Map + slope/valley mask + 颜色/遮蔽贴图**，不是 VulkanLearn
  的 256 层 catalog。转换器需要把 ID Map/通道组合成每层 mask，再映射到 layer key。
- HeightField Output 可以把 height/mask 写成图片：packed raster 最多 RGBA 四层，
  deep raster EXR 可多平面，扩展名决定格式，分辨率要求是 8 的倍数
  （来源：<https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_output.html>）。
- 官方页面的 Phase 1/2 教程反复使用 **Houdini Engine for Unreal** 与 UE 的
  Houdini Engine Public API、Editor Utility Widget。这些是 UE 宿主侧代码，
  VulkanLearn 不能直接拿来用（来源：<https://www.sidefx.com/pegasus/>）。
- Flora 工具以 `.hip`/`.hda` 形式免费发布，但仍是原型；官方明确提示早期不稳定
  （来源：<https://www.cgchannel.com/2024/02/download-free-custom-houdini-tools-from-project-pegasus/>）。
- Pegasus 的资产大量依赖 Megascans/UE 内容。VulkanLearn 的既有约束是
  **不搬 `.uasset`/UE-only 资产，贴图/模型只用 CC0 或确认许可的来源**
  （见 [开放世界场景栈路线图](open-world-scene-stack-roadmap.md) §5.4）。

## 2. 四条接入路径

| 路径 | 做法 | 优点 | 代价/风险 | 结论 |
| --- | --- | --- | --- | --- |
| A. 离线烘焙（推荐） | Houdini/PDG 导出 EXR/PNG、bgeo 或 GLB/FBX、JSON 清单；VL 侧转换器生成 TR/SM/MI/实例清单 | 无运行时依赖；可复现；可进版本库；不改变发行包 | Pegasus 原工程没有现成 glTF/FBX，需要自己导出；HDA 参数改动要重新烘；资产体积大 | **P0，先做** |
| B. 嵌入 Houdini Engine (HAPI) | 在 VL 编辑器进程内加载 `libHAPI`，cook HDA，读 geometry/attribute | 参数化、可在编辑器里调 HDA | 需要装 Houdini + license server；HAPI 是 C API 且要版本锁定；商业授权成本；线程/崩溃隔离复杂；发行版不能依赖 | **P2 再评估，只做编辑器工具** |
| C. USD 中间层 | Houdini Solaris 导出 USD/USDZ，VL 侧用 OpenUSD 或离线 flatten | 标准化分层、instancing/variant、材质绑定 | 引入 OpenUSD 大依赖；材质映射仍要自己做；当前收益不足 | **P1 做一次 spike，不默认采用** |
| D. 直接读 bgeo/geo | 自己解析 Houdini 原生几何 | 保留全部属性 | 解析器/HDK 维护成本高；只有离线价值 | **不推荐**，A 的 GLB + JSON 已够 |

HAPI 的事实依据：

- 原生应用需要链接 `libHAPI.lib`，头文件在 Houdini 安装目录的
  `toolkit/include/HAPI`（来源：<https://www.sidefx.com/docs/hengine/_h_a_p_i__integration.html>）。
- 该 import library 是 MSVC 产物；VulkanLearn 当前用 MinGW Makefiles，
  直接链接需要先做兼容性验证。更稳妥的形态是**独立 cook 进程**（HARS/named pipe），
  输出 bgeo/自有 cache，再由渲染进程离线读取；不让渲染进程依赖 Houdini DLL 与许可证。
- 授权检查顺序是 Houdini Engine → Houdini Core → Houdini FX → Houdini Engine Indie →
  Houdini Indie；**Apprentice 不支持，且 Houdini Engine Indie 只能用于授权插件
  （Unity/Unreal/Maya/Max/C4D）**（来源：<https://www.sidefx.com/docs/hengine/_h_a_p_i__licensing.html>）。
  这意味着自研 C++ 宿主通常需要商业 Houdini Engine/Core/FX 授权，不适合作为学习项目的默认路径。

## 3. VulkanLearn 现状与 Pegasus 产物的映射

| Pegasus 产物 | VulkanLearn 承载 | 现状证据 | 缺口 / 决策 |
| --- | --- | --- | --- |
| Heightfield（高度层） | `TR_*.json -> heightmapPath`，加载期生成 CPU chunk 网格 | `documents/contracts/terrain.md` §TR JSON/高度图；`source/terrain/terrainAssetLoader.cpp` | 只接受 **RGBA float EXR/HDR** 或 **8-bit PNG/BMP/TGA/JPG** 等；Houdini 的 packed raster（最多 RGBA 四层）与单通道 R16/R32 EXR 都需先转成 VL 能读的 RGBA。Houdini 输出侧要求分辨率是 8 的倍数；**VL 本身只要求 ≥2×2 并双线性采样**，不要把 8 的倍数写进 VL 校验。`gridResolution <= 4096`，4km 要拆多块 TR |
| Heightfield masks / splat | 层 ID/权重图（top-4） | `source/terrain/terrainWeightMap.cpp:121` 按 height/slope/noise 程序生成；TR 无外部权重路径 | **P0 新能力**：TR 增加可选 `weightMapPath`/`layerMasks`，builder 优先采样外部 mask，再归一化 top-4；无 mask 时保持现状 |
| Landscape ID Map / color map | 层 ID/权重 + 层 albedo | `M_terrain` 用 layer albedo/normal/pbr 数组 + VT atlas；UE 侧是 ID Map + Tint Table | 没有独立 color map/ID 槽；建议把 ID Map 转成层 mask，color map 烘成层 albedo 或额外 weight，不新增运行时材质路径 |
| Cliff / rock / architecture / props | `SM_*.json` + `MI_*.json` | `source/mesh/loader/mesh/assimpSourceAdapter.cpp:72`；`documents/contracts/asset-organization.md` | Assimp 可读 FBX/OBJ/glTF/GLB；需离线统一到 cm、Y-up，并核对材质槽顺序。静态网格没有运行时 LOD |
| Flora（树/草） | `SM_*.json` + 未来 PP2/风 | `documents/plan/rendering/open-world-scene-stack-roadmap.md` §2.3 | PP2 与植被风均未实现；第一版只做静态树，风后置 |
| Scatter points（`orient`/`pscale`/`name`） | 无承载类型 | `source/scene/validation/sceneJsonValidator.h:93` 白名单无 instancing/scatter | **P0 数据 + P2 渲染**：先定义 `VEG_*.json` 实例清单；渲染依赖 roadmap 的 `DrawIndexedInstanced` |
| Roads / rivers / linescape | 无 spline/decal 类型 | 场景对象白名单同上 | 第一版烘成 terrain mask（路面/河床权重）+ 条带 mesh；不新增 spline 系统 |
| Clouds（VDB） | 程序化天空，无体积采样 | `shader/glsl/common/proceduralSky.glsl:4`；DDS/KTX2 volume 被拒绝 | 第一版只烘 skybox/全景云图；VDB 体积云单独立项，见 sky-pass 路线图 |
| Pile / RBD / 破碎 | Jolt 静态/动态体与约束 | `source/script/scriptPhysics.cpp:402`；`source/terrain/terrainCollision.cpp:99` | 预烘"已稳定"的静态堆叠网格；运行时破碎/断裂事件暂不做 |
| Nanite 高模 / PDG 变体 | 普通 mesh + 离线 LOD | `source/renderableObject.cpp:109` 只有 `drawIndexed(...,1,...)` | 离线减面出 LOD0..2；没有虚拟几何，不要按 Nanite 的三角形预算做场景 |
| 世界分区 / HLOD | 地形材质页 VT；几何 chunk 仅加载期驻留 | `documents/contracts/terrain.md` §几何 chunk 驻留；`source/terrain/terrainPageManager.h:27` | 没有 actor/cell/HLOD streaming；第一版限制在 1km 可完整驻留的切片 |

## 4. 场景规模建议

- 4km 岛不要做成单个 TR：`gridResolution` 上限 4096，单块 4km 只有约 1m/quad，
  且一次加载无法流式卸载几何。
- 推荐 **4×4 = 16 个 terrain 摆放**，每个 1km、`pageTilesPerSide = 8`，
  正好铺满 32×32 全局 page table（`pageOrigin` 取 `(0/8/16/24)`）。
- 每个 1km TR 先沿用既有 1km 方案的几何预算：高度图 1024×1024、
  `gridResolution = 256`、`chunkQuads = 64`（4×4 chunk）、`lodLevels = 3`。
  16 个 TR 的最粗层共 256 个 chunk draw；当前每 chunk 一次 `drawIndexed`，
  1024/32 那种预算会变成每 TR 1024 个 chunk、总计 1.6 万次 draw，不能直接上。
  需要更高几何密度时单独评估 512/8×8，并先补合批或几何流式。
- 共享层必须写显式 `layer.key`（例如 `pegasus/forest_floor`）。缺省 key 是
  `<assetPath>#<name>`，16 个 TR 会把同一层注册成 16 个 globalId，256 层容量很快耗尽。
- 同一场景可以有多个 `terrain` 对象；身份是 `terrain|<name>`，同一个 TR 可重复摆放
  （见 `documents/contracts/terrain.md` §场景对象）。

## 5. 推荐落地路线

### P0：离线数据通路（1–2 周）

1. 定义 `Pegasus export manifest v0`：记录源 `.hip` 哈希、单位/轴向、heightfield、
   mask 列表、mesh/材质引用、实例点属性映射。清单是**生成物**，放
   `Generated/Import/SC_<scene>/`，不作为运行时资产。
2. 写 `tool/houdini/` 转换器（Python/hython 或独立脚本）：
   heightfield → RGBA EXR（R=归一化高度）；mask → 8-bit PNG；
   ID Map/slope/valley mask → 每层 8-bit mask；mesh → glTF/GLB（cm、Y-up）；
   散布点 → `VEG_*.json`。
3. TR 增加可选外部权重输入（见 §6），先只支持"每层一张 mask + 通道选择"。
4. 用 1 个 1km 样例打通：Houdini 出图 → 转换 → TR + MI → 场景加载 → 截图。

验收：同一 `.hip` + 同一 seed 重跑，**规范化 manifest 哈希**（去掉绝对路径/时间戳）
一致；图像输出用数值容差（例如 8-bit mask 每通道 ≤1 LSB、float 高度 ≤1e-4），
不要求字节哈希；高度采样与 Houdini viewport 对齐（固定 9 个采样点）；
mask 权重 top-4 归一化后无空洞；现有 `SC_terrain_b32_32x32` 等场景不回归。

### P1：地形与材质（2–4 周）

1. 外部 weight map 接入 `TerrainWeightMapBuilder`，支持 4 通道 mask、layer key 映射、
   `top-4` 归一化与缺失层回退。
2. 把 Pegasus 的 color map/道路/河床 mask 烘成层权重或层 albedo。
3. 16 个 1km TR 拼接，验证 page table/atlas、碰撞高度场与 LOD 接缝。
4. cliff/rock 资产导入，核对单位、法线、切线、材质槽。

验收：1km 切片 8 层地表无明显接缝；4×4 拼接处高度/法线/权重连续；碰撞与渲染高度一致；
`terrainlayerstats`/`terrainlod stats` 可解释驻留与切换。外部 mask 的单测覆盖通道选择、
尺寸不符拒绝、V 翻转语义与全零 texel 行为。

### P2：散布与资产（3–5 周，依赖 roadmap P2）

1. `DrawIndexedInstanced` + 实例缓冲 + 实例化阴影（开放世界路线图 §2.2）。
2. `VEG_*.json` 消费：position/orient/scale/variant/seed/lodBias；确定性排序。
3. 地形查询 API（高度/法线/坡度/层权重）复用到对齐与剔除。
4. 树/石/草分批接入；先 1k 树 + 5k 石，再加草。

验收：同 seed 实例完全一致；树竖直、石贴地半埋、草贴法线；draw call/实例数可观测；
无悬空/穿地；阴影 pass 同步实例化。VEG 单测覆盖四元数归一化/xyzw 顺序、
prototype + 多材质槽覆盖、实例排序确定性。

### P3：植被风与远景（后置）

- PP2/风数据、VDB 云、HLOD/actor streaming 都按各自计划推进；不要塞进 P0 的转换器。

## 6. 数据契约草案（实现时再拆进 contracts）

### 6.1 TR 外部权重（建议字段）

```json
{
  "name": "ForestFloor",
  "heightRange": [0.0, 0.35],
  "slopeRange": [0.0, 35.0],
  "weightMapPath": "Maps/SC_pegasus/Terrains/Masks/forest_floor.png",
  "weightChannel": "r",
  "weightInvert": false
}
```

- `weightMapPath` 可选；存在时该层权重 = mask × 现有 height/slope 规则（或按实现决定二选一，
  必须在合同里写死，不能两套语义并存）。
- mask 分辨率与地形 UV 对齐；缺失/尺寸不符在预检阶段失败。
- 归一化后仍只保留 top-4。**外部 mask 路径**全零 texel 回退到中性层/显式无层，
  不能静默落到 layer 0；**程序化路径保持现状**（全零落 layer 0），
  这样旧场景才能逐字节不回归。

### 6.2 实例清单（建议）

```json
{
  "version": 1,
  "prototypes": [
    {
      "mesh": "Maps/SC_pegasus/Meshes/SM_pine.json",
      "materialSlots": { "0": "Maps/SC_pegasus/Materials/MI_pine.json" }
    }
  ],
  "instances": [
    {
      "prototype": 0,
      "position": [0.0, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "scale": [1.0, 1.0, 1.0],
      "variant": 0,
      "seed": 12345,
      "lodBias": 0
    }
  ]
}
```

- mesh/material 只在 `prototypes` 出现一次，实例只存 `prototype` 索引；
  6k 实例不重复字符串。`materialSlots` 按 SM 的有序槽位覆盖，未列出的槽沿用 SM 默认。
- 单位 cm、Y-up；`rotation` 是归一化四元数，分量顺序固定为 `xyzw`，右手系。
- Houdini 点属性映射：`P -> position`，`orient`（float4 quaternion）-> `rotation`，
  `pscale`/`scale -> scale`，`name`/`variant -> variant`，`N`/`up` 可用于对齐。
  `trans/pivot/transform` 不直接映射到运行时字段：转换器先在 Houdini 内把完整实例变换
  烘进 `P`/`orient`/`scale`。`unreal_instance`/`unreal_foliage` 是 UE 专用属性，不要依赖。
- 运行时不重新播种；清单是唯一事实。

## 7. 风险与许可

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| Houdini Engine 授权 | 自研宿主需要商业授权；Indie 仅限官方插件 | 默认走离线烘焙；HAPI 只做本地实验 |
| 离线烘焙授权 | 离线烘焙仍需要 Houdini Core/FX/Indie 授权；Apprentice 产物非商业且不能进入 Engine | 用 Indie 或商业授权制作；不把 Apprentice 产物入库/分发。调研时报价：Engine $525/yr、Indie $299/yr |
| 教程/工具许可 | 按 SideFX Terms，教程 ZIP 默认非商业、不得修改/装入分发产品；Pegasus 还依赖 Megascans/UE 内容 | 只复用流程；商用发行需书面授权或改用 BSD-2 的 SideFXLabs/自建 HDA；资产换 CC0/确认许可并记录来源。条款：<https://www.sidefx.com/legal/terms-and-conditions/>；Indie 资格：<https://www.sidefx.com/faq/question/who-may-use-indie/> |
| 版本漂移 | HDA/Houdini 升级改变输出 | manifest 记录 Houdini 版本、`.hip` 哈希、seed |
| 坐标/单位 | 模型比例、朝向错误 | manifest 强制 `sourceUnits`/`upAxis`；转换器单测 |
| 地形规模 | 4km 单 TR 超上限/无法流式 | 4×4 分块；先 1km 切片 |
| 体积云预期 | Pegasus 的 VDB 云在 VL 无法直接显示 | 第一版只做 skybox；体积云单独立项 |

## 8. 明确不做

- 不把 Houdini/HAPI 作为 VulkanLearn 运行时或发行依赖。
- 不在第一版实现 VDB 体积云、Nanite/虚拟几何、世界分区、HLOD、运行时破碎。
- 不复制 UE 的 Editor Utility Widget/Blueprint 工具；VL 侧只保留离线转换器与资产命令。
- 不为 Pegasus 新建第二套场景/材质/渲染作者路径。

## 9. 开放问题

1. Pegasus 教程/Flora 工具的具体许可是否允许把生成资产用于非 UE 的自研引擎？
2. 4km 岛的目标几何密度是多少？先沿用 256 quad/1km，还是需要 512/8×8？
3. 外部 mask 与现有 height/slope/noise 规则是"相乘"还是"覆盖"？需要在合同里定死。
4. 云用烘焙 skybox 还是等体积渲染？这决定 Pegasus Clouds 阶段是否值得接。
5. 是否接受"离线转换器只支持固定 manifest 版本"，还是需要通用 HDA 参数面板？

## 10. Definition of Done（第一版）

- [ ] 一个 1km、8 层的 Pegasus 派生切片可由固定命令重新生成。
- [ ] heightfield/mask 转换有数值回归；规范化 manifest 哈希稳定，图像按容差比较。
- [ ] TR 外部权重接入后，现有程序化权重场景逐字节不回归。
- [ ] 外部 mask 通道/尺寸/V 翻转、VEG 四元数/多材质槽、实例确定性有 GTest。
- [ ] 1k 树 + 5k 石实例化加载，无悬空/穿地，阴影同步。
- [ ] 固定机位截图与参考构图对比通过；Tracy 有加载/转换/提交区间。
- [ ] 文档门禁 `py -3 tool\validate-docs.py` 通过。
