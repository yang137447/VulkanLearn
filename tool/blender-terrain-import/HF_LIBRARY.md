# 地形节点组库（VL_HF_*）

## 为什么要有这一层

Houdini 的地形不是一个大节点，而是一组**单算子树**：`heightfield_noise`、
`heightfield_distort`、`heightfield_remap`、`heightfield_erode` …
Pegasus 的整条地形链就是把它们串起来。

如果复刻时也写成一个巨型节点组，每做一个新场景都要重写一大坨连线和参数，
无法复用也无法验证。所以本库按 **Houdini 的粒度**拆分，每个组对应一个
Houdini 节点，场景只需拼接。

## 命名与接口规范（强制）

1. **组名**一律 `VL_HF_<算子>`，与 Houdini 节点名一一对应：
   `VL_HF_Noise` ↔ `heightfield_noise`。
2. **几何端口只有一对**：输入 `Geometry`、输出 `Geometry`。
3. **地形数据只走点属性**，名字固定：

   | 属性 | 含义 | Houdini layer |
   |---|---|---|
   | `height` | 地形高度（归一化层） | `height` |
   | `water` | 水深 | `water` |
   | `debris` | 松散碎屑厚度 | `debris` |
   | `sediment` | 悬浮/沉积物 | `sediment` |
   | `bedrock` | 基岩高度 | `bedrock` |
   | `flow` | 汇水面积（=上游格子数） | `flow` |

4. **参数名**以 Houdini 参数名为准，CamelCase 转写并保留语汇：
   `elementsize` → `Element Size`、`bankangle` → `Bank Angle`。
   侵蚀组的参数名必须与 `pegasus_erode_gn.py` 里 `group_input.outputs["..."]`
   引用的字符串**完全一致**，否则会 `KeyError`。
5. **组内不引用组外节点**；组合组（preset）只能嵌套 `VL_HF_*` 子组。
6. 每个组的 `description` 必须写明它对应哪个 Houdini 节点。

## 组清单

### 基础算子

| 组 | Houdini 节点 | 参数 |
|---|---|---|
| `VL_HF_Heightfield` | `heightfield` | Size / Samples / Initial Height |
| `VL_HF_Noise` | `heightfield_noise` | Amplitude / Element Size / Octaves / Lacunarity / Roughness / Distortion / Seed / Layer / Center Noise |
| `VL_HF_Distort` | `heightfield_distort` | Amplitude / Element Size / Roughness / Seed |
| `VL_HF_Remap` | `heightfield_remap` | Layer / From Min / From Max / To Min / To Max / Clamp |
| `VL_HF_Resample` | `heightfield_resample` | Size / Samples |
| `VL_HF_Blur` | `heightfield_blur` | Layer / Cell Size / Radius / Strength / **Method** |

`VL_HF_Blur` 的 `Method` 支持 Houdini 的全部五种：

| Method | 行为 |
|---|---|
| `blur` | 8 邻域平均，按 Strength 与原值混合 |
| `boxblur` | 等权平均（与 blur 的区别在 Houdini 里是权重核不同） |
| `expand` | 取邻域**最大**——掩膜膨胀 |
| `shrink` | 取邻域**最小**——掩膜收缩（Pegasus 的 `heightfield_maskshrink1` 就是它） |
| `sharpen` | 原值 + (原值 − 平均) × Strength，锐化 |

### 层与掩膜

| 组 | Houdini 节点 | 用途 |
|---|---|---|
| `VL_HF_LayerCopy` | `heightfield_copylayer` | 把 Source Layer 复制到 Destination Layer（Houdini 用了 71 次） |
| `VL_HF_Layer` | `heightfield_layer` | 向层写常量，可乘掩膜层 |
| `VL_HF_LayerClear` | `heightfield_layerclear` | 把层重置为固定值 |
| `VL_HF_Crop` | `heightfield_crop` | 按高度阈值保留/剔除，其余填 Crop Value |
| `VL_HF_MaskByFeature` | `heightfield_maskbyfeature` | 坡度 / 高度 / 曲率三类特征掩膜，可叠加与反转 |

### 侵蚀算子

对应 `heightfield_erode::2.0` 的内部结构（hython 直读确认）：

```
heightfield_erode::2.0
  └ solve_erosion/d/s/
      precipitation  heightfield_erode_precipitation
      evaporate      heightfield_erode_precipitation（rain_amount = -evaporation）
      erode_hydro    heightfield_erode_hydro
      erode_thermal  heightfield_erode_thermal
```

| 组 | Houdini 节点 | 说明 |
|---|---|---|
| `VL_HF_Precipitation` | `heightfield_erode_precipitation` | 降水，`Rain Density` 控制落点疏密 |
| `VL_HF_WaterSpread` | `water_spreaditers` 单次迭代 | 沿坡单向搬运 + 汇水面积累积 |
| `VL_HF_ErodeHydro` | `heightfield_erode_hydro` | 汇水驱动下切 + bed/bank 分型 + 挟沙 |
| `VL_HF_ErodeThermal` | `heightfield_erode_thermal` | 高度 vs 模糊版 + 休止角塌落 |
| `VL_HF_Erode` | `heightfield_erode::2.0` | 组合组：降水→扩散×N→水力→热力，外套帧循环 |

### 组合组（Preset）

| 组 | 内容 |
|---|---|
| `VL_HF_PegasusTerrain` | Heightfield → Noise → Distort → Erode，一行调取完整 Pegasus 地形链 |

`Erosion Enabled` 关掉时只输出基座地形，方便对比侵蚀前后的差异。

## 使用

```python
# 在任意 Geometry Node Tree 里：
node = tree.nodes.new("GeometryNodeGroup")
node.node_tree = bpy.data.node_groups["VL_HF_PegasusTerrain"]
```

或者直接追加 `.blend` 里的组：
`File → Append → NodeTree → VL_HF_*`。

## 构建与验证

```powershell
$blender = "D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\blender.exe"

# 建库
& $blender --background --python tool\blender-terrain-import\build_hf_library.py -- `
  --blend artifacts\pegasus\hf_library\hf_library.blend

# 回归测试（必须全过）
& $blender --background --python tool\blender-terrain-import\test_hf_library.py
```

`test_hf_library.py` 会把各组串成两条链（基础链 + 侵蚀链）并求值，
打印 `height` / `water` / `flow` 的取值范围。任何组接错都会在这里暴露。

当前基线（`Samples=64`、`1 km`）：

```
TEST chain.height        -0.14467 .. 0.19177
TEST erode_group.height  -0.14288 .. 0.18866
TEST erode_group.water    0.14851 .. 0.23885
TEST erode_group.flow     1.00000 .. 1.79299
TEST preset.height       -0.14287 .. 0.18862
TEST VL_HF_LayerCopy.bedrock      -0.13932 .. 0.19021   层复制正确
TEST VL_HF_Layer.water             0.25000（恒定）       常量写入正确
TEST VL_HF_LayerClear.water        0.00000（全零）        清空正确
TEST VL_HF_Crop.height             0.00000 .. 0.19021    裁剪生效（546/1024 保留）
TEST VL_HF_MaskByFeature.mask      0.00000 .. 1.00000    掩膜生效（1006/1024 非零）
TEST VL_HF_Blur.height(shrink)   -0.13822 .. 0.17160    收缩生效
```

`Freeze Frames` 语义验证（同一地形，迭代 20 次）：

```
frames=1  height max=0.18913  mean=0.01379
frames=5  height max=0.18462  mean=0.01194   ← 多帧累积侵蚀更深
```

## 实现文件

| 文件 | 职责 |
|---|---|
| `pegasus_hf_core.py` | 建组框架、层读写、socket 定位工具 |
| `pegasus_hf_nodes.py` | 基础算子（Heightfield / Noise / Distort / Remap / Resample / Blur） |
| `pegasus_hf_erode.py` | 侵蚀算子（Precipitation / WaterSpread / ErodeHydro / ErodeThermal / Erode） |
| `pegasus_hf_presets.py` | 组合组（PegasusTerrain） |
| `build_hf_library.py` | 建库入口 |
| `test_hf_library.py` | 回归测试 |

## 已知坑（都在代码注释里标了）

1. **`Store Named Attribute` 的 socket 必须按"名字 + 类型"定位**，只按类型会命中
   错误的隐式槽，属性写不进去。
2. **`Set Position` 用 `Offset` 不用 `Position`**：后者是绝对赋值。
3. **`Repeat` 区**：`RepeatInput.inputs[1]` 是入口几何、`outputs[1]` 是迭代体几何；
   用 `[0]` 拿到的是 Iteration 整数，循环体不注册，结果为空。
4. **Noise 的幅度换算**是 `Amplitude / Height Scale`，不要再除一次
   （早期写成除以 `Height Scale²`，height 层只剩 ±0.0003，整条链全平坦）。
5. 地形属性是**列优先**存储，用 numpy 对比时要 `reshape(side, side, order="F")`。
