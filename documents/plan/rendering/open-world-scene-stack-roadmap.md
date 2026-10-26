# 开放世界场景栈路线图：地形 / 碰撞 / 植被 / 风 / LOD / Shading

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 跨系统路线图（未来工作，不是契约） |
| 状态 | 概念梳理，未实现；地形渲染先按 `terrain-openworld-demo-plan.md` 推进 |
| 修订 | 2026-10-19 第 1 版 |
| 关联 | [地形复刻计划](terrain-openworld-demo-plan.md)、[GASP 计划](../architecture/gasp-csharp-animation-plan.md)、[Shading Model 计划](shading-model-alignment-plan.md) |
| 范围 | 地形碰撞、GASP 角色、植被/石头撒点、PP2 风数据、3D 风力点、LOD、自定义 shading model |
| 非目标 | GPU-driven、VT/clipmap、streaming、地形编辑、完整编辑器工具链 |

> 本文件是计划，不是契约。每个子系统落地后再拆出 `documents/contracts/*`；
> 在那之前，源码与测试才是事实。

## 0. 结论与顺序

整套东西不是并列的，依赖关系很强：

```text
地形渲染（terrain plan）
 ├─ 地形碰撞 ──> 胶囊角色 ──> GASP 跑图
 ├─ 撒点/对齐 ──> GPU instancing ──> 植被/石头
 │                             └─ PP2 风数据 ──> 3D 风力点 ──> 植被风
 ├─ 地形 LOD
 └─ 自定义 shading model（可与上面并行，但 foliage shading 要等风法线）
```

建议顺序：

1. **P0 地形渲染**：先把 1km 地形跑起来（当前计划）。
2. **P1 地形碰撞 + 胶囊角色**：GASP 能站、能走、能跑；碰撞也提供撒点需要的高度/法线查询。
3. **P2 GPU instancing + 撒点/对齐**：先把静态植被/石头铺出去，不接风。
4. **P3 PP2 + 全局风**：单棵树的分层摆动，再扩到实例化森林。
5. **P4 3D 风力点 + 角色交互**：场景里部署风点，植被局部响应；角色推草/推枝。
6. **P5 LOD**：地形、植被、石头的 LOD 和 impostor；这时才有足够的实例数调参。
7. **P6 自定义 shading + 画面**：foliage/草/地表 shading，最后做 Open World Demo 的画面。

两个容易误判的点：

- **PP2 不是撒点算法**。PP2 = Pivot Painter 2，把 pivot/层级/风数据烘进纹理或顶点，解决"树怎么分层摆动"；
  "植被/石头怎么对齐地形"是撒点系统，不是 PP2。石头一般不需要 PP2。
- **LOD 和 instancing 不是后期优化**。没有 instancing，植被数量上不去；没有 LOD，远景必炸。
  它们应该在撒点阶段就进入设计，而不是等画面做完再补。

## 1. 现状盘点

| 子系统 | 现状 | 缺口 |
| --- | --- | --- |
| 地形渲染 | 无运行时；只有编辑器资产层；有复刻计划 | 高度图网格、材质、texture array、场景接入 |
| 地形碰撞 | Jolt 只在 C# Blueprints 开启时编译；现有 `ScriptPhysicsWorld` 用 capsule/box/mesh | 没有 heightfield；没有地形静态体；物理单位与 cm 场景的换算要固定 |
| 角色 | Jolt `CharacterVirtual` 胶囊已存在；GASP 计划未实现 | 地形上的走/跑/贴地/脚部 IK；GASP Motion Matching |
| GPU instancing | 没有 `DrawIndexedInstanced`；每个 `RenderableObject` 一次 draw | 实例缓冲、逐实例剔除、实例化阴影、批处理 |
| 植被/石头 | 没有撒点系统；只有普通 mesh 场景对象 | 地形查询、密度/排除、Poisson/聚类、对齐、嵌入、确定性种子 |
| PP2 | 没有 | DCC 烘焙格式、导入/sidecar、顶点/纹理数据、分层风 shader |
| 风 | 只有 SpeedTree 专用风（`speedTreeWindSystem.*`） | 通用植被风、3D 风力点、角色交互 |
| LOD | 没有运行时 LOD；SpeedTree 只取最高细节 | 网格 LOD 导入/生成、地形 LOD、植被 LOD/impostor、HLOD |
| 自定义 shading | `ShadingModelID` 与 `MaterialInputs` 接口在，模型实现已删 | 每个模型的求值器、dispatch、材质校验、GBuffer/forward、测试 |
| 编辑器工具 | 无地形/植被/风编辑 | 撒点预览、风点 gizmo、LOD debug；可后置 |

## 2. 子系统拆解

### 2.1 地形碰撞与角色

**目标**：胶囊角色能在 1km 地形上稳定行走；GASP 后续接入时不需要重做碰撞。

**设计要点**

- Jolt 当前用米（`SetGravity({0, -9.81, 0})`），场景单位是 cm；地形高度场要按 cm→m 缩放，
  胶囊/物理参数保持米制。这个换算必须在边界一次做对。
- 用 `JPH::HeightFieldShapeSettings`：`mSampleCount`、`mOffset`、`mScale`、`mHeightSamples`。
  高度场比三角网格省内存，也适合 256²/512² 地形。
- 地形静态体单独 object layer；角色 capsule 与地形、石头、树干分层过滤。
- `GroundProbe` 已有脚部射线接口；地形法线、脚底支撑、坡度限制都走它。
- 石头用简化碰撞（box/convex/trimesh）；树干用 capsule；草不做碰撞。
- 物理当前耦合在 `VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS`；如果要"地形碰撞独立于 C#"，
  需要把 Jolt 的编译开关拆出来，这是一项要先决定的事。

**任务**

1. 地形高度场 → Jolt 静态体；cm→m 换算；调试线框。
2. 胶囊角色在地形上的 slope/step/贴地测试；`CharacterVirtual` 参数校准。
3. `GroundProbe` 接地形；脚部 IK/贴地验收。
4. 石头/树干碰撞体；碰撞层与过滤。
5. GASP 接入：Motion Matching 跑图、跳跃、翻越（按 GASP 计划）。
6. GTest：高度场采样、射线命中、坡度限制、单位换算。

**验收**

- 胶囊沿 1km 地形走一圈不穿地、不抖；30°/45° 坡、20cm/40cm 台阶有明确行为。
- 脚部探测在地形法线变化处稳定；GASP 跑图不滑步。
- 物理开启/关闭不影响纯渲染场景。

### 2.2 GPU instancing 与撒点/对齐

**目标**：把植被、石头按规则铺到地形上，数量到"像森林"的级别，且能被剔除/LOD。

**为什么先做 instancing**

- 现在每个对象一次 draw；几千棵树/石头会直接打爆 CPU 提交。
- 植被需要逐实例 transform、随机相位、LOD、风参数；这些都要实例缓冲。

**设计要点**

- 后端增加 `DrawIndexedInstanced`；每实例缓冲至少包含：矩阵/TRS、LOD、随机种子、风相位、颜色变体。
- 批处理按 mesh + material 分组；逐实例视锥/距离剔除；阴影 pass 同样实例化。
- 地形查询 API：高度、法线、坡度、层权重、是否水面/道路；撒点和碰撞共用。
- 撒点算法：Poisson disk / blue noise 做最小间距；再叠 density mask（高度、坡度、地表层、噪声）
  和 exclusion（道路、水、岩石、已有树）。聚类用低频噪声控制成片/稀疏。
- 对齐：
  - **树**：保持竖直（重力），只随机 yaw/scale；坡上可轻微倾斜，但不能完全贴法线。
  - **石头**：贴合地形法线、随机旋转、部分嵌入地面，避免悬空。
  - **草**：贴法线 + 随机 yaw/scale；密度最高，必须 instancing + LOD。
- 确定性：固定 seed + 版本号；同一输入生成同一批实例，便于回归和截图对比。
- 数据：离线烘 `VEG_*.json` / 运行时撒点二选一；先离线烘，运行时只加载实例缓冲。

**任务**

1. 后端 `DrawIndexedInstanced` + 实例缓冲 + 描述符；现有单实例路径不变。
2. 地形查询 API（高度/法线/坡度/层权重）。
3. 撒点器：Poisson/blue noise、density mask、exclusion、随机 TRS、确定性 seed。
4. 对齐规则：树竖直、石贴法线+嵌入、草贴法线。
5. 实例化剔除/批处理/阴影；实例数据离线烘。
6. 碰撞体生成（石头/树干）；草不碰。
7. GTest：撒点确定性、最小间距、坡度过滤、悬空检测。

**验收**

- 1km 场景 1k 棵树 + 5k 块石头 + 草（可选）能稳定提交；draw call 与实例数可观测。
- 没有明显悬空/穿地；树在陡坡上不"倒栽"；石头贴地自然。
- 同 seed 两次生成结果一致；剔除后不可见实例不产生 GPU 提交。

### 2.3 PP2：植被分层风数据

**目标**：一棵树/一丛草能按"主干 → 枝 → 叶"的层级摆动，而不是整棵平移。

**PP2 是什么**

- Epic 的 **Pivot Painter 2**：在 3ds Max 里按层级绑定元素，把 pivot 位置、X 轴/长度、父索引、
  距根步数、随机值、包围盒等信息烘成纹理（UE 用额外 UV 采样）。
- 官方支持最多 4 层、30,000 个元素；一片叶子/一根枝是一个 element。
- 运行时在顶点着色器里按父级链旋转/偏移，得到分层风；法线也要跟着转。
- 它解决的是**动画数据**，不解决撒点/对齐；石头不需要 PP2。

**VL 的落地选项**

| 方案 | 数据 | 优点 | 缺点 |
| --- | --- | --- | --- |
| A. 顶点属性 | pivot、x-axis、parent、depth、random 打进 `Vertex` | 无额外纹理绑定；实现直接 | 顶点变大；30k 元素不现实；只能做浅层级 |
| B. PP2 数据纹理 | 两张纹理（pivot/index、x-vector/extent）+ UV2 | 和 UE 一致；元素多 | 每树种要独立 MI/纹理；顶点着色器采样 |
| C. sidecar + 顶点压缩 | DCC 导出 JSON，导入时压成 2~4 个 vec4 | 可控；适合 2~3 层 | 自定义工具链；每树种一份数据 |

建议：**M1 先做方案 C（单棵树、2 层）**跑通分层风；森林阶段再评估 B（数据纹理）或压缩属性。
不要一上来做 30k 元素的完整 PP2。

**任务**

1. DCC 导出约定：层级、pivot、X 轴、父索引、深度、随机值；单位 cm。
2. 导入/sidecar：Assimp 网格 + PP2 数据；校验元素数/索引范围。
3. 顶点着色器：解码层级、按 pivot 旋转、风向量、法线旋转。
4. 材质参数：每层风强度/频率/相位；每实例随机相位。
5. 实例化：per-instance phase/scale 进实例缓冲。
6. LOD：每级 LOD 的 PP2 数据/层级简化。
7. GTest：解码、单枝旋转、法线方向、越界索引。

**验收**

- 单棵树主干/枝/叶三层摆动，相位不一致，没有"整棵平移"。
- 同树种多实例相位不同；风停时回到静止姿态。
- 法线跟着弯，光照不穿帮；LOD 切换不跳层级。

### 2.4 3D 风力点系统

**目标**：在场景里"撒"风点/风区，局部改变风向/强度，接入植被风；角色经过时也能推植被。

**设计要点**

- 全局风：方向、速度、阵风、湍流、时间相位；所有植被的基础输入。
- 3D 风点（`WindPoint`）：位置、半径、方向、强度、falloff、频率、相位、优先级。
  可以理解为场景里的"风源"，不是粒子。
- 组合：`wind = global + Σ(point_i.falloff(distance) * point_i.vector)`；
  再加 noise/turbulence；不要让风点数量线性爆掉。
- 空间加速：按地形 chunk/网格分桶，只把最近/影响范围内的点传给 shader；
  GPU 端用 UBO/SSBO + active list，限制每像素/每顶点采样数（例如 4 个）。
- 角色交互：胶囊/点推挤单独一个 buffer；植被顶点按距离/朝向弯曲；
  和风点共用"力场采样"接口，但不要混为同一数据。
- 数据/作者：`W_*.json` 或场景 `windField` 对象；编辑器 gizmo 后置。
- 性能：风点数量、每顶点采样数、LOD 降频（远处每实例一次而不是每顶点）。

**任务**

1. `W_*.json` / `windField` 场景对象 schema；全局风 + 点列表。
2. CPU 空间分桶/活跃列表；GPU UBO/SSBO 上传。
3. 顶点着色器风场采样：全局 + 最近 4 点 + 噪声；PP2 层级权重。
4. 角色交互 buffer + 推挤。
5. 调试可视化：风向量、影响半径、活跃点。
6. GTest：falloff、组合、确定性、边界。

**验收**

- 场景里放 8~32 个风点，植被局部摆动方向/强度正确；没有风点处回到全局风。
- 角色穿过草丛/树枝时有可见推挤；不产生 NaN/爆炸。
- 风点数量增加时，帧时间可控；远处 LOD 降频。

### 2.5 LOD 系统

**目标**：地形、植被、石头在远景下保持形状和性能；没有明显裂缝/跳变。

**现状**

- 运行时没有 LOD；SpeedTree 导入只取最高细节（源码注释已说明）。
- 没有 impostor/billboard/HLOD。

**分层**

| 对象 | LOD 策略 |
| --- | --- |
| 地形 | chunk 四叉树 4~6 级；边缘 stitch/skirt；CPU 选择；阴影用低一级 |
| 树 | 3 级 mesh LOD + 远景 billboard/impostor；dithered cross-fade；per-instance |
| 石头 | 2~3 级 mesh LOD；远景 impostor 可选 |
| 草 | 近处 mesh/卡片，远处 impostor/贴地片；距离剔除 |
| 场景 | HLOD 合并远景 cluster（后置） |

**设计要点**

- LOD 选择：距离 + 屏幕误差 + hysteresis；每实例独立；阴影和主视图分开。
- 切换：dithered fade 或 alpha cross-fade；避免硬跳。
- 地形 LOD：CPU 四叉树（GPU-driven 仍搁置）；stitch 用 LOD map/skirt；先做 4 级。
- 植被 LOD：实例缓冲里带 LOD 索引；按 chunk 批处理；远景不提交阴影。
- 网格 LOD 来源：源资产多 LOD 文件，或离线 meshoptimizer 简化；不要运行时简化。
- 指标：三角形数、实例数、draw call、GPU 时间；Tracy 标记。

**任务**

1. 网格 LOD 导入/生成与 `MeshSection` LOD 分组。
2. 地形 chunk 四叉树 + stitch/skirt + 选择。
3. 植被/石头 per-instance LOD + cross-fade。
4. 阴影 LOD 与远距离剔除。
5. impostor/billboard 烘焙（树/草）。
6. HLOD（后置）。
7. GTest：LOD 选择、无裂缝、fade 不闪。

**验收**

- 1km 地形 4 级 LOD，边缘无裂缝；相机快速移动不闪。
- 森林远景三角形数下降明显；LOD 切换可见但不跳。
- 阴影随 LOD 降级，不产生明显漏光。

### 2.6 自定义 shading model

**目标**：植被/草/地表有匹配的 shading；不依赖 UE 的模型实现。

**前提**

- 现有 `ShadingModelID`、`MaterialInputs`、`MaterialSurface`、GBuffer 布局是对齐 UE 的接口，
  但模型实现已删除；加模型必须同时加**求值器 + dispatch + 材质校验 + 输出路径**，
  否则 ID 会静默回退到 DefaultLit（仓库已踩过这个坑）。
- Deferred 模型还要 GBuffer 编解码/customData；forward-only 模型必须拒绝 deferred 作者。

**优先模型**

1. **TwoSidedFoliage**：双面、背面法线翻转、透射/背光；树叶/草的第一优先。
2. **Subsurface/PreintegratedSkin**：如果草/苔藓需要柔和次表面，再评估。
3. 地形保持 `DefaultLit` + 8 层混合，不新增地形专用 shading model。
4. 自定义模型可以偏离 UE，但接口/GBuffer 预算要写清楚。

**任务**

1. 审一遍 `materialInputs.glsl` / `materialSurface.glsl` / `gbufferCodec.glsl` 的现有字段。
2. TwoSidedFoliage 求值器 + deferred dispatch + forward 路径。
3. 材质校验：哪些字段/输出组合合法；deferred 拒绝。
4. Debug view：模型 ID、关键项、法线。
5. 参考图/数值测试；GBuffer roundtrip。
6. 后续模型按同一流程，一次一个。

**验收**

- 树叶双面光照正确；背面不透光穿帮；风弯后法线/光照连续。
- 材质作者写错模型/字段时有明确报错，不静默回退。
- 现有 DefaultLit 场景不回归。

### 2.7 场景与资产管线

- 高度图、8 组地表贴图、树/石头资产、PP2 数据、风点、LOD、场景 JSON。
- Open World Demo Collection 只做**构图/地表参考**；资产用 CC0 或确认许可。
- 资产校验：单位、包围盒、碰撞体、PP2 索引、LOD 级别、材质槽。
- 工具后置：撒点预览、风点 gizmo、LOD debug、PP2 预览。

## 3. 阶段计划

| 阶段 | 目标 | 主要交付 | 验收 | 粗估 |
| --- | --- | --- | --- | --- |
| P0 | 地形渲染 | terrain plan 的 M0–M5 | 1km 地形、8 层 top-4、texture array | 2–3 周 |
| P1 | 碰撞 + 角色 | heightfield、capsule、GroundProbe、GASP 接入 | 胶囊走/跑 1km，坡/台阶稳定 | 2–4 周 |
| P2 | instancing + 撒点 | `DrawIndexedInstanced`、撒点器、树/石对齐、剔除 | 1k 树 + 5k 石，稳定帧时间 | 3–5 周 |
| P3 | PP2 + 全局风 | 单树分层风、实例相位、全局风 | 树分层摆动，多实例相位不同 | 3–6 周 |
| P4 | 3D 风点 + 交互 | `W_*.json`、风点 buffer、角色推挤 | 8–32 风点局部影响，角色推草 | 2–4 周 |
| P5 | LOD | 地形四叉树、植被 3 级 + impostor、阴影 LOD | 远景性能达标，无裂缝/大跳变 | 3–6 周 |
| P6 | shading + 画面 | TwoSidedFoliage、草地表、光照/雾、最终场景 | 接近参考构图/材质 | 3–6 周 |

总计约 **5–8 个月**（不含美术资产制作；按单人/小团队粗估）。每个阶段都要有可截图/可交互的验收，
不以"编译通过"代替效果验收。

## 4. 依赖与并行

- P1 依赖 P0；P1 不依赖植被。
- P2 依赖 P0 的地形查询；instancing 可以先于撒点单独做。
- P3 依赖 P2 的实例化；单树 PP2 可以在 P2 之前做实验，但森林验收要等 instancing。
- P4 依赖 P3 的全局风；风点数据/调试可与 PP2 并行。
- P5 依赖 P2/P3；地形 LOD 可以在 P1 之后单独开始。
- P6 依赖 P3/P5；TwoSidedFoliage 可以提前在球/片上开发，最终验收要等风和 LOD。
- 自定义 shading 与地形渲染接口有交叉，但不要阻塞地形主链路。

## 5. 横切关注点

### 5.1 性能预算

- 每阶段记录：CPU submission、GPU 时间、draw call、实例数、三角形数、显存。
- 目标：1080p 可交互；森林/草地的预算按场景定，不先承诺 60fps。
- 用 Tracy/NVTX 标记：地形生成、撒点、实例剔除、风更新、LOD 选择。

### 5.2 测试

- GTest：高度场/单位换算、撒点确定性、PP2 解码、风场采样、LOD 选择、材质校验。
- 场景验收：可交互 scene + 3 张固定机位截图；和参考图比构图/明度/色相。
- 回归：现有 `SC_sphere_array` / `SC_uds_mountain_range` / paper case 场景不回归。

### 5.3 文档

- 每个子系统落地后写 `documents/contracts/*`；计划文档只保留决策与参考。
- 更新 `routing.md` 的源码路径；更新 `AGENTS.md` 的地形/物理/植被说明。
- `py -3 tool\validate-docs.py` 是门禁。

### 5.4 许可

- UE 场景只做参考；不把 `.uasset`/UE-only 资产搬进 VL。
- 贴图/模型用 CC0 或确认许可的 Fab Standard；PP2 数据如果是自烘，资产来源要记录。

## 6. 风险

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 范围爆炸 | 每个系统都很大，容易做不完 | 先 vertical slice；P0–P4 是一条可玩的线，LOD/shading 后置 |
| Jolt 耦合 C# | 不启 C# 就没有物理 | 先决定是否拆 Jolt 开关；地形碰撞独立编译 |
| 没有 instancing | 植被数量上不去 | P2 先做 `DrawIndexedInstanced`，再做撒点 |
| PP2 工具链 | DCC 导出/导入成本高 | 先单树 2 层 + sidecar；不要一上来 30k 元素 |
| 风点作者/调参 | 效果难复现 | `W_*.json` + seed + debug 可视化；先全局风再风点 |
| LOD 裂缝/跳变 | 画面穿帮 | stitch/skirt、dithered fade、hysteresis；固定机位回归 |
| 自定义 shading 的 GBuffer 预算 | 通道不够/回退 | 一次一个模型；deferred/forward 明确；材质校验 |
| GPU-driven 搁置 | 密植被/大地形可能回到 CPU 瓶颈 | 先 CPU 方案；用 Tracy 判断是否必须回头做 GPU-driven |
| 许可 | 不能分发 | 参考与资产分离；CC0/确认许可 |

## 7. 开放问题

1. GASP 用哪个版本/哪套角色资产？当前 GASP 计划说尚未定位 GASP 项目，这会阻塞 P1 的最终验收。
2. 地形碰撞用 heightfield 还是 trimesh？分辨率跟 256² 渲染网格还是 1024² 高度图？
3. Jolt 是否从 `VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS` 拆出来，让地形碰撞独立？
4. 植被目标数量：多少树/石头/草？这决定 instancing 和 LOD 的预算。
5. PP2 的 DCC 管线：3ds Max / Houdini 哪条？有没有可用的树资产？先做几层？
6. 3D 风点：作者用 `W_*.json` 还是场景对象？最多多少个点？每顶点采样几个？
7. LOD：树用 3 级 mesh + impostor，还是直接 billboard？网格简化用 meshoptimizer 还是源资产多 LOD？
8. 自定义 shading 的优先级：TwoSidedFoliage 先做，还是先做草的 subsurface？
9. 编辑器工具（撒点/风点/LOD debug）放到哪个阶段？

## 8. 整条 vertical slice 的 Definition of Done

- [ ] 1km 地形可渲染，8 层 top-4 正常。
- [ ] 胶囊角色能在地形上走/跑，坡/台阶/贴地稳定；GASP 可接入。
- [ ] 1k 树 + 5k 石实例化、对齐、剔除；无悬空/穿地。
- [ ] 单树 PP2 分层风 + 全局风 + 3D 风点局部影响。
- [ ] 角色经过植被有推挤。
- [ ] 地形/植被/石头有 LOD，远景无明显裂缝/跳变。
- [ ] TwoSidedFoliage 正常，树叶双面/背光/风法线正确。
- [ ] 1080p 可交互；Tracy 有时间线；回归与文档门禁通过。
