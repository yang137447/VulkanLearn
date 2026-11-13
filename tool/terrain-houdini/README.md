# Houdini weightmap export

Pegasus 自带的 `PE_Heightfield_Layers_to_ID` 只做**阈值化 ID 图**：
`volumewrangle1` 按 `tolerance` 把每层二值化，再写 `id = index/255`。
它不输出逐层权重，而 VulkanLearn 的地形 VT 需要的是
**每层 0..1、逐 texel 归一化、带过渡宽度**的 weightmap。

`houdini_weightmap_export.py` 是一个 Python SOP，输入 heightfield（带命名
volume 层），输出：

- `<prefix>_<layer>.png`：16-bit 灰度逐层权重
- `<vlpaint_prefix>_tile_x_y.vlpaint`：`TR_*.json -> paintPath` 直接可读的
  VLP1 切片（float32 权重 + 全 255 opacity，layer 顺序与 TR 一致）

## 建节点

SOP 里加一个 **Python SOP**，Python 参数填：

```python
exec(open(r"<repo>/tool/terrain-houdini/houdini_weightmap_export.py").read())
```

第一次 cook 会自动补上 spare 参数（`export` / `output_dir` / `layer_spec` /
`id_mode` / `feather_texels` / `write_vlpaint` / ...）。之后：

1. 输入 0 接 heightfield（例如 `Houdini_Load_Landscape` 的输出）。
2. `feather_texels` 设过渡宽度：2016² 覆盖 4032 m 时 1 texel = 2 m，
   4 texel ≈ 8 m 过渡带。
3. `export` 打勾再 cook：写文件，脚本会把 `export` 自动关回 0。

## 两种输入

**分层 volume（推荐）**：每个 volume 是一层 0..1 权重。

```
layer_spec = "Gravel Ground:mask, Rock Face:bedrock:0.5, River Water:water:0.4:1"
```

`layer_spec` 留空时使用除 `height`/`id`/`mask` 外的所有 volume。
每项格式是 `输出名:volume[:阈值[:是否取反]]`；阈值缺省用全局 `threshold`，
`soft_width` 控制阈值两侧的平滑带（value 空间）。

### 层序与剔除（重要）

层的 volume 是**覆盖率**，重叠部分必须按订单剔除，不能各自归一化，否则
重叠区会被两层同时计权。`composite` 默认 `Auto`：先看逐 texel 覆盖率和——
接近 1 说明输入已经是最终权重，走 `Normalize`（顺序无关）；明显大于 1
说明是覆盖率堆叠，走 `Stack`。日志会打印判断依据：

```
[weightmap] auto composite=stack (coverage median=1.544, >1.02=100.0%)
```

`Stack` 用 "over" 运算按顺序剔除：

```
w_i = mask_i * (1 - 已由上方层占用的覆盖率)
剩余覆盖率归给最底层，保证逐 texel 权重和 = 1
```

`order` 控制谁在上面：

- `Last On Top`（默认）：**列表最后一项在最上面**，与
  `PE_Heightfield_Layers_to_ID` 里 `Replace` 按 i=0..n 依次覆盖的语义一致。
- `First On Top`：列表第一项在最上面。

如果输入 volume 本身已经是最终权重（互不重叠、逐 texel 和=1），把
`composite` 改成 `Normalize`（或保持 `Auto`），脚本只做羽化+归一化，
不再做 over 剔除。

**ID volume（迁移旧资产）**：`id_mode=1`，把 `PE_Heightfield_Layers_to_ID`
风格的 ID 图（Pegasus 的 8-bit id 每级 17）解码成 one-hot 层再羽化归一化。

```
id_mode    = 1
id_step    = 17
layer_spec = "Gravel Ground:0, Rock Face:1, River Water:2, Scree Debris:3,
              River Silt:4, Sea:5, Beach Sand:6"
```

ID 是独占的 one-hot，`Stack`/`order` 在 id 模式下不参与计算。

层顺序必须与 `TR_*.json -> layers[]` 完全一致，VLP1 的层数也必须等于 TR
层数（运行时会按 TR 层数校验）。

## 和 VT 的关系

本节点直接产出 `paintPath` 用的 VLP1，属于 **World 级覆盖页烘焙的输入**，
不需要 per-tile MI，也不走 `u_terrainWeightMapMode=1` 的单 tile 掩码路径。
导出的 VLP1 会由 `TerrainPageManager::BuildPage` 逐页烘焙成 ID/权重页表。

## 限制

- 只处理 z=1 的 heightfield（2D）；3D volume 不支持写 PNG/VLP1。
- 羽化是全局图上的三次 box blur（近似高斯），VLP1 切片在羽化之后做，
  所以 tile 边缘连续；VLP1 本身没有 margin 字段，tile 边界就是 TR 的边界。
- 一次导出全部层会占用 层数 × 4M × 4B 内存（7 层约 112 MB），正常。
