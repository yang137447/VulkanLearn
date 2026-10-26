# 地形系统：Open World Demo Collection 复刻计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 地形运行时与首个复刻场景的**实施计划**（未来工作，不是契约） |
| 状态 | 未实现。`source/` 没有 terrain 运行时，`SceneAssetValidator` 仍拒绝 `terrain`；本计划取代 World Creator 路线 |
| 修订 | 2026-10-19 第 1 版 |
| 目标 | 1km 林地地形；8 层、top-4、texture array；复刻 Open World Demo Collection 的地表与构图 |
| 非目标 | GPU-driven、indirect draw、GPU 四叉树、LOD、streaming、VT/clipmap、编辑器绘制、草地系统；地形碰撞不在 M1，P1 见开放世界栈路线图 |
| 当前参考 | Open World Demo Collection（§1） |
| 后续候选 | Landscape Mountains、Kite Demo、Project Titan（§9） |
| 关联路线图 | [open-world-scene-stack-roadmap.md](open-world-scene-stack-roadmap.md)（碰撞/GASP/植被/风/LOD/shading 的整套顺序） |
| 取代 | `terrain-worldcreator-mvp.md`（World Creator 高度图路线，未实施；保留为历史） |

> 本文件是计划，不是契约。运行时落地后，行为写进 `documents/contracts/terrain.md`（新文件）；
> 本文件只保留参考与决策，不描述"现在是什么"。

## 0. 决策摘要

1. 地形是"带材质的 mesh"：高度图在加载期生成 CPU chunk 网格，走现有 geometry pass 写 GBuffer；
   `DefaultLit` 足够，不新增 pass / shading model / GBuffer 通道。
2. 运行时固定 **4 层**：8 个层权重 → 每像素 top-4 → 3 个 texture array 各采 4 层。
   top-4 的前提是 `sampler2DArray`，这是本计划唯一的新引擎能力。
3. 权重先用**程序化 height/slope/noise**，零绘制资产；以后要画 splat，再加权重贴图覆盖，
   运行时接口不变。
4. GPU-driven 几何、VT/clipmap、LOD、streaming 全部搁置；CPU 逐 chunk 提交。
5. 当前只复刻 **Open World Demo Collection** 的林地/岩石/草地貌；其他 UE 场景记录为后续候选。
6. 世界单位是 cm；1km = 100000cm。所有 `size` / `heightScale` 按 cm 写。

## 1. 参考场景库（后续候选）

| 场景 | 类型 / 规模 | 画面特征 | 对 VL 的用途 | 状态 |
| --- | --- | --- | --- | --- |
| **Open World Demo Collection** | Epic 的 Open World Demo 资产合集 | 林地山坡、落叶/泥土/岩石/草地、远处山谷与水面 | **当前复刻目标**：8 层 palette 与构图 | 当前 |
| Landscape Mountains | UE4 山地 sample | 山谷、湖、草坡、岩壁、雪线 | 写实高山候选；加雪层和更大高差 | 后续 |
| Kite Demo | UE4 森林 demo | 森林、草地、岩石、落叶 | 近景地表/植被候选；需要草系统 | 后续 |
| Project Titan | UE5.5，8km×8km，风格化 | 多 biome、大尺度、风格化材质 | 规模/biome 参考；不借材质 | 后续 |
| City Sample | UE5，城市 | 城市街区，非地形 | 仅规模参考；地形价值低 | 记录 |

链接：

- Open World Demo Collection：[演示视频](https://www.youtube.com/watch?v=lias601tMFs)、
  [Epic Marketplace](https://www.unrealengine.com/marketplace/en-US/product/open-world-demo-collection)
- Landscape Mountains：[演示视频](https://www.youtube.com/watch?v=13vTtFFTMj8)、
  [Epic 博客](https://www.unrealengine.com/blog/new-on-marketplace-landscape-mountains)
- Kite Demo：[探索视频](https://www.youtube.com/watch?v=zMHUyN0HvpM)、
  [资产下载说明](https://www.cgchannel.com/2015/04/download-the-assets-from-epic-games-ue4-kite-demo/)
- Project Titan：[Overview](https://www.youtube.com/watch?v=EG9UqwqS2ug)、
  [Fab](https://www.fab.com/listings/c05aac82-4c1a-4e42-96b3-be668dc40fca)
- City Sample：[Cinematic](https://www.youtube.com/watch?v=6hqQ2ioSzss)

**授权与素材边界**

- 这些链接只做**视觉参考**；本计划不分发 UE 资产。
- 不把 `.uasset` / UE-only 内容导进 VL。地表贴图用 CC0（Poly Haven、ambientCG）或确认许可的
  Fab Standard 素材。
- Fab Standard 允许商用，但旧 UE Marketplace 许可可能只限 UE；导入前看 listing 的 Details/EULA。
- 高度图如果从 UE Landscape 导出，只取高度数据；材质/贴图仍用可分发素材重做。

**为什么当前选 Open World Demo Collection**

- 地貌是林地/岩石/草地，和 8 层 palette 直接对应。
- 不依赖雪山、大水面、密集草地等我们暂时没有的能力。
- 参考视频/截图多，1km vertical slice 容易定构图。
- 体量比 Project Titan 小，适合先把"高度图 → 网格 → 材质"跑通。

## 2. 目标画面

### 2.1 构图

- **前景（0–30m）**：林下地表。落叶层、裸露泥土、苔藓、碎石、草；树根和岩石边缘是重点。
- **中景（30–300m）**：起伏丘陵、林间空地、土路/浅沟；树影投在地表上。
- **远景（300m–1km）**：山谷、远处水面或雾气；不要求雪山。
- **相机**：近景 1.6m 眼高、24–35mm；另有一个 80–120m 的俯瞰机位检查构图。
- **光照**：柔和阴天/侧逆光，低对比，冷灰绿；太阳高度 20–35°。

### 2.2 8 层 palette

| # | 层 | 角色 | heightRange（归一化） | slopeRange（度） | 备注 |
| --- | --- | --- | --- | --- | --- |
| 0 | leaf_litter | 林下落叶 | 0.00–0.40 | 0–25 | 主基调，暖褐 |
| 1 | grass | 草地 | 0.10–0.60 | 0–30 | 中绿 |
| 2 | dry_grass | 枯草 | 0.20–0.70 | 0–35 | 黄褐，做变化 |
| 3 | dirt | 泥土/土路 | 0.00–0.50 | 0–20 | 路径/裸露 |
| 4 | mud | 湿泥 | 0.00–0.25 | 0–10 | 低洼/沟底 |
| 5 | rock | 岩石 | 0.20–0.90 | 25–90 | 陡坡 |
| 6 | moss | 苔藓 | 0.30–0.80 | 20–60 | 岩石/树根过渡 |
| 7 | gravel | 碎石 | 0.00–0.50 | 10–45 | 溪沟/路肩 |

- `heightRange` 是 `heightScale` 归一化后的 0..1；`slopeRange` 是法线与 up 的夹角。
- 区间故意重叠；顶层用 overlap + noise 做过渡，不硬切。
- 8 层是建议；素材不齐时可以先 6 层，运行时接口不变。

### 2.3 和原场景的差距

- 原场景的森林密度、草、Nanite、RVT、Lumen 我们都没有；只复刻**地表材质和构图**。
- M1 用现有 SpeedTree 树 + 石头做尺度；草后置。
- 当前没有 height fog；M1 先不做，M2 可选在 `deferredLighting` 加一个简单的距离/高度雾，
  否则远景会比参考"干净"。

## 3. 运行时架构

### 3.1 数据流

```text
SC_*.json (terrain object)
  -> SceneAssetValidator: TerrainAssetLoader::Prepare(terrainPath)
  -> WorldLoader / WorldBuilder: TerrainBuildPlan
  -> RendererResourceLoadCoordinator
  -> RendererTerrainLoader
       -> TerrainMeshBuilder: CPU vertices/indices per chunk
       -> RenderableObject per chunk
       -> MaterialInstance (MI_terrain)
       -> MeshObjectBuildPlan
  -> World / RenderableObject -> geometry pass
```

- 不新增 render pass；地形和普通 mesh 同一条 geometry pass。
- 不改 GBuffer 合同；`DefaultLit` 的 baseColor/normal/roughness/metallic/AO 足够。

### 3.2 网格

| 项 | 值 |
| --- | --- |
| 地形尺寸 | 100000cm × 100000cm（1km） |
| 高度图 | 1024×1024，EXR float（1m/texel） |
| 网格 | 256×256 quad（约 3.9m/quad） |
| 分块 | 4×4 chunk，每块 64×64 quad |
| 顶点 | 257² ≈ 66049 |
| 三角 | 131072 |
| 索引 | uint32 |

- 高度采样：双线性；`h = sample * heightScale + heightOffset`。
- 法线：**全局高度图中心差分**，不能按 chunk 单独算，否则块边缘有法线接缝。
- 切线：`T=(1,0,0)`、`B=(0,0,1)`、handedness `+1`；坡面切向后置。
- UV：整块 `[0,1]`；shader 里乘 `u_uvScale`。
- bounds：每 chunk 从顶点算 AABB，供 CPU 视锥剔除和阴影裁剪。
- 不做 LOD、不做 skirt。

### 3.3 材质

- `M_terrain`：`DefaultLit`，只写 surface，不写自定义光照。
- 3 个 texture array：
  - `layerAlbedo`：sRGB，RGBA8；
  - `layerNormal`：linear，RGBA8（或 BC5 后置）；
  - `layerPbr`：linear，R=roughness、G=metallic、B=AO、A=height。
- 权重：每层一个 `heightRange` + `slopeRange` + `noise`，在 fragment 里算：

```text
w_i = heightWindow(h) * slopeWindow(slope) * (1 + noise * n)
```

- 归一化后做 top-4：4 槽插入排序；选完重新归一化；总和 < 1e-4 时回退 layer 0。
- 采样：每层 3 次 array 采样 × 4 层 = 12 次；权重只是 ALU。
- 混合：albedo/roughness/metallic/AO 线性加权；法线用 whiteout（M1）或 RNM（M2）。
- 距离：`pow(w, 2–4)` 锐化，减少远处排名跳变；没有 VT，远处靠雾/低配 fallback。
- Debug：`u_debugMode` 输出权重、top-4 索引、单层隔离。

**top-4 伪代码**

```text
top[0..3] = 0; idx[0..3] = 0
for i in 0..7:
    insert (w[i], i) into top/idx descending
sum = top[0] + top[1] + top[2] + top[3]
if sum < 1e-4: return layer 0
for k in 0..3: top[k] /= sum
```

- 不能用 `if (idx == 0) texture(layer0) ...` 选独立 sampler；warp 内会全采样。
  必须 array/bindless。

### 3.4 提交与生命周期

- geometry pass 画所有 chunk；CPU 逐 chunk 视锥剔除；16 个 draw call。
- 阴影 pass 复用同一网格；terrain 材质的 shadow caster 走现有策略，需要单独验证 bias。
- 地形 CPU 网格在 prepared world 的资源加载阶段生成；GPU `RenderableObject` 进
  `RendererResourceCache`，key 为 `terrainPath|gridResolution|chunkQuads`。
- world 切换/退休沿用现有 prepared/commit 流程；不引入全局 terrain 单例。
- 材质热重载沿用现有材质 reload；数组纹理绑定稳定。

## 4. 资产格式（草案）

> 以下是拟议格式，不是当前契约；落地后以代码与 `contracts/terrain.md` 为准。

### 4.1 TR_*.json

```json
{
  "name": "TR_openworld_01",
  "type": "terrain",
  "heightmapPath": "Maps/SC_terrain_openworld/Source/Heightmaps/HM_openworld_01.exr",
  "size": [100000.0, 100000.0],
  "heightScale": 18000.0,
  "heightOffset": 0.0,
  "gridResolution": 256,
  "chunkQuads": 64,
  "materialInstancePath": "Maps/SC_terrain_openworld/Materials/MI_terrain_openworld.json",
  "layers": [
    { "name": "leaf_litter", "heightRange": [0.00, 0.40], "slopeRange": [0, 25], "noise": 0.15 },
    { "name": "grass",       "heightRange": [0.10, 0.60], "slopeRange": [0, 30], "noise": 0.20 },
    { "name": "dry_grass",   "heightRange": [0.20, 0.70], "slopeRange": [0, 35], "noise": 0.25 },
    { "name": "dirt",        "heightRange": [0.00, 0.50], "slopeRange": [0, 20], "noise": 0.10 },
    { "name": "mud",         "heightRange": [0.00, 0.25], "slopeRange": [0, 10], "noise": 0.10 },
    { "name": "rock",        "heightRange": [0.20, 0.90], "slopeRange": [25, 90], "noise": 0.30 },
    { "name": "moss",        "heightRange": [0.30, 0.80], "slopeRange": [20, 60], "noise": 0.25 },
    { "name": "gravel",      "heightRange": [0.00, 0.50], "slopeRange": [10, 45], "noise": 0.20 }
  ]
}
```

- `size` / `heightScale` / `heightOffset` 单位 cm。
- `gridResolution` 是总 quad 数；`chunkQuads` 是每块 quad 数；两者必须整除。
- `layers` 顺序 = texture array 层顺序，硬契约。
- 可选后置：`weightMapPath`、`farAlbedoPath`、`perLayerUvScale`。

### 4.2 T_* array.json

```json
{
  "name": "T_terrain_openworld_albedo_array",
  "type": "textureArray",
  "sources": [
    "Maps/SC_terrain_openworld/Textures/T_leaf_litter_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_grass_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_dry_grass_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_dirt_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_mud_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_rock_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_moss_albedo.png",
    "Maps/SC_terrain_openworld/Textures/T_gravel_albedo.png"
  ],
  "colorSpace": "srgb",
  "mipmaps": true,
  "filter": "linear",
  "wrapMode": "repeat"
}
```

- 所有 source 必须同尺寸/同格式；顺序 = TR 层顺序。
- normal/pbr array 的 `colorSpace` 为 `linear`。
- cache key 包含 sources 顺序和全部采样设置。

### 4.3 MI_terrain.json

```json
{
  "name": "MI_terrain_openworld",
  "type": "materialInstance",
  "material": "shader/glsl/M_terrain.json",
  "parameters": {
    "u_uvScale": 64.0,
    "u_weightSharpness": 2.0,
    "u_debugMode": 0.0
  },
  "textures": {
    "layerAlbedo": "Maps/SC_terrain_openworld/Textures/T_terrain_openworld_albedo_array.json",
    "layerNormal": "Maps/SC_terrain_openworld/Textures/T_terrain_openworld_normal_array.json",
    "layerPbr": "Maps/SC_terrain_openworld/Textures/T_terrain_openworld_pbr_array.json"
  }
}
```

### 4.4 SC_terrain_openworld.json

```json
{
  "name": "Terrain Open World Demo",
  "type": "scene",
  "description": "学习场景：复刻 Open World Demo Collection 的林下地表/岩石/草地貌，验证高度图地形、8 层 top-4 和 texture array；不参与论文 case。",
  "worldUnit": "cm",
  "objects": [
    {
      "name": "Terrain_Main",
      "type": "terrain",
      "terrainPath": "Maps/SC_terrain_openworld/Terrains/TR_openworld_01.json",
      "position": [0.0, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0],
      "scale": [1.0, 1.0, 1.0]
    },
    {
      "name": "Sun",
      "type": "directionalLight",
      "position": [0.0, 10000.0, 0.0],
      "rotation": [-30.0, -40.0, 0.0],
      "color": [1.0, 0.98, 0.95],
      "intensity": 8000.0,
      "intensityUnit": "lux",
      "shadow": { "dynamicShadowDistance": 30000.0 }
    },
    {
      "name": "Camera_Main",
      "type": "camera",
      "fov": 35.0,
      "near_clip": 10.0,
      "far_clip": 200000.0,
      "position": [0.0, 160.0, 12000.0],
      "rotation": [-5.0, 180.0, 0.0]
    },
    {
      "name": "Environment",
      "type": "environment",
      "environment": { "type": "proceduralSky" }
    }
  ]
}
```

- `description` 必须非空，按仓库场景合同写清用途。
- `environment` 的具体字段按现有 schema 补；上面只是骨架。

## 5. 引擎改动清单

| 区域 | 文件 | 改动 |
| --- | --- | --- |
| texture array | `source/resource/image/hostImage.*`、`source/resource/image/textureIO.*` | 多 source 组装 `HostImage`，保留 `arrayLayers` |
| texture array | `source/resource/device/deviceTextureFactory.*` | 按 `arrayLayers` 建 image，逐层上传，建 2D_ARRAY view |
| texture array | `source/texture.*` | `CreateDesc.arrayLayers`；view type 2D_ARRAY |
| texture array | `source/render/backend/rendererBackendVulkan.*`、`source/render/rhi/vulkan/rhiDeviceVulkan.*` | copy/mip 支持 baseLayer/layerCount |
| texture asset | `source/textureAssetLoader.*` | 解析 `type: "textureArray"` 与 `sources` |
| material | `source/material/materialDescriptorSchema.cpp` | 读取 texture `type`，允许 `sampler2DArray` |
| material | `source/material/validation/materialAssetValidator.*` | 放开 `sampler2DArray`；校验 MI 绑定类型 |
| material | `source/render/resource/rendererMaterialLoader.cpp` | 数组纹理走同一 combined image sampler |
| terrain | 新 `source/terrain/terrainAssetTypes.h`、`terrainAssetLoader.*`、`terrainMeshBuilder.*`、`validation/terrainAssetValidator.*` | TR 解析/校验、高度图采样、网格生成 |
| terrain | 新 `source/render/resource/rendererTerrainLoader.*` | `RenderableObject` + `MeshObjectBuildPlan` + MI |
| scene | `source/scene/validation/sceneJsonValidator.h` | 接受 `terrain`，要求 `terrainPath` |
| scene | `source/scene/sceneAssetTypes.h` | `SceneAssetObjectPlan` 增加 `terrainLoadRequest` |
| scene | `source/scene/validation/sceneAssetValidator.cpp` | `TerrainAssetLoader::Prepare` |
| world | `source/world/loading/worldLoader.*`、`worldBuilder.*` | 携带 terrain plan；object identity |
| renderer | `source/render/resource/rendererResourceLoadCoordinator.*` | 分派 terrain，追加 `MeshObjectBuildPlan` |
| shader | `shader/glsl/M_terrain.json`、`M_terrain.vertex.glsl`、`M_terrain.surface.glsl`、`materialFunction/mf_terrainLayerBlend.glsl` | 8 权重、top-4、array 采样、debug |
| tests | `tool/texture-asset-tests`、`tool/material-schema-tests`、新 `tool/terrain-tests` | 见 §7 |
| docs | `documents/routing.md`、`AGENTS.md`、新 `contracts/terrain.md` | 落地后更新；本阶段只加计划入口 |

**风险点**：texture array 会动 RHI/材质绑定，属于跨模块改动；先做最小 2D_ARRAY 用例，
确认现有材质不受影响。

## 6. 实施阶段

| 阶段 | 内容 | 交付 | 验收 | 粗估 |
| --- | --- | --- | --- | --- |
| M0 | 参考冻结与素材选型 | 1km 高度图、8 组 albedo/normal/pbr、构图参考 | 素材许可清楚；高度图 1024² | 1d |
| M1 | texture array + `sampler2DArray` | 数组纹理资产、上传、view、mip、材质绑定 | 8 层数组能采样；2D 材质回归通过 | 3–5d |
| M2 | TR/场景/网格/World | `TR_*.json`、`SC_*.json`、chunk 网格、bounds | 场景加载出地形；GTest 通过 | 3–4d |
| M3 | 材质与 top-4 | `M_terrain`、8 权重、top-4、debug | 8 层过渡自然；debug 视图正确；无 NaN | 2–3d |
| M4 | 场景资产与画面 | 8 层贴图、树/石、3 张验证截图 | 构图接近参考；1080p 可交互 | 2–3d |
| M5 | 测试与文档收口 | GTest、contract、routing/AGENTS | `validate-docs` 通过；回归 | 1–2d |

### 2026-10-22 开发启动

本地分支：`codex/terrain-openworld`。M1 首批实现 CPU 数组准备：
`TextureIO::LoadArray` 复用逐图片解码与行方向策略，
`HostImage::CombineArrayLayers` 校验尺寸/格式/像素布局并保留 layer 顺序，
`GetMipData(0, layer)` 支持逐层寻址。覆盖归入现有 `texture_asset_tests`；
已落地行为见 [纹理合同](../../contracts/texture-asset-json-v1.md#cpu-array-preparation)。

M1 尚未完成：GPU 多层上传、2D_ARRAY view/mip、`textureArray` JSON、
`sampler2DArray` schema/反射/MI 类型校验及画面验收仍待实现。
M0 素材未冻结，M2–M5 未开始；本批不修改运行时资源或默认场景。

总计约 **2–3 周**（按本仓库的测试/文档标准）。

## 7. 验证

### 7.1 GoogleTest

- `texture_asset_tests`：`textureArray` 解析、source 顺序、cache key、层数不一致报错。
- `material_schema_tests`：`sampler2DArray` schema/binding、生成 include、MI 类型校验。
- 新 `terrain_tests`：
  - 2×2 / 3×3 高度图的顶点数、索引数、bounds；
  - 中心差分法线方向与斜率；
  - chunk 边缘法线与全局一致；
  - TR 校验：size/heightScale/grid/chunk 整除、8 层、range 合法；
  - top-4 C++ 参考：N=8 的已知权重选择、全零 fallback、归一化。
- 回归：现有 `SC_sphere_array` / `SC_uds_mountain_range` 等场景正常加载，2D 材质不受影响。

### 7.2 场景与画面

- `SC_terrain_openworld` 加载：16 chunk、bounds 正确、无 validation error。
- 3 张验证图：
  - hero：近景林下地表 + 中景丘陵；
  - close-up：leaf/dirt/rock 过渡；
  - debug：top-4 索引/权重。
- 和参考图比**构图/明度/色相**，不做像素匹配（技术栈不同）。

### 7.3 性能

- 目标 1080p 可交互（具体帧率以机器为准）；记录 CPU submission、GPU 时间、
  draw call（16）、纹理采样数。
- 确认地形网格只在加载期生成，不在每帧 CPU 重建。
- 阴影 pass 的 terrain 自阴影/bias 单独检查。

### 7.4 文档门禁

```powershell
py -3 tool\validate-docs.py
```

## 8. 风险与未决

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| top-4 截断/排名跳变 | 颜色/法线 pop | 程序化权重保持稀疏；`pow(w, k)` 锐化；重叠区间 |
| texture array 改动面大 | 现有材质/RHI 回归 | 先做最小 2D_ARRAY 用例；2D 路径保持不变 |
| 数组显存 | 8×3×1024² 约 100MB | M1 用 512²；后续 BC 压缩 |
| 8-bit 高度图 | 台阶 | 用 EXR；16-bit PNG 后置 |
| 没有草/密林 | 画面比参考空 | 用现有树/石；草后置 |
| 没有 height fog | 远景偏"干净" | M2 可选加简单雾；或靠天空/曝光 |
| 没有碰撞 | 只能看不能走 | 明确非目标；Jolt heightfield 后置 |
| 世界单位 cm | 尺度错 | TR 全部按 cm；场景 `worldUnit: "cm"` |
| 许可 | 不能分发 | 只参考；素材用 CC0/确认许可 |

## 9. 后续场景扩展

所有场景共用同一个运行时合同（TR + 8 层 + top-4 + 3 arrays），只换资产：

- **Landscape Mountains**：加 snow 层、放大高差、加湖面；需要更远的 far clip 和雾。
- **Kite Demo**：提高树/草密度；需要草系统和更好的植被光照。
- **Project Titan**：多 biome、8km 世界；需要 LOD/streaming/GPU-driven，当前明确不做。
- **City Sample**：不是地形，跳过。

整套顺序（地形 → 碰撞 → 角色 → 植被/撒点 → PP2/风 → LOD → shading）见
[open-world-scene-stack-roadmap.md](open-world-scene-stack-roadmap.md)。

## 10. 开放问题

1. 权重来源：程序化（推荐 M1）还是先画 splat？如果画，权重贴图放 2 张 RGBA，仍然 top-4。
2. 规模：1km / 1024² / 256² / 4×4 是否 OK？
3. 高度图：EXR（推荐）还是补 16-bit PNG 解码？
4. 碰撞：M1 不做渲染期碰撞；P1 用 Jolt heightfield 接 GASP，这个分期是否 OK？
5. 每层独立 uvScale：M1 先全局，还是一步到位？
6. 法线混合：M1 whiteout，M2 是否换 RNM？

## 11. Definition of Done

- [ ] `textureArray` 资产能加载 8 层，material 能绑定 `sampler2DArray`。
- [ ] `SC_terrain_openworld` 能加载，16 chunk，bounds/culling 正确。
- [ ] 8 层权重 + top-4 正常，debug 视图可切换。
- [ ] 3 张验证图通过构图/过渡检查。
- [ ] GTest、现有场景回归、`validate-docs` 全通过。
- [ ] 新 `contracts/terrain.md` 落地，`routing.md` / `AGENTS.md` 更新。
- [ ] 明确记录未做项：LOD/streaming/VT/GPU-driven/grass；碰撞按 P1 的 Jolt heightfield 计划推进。
