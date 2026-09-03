# Shading Model 验证案例计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 资产驱动的 Shading Model 展示与验证计划 |
| 状态 | 已完成案例方向整理，等待外部资产导入 |
| 建立日期 | 2026-09-02 |
| 适用范围 | 场景、模型、材质实例、纹理和验证流程的规划 |
| 当前实现合同 | `documents/rendering/` 下对应的 Shading Model 合同 |
| 运行时原则 | 渲染器只负责加载和渲染静态验证资产，不在运行时执行布料或头发模拟 |

本文冻结 VulkanLearn 当前 Shading Model 验证资产的选择、场景分层、落地顺序和验收门槛。
本文是未来工作计划，不替代 `documents/rendering/` 下已经实现的渲染合同，也不代表所有资产
已经导入或所有场景已经完成。

## 1. 目标与原则

### 1.1 目标

- 为每个需要美术验证的 Shading Model 建立一个具象、可重复的标准展示案例。
- 保留球体阵列作为参数和材质标尺，同时增加头模、植物、雕像、发型、服装和灯笼等有轮廓的案例。
- 把“单一模型的标准展示”和“多材质综合回归”分开，避免一个复杂角色掩盖单个模型的问题。
- 在相同的相机、曝光和灯光约定下，支持 Forward、Deferred、GBuffer 和 Debug View 对照。
- 让资产来源、授权、贴图通道和转换过程可追溯。

### 1.2 固定原则

- 一个标准展示 Case 只突出一个主 Shading Model；底座、支架、灯源等辅助结构使用明确的辅助材质。
- `simple_character` 作为综合回归场景保留，不把它当作所有模型的最终美术样例。
- 运行时只消费预先制作好的静态网格、UV、法线、切线和贴图；不把模拟、重拓扑或导出逻辑放进引擎。
- Hair、Cloth、ThinTranslucent 等对几何和 RenderState 有特殊要求的模型，必须在资产转换阶段解决输入问题。
- 未完成授权确认的资产只能作为视觉参考，不能进入正式资源库或发布包。
- 不重新创建已经清理的专用探针场景：
  `SC_subsurface_models`、`SC_cloth_models`、`SC_eye_probe`、
  `SC_eye_dual_shell_probe`、`SC_eye_deferred_probe`。

## 2. 当前 Shading Model 边界

当前可 authoring 的主要入口如下。ID、GBuffer、材质参数和渲染路径以对应的
`documents/rendering/` 合同为准。

| Shading Model | ID | 当前材质入口 | 本计划定位 |
| --- | ---: | --- | --- |
| `DefaultLit` | 1 | `M_pbr` | 基础标尺、辅助材质和普通物体 |
| `Subsurface` | 2 | `M_subsurface` | 皮肤或石材的局部次表面响应 |
| `PreintegratedSkin` | 3 | `M_preintegratedSkin` | 皮肤头模的预积分皮肤对照 |
| `ClearCoat` | 4 | `M_carPaint` | 汽车漆和清漆高光 |
| `SubsurfaceProfile` | 5 | `M_subsurfaceProfile` | 皮肤或大理石的 profile filter |
| `TwoSidedFoliage` | 6 | 当前没有独立的标准展示入口 | 植物案例；需要先确认材质 authoring 路径 |
| `Hair` | 7 | `M_hair` | 发片、Alpha Clip、切线和轮廓光 |
| `Cloth` | 8 | `M_cloth` | 披挂服装、织物高光和各向异性 |
| `Eye` | 9 | `M_eye`、`M_eyeCornea`、`M_eyeInner`、`M_eyeDeferred` | 眼球综合案例，暂以角色回归为主 |
| `ThinTranslucent` | 11 | `M_thinTranslucent` | 孔明灯纸罩等单层薄介质 |

以下内容不纳入本轮正式展示清单：

- `M_speedtree` 当前声明的是 `DefaultLit`，因此 `SC_speedtree` 不是严格的
  `TwoSidedFoliage` 标准案例。
- `SingleLayerWater` 和 `Strata` 目前没有完整的独立材质 authoring 和验证入口。
- `Unlit`、`VertexColor`、`Shadow` 作为辅助或调试材质，不作为主展示 Case。

## 3. 标准案例清单

### 3.1 已有并继续保留

| 场景 | 主模型 | 主 Shading Model | 用途 |
| --- | --- | --- | --- |
| `SC_sphere_array` | PBR Sphere Array | `DefaultLit` | 粗糙度、金属度、法线、AO 和材质参数基础标尺 |
| `SC_car_showcase` | 汽车及车轮 | `ClearCoat` | 车漆清漆层、底层反射和边缘高光 |

`SC_car_showcase` 中已有的普通 PBR 和灯罩材质继续作为汽车结构的辅助材质。汽车灯罩
不再承担 `ThinTranslucent` 的主展示职责，后续由孔明灯案例替代。

### 3.2 本轮确定的新案例

| 场景 | 资产 | 主材质分配 | 状态 |
| --- | --- | --- | --- |
| `SC_skin_head_scan` | Lee Perry Smith Head Scan | 头部使用 `PreintegratedSkin`、`Subsurface`、`SubsurfaceProfile` 三个变体 | 资产已选，入库时复核来源和授权 |
| `SC_marble_bust_01` | Poly Haven Marble Bust 01 | 大理石主体使用 `Subsurface`，辅助结构使用 `DefaultLit` | 已导入，已使用 `.blend` 参数；待运行时 smoke 与 Beauty 验收 |
| `SC_foliage_potted_plant_02` | Poly Haven Potted Plant 02 | 叶片使用 `TwoSidedFoliage`，花盆和土壤使用 `DefaultLit` | 资产已选，待确认独立 foliage 入口 |
| `SC_hair_showcase` | zHairezt 的 Short Wavy Hair With Bangs | 灰色头模使用 `DefaultLit`，发型使用 `Hair` | 模型已选，待官方下载和转换 |
| `SC_cloth_showcase` | 灰色人模和 MD/CLO 披挂服装 | 人模使用 `DefaultLit`，服装使用 `Cloth` | 待获得可再分发的 MD/CLO 工程 |
| `SC_thin_translucent_lantern` | Kong Ming Lantern / Sky Lantern | 纸罩使用 `ThinTranslucent`，骨架使用 `DefaultLit`，燃烧器和火焰使用 `Unlit` | 视觉方向已选，待授权或自制替代资产 |

### 3.3 Eye 的当前定位

Eye 暂不新增外部资产。现阶段使用 `simple_character` 作为综合回归入口，覆盖：

- 眼球、角膜和内层的材质绑定；
- Forward Eye 路径和 Deferred fallback；
- 眼球局部 SSS、湿润高光和多材质排序；
- 角色场景中的 Eye 与 Hair、Skin 同时存在时的资源生命周期。

后续如果需要独立 Eye Hero Case，再单独选择带角膜和虹膜贴图的近景眼球资产，避免重新引入
已经删除的 probe 场景结构。

## 4. 案例的视觉与材质构图

### 4.1 皮肤头模

同一个头模保留三份 Material Instance 变体，在同一个相机和同一组灯光下切换：

```text
MI_skin_head_preintegrated.json
MI_skin_head_subsurface.json
MI_skin_head_profile.json
```

验证重点：

- 耳朵、鼻翼、脸颊和额头的局部透光差异；
- thickness / profile 参数对结果的影响；
- 皮肤高光、粗糙度和阴影的稳定性；
- 三种 SSS 路径和 DefaultLit 基线的差异是否可解释。

皮肤资产的公开版本需要在入库时记录作者、原始页面、许可文本和允许的再分发范围。
如授权链不能确认，保留为外部参考，不复制到资源仓库。

### 4.2 大理石胸像

大理石胸像用于验证“非皮肤材质的柔和次表面响应”，不直接套用皮肤的散射 profile。

建议至少制作两个变体：

```text
Marble_DefaultLit
Marble_SubsurfaceProfile
```

如果 `SubsurfaceProfile` 需要专用散射参数，新增独立的 `SSP_marble.json`，不复用
`SSP_skin.json` 的语义。底座、展示台和背景使用 `DefaultLit`，避免把次表面效果烘进环境。

### 4.3 盆栽植物

植物案例明确拆成叶片和普通硬表面：

```text
Leaves      -> TwoSidedFoliage
Pot         -> DefaultLit
Soil        -> DefaultLit
```

验证重点：

- 叶片正反面和背光透光；
- Alpha Clip 边缘和远近观察距离；
- 叶脉法线、双面法线和阴影投射；
- 同一场景中 Foliage 与普通 PBR 花盆的材质边界。

该案例落地前必须先确认 `TwoSidedFoliage` 的材质入口、RenderState、Alpha 约定和 ShadowCaster
路由；不能仅把现有 `M_speedtree` 改名来模拟该模型。

### 4.4 Hair 头模

Hair Case 采用灰色头肩模和独立发型，不使用完整真人角色作为标准 Hair 资产：

```text
GrayBust      -> M_pbr
HeroHairCards -> M_hair
```

建议头部转向约 20 度，使用侧后方轮廓光和较暗的中性背景，突出短波浪发、刘海分层、
发片边缘和头发体积。

模型页面的源数据显示该发型没有可靠的 mesh tangent 输出；转换工具必须在离线阶段生成或修正
稳定的 Hair tangent，并把 rootward 方向和 `tangent.w` handedness 固定下来。运行时不能依靠
逐像素随机或隐式 fallback 掩盖错误切线。

### 4.5 Cloth 人模

Cloth Case 不使用悬空布片、布料球或沙发作为主对象，采用展示型人模构图：

```text
Body_Gray  -> M_pbr
Garment    -> M_cloth
```

服装需要有明确的静态 Pose、独立版型和贴身褶皱，重点保留肩部、腰部、袖口和搭接区域的接触
关系。MD/CLO 源工程只用于离线模拟和导出，运行时不执行布料模拟。

### 4.6 孔明灯

薄透射案例采用分层结构，不能把发光或反射结构烘进纸罩：

```text
PaperShell  -> M_thinTranslucent
WireFrame   -> M_pbr
Burner      -> M_unlit 或明确的辅助发光材质
Flame       -> M_unlit
```

验证重点是纸罩自身的薄介质反射、透射色和背光轮廓。当前链接资产的视觉结构已确定，
正式入库前仍需获得明确再分发授权，或制作结构相近且授权清晰的替代资产。

## 5. 资源组织约定

每个独立 Case 按以下方式组织，避免把外部源文件、运行时描述和场景布局混在一起：

```text
<resourcePath>/
  scenes/SC_<case>.json
  models/<case>/SM_*.json
  models/datas/<case>/      # GLB/GLTF/FBX 转换后的运行时网格
  materials/<case>/MI_*.json
  textures/<case>/T_*.json
  textures/datas/<case>/    # 原始或转换后的像素数据
  <case>/ATTRIBUTION.md     # 作者、来源、许可证和转换记录
```

资产入库必须完成以下检查：

- 网格对象和材质槽拆分符合主材质 / 辅助材质边界；
- UV、Normal、Tangent 和 `tangent.w` 语义稳定；
- 贴图颜色空间和通道含义写入纹理描述或材质合同；
- `SM_*.json`、`MI_*.json`、纹理 JSON 和场景引用全部使用资源根相对路径；
- 外部授权和署名文件与运行时资源一起保存；
- 不把 Blender、Marvelous Designer、CLO 或其它 DCC 的临时缓存提交为运行时依赖。

## 6. 场景与灯光约定

### 6.1 轻量验证机位

当前阶段不要求每个 Case 都制作完整的多机位证据集，统一采用渐进式验证：

1. `Beauty` 是默认机位：使用统一相机、环境和主光，先确认模型、材质和整体观感。
2. `Backlight` 只在验证皮肤、叶片、头发或薄透射时启用，用来回答明确的透光问题。
3. `Debug` 只在出现材质绑定、法线、切线、Alpha 或 Shading Model ID 疑问时启用。
4. `KeyOnly`、Forward / Deferred 对照和性能记录不作为每个 Case 的前置门槛，等视觉结果稳定后再针对问题补做。

场景可以通过多个相机或运行时控制切换观察条件，但不要为每个观察条件复制一套模型资源。

### 6.2 统一环境

- 背景使用低干扰的中性颜色，避免背景色被误认为材质散射颜色。
- 默认曝光、色调映射和 Bloom 设置保持一致，另存艺术化 Beauty 视图时必须记录差异。
- 主光方向和光源尺寸在可比较的案例之间尽量固定。
- 透明、Alpha Clip、Eye 和 ThinTranslucent 需要明确记录其 Forward / Deferred 限制，不能把不同路径的结果直接当作公式差异。

## 7. 分阶段落地计划

### Phase 0：冻结案例和授权边界

- [ ] 建立本计划中的场景名、资产名和主材质分配。
- [ ] 为每个外部资产记录原始页面、作者、许可证、下载时间和再分发条件。
- [ ] 明确 Lee Perry Smith 头模和 Hair 资产能否进入正式资源仓库。
- [ ] 明确孔明灯是取得授权还是制作自有替代资产。
- [ ] 明确 Cloth 是否能获得带 Pose、可导出、可再分发的 MD/CLO 工程。

### Phase 1：先落地授权清晰的静态资产

优先处理 Poly Haven 的植物和大理石资产，以及授权链确认后的皮肤头模：

- [ ] 导入 `Marble Bust 01`，建立 `DefaultLit` / `SubsurfaceProfile` 变体。
- [ ] 导入 `Potted Plant 02`，确认叶片 Alpha、双面材质和阴影路径。
- [ ] 导入 Lee Perry Smith Head Scan，建立三种 SSS Material Instance。
- [ ] 为每个 Case 写入来源和转换记录。

### Phase 2：导入 Hair 和建立独立头发 Case

- [ ] 获取 Short Wavy Hair With Bangs 的官方源文件和贴图。
- [ ] 检查 FBX / GLB 的网格、骨骼、材质槽、UV、法线和 Alpha。
- [ ] 离线生成稳定 Hair tangent，确认发片的 rootward 方向。
- [ ] 建立灰色头模、Hair MI 和专用展示场景。
- [ ] 使用 `OpaqueClip` 路径验证发片边缘、ShadowDepth 和轮廓光。

### Phase 3：建立 Cloth 和 ThinTranslucent Case

- [ ] 获取或制作可再分发的 MD/CLO 静态服装工程。
- [ ] 分离 `Body_Gray` 和 `Garment`，确认服装 UV、法线、切线和褶皱。
- [ ] 获取孔明灯的授权源文件，或按纸罩 / 骨架 / 燃烧器结构制作替代资产。
- [ ] 保持纸罩、骨架、火焰和发光源为独立材质和独立几何。
- [ ] 验证 Cloth 的 Forward / Deferred 共用 evaluator，以及 ThinTranslucent 的 Forward 透明路径。

### Phase 4：按需验证与证据归档

- [ ] 对每个 Case 执行资源引用和材质反射校验。
- [ ] 执行启动期 shader 编译、场景加载和运行时 smoke。
- [ ] 为已稳定 Case 记录一张 Beauty 截图，截图文件放在资源仓库 `Generated/Validation/` 下。
- [ ] 只有在具体视觉问题需要定位时，才补充 Backlight 或 Debug 截图。
- [ ] Forward / Deferred、GBuffer 对照和性能基线改为专项回归任务，不作为资产入库前置条件。
- [ ] 把发现的资产问题修复在转换或 authoring 阶段，不在 shader 中增加逐帧防御逻辑。

### Phase 5：可选的统一 Gallery

独立 Case 稳定后，再建立：

```text
SC_shading_model_gallery.json
```

Gallery 只负责把已验收的标准 Case 组织到同一展示空间，不复制材质和模型数据，也不替代
各自的独立场景。每个展示位保留名称牌、主 Shading Model、资产来源和验证状态，方便录屏、
回归截图和人工对比。

## 8. 验证矩阵与验收标准

### 8.1 通用验收

- [ ] 场景可以从 `config/config.json -> resourcePath` 正确解析全部资源。
- [ ] 所有 `SM_*.json`、`MI_*.json` 和纹理引用无悬挂路径。
- [ ] 主模型使用计划指定的 `M_*.json` 和 Shading Model ID。
- [ ] 辅助几何使用明确的 `DefaultLit` / `Unlit` 材质，不污染主案例结论。
- [ ] 相机、主光、轮廓光、环境和曝光配置可重复。
- [ ] 资产来源、许可证和转换记录齐全。
- [ ] 已稳定 Case 至少有一张可复现的 Beauty 截图；Backlight / Debug 仅按需补充。

### 8.2 按模型的验收重点

| 模型 | 必须观察的结果 |
| --- | --- |
| `DefaultLit` | 粗糙度、金属度、法线、AO 和阴影基线可解释 |
| `ClearCoat` | 清漆高光、底层透射 / 反射和边缘响应独立可见 |
| `Subsurface` | 局部 wrap、backscatter 和 thickness transmission 有稳定差异 |
| `PreintegratedSkin` | 皮肤 LUT、厚度和局部光照响应可对照 |
| `SubsurfaceProfile` | profile filter、邻域影响和非散射高光边界清晰 |
| `TwoSidedFoliage` | 叶片双面、背光、Alpha Clip 和阴影投射可见 |
| `Hair` | 发片 Alpha、Hair tangent、层叠、边缘高光和轮廓光可见 |
| `Cloth` | 织物褶皱、sheen、切线方向和服装接触关系可见 |
| `Eye` | 角膜、内层、虹膜、高光和角色内多材质排序稳定 |
| `ThinTranslucent` | 纸罩薄透射与独立燃烧器 / 火焰结构可区分 |

### 8.3 测试治理

本计划本身不新增运行时测试命令，不把场景截图当作数值正确性的替代品。实现阶段优先使用：

- 现有资源解析和材质反射校验；
- 现有启动期 shader 编译和场景 smoke；
- 仓库允许的 `--shader-reload-test`、`--shader-compute-reload-test` 和
  `--world-graph-transaction-test`，并按测试治理要求串行执行；
- 只有当现有模块测试无法覆盖资产或场景合同，且得到明确同意时，才增加新的测试目标。

## 9. 非目标与风险

### 9.1 非目标

- 本计划不重新实现 Hair、Cloth、Eye 或 Subsurface 的光照公式。
- 本计划不引入实时布料模拟、发丝动力学、DCC 插件或模型在线下载器。
- 本计划不把 `Strata` 或 `SingleLayerWater` 提前包装成已经支持的正式案例。
- 本计划不清理与案例无关的普通学习场景，也不移动尚未确认的归档候选。

### 9.2 主要风险

| 风险 | 影响 | 处理方式 |
| --- | --- | --- |
| 外部资产授权不完整 | 不能进入正式资源库 | 先记录为参考，改用授权清晰的替代资产 |
| Hair 缺少可靠切线 | 高光、轮廓和 Deferred 结果不稳定 | 离线转换阶段生成并验证 tangent |
| Foliage 没有独立 authoring 入口 | 植物只能落到 DefaultLit | 先完成 `TwoSidedFoliage` 材质和 Shadow 路由确认 |
| Cloth 服装和人体没有独立 mesh | 无法观察织物与人体接触关系 | 只接受 MD/CLO 导出或明确分离的资产 |
| ThinTranslucent 混入发光结构 | 无法判断纸罩本身的薄透射 | 纸罩、骨架、燃烧器和火焰保持独立 |
| 复杂场景遮蔽主效果 | 验证结论不可复现 | 先验收独立 Case，再组装 Gallery |

## 10. 本计划的完成定义

本计划在以下条件全部满足后，才可从“计划”迁移为资源验收记录：

- [ ] `SC_sphere_array` 和 `SC_car_showcase` 继续作为稳定基线。
- [ ] 皮肤、石材、植物、Hair、Cloth 和 ThinTranslucent 的独立场景全部有明确状态。
- [ ] 每个已入库资产都有可追溯授权和转换记录。
- [ ] 每个主材质都能在场景中被明确识别，辅助材质没有混淆主结论。
- [ ] Beauty、Backlight、Debug 三类观察证据已经归档。
- [ ] 资源引用、shader 编译、场景加载和允许的 runtime smoke 均通过。
- [ ] 尚未完成的 Eye、Foliage authoring、Water 和 Strata 边界仍被明确标记，没有过度宣称支持。
