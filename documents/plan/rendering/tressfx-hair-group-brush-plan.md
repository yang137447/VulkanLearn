# TressFX 头发分组笔刷与可视化方案

状态：Phase 1–4 已于 2026-10-28 实现（Groups 数据/UI、GPU 分组着色、视口笔刷与 ghost、
per-group CPU/GPU 求解）；Phase 5（TressFXGroup/Visibility/Tile 的 draw/culling/LOD）未实现，
且按 §1 非目标不属于本方案。本文属于 `documents/plan/`，不是当前实现契约。

基线：`codex/tressfx5-hair-port`，2026-10-28 的头发物理阻力提交 `3a849e1`。当前运行期边界以
`documents/contracts/tressfx-asset-import.md` 为准。

## 1. 目标与非目标

目标：

- 在 TF 资产 Tab 里可视化地创建、编辑、删除 TressFX guide 组；
- 用笔刷按发根选择整根 guide，并把整根发丝归入目标组；
- 笔刷有虚拟工作面、朝向/深度剔除和 ghost 预览；
- 组参数覆盖继续使用现有 `groups[].simulationOverrides`；
- 分组视觉和参数编辑在同一个上下文里，能直接看出“哪一撮头发属于哪个组”；
- 把 surface brush / root selection 抽成可复用组件，后续可用于 fur、植被等。

非目标：

- 不把 TF 资产 `groups[]` 和 TressFX Renderer 的 `TressFXGroup` 绘制层混为一谈；
- 本方案不实现 `TressFXVisibility` / `TressFXTile` / `TressFXGroup` 的 LOD/culling；
- 不实现发丝自碰撞、发束遮蔽、升力；
- 不允许按组覆盖场景风源、重力、迭代次数、backend 或碰撞；
- 不改变 `SceneWindRuntime` 和树木风响应。

## 2. 当前事实

### 2.1 已经有的

- TF 描述符支持 `groups[]`：
  - `name` 唯一；
  - `guideRanges` 为半开区间 `[start, end)`，组内/跨组不重叠；
  - `simulationOverrides` 只允许 6 个字段：`damping`、`bendStiffness`、
    `globalShapeStiffness`、`globalShapeEffectiveRange`、`vspCoefficient`、
    `vspAccelThresholdMax`。
  - 见 `documents/contracts/tressfx-asset-import.md#分组groups`。
- 导入期生成 `MeshSection::tressfxGuideGroupIds`：0 = 默认组，k = 声明序第 k 组。
- `asset-modify-api` 已支持整段 `groups` 数组修改。
- `MeshSection` 已有 `tressfxRootPositions`，可用于 CPU 发根点云。
- TF 资产 Tab 已有 `Simulation Defaults`、`Preview Wind`、预览视口和保存流程。
- 头发材质有 `M_strandProbe`（Unlit）与 `M_strandPbr`（DefaultLit）两个预览路径。

### 2.2 当前缺口

- `tressfxGuideGroupIds` 只存在于 CPU 导入结果，不进入 GPU；着色器没有 group id；
- `simulationOverrides` 尚未参与运行期求解，所有 guide 共享一份 `TressfxSimulationConfig`；
- TF Tab 没有 Groups 列表、组范围编辑、组参数覆盖 UI；
- 编辑器侧没有可直接用于 range 上界的 `guideCount` 字段；需要把导入后的 guide 数暴露到
  Tressfx 文档快照/预览快照，否则 UI 无法校验 `end <= guideCount`；
- 没有笔刷、虚拟工作面、发根拾取或 ghost 预览；
- `MeshSection` 只有 `tressfxRootPositions`（根位置）和 `tressfxGuideGroupIds`；
  没有 `rootGrowthDir` / `rootNormal` 存储，`.tfx` 也没有头皮法线；笔刷前必须补缓存；
- 当前 `modelCacheKey` 把组名/区间拼进几何身份；任何组编辑都可能触发整份 ribbon 重新导入，
  这是 Phase 1 必须先拆掉的缓存问题；
- `TressFXStrands` 目前只绑定 Object Set 2 binding 3 的中心线 SSBO，没有 group id buffer；
- TressFX Renderer 的 `TressFXGroup` / `TressFXVisibility` / `TressFXTile`
  仍按 `tressfx5-port-map.md` 的 V3 计划推迟，不属于本方案第一阶段。

## 3. 术语

- **Default / 默认组**：组号 0，未显式分组的 guide；不能删除，参数等于资产默认/placement 覆盖。
- **目标组**：当前笔刷要写入的命名组；从 Groups 列表选中。
- **发根点云**：每个 guide 的根位置、生长方向、局部法线、group id。
- **虚拟工作面**：由笔刷附近发根的位置和法线拟合出的局部平面；笔刷圆环、落点和选择距离都在这个平面/世界空间里，不是屏幕空间圆形。
- **Ghost preview**：笔刷范围内、尚未提交的整根发丝用目标色低强度 tint 显示，不写资产。
- **提交**：松开笔刷后把选择结果写入 `groups[].guideRanges`。

## 4. Tab 交互

### 4.1 布局

在 `Simulation Defaults` 下面增加可折叠的 `Groups` 区：

```text
Groups                                   [Group Colors: On]
  ● Default        39,934 guides
  ● fringe            180 guides   [0,120) [340,400)   2 overrides   >
  ● side              220 guides   [120,340)            0 overrides   >

  [+ New Group]
```

组 0 就是默认/未分组，不能单独再列一个 `unassigned`。`Default` 的数量等于
`guideCount - 所有命名组覆盖的 guide 数`；上例 `40,334 - 180 - 220 = 39,934`。

选中组后显示组检查器：

```text
Group: fringe                             [■ color]
Name: [fringe................]

Guides
  [0] – [120)   [Remove]
  [340] – [400) [Remove]
  [+ Add Range]   [Select in Viewport]

Overrides
  Damping          [x] override  0.020   (asset default 0.035)
  Bend             [ ] inherit   (asset default 0.300)
  Global Shape     [ ] inherit   (asset default 0.000)
  Shape Range      [ ] inherit   (asset default 0.000)
  VSP              [ ] inherit   (asset default 0.000)
  VSP Threshold    [ ] inherit   (asset default 140.0)

  [Clear Overrides]   [Delete Group]
```

`asset default` 是当前资产/placement 解析后的值，不是写死的 UI 常量；不要把 Female
资产里的 `0.458/0.256/0.722` 当成所有资产的默认值。

实现取舍（2026-10-29）：区间列表不作为作者 UI 暴露。组内 guide 的增删全部通过
`Paint Groups` 圆形笔刷/套索/Remove 模式完成；列表只显示组名、guide 数和 override 数。
`+ New Group` 只输入名称；空组是合法作者状态，之后用笔刷填充。区间列表不暴露。

`Default` 不可重命名/删除，也不能作为笔刷目标组：选中 Default 时 `Paint Groups` 禁用，
避免把 guide 写回隐式默认组。命名组才写 `simulationOverrides`。

### 4.2 新建组

`groups[].guideRanges` 允许为空；新建组可以先没有 guide。

- `+ New Group` 打开创建行，只要求名称；`Create` 在名称合法时可用；
- 名称进入 inline 编辑；
- `Esc` 取消创建，`Enter` 确认名称；
- 名称重复、空名称、`Default`/`unassigned` 保留名都在行内报错；
- 删除组的最后一个 range 后保留空组；是否删除组由显式 Delete Group 决定；
- 组列表顺序就是 `groups[]` 的声明顺序；删除组 k 会把 k+1..N 的运行时组号前移，
  提交前必须重映射颜色、override 控件和 GPU group id。

### 4.3 笔刷模式

点击 `Paint Groups` 进入：

```text
[Paint Groups]  Brush: [20 px]  Mode: [Add ▾]  [Replace existing: Off]
```

视口左上角显示：

```text
Painting → fringe
+18 guides in brush
```

笔刷圆环绘制在发根工作面内，不是贴在屏幕上的圆形；它随 `brushNormal` 倾斜，并在平面内按
`r_world` 缩放。移动鼠标只改变 ray 与平面的交点，不改变选择度量。

操作：

| 操作 | 行为 |
| --- | --- |
| 左键拖动 | 把发根加入目标组 |
| `Shift` + 左键 | 追加 |
| `Alt` + 左键 | 从目标组移除 |
| 右键/中键 | 旋转/平移相机，不退出笔刷 |
| `[` / `]` | 调整笔刷半径 |
| `Alt` + 滚轮 | 调整笔刷半径；普通滚轮仍缩放相机 |
| `Enter` / 松开 | 提交当前笔画 |
| `Esc` | 放弃当前笔画并退出 |
| `Ctrl+Z` | 撤销上一笔提交 |

笔画期间只更新 ghost preview；提交时一次性写入 `groups[]` 草稿。

`Ctrl+Z` 只撤销本笔刷会话内的上一笔提交，不是编辑器全局 undo；资产级撤销仍走现有草稿/重载流程。

### 4.4 冲突处理

- 默认 `Replace existing: Off`：
  - 已属于其他组的发根不移动；
  - 状态栏显示 `12 guides belong to "side"`；
  - 这些 guide 仍显示目标色 ghost，但保留原组颜色边缘/角标，提示不会被移动。
- 打开 `Replace existing: On`：
  - 允许把 guide 从旧组移到目标组；
  - 旧组变空时保留为空组，不自动删除；需要时显式 Delete Group；
  - 提交前可撤销。
- 不在目标组的默认组 guide：正常加入目标组。

## 5. 视觉状态

| 状态 | 显示 |
| --- | --- |
| 已在目标组 | 组色直接顶掉 `MaterialInputs.baseColor`（等价 `u_tintColor`），PBR 光照/粗糙度/高光保留 |
| 不在目标组，在笔刷范围 | 目标色 ghost，约 25–35%，按发根权重衰减 |
| 属于其他组，在笔刷范围 | 目标色 ghost + 原组色边缘/角标 |
| Remove 模式下的目标组 | 橙红 ghost，表示将被移除 |
| 笔刷范围外 | 正常组色或材质色 |

Ghost 只改 RGB tint，不改 alpha。因为 strand 路径当前强制 `inputs.opacity = 1.0` 并通过
coverage discard 决定保留，ghost 头发必须保持完全不透明；不能靠 alpha 淡出。
分组色写入 `MaterialInputs.baseColor` 而不是最终 `sceneColor`：这样颜色被替换，
但 roughness/specular/AO/阴影仍由 PBR 计算。默认组保持原材质基色；调色板按 sRGB
存储，shader 在写入基色前转线性。

Tint 不能放在 `M_strandProbe.surface.glsl` / `M_strandPbr.surface.glsl`：它们只返回
`MaterialInputs`，最终颜色在 `shader/glsl/engine/passTemplate/base.frag.glsl` 的
`BuildMaterialForwardOutput` 之后生成。正确落点是 **forward output 阶段**：

```glsl
// base.frag.glsl：BuildMaterialForwardOutput 之后
vec3 shaded = outSceneColor.rgb; // 或对应 forward output 变量
float ghost = tressfxBrushActive && !tressfxInTargetGroup
    ? tressfxBrushWeight * 0.30 : 0.0;
outSceneColor.rgb = mix(shaded, tressfxTargetGroupColor, ghost);
```

需要新增 flat varying（建议继续接在现有 coverage seed 后面的 location 9）：

- `flat uint tressfxGroupId`
- `flat float tressfxBrushWeight`
- `flat uint tressfxBrushFlags` / target group

`M_strandProbe` 与 `M_strandPbr` 的 surface shader 不需要改 tint 逻辑；它们只负责原有
材质输入。`Group Colors` / `Selected` / `Solo` 是 preview-only uniform 状态，不写资产。

整根 guide 使用发根权重，因此整根均匀变色、边缘自然淡出；但 ghost 不能改变覆盖率，
只能改变 RGB。

## 6. 选择算法

### 6.1 发根点云

每个 guide 一条：

```text
guideIndex
rootPosition        // hair local，渲染时乘当前 head/model 矩阵
rootGrowthDir       // normalize(p1 - p0)，发根生长方向
rootNormal          // 头皮/发根表面法线，笔刷选择的主判据；Phase 3 必须提供
groupIndex
```

`rootGrowthDir` 与 `rootNormal` 不是同一个量：

- `rootGrowthDir` 是发丝从根向梢的方向，适合作 comb 模式的笔画方向 gate；
- `rootNormal` 是头皮表面法线，适合做正反面/背面剔除。

`.tfx` 本身没有头皮法线；加载/预览初始化时从根点邻域 PCA 估计，或从头部三角面法线取最近值。
`rootNormal` 是笔刷 normal 的主判据，不能把 `p1-p0` 硬当成头皮法线；PCA 只能作为无头皮网格时的
近似。两者都不可用时，笔刷 normal 使用相机朝向 `-viewDir` 兜底，并在 UI 标注这是降级行为。

根点使用 rest/root 位置，保证分组是作者数据，不随每帧模拟漂移；如果角色有 pose，使用根点随 head 矩阵变换后的位置。

根方向和根法线需要在导入/预览准备阶段缓存：

- `MeshSection::tressfxRootDirections`（或编辑器快照中的等价字段）由 `p1 - p0` 生成；
- `tressfxRootNormals` 从根点邻域 PCA 或头/头皮三角面法线生成；
- 当前 `MeshSection` 只有 `tressfxRootPositions`，没有这两个数组，Phase 3 前必须补；
- 缓存按资产/pose 失效，不在每次笔刷采样时重算整份法线。

### 6.2 虚拟工作面

原则：**屏幕只负责生成 cursor ray；笔刷 stamp、选择距离、法线判定都在发根平面/世界空间里。**
不要用屏幕距离当 falloff，也不要用屏幕圆形决定命中。

每次笔刷采样：

1. 鼠标 screen position 生成 camera ray；
2. 在发根点云的 **3D 空间索引**（hair local/world 的 uniform grid 或 BVH）里查询 ray 附近的候选根；
3. 对候选做位置 + 法线加权拟合：

```text
S = weighted average(root.position)
N = normalize(weighted average(root.normal))
if N 退化: N = 位置 PCA 最小特征向量
if 仍无有效平面: N = -viewDir，S = 最近根位置
```

4. 笔刷落点 `H = ray ∩ plane(S, N)`；
5. 在平面内建立 `T/B` 基，笔刷是平面内的圆盘，不是屏幕圆；
6. 候选不足或平面退化时才回退相机朝向平面，并显示降级提示。

UI 可以把笔刷尺寸显示为像素半径，但必须转换成世界半径 `r_world` 后再进行选择：

```text
r_world = r_px * 2 * tan(fovY / 2) * depth / viewportHeight
```

转换只服务 UI 手感；实际 stamp 的 `center/normal/radius` 都是世界/hair local 值。

### 6.3 剔除

对每个发根：

```text
normalGate  = dot(normalize(rootNormal), N_brush) >= cos(maxAngle)
planeGate   = abs(dot(root.position - H, N_brush)) <= thickness
discGate    = distance(projectToPlane(root.position), H) <= r_world
```

- `normalGate` 是主判据：发根头皮法线必须和笔刷平面法线同向；背面、侧面发根会被剔除；
- `planeGate` 限制在笔刷平面的薄层内，避免穿过头部选到另一侧；
- `discGate` 是平面内距离，不是屏幕距离；`projectToPlane` 后只取平面内的 `T/B` 坐标；
- `rootNormal` 缺失时才用相机朝向兜底；`rootGrowthDir` 不是头皮法线；
- 笔刷的 `strokeDir` 默认不参与剔除；如果要做 comb 笔刷，再加
  `abs(dot(rootGrowthDir, strokeDir))` 作为可选 gate，不要影响普通分组笔刷。

### 6.4 权重

```text
inPlane = length(projectToPlane(root.position) - H)
falloff = smoothstep(1.0, 0.55, inPlane / r_world)
normal  = smoothstep(cosMaxAngle, 1.0, dot(rootNormal, N_brush))
depth   = smoothstep(thickness, 0.0, abs(dot(root.position - H, N_brush)))
weight  = falloff * normal * depth
```

`falloff` 用平面内世界距离，`normal` 用发根法线与笔刷平面法线；不要用屏幕距离或
`rootGrowthDir` 代替。选择阈值建议 `weight > 0.05`；ghost 强度直接使用 `weight`。

### 6.5 笔画与性能

- 鼠标移动可以按屏幕间距决定采样频率，但每次 stamp 的选择仍在世界/hair local 平面内；
- 发根使用 3D spatial grid/BVH；只查询 ray 附近和笔刷平面附近的候选；
- 一次笔画维护 `guide -> weight` 的并集/差集；
- 笔画结束才压缩成 `guideRanges` 并写资产；
- 40,334 guide 的 CPU 选择可接受；3D 索引用于避免每次全量扫描；
- GPU 只接收 group id / preview weight，不需要 GPU picking。

## 7. 组件拆分

### 7.1 `SurfaceBrush`（通用）

```cpp
struct BrushSurfaceHit
{
    bool valid;
    Vec3 position;
    Vec3 normal;
    float depth;
};

struct BrushStamp
{
    BrushSurfaceHit surface;
    Vec3 rayDirection;
    Vec3 strokeDirection;
    float radiusWorld;
};

class SurfaceBrush
{
public:
    BrushStamp Sample(
        Vec2 cursor,
        const Viewport& viewport,
        const RootCloud& roots,
        const BrushSettings& settings);
};
```

职责：屏幕只用于生成 ray；用发根点云的空间索引拟合世界/hair-local 平面，输出 stamp 的
世界位置、笔刷 normal 和世界半径。选择逻辑不得依赖屏幕距离；不得依赖 TressFX、World 或 Vulkan。

### 7.2 `RootCloudSelector`（头发/毛发通用）

```cpp
struct RootHit
{
    uint32_t guideIndex;
    float weight;
};

class RootCloudSelector
{
public:
    std::vector<RootHit> Select(
        const RootCloud& roots,
        const BrushStamp& stamp,
        const BrushSettings& settings);
};
```

职责：发根 normal 与 brush normal 对齐、平面层厚度、平面内距离、权重；`rootGrowthDir`
只用于 comb 模式的 stroke gate。

### 7.3 `GroupPaintController`（编辑器业务）

- 当前目标组、Add/Remove/Replace 模式；
- 笔画开始/结束/撤销；
- ghost preview 状态；
- 将选择压缩成 `guideRanges`；
- 通过 asset modify 写 `groups[]`；
- 通知 GPU group id / preview buffer 更新。

## 8. 数据与运行时

### 8.1 组数据

继续使用现有 schema，不新增颜色字段：

```json
"groups": [
  {
    "name": "fringe",
    "guideRanges": [[0, 120], [340, 400]],
    "simulationOverrides": {
      "bendStiffness": 0.35
    }
  }
]
```

- 颜色由确定性的 `hash(name)` 映射到固定调色板，跨会话稳定，不写资产；
- 组名保留 `Default` / `unassigned`（大小写不敏感），避免和隐式组 0 冲突；
- 组区间必须压缩连续 guide，避免 40k guide 产生大量单元素 range；
- 画笔选择可能产生很多不连续 range；提交前合并连续项，并设置可配置上限（例如 4096 段），
  超过时提示拆组或改用数值范围，防止单个 groups[] 数组把资产 JSON 撑大；
- 组参数 precedence 采用：`base asset defaults -> group overrides -> placement overrides`
  （placement 是对象级，最后覆盖），组 0 只用 base + placement；如果用户希望组压过
  placement，必须在实现前改这条规则并加测试；
- 组变化必须进入 renderable **行为身份**；当前 `modelCacheKey` 还会把组名和区间拼进
  几何身份（契约 378 行），这会导致一次笔刷提交触发 1.29M 点的重新导入。Phase 1
  必须把 groups 从几何 cache key 拆出，新增独立的 group binding/behavior revision，
  并同步修改契约中“groups 影响 modelCacheKey”的陈述；否则 §8.2 的“只更新 buffer”不成立。
- `simulationOverrides` 保持现有 6 字段白名单；
- 如果以后允许 `windGain` 按组覆盖，需要单独扩展契约；场景风源仍不可按组覆盖。

### 8.1.1 组身份与顺序

- 组列表顺序 == `groups[]` 声明顺序；新建追加到末尾；
- 删除组 k 时，k+1..N 的**运行时组号**会前移；提交前重映射颜色、override 控件、
  GPU group id 和参数表索引；UI 不能只删行而不处理后续组；
- 重命名不改变声明顺序/组号，但会改变 `hash(name)` 颜色和 `modelCacheKey`（如果尚未拆出）；
- 保存/重载后，组号按新 `groups[]` 顺序重建；旧组号不是持久 ID；
- 如果未来需要跨编辑稳定 ID，必须给 schema 增加显式 `id`，不能继续依赖声明顺序。

### 8.2 GPU group id

- **不要**把 group id 作为新的 per-vertex attribute：那会改 vertex layout、需要重建 ribbon；
- 新增 per-object `groupIds` SSBO，按 **guideIndex** 索引，每个 guide 一个 uint；
- ribbon vertex factory 已知 `pointIndex = gl_VertexIndex / 2`，再按 `pointsPerGuide`
  得到 `guideIndex`，从 SSBO 读取 group id，写成 flat varying；
- group 颜色表/选中组/ghost 目标组走小 UBO 或 material uniform，不写资产；
- ghost 另行上传 per-guide `previewWeight`（CPU 选择结果），fragment 只读取 flat varying；
- 新增 flat varying 接在现有 coverage seed 的 location 8 之后（建议 location 9）；
- buffer owner 是 `RenderableObject`；descriptor 由 `HairSimulationPass` / 对应 graphics
  pass 的 descriptor pool 管理；不要新增 renderer 全局单例；
- 图纸/仿真 pass 的 descriptor layout 与 pool 要一起改，不能只改 shader；
- 当前契约“`tressfxGuideGroupIds` 只存在于 CPU、不进入 GPU”这句话必须在同一实现改动中改写；
- `Group Colors` / `Selected` / `Solo` 是预览状态，不写资产，也不改变材质身份。

### 8.3 每组参数生效

当前所有 guide 共享一个 `TressfxSimulationConfig`。要真正“不同组不同参数”，需要：

- CPU 参考 solver：每组一份 `SimulationConfig`，或按 guide group 选择配置；
- GPU：每组一份 params UBO + 按组 dispatch，或一份 params + per-guide group 参数表；
- 推荐先做“每 guide group id + 组参数表”的单 dispatch 方案，避免组数增加时 dispatch 数量线性增长；
- 具体数据建议：新增 `groupIds`（每 guide/point 一个 uint）和 `GroupSimulationConfig` 数组（SSBO），
  由 shader 按 guide 的 group id 读取；不要把组配置直接塞进每个 pass 的现有 816-byte UBO；
- 所有 pass 需要按 group id 选择控制参数；同一根 guide 的所有 point 必须使用同一个 group id；
- placement overrides / skinning / collision 仍是对象级，不按组拆分；组只覆盖 6 个
  `simulationOverrides` 字段。
- CPU 与 GPU 必须共用同一份“基础配置 + 组覆盖”解析，至少做一次 CPU/GPU 同输入
  `per-group` 参数对照测试；不能只实现一端后静默分叉。
- `groupIds` SSBO、preview weight buffer、descriptor set/pool 的 owner 是
  `RenderableObject` 与对应 pass 的 descriptor 管理；不允许从 UI 直接写 GPU buffer。
- brush 的拾取使用编辑器 viewport 的 camera/VP 快照；笔刷模式激活时按
  `game-ui-stack` 的输入仲裁，RmlUi 面板滚动/点击不能同时落到 SDL 视口。

这是独立于笔刷的阶段；笔刷和分组可视化可以先落地。

## 9. 分阶段实施

### Phase 1：Groups UI + 数据

- 已实现（2026-10-28）：`groups` 从几何 `modelCacheKey` 拆到
  `tressfxGroupBindingKey`；`.tfx` guideCount 暴露到编辑器目录/文档快照；
  Groups 列表、创建/删除/重命名、数值 range、6 字段 inherit/override、保存与校验。
- 已实现（2026-10-29）：`TressfxGroupBinding` 与几何解耦；进程级 Tressfx 几何 LRU 缓存按
  `.tfx BLAKE3 + 几何身份` 复用 ribbon/root cloud；候选缓存命中路径原地更新 group buffer，
  组数变化才重建对象。整 World 重载仍会创建新的 renderable/GPU buffer，但不再重解析 `.tfx`。
- 先把 groups 从 ribbon 几何 `modelCacheKey` 拆出，新增 group binding/behavior revision；
- 把 `guideCount` 暴露到 Tressfx 文档/预览快照；
- Groups 列表、创建/删除/重命名；
- 数值 range 编辑；
- inherit/override 控件；
- 保存/校验；
- 无运行期组参数效果时明确标注。

### Phase 2：GPU 分组着色 + 选中高亮

- 已实现（2026-10-28）：per-object group id/颜色 SSBO、forward-output RGB tint、
  Group Colors / Selected / Solo 与预览视口左上角图例；未改 vertex layout。
- 新增 per-object `groupIds` SSBO（按 guideIndex），不改 vertex layout；
- 扩展 graphics descriptor set/pool，`TressFXStrands` 读 group id；
- 在 `base.frag.glsl` forward output 增加 location 9 的 flat varying / RGB tint；
- Group Colors / Selected Group / Solo；
- 视口图例；
- 不做 per-group solver。

### Phase 3：笔刷虚拟工作面 + ghost

- 已实现（2026-10-28）：导入期发根点云（位置/生长方向/PCA 法线/组号/3D grid）、
  SurfaceBrush + RootCloudSelector、Add/Remove/Replace、ghost RGB、提交/撤销/冲突提示。
- 已实现（2026-10-29）：grid DDA 射线遍历、世界空间 debug-draw 圆环、套索选择、
  resolved asset default；40,334 guide 基准 P95 ≈ 1.15 ms。屏幕圆环仅作平面退化回退。
  新建/重命名/删除/改 range 的草稿 binding 会即时同步到预览 GPU buffer，无需保存重载即可
  看到组色并立即笔刷；但组 override 仍需保存重载后才参与求解。
- `SurfaceBrush` + `RootCloudSelector`；
- 套索/笔刷、ghost preview；
- 提交/撤销/冲突处理；
- 性能 tile 索引。

### Phase 4：per-group simulation

- 已实现（2026-10-28）：共享 `base -> group -> placement` 解析，CPU solver 与 GPU
  compute 同源；GPU 每组 8 float（前 6 字段）+ 每 guide group id，绑定 17/18。
  保存并重载后生效；实时 `hair set` 是会话级调试覆盖。
  尚未做 40k guide 的 CPU/GPU 逐帧数值 readback 对照。
- CPU/GPU 参数表；
- 让 `simulationOverrides` 真正影响求解；
- 增加实时预览/保存后生效的状态提示。

### Phase 5：Group draw/culling/LOD

- 单独的 `TressFXGroup`/visibility/tile 层；
- 不复用本方案的参数分组 UI 作为渲染分组。

## 10. 测试与验收

单元测试：

- 局部平面拟合退化/回退；
- 发根法线缺失时的 `-viewDir` 回退；
- brush normal 来自根法线/平面拟合，而不是屏幕方向；
- 发根 normal 与 brush normal 反向时被剔除；
- 同一屏幕距离但不同平面内的发根按世界距离区分；
- 射线-平面落点与世界半径换算；
- 平面层厚度剔除；
- 同一条笔画不重复加入；
- guide 集合压缩成连续 range；
- 超过 range 上限时拒绝并提示；
- range 重叠/越界拒绝；空组允许；
- `end > guideCount` 在编辑器保存前被拦截，而不是等到下一次 import 才失败；
- 新建组可以没有 range；删除最后一个 range 保留空组；
- `Default` / `unassigned` 保留名（大小写不敏感）被拒绝；
- 删除组后后续组号、override 控件和 GPU id 重映射正确；
- 组名 hash 颜色跨重启稳定；
- 组编辑不触发 ribbon 几何重新导入，只更新 group binding/behavior revision；
- ghost 选择不写资产，提交才写；
- Esc 取消不改变 groups；
- group override precedence（base -> group -> placement）按选定规则成立；
- CPU/GPU 同输入 per-group 参数对照一致。

UI 测试：

- 新建组、重命名、删除；
- 名称为空/非法时 Create 禁用；删除最后 range 保留空组；
- `end > guideCount` 行内报错且无法保存；
- 保留名/重名/空名行内报错；
- inherit/override 切换；
- 冲突组提示；
- 保存后重新打开组数据一致；
- 40k guide 下笔刷不阻塞 UI。

视觉验收：

- 笔刷圆环贴合头皮工作面；
- 背面发根不被选中；
- 目标组实色、范围内非目标组 ghost、其他组有边缘区分；
- ghost 只改 RGB，不透明度仍为 1，不破坏 coverage discard；
- 整根发丝颜色一致；
- 关闭 Group Colors 后恢复材质外观。

## 11. 风险与待定

- 发根法线从哪里来：`p1-p0` 生长方向只适合近似；可能需要头皮法线或局部 PCA；
- 根点使用 rest 还是 posed：建议 rest/head local 作为作者数据，渲染时应用当前 head 矩阵；
- 笔刷范围和组范围不一致：笔刷即时选择，提交后压缩为 range；大范围框选可能产生很多 range；
- 组颜色是否持久化：当前建议不持久化，由索引/名称确定；如果用户需要自定义颜色，需要扩展 schema；
- `windGain` 是否允许按组覆盖：当前契约不允许，需单独决定；
- per-group solver 的 UBO/参数表方案需要性能验证；
- 本方案不解决 `TressFXGroup` 的 draw/culling/LOD；
- 笔刷只能在 TF 预览有效时工作；预览未加载模拟时仍可用于分组。

## 12. 新线程启动清单

新线程开始实现前：

1. 读 `documents/routing.md`、`documents/contracts/tressfx-asset-import.md` 的 groups 节、
   `documents/contracts/asset-modify-api.md`；
2. 确认当前分支仍是 `codex/tressfx5-hair-port`，并检查 `groups` 是否仍只完成解析；
3. 先做 Phase 1（Groups UI + 保存），不要先接 group solver；
4. Phase 2 再加 GPU group id / 颜色；Phase 3 再加笔刷；Phase 4 才改 per-group solver；
5. 测试入口：
   - `cmake --build build-tressfx-groups --target main hair_asset_tests editor_ui_tests`
   - `build-tressfx-groups/bin/hair_asset_tests.exe`
   - `build-tressfx-groups/bin/editor_ui_tests.exe --gtest_filter='EditorUiTest.*:TressfxGroups.*:TressfxGroupEditor.*:JsonAssetLibraryTest.*'`
   - `py -3 tool/validate-docs.py`
6. 不要把 `TressFXGroup` 的 draw/culling/LOD 混进本方案；它是独立 Phase 5。

## 13. 独立审查修订

独立审查已指出并合入以下修正：

- `rootGrowthDir` / `rootNormal` 当前不存在，Phase 3 必须先补缓存；没有头皮法线时固定使用
  `-viewDir` 兜底，不把 `p1-p0` 当法线；
- 空组可表示：新组只输入名称，删除最后 range/替换后变空都保留命名组；
- 移除 `unassigned` 概念，Default 就是隐式组 0；修正示例计数；
- ghost tint 放到 `base.frag.glsl` 的 forward output，不改 material surface；ghost 只改 RGB，
  strand 仍完全不透明；
- group id 走 per-object SSBO，不做 per-vertex attribute；需要 descriptor layout/pool 变更；
- `modelCacheKey` 当前会把 groups 拼进几何身份，Phase 1 必须拆分，否则笔刷提交会触发
  1.29M 点重新导入；
- 明确组顺序/重映射、保留名、`guideCount` 编辑器校验、precedence、CPU/GPU parity、
  buffer ownership 和输入仲裁。

## 14. 参考

- `documents/contracts/tressfx-asset-import.md#分组groups`
- `documents/contracts/asset-modify-api.md`（TressFX 能力表包含 `groups`）
- `documents/plan/rendering/tressfx5-port-map.md#L149`（TressFXVisibility/Tile/Group 推迟到 V3）
- `documents/plan/rendering/tressfx5-hair-port-plan.md#L518-L525`（M4 V3 对齐与 LOD）
- `source/mesh/meshAssetTypes.h`（`TressfxGuideGroupPlan`、`tressfxGuideGroupIds`）
- `source/mesh/loader/hair/tressfxModelImporter.cpp`（`ResolveGuideGroupIds`）
- `source/editor/ui/materialInstanceAssetEditorPanel.cpp`（TF Tab 的 `resource-hair-simulation`）
