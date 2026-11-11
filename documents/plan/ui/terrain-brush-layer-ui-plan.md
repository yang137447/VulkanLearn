# 地形笔刷与层库 UI 计划

状态：2026-11-01。UI 先行；GPU-driven 页烘焙与 indirect 绘制属于后续阶段。
本文是计划，不是契约；当前行为以 `documents/contracts/terrain.md`、
`documents/contracts/ui-component-system.md` 为准。

## 目标

- 在底部停靠区 Log 右侧增加 `Landscape` 页签，作为地形编辑的主工作区。
- 层库支持上百层：List/Grid、搜索、虚拟滚动、球体缩略图与悬浮平面预览。
- 抽出公共笔刷体系：雕刻、地形材质绘制以及后续网格绘制共用同一套描述、采样和库组件。
- UI 页签可拖出为主窗口内的浮动面板并重新停靠；不承诺操作系统级独立窗口。

## 已定决策

- 层数上限暂定 256；编辑器与 API 使用不透明 `TerrainLayerId`，不把 256 写进组件。
- 运行时仍是每个覆盖 texel 选 top-4；ID 用 nearest 采样、权重用 linear 采样。
- 超过 256 的升级路径优先 tile palette（局部槽位 -> 全局层号），而不是全链路改 16 位。
- “工具”和“笔刷”分离：
  - Tool：Raise/Lower/Smooth/Paint/Erase，决定改什么；
  - Brush：形状、falloff、stamp、间距、抖动、压力，决定怎么落笔。
- Undo/Redo/Save 是全局编辑命令，放在编辑器公共工具条，不在地形面板或视口工具条里重复。
  - Undo/Redo 按活动上下文路由：Landscape 工具激活时是地形 stroke，Blueprint 资产页是
    该文档历史，否则是 Scene 历史。
  - Save 保存当前文档（Scene/TR/MI/BP）；地形的 `.vlpaint` 与高度图是源数据写入，
    单独叫 `Write Paint` / `Write Heightmap`，不与全局 Save 混用。
- 不新增无实现的按钮；未完成能力用显式空状态或诊断表达。
- 预览生成不得再走主线程同步加载；必须使用预算队列和内容哈希缓存。

## 布局

```text
底部停靠组：Content Browser | Renderer | Log | Landscape

Landscape 面板：
┌ [Sculpt] [Paint] [Layers] ───────────── [搜索层] [List|Grid] ┐
│ ┌ Brushes ───────┐ ┌ Layer Library ──────────────────────┐ │
│ │ ● Soft Round   │ │ 行/卡片：swatch/球体 + 名称 + 范围   │ │
│ │ ● Hard Round   │ │ 虚拟滚动，只挂可视行                 │ │
│ │ ■ Square       │ │                                      │ │
│ └────────────────┘ └──────────────────────────────────────┘ │
│ Selected: Rock · sourceSet 0 · uvScale 8 · 12.4% painted     │
└──────────────────────────────────────────────────────────────┘
```

- 行内预览 24px 球；Grid 卡片 64px 球。
- 悬浮只显示无边框的平面平铺缩略图；不套预览框/边框，也不显示标题、参数或按钮。
  选中层由点击卡片完成。平铺倍率切换留到层详情，不占悬浮预览。
- 底栏高度可拖拽；面板拖出后保持同一套内容与滚动位置。
- 没有选中 Terrain 时显示空状态，不显示伪造的层数据。

## 数据与组件

### Brush

- `BrushProfile`（JSON 资产，`Brushes/Brush_*.json`）：
  `name`、`category`、`shape`（circle/square/stamp）、`falloff`
  （smooth/linear/constant/lut）、`radiusCm`、`strength`、`spacing`、`jitter`、
  `rotation`、`noise`、`pressure`（size/strength 曲线）。
- `BrushSampler`：纯 CPU 采样与落点逻辑，供所有工具适配器复用。
- `BrushLibraryPanel`：RmlUi 组件，只依赖 `BrushProfile` 与选中事件；
  不依赖 World、资产命令或 Vulkan。预览是 2D stamp + falloff 曲线。
- 工具适配：
  - Sculpt：BrushProfile + Raise/Lower/Smooth -> `TerrainSculptBrush`
  - Paint：BrushProfile + layer + erase -> `TerrainPaintBrush`
  - 后续网格绘制复用同一 Profile/Sampler。

### Layer Library

- 预览键由层内容决定：`sourceSet`、slice、uvScale/rotation、avgColor、noise、
  height/slope 范围；同内容共享缓存。
- 有 source set 纹理时用 albedo/normal/pbr；缺纹理时退化为 `avgColor` swatch，
  并明确标记 procedural。
- List/Grid 都虚拟化；缩略图只请求可视区域 + 一页预取。
- 预览缓存有 LRU 和每帧预算；失败的重试有上限，不阻塞 GT。

## 阶段

### P0 停靠与页签（本轮）

- `DockWorkspace::Panels()` 增加 `terrain`，默认布局插在 `log` 之后。
- 布局持久化升到 v3：旧 v2 布局迁移时把 `terrain` 追加到 `content` 组，保留分栏/活动页。
- RML 增加 `#workspace-terrain-tab` 与 `#terrain-page`；支持现有拖出/停回/隐藏/Reset。
- 页面先显示选中地形摘要与显式空状态；不接预览渲染。

验收：页签在 Log 右侧；拖出为浮动面板并可停回；旧布局可恢复；Window 菜单可隐藏/恢复；
默认布局测试与持久化迁移测试通过。

### P1 笔刷核心与库组件

- `BrushProfile` + `BrushSampler` + JSON 预设 + 单元测试。
- `BrushLibraryPanel`：分类、搜索、选择、新建/复制（先不做导入 stamp）。
- 2D stamp/falloff 预览；压力曲线字段先支持采样，UI 在有压感设备时启用。

验收：纯逻辑测试覆盖 falloff/间距/压力；组件测试覆盖选择与刷新；无 World 依赖。

### P1.5 全局 Undo/Redo/Save

- 公共工具条增加 Undo/Redo，与现有 Save 并列；移除地形视口里的 Undo/Redo/Save。
- 同一套 `scene-undo`/`scene-redo` 动作按活动上下文路由，地形 stroke 优先。
- 地形源写入改名为 `Write Heightmap` / `Write Paint`，保留在 Landscape 面板内。

验收：地形笔刷、Blueprint 文档、Scene 三种上下文各有一条撤销测试；全局 Save 不写
`.vlpaint`/高度图；地形写入按钮的文案随 Sculpt/Paint 切换。

### P2 层库 List/Grid

- 层行/卡片、swatch、搜索、虚拟滚动、多选和 Set Paint Target。
- 与现有 Paint 图层选择同步（单一状态在编辑器面板）。

验收：256 层夹具下只挂可视行；搜索/选择/滚动稳定；截图验证。

### P3 预览渲染器

- 独立预算队列；球体与平面两种预览；内容哈希 + LRU。
- 先修现有内容浏览器缩略图的主线程同步加载问题，复用或替换为同一套预览服务。

验收：128px 预览每帧预算可配置；无 GT 长停顿；缓存命中后零渲染；失败可诊断。

### P4 笔刷接入工具

- 用 BrushProfile 替换雕刻/绘制的重复 radius/strength/falloff 参数。
- 噪声、抖动、间距、压力接入；工具条与面板共享同一状态。

### P5 Paint 分辨率与稀疏

- 分辨率与层数解耦（默认 256²/128² 可配置）。
- 只存储实际绘制过的 tile；保存格式随之升级并保持向后兼容。

### P6 GPU-driven 烘焙（后续）

- 高度图/层参数/Paint 常驻 GPU；`pageBake.comp` 计算权重与 top-4；
- 页调度可先保留 CPU，再迁移 free list/去重到 compute；
- 再做 GPU culling 与 `vkCmdDrawIndexedIndirectCount`。

## 风险

- 旧布局迁移必须保留用户分栏；不能因为新增常驻面板直接 Reset。
- 100+ 层时 Paint 权重图内存和烘焙候选扫描会放大；预览/烘焙都必须有预算。
- 预览渲染器是 P3 的硬依赖；在那之前只用 swatch，不承诺球体真实材质预览。
- 浮动面板是主窗口内 dock，不是原生多窗口。

## 进度

- 2026-11-01 P0 完成：`terrain` 常驻页签位于 Log 右侧，布局 v3 迁移保留旧分栏，
  页签可拖出/停回；页面提供选中地形摘要与显式空状态。
- 2026-11-01 P1a 完成：`BrushProfile`/`BrushSampler`/JSON/内置预设与 19 项模块测试；
  Circle/Square 形状与旋转已进入 2D 采样。
- 2026-11-02 P2 第一版完成：Landscape 面板提供 Sculpt/Paint/Layers、层卡 Grid/List、
  搜索、选择、纯平面 hover；现有 Raise/Lower/Smooth 与半径/强度/falloff/Erase 已移入
  面板左列，视口第二行工具条移除。缩略图仍是 `avgColor` 程序化 swatch，256 张卡片
  一次挂载、暂无纹理预览。
- 2026-11-02 P1b 完成：可复用 `BrushLibraryPanel`（中性条目、搜索/分类、2D stamp 预览、
  选中态、空状态、幂等构建），内置 Soft Round / Hard Round / Square 三个预设；
  选择预设会更新半径/强度/falloff/Circle-Square，手工改值会取消预设高亮。
- 地形笔刷的 Circle/Square 已贯通雕刻与绘制（Square 用归一化 UV 的 Chebyshev 距离）。
- 首次切到 Landscape 时会自动把底栏扩到约 300px；用户之后仍可拖动分隔条。
- 2026-11-02 笔刷域拆分完成：Sculpt/Paint 各自拥有目录、选中项和参数状态；
  Sculpt 使用 Radius/Strength/Falloff + Surface 面板，Paint 使用 Radius/Hardness/
  Opacity/Flow + Target Layers。Paint 的 opacity/flow 目前映射为 strength，完整
  Photoshop 式累积语义留到 P4。
- 2026-11-02 Paint 语义完成：Flow 控制单笔推进速度，Opacity 是覆盖率上限，
  Spray noise/seed 参与逐 texel 落笔；Erase 同样受 Flow 影响。Sculpt 语义不变。
- 2026-11-02 P3 第一版完成：层库通过 MI `layerAlbedo` -> textureArray `sources`
  解析真实 albedo，CPU 生成 64px 球体与 160px 平面缩略图并做 BLAKE3/磁盘缓存；
  无源纹理时回退 avgColor swatch。层库已对 256 层做窗口化（网格 <=60 张卡片），
  方形笔刷环也已生效。
- 2026-11-02 预览升级完成：96px PBR 球体（Lambert + GGX + normal map +
  roughness/metallic/AO + ACES），卡片放大到 144×116、球形 84px；每组地形贴图
  额外携带 16-bit `displacementSources`（测试资产为 16-bit PNG），球体半径与
  法线扰动使用该高度场，pbr alpha/亮度只做 legacy fallback。缓存键覆盖
  albedo/normal/pbr/displacement 的路径、尺寸、mtime 与着色参数。
- 下一步：GPU-driven 页烘焙（P6）；stamp/pressure 材质笔刷与真实运行时层置换
  的 shader 接入留到后续。
