# 地形大世界与上百层地表：架构计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 实施计划 / 架构决策（未来工作，不是契约） |
| 状态 | 未实现。当前运行时仍是 8 层固定 palette + top-4，见 `contracts/terrain.md` |
| 修订 | 2026-10-29 第 1 版 |
| 目标 | 上百层地表、16km 级大世界、可流式加载的地形材质与数据通路 |
| 非目标 | 编辑器绘制工具、完整 VT 编辑器、GPU-driven 几何、最终美术资产 |
| 关联 | [地形复刻计划](terrain-openworld-demo-plan.md)、[开放世界场景栈](open-world-scene-stack-roadmap.md)、[terrain 契约](../../contracts/terrain.md) |

> 本文件是计划，不是契约。当前行为只看 `documents/contracts/terrain.md` 与源码；
> 本文件里的格式、预算和阶段都可以在实现前调整。

## 0. 结论

上百层不能靠“把 shader palette 从 8 个扩到 100 个”实现。必须把**全局层数**和
**每像素求值数**解耦：

```text
全局 layer table（100~512 层）
  + 每 tile 候选层列表（8~32 层）
  + 每 texel 的 layer ID / weight 图（top-4 或 top-8）
  + 虚拟纹理/页表按需驻留
= shader 每像素只处理 4~8 层
```

程序化 height/slope/noise 只用于**筛选 tile 候选层**，不再承担每像素 100 层求值。

## 1. 默认目标（待用户最终确认）

这些数字决定实现规模，先用一组可执行的默认值推进：

| 项目 | 默认值 | 影响 |
| --- | --- | --- |
| 全局层数 | 256 | layer table、ID 位宽、调色板工具 |
| 每 tile 候选层 | 32 | shader 循环上限、候选表大小 |
| 每像素有效层 | top-4（可扩 top-8） | 数组采样次数、权重精度 |
| 世界尺寸 | 16km × 16km（1,600,000 cm） | 精度、tile 数、流式 |
| tile 尺寸 | 512m（51,200 cm） | 32×32 = 1024 tiles |
| 高度图 | 每 tile 512² / 1024² | 网格与碰撞分辨率 |
| 单层贴图 | 1K，BC7 albedo + BC5 normal + BC7 pbr | 显存与带宽 |
| 显存预算 | 8GB，地形常驻上限 3~4GB | VT 页面预算 |
| 权重/ID 图 | RGBA8 IDs + RGBA8 weights，128 page | top-4 表达 |

若最终是 512 层或 64km，必须把 tile 尺寸、页面大小和 ID 位宽重新算一遍；
不要把 256/16km 当成永久上限。

## 2. 数据分层

### 2.1 全局层表 `TerrainLayerTable`

每层一条只读数据，按稳定 `layerId` 索引：

```text
struct TerrainLayerGPU
{
    uint  albedoPage;      // VT page / array page 入口
    uint  normalPage;
    uint  pbrPage;
    float uvScale;
    float uvRotation;
    vec4  heightSlope;     // heightMin, heightMax, slopeMin, slopeMax
    float blendEdge;
    float noise;
    uint  flags;           // biome/岩石/道路等过滤位
};
```

- 用 SSBO 或 `samplerBuffer`，不要继续塞进材质 UBO 的独立 vec4。
- 层 ID 稳定；权重图、候选表、编辑器都引用 ID，不引用名字。
- 层表是全局只读资源，可跨 World 复用；页面句柄变化时走资源重绑定。

### 2.2 tile 候选层

每个 terrain tile 只声明自己可能出现的 8~32 层：

```text
struct TerrainTileLayers
{
    uint count;
    uint layerIds[32];
    // 可选：每层的局部权重缩放/覆盖
};
```

- 候选层由 biome、高度、坡度、道路/水/岩石排除规则离线或加载期生成。
- shader 不需要知道全球 256 层，只处理本 tile 的候选列表。
- 候选表是 tile 级资源，随 tile 加载/退休。

### 2.3 每 texel 权重/ID 图

建议两张 RGBA8：

```text
ID map:      R/G/B/A = 4 个 layer ID（0..255，或 16-bit 扩展）
Weight map:  R/G/B/A = 4 个 8-bit 权重
```

或一张 RGBA16UI（4×16-bit ID）加权重图。每 texel 最多 4 个有效层；
需要 top-8 时用 8 个 ID/8 个权重，或两张 ID 图。

- shader 直接按 ID 读层表，按权重混合；总层数不影响采样次数。
- 权重图是作者数据/程序化烘焙结果，不在 shader 里遍历全球层表。
- 远处 LOD 可以把 4 层合并成 1~2 层，或使用低分辨率权重页。

### 2.4 虚拟纹理/页表

- 层贴图、权重/ID 图都可以 VT；shared page table + physical atlas + feedback。
- 页面建议 128 或 256；页表/atlas 按 tile 生命周期回收。
- 页面 miss 时先回退低分辨率页或 tile 级平均色，不能每帧同步等 IO。
- feedback 只记录实际访问的页；避免“全部页面常驻”。

## 3. 运行时管线

```text
TR / terrain layer palette
  -> TerrainLayerTable (global, stable IDs)
  -> tile builder: heightmap + biome rules
       -> TerrainTileLayers (candidate list)
       -> weight/ID pages (authored or baked)
  -> page streaming (async)
  -> GPU page table + layer table SSBO
  -> M_terrain: sample ID/weight -> top-K -> 3 arrays/pages -> blend
```

### 3.1 shader 形状

```text
1. 读 ID/weight 图（4 个 ID + 4 个 weight）
2. 按 ID 读 layer table（UV scale/rotation、页面句柄）
3. 归一化权重，top-K 截断
4. 对每个有效层采样 albedo/normal/pbr（3 次）
5. 混合；远处 LOD 减少有效层
```

每像素仍然是 12 次数组/页面采样（top-4），与全局 256 层无关。

### 3.2 资源与生命周期

- `TerrainLayerTable`：全局只读，跨 World 复用；层表变化走显式版本。
- tile/weight/page：world-local package，随 tile 加载、替换、退休。
- 页面资源必须有 cache key（tile + layer + mip/page）、GPU epoch 退休。
- World 切换沿用现有 prepared/commit 事务；不能把半加载 tile 发布给渲染线程。

## 4. 大世界约束

- 世界单位仍是 cm；16km = 1.6M cm，float32 可用但要做精度审查。
- 更大世界（64km+）需要 camera-relative rendering / floating origin。
- 地形 chunk 仍 CPU 生成；上百层不改变几何路径，但 LOD/streaming 必须与 tile 一起设计。
- 碰撞与渲染分离：Jolt heightfield/trimesh 只消费高度图/tile 元数据，不消费层表。
- 阴影 pass 需要与主视图共享 tile/LOD 选择；远处低 LOD 不应产生高精度阴影。

## 5. 实施阶段

### Phase A：数据通路最小闭环（仍 8 层）

- 引入 `TerrainLayerTable` CPU/GPU 描述、tile 候选表和 ID/weight 图格式。
- 2026-10-29 已完成第一小步：`TerrainLayerTable::BuildImage` 把 TR.layers 打包成
  32×2 RGBA32F LUT，经 `layerTable` sampler2D 进入 M_terrain；shader 不再硬编码层范围。
- 同日 Phase C2b：`TerrainPageManager` 把同一 World 的多个 terrain 累积到一张 32×32 page
  table/4096 atlas；顶点 `texCoord1` 携带 pageOrigin；双地形共享页表场景已出图。
- C2c CPU 基础已落地：per-tile page 烘焙、resident set、LRU/priority/refcount、
  page table 无效项与保留 fallback 页、layer table World 级共享和数组层数校验。
- 已完成：dirty page 描述 + `CopyBufferToImageRegion` 子区域上传；初始 atlas 先清零再按 dirty 页上传，
  上传成功后才提交 dirty（失败可重试）。
- 已完成：虚拟页与物理 atlas 槽位分离；页表项写物理坐标，淘汰释放槽位并复用；
  页表/ID/权重按批量 region 上传，三张 image 合并进同一 command buffer/一次 submit。
- 已完成：GPU feedback 回读、真实 GPU 淘汰/按需驻留、每帧上传限流、GPU epoch 安全窗与
  物理槽位 quarantine/回收；P3.1 收口了非 VT 地形隔离、epoch-aware LRU、按 epoch 降预算、
  512 默认预算与页范围末端内缩。
- 尚未完成：异步/后台页烘焙、相机预取、低分辨率 fallback/淡入、4a 远景烘焙、
  256 层三张层纹理数组的完整 VT。
- 验收：8 层与 32 层场景数据驱动出图；top-4/权重 debug 已有截图（已达成）。

### Phase B：32~64 层/tile + 页面/数组页

- 候选层扩到 32~64；层贴图从“一个全局数组”改成页面/数组页。
- tile 级流式加载、页表、低分辨率回退。
- 验收：至少 3 个 biome，同屏 32 层候选，权重/tile 切换无黑块。

### Phase C：VT + 上百层

- 页表 + feedback + physical atlas；256 层全局，tile 32 层候选。
- LOD/远景降级、页面预算和 eviction。
- 验收：16km 地形流式走查，显存稳定在预算内，无同步卡顿。

### Phase D：大世界收口

- GPU-driven/indirect、HLOD、阴影 LOD、碰撞/角色接入、编辑器工具。
- 验收：可玩的 open-world vertical slice，而不是单张截图。

## 6. 关键决策（实现前必须确认）

1. 全局层数：100 / 256 / 512？
2. 世界尺寸：8 / 16 / 64 km？
3. 单层贴图分辨率与显存预算：1K/2K，BC 压缩，地形常驻上限？
4. 权重来源：手绘 splat、程序化 biome、离线烘焙，还是混合？
5. 每像素 top-4 还是 top-8？是否允许远处降到 top-1/2？
6. 是否启用 descriptor indexing/bindless？页面/数组页方案依赖它。
7. tile 尺寸、页面大小、LOD 级数、阴影 LOD 策略。

## 7. 风险

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 层数/世界尺寸先定太小 | 后期重写数据格式 | 先按 256/16km 设计，参数化 tile/page |
| 全量 texture array 常驻 | 显存爆 | VT/页面流式，BC 压缩，预算上限 |
| 每像素遍历全球层 | shader 爆炸 | tile 候选表 + ID/weight 图 + top-K |
| 权重图分辨率不足 | 层边界糊/块状 | 页表 + 局部高分辨率页，远处降级 |
| 页面加载同步卡顿 | 走查卡顿 | 异步 + 低分辨率回退 + 预取 |
| 大世界 float 精度 | 抖动/穿模 | camera-relative/floating origin 评估 |
| 候选层/ID 越界 | 黑块/采样错误 | 加载期校验 + debug view + 单测 |

## 8. 与当前实现的关系

- 当前 `M_terrain` 的 8 层固定 palette 是 Phase A 之前的功能冒烟，不是最终架构。
- Phase A 完成后，`contracts/terrain.md` 要更新为“层表 + 候选层 + ID/weight 图”的当前行为。
- 本文不替代 `contracts/terrain.md`；落地前不要把这里的 VT/SSBO/上百层写成现状。
