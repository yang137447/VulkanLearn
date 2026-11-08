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
| `VL_HF_WaterSpread` | `water_spreaditers` 单次迭代 | 保守多向搬运 + 汇水面积累积 |
| `VL_HF_ErodeHydro` | `heightfield_erode_hydro` | 汇水驱动下切 + bed/bank 分型 + 挟沙 |
| `VL_HF_ErodeThermal` | `heightfield_erode_thermal` | 高度 vs 模糊版 + 休止角塌落 |
| `VL_HF_Erode` | `heightfield_erode::2.0` | 组合组：热力→降水→(扩散×N+水力)→蒸发，外套帧循环 |

### 组合组（Preset）

| 组 | 内容 |
|---|---|
| `VL_HF_PegasusTerrain` | Heightfield → Noise ×2 → Distort → Erode，一行调取完整 Pegasus 地形链 |

`Erosion Enabled` 关掉时只输出基座地形，方便对比侵蚀前后的差异。

## 关键语义（2026-11 复核，均对 `_ref4km` 基准实测过）

1. **层单位**：所有地形层都是「米 / Height Scale」的归一化值，`height` 也不例外。
   降水必须同样除以 `Height Scale`。早期把 `water` 按米存、`height` 按归一化存，
   `surface = height + water` 被水主导（water 均值 0.907、p1..p99 只有 0.86..0.99），
   水不沿地形走，河道和侵蚀都消失。
2. **降水**：面平均深度是 `Rain Amount × Rain Density`，`Rain Density` 是落点覆盖率；
   蒸发按每帧比例 `water *= (1 - Rain Evaporation)`（参数标签是 "Evaporation Rate"）。
3. **水扩散**必须**按方向路由且守恒**：每格把 `alpha × water` 按各下坡落差分配到 8 邻域，
   接收端对同一对格子求值同一个表达式。只把 `min(drop, water)/8` 抹掉不守恒，
   水会均匀堆起来。
4. **汇水面积 `flow`** 用地形落差累积（静态 DAG），不用水面落差——水面会形成
   A→B→A 环流，实测 20 轮就到 6.4e7，40 轮溢出成 inf/NaN。上限是总格数。
5. **水力下切**用 stream power：`GlobalErosionRate × 可蚀性 × 坡度系数 × (A/Atotal)`，
   `m=1`。`Global Erosion Rate` 的 HDA 单位是 `m1frame-1`（米/帧），同时是单帧硬上限。
   旧实现按米/迭代、且用 `map_range(log10(flow)+1, 0.15..1)` 当权重，等于全图均匀下切
   （侵蚀 rms 2.16 m，参考 0.084 m）。
6. **热力侵蚀**的超休止角项也必须乘 `Thermal Erosion Rate`（0.03）。之前直接整块搬走，
   地形整体下降 1.7 m、最大 37 m。
7. **网格密度与阶段对齐**：噪声/扭曲在输出密度（1024），侵蚀在 `Work Samples`（512）
   上解，再把**侵蚀增量**加回细网格。把整张场降采样再升采样会丢掉高频细节
   （curv_norm 从 0.157 掉到 0.048）；1024 上直接跑侵蚀则要 15 分钟以上。

## 资产流水线（快迭代路径）

几何节点只负责**造型**；水文学交给 `terrain_erosion.py` 的 CPU 求解器。
这条分层不是为了省事——几何节点的汇流是迭代扩散，需要 O(N) 轮才收敛，
1024 格上 40 轮只传播 40 格，既慢又刻不出河道。

```powershell
# 1) 造型（一次性；改噪声参数才需要重跑）
blender.exe --background --python build_pegasus_library_global.py -- ^
  --output-dir <res>\Generated\BlenderPegasusGN\Global\Source ^
  --manifest   <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --layer-texture-root <res>\Generated\KiteDemo\Textures\Source ^
  --resolution 1024 --no-erosion

# 2) 侵蚀（改任意侵蚀参数只跑这一步）
python.exe erode_pegasus_global.py -- ^
  --manifest <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --cache-dir <res>\Generated\BlenderPegasusGN\Global\Cache ^
  --erodibility 3e-6 --dt-years 1000 --steps 12

# 3) 切片
split_pegasus_tiles.py --source-manifest <...>\blender_gn_import.json ...
```

| 阶段 | 耗时（1024²，本机） |
|---|---|
| 造型烘焙 | 11.5 s |
| 侵蚀（冷：建汇流网络） | ~7 s |
| **侵蚀（热：只改参数）** | **2.7 s** |
| 切片 16 块 | 2 s |

### 侵蚀算法（按论文，不拟合商用软件）

| 环节 | 采用的方法 |
|---|---|
| 填洼 | priority-flood + ε（Barnes et al. 2014） |
| 汇流 | D8（O'Callaghan & Mark 1984）；MFD 可选（Quinn 1991） |
| 河道下切 | 隐式 stream power `E = K A^m S^n`（Braun & Willett 2013），默认 `m=0.5, n=1` |
| 坡面 | 临界坡松弛（Roering et al. 1999 的离散形式） |

默认 `k = 3e-3`（A 以 m² 计）。在 4 km / 1024 的造型上单次 12 步的
高度差：rms 0.080 m、max 2.02 m、`≥10 cm` 占 10.3%。

### 物理参数化（推荐）

```text
∂h/∂t = U - K·A^m·S^n        U [m/yr]，K [1/yr]（m=0.5、A 以 m² 计）
```

求解器内部每一步的系数是 `k·dt·A^m`，所以命令行传的是**可蚀性 K**，
不是拟合出来的 `k`：

| 参数 | 含义 | 默认 |
|---|---|---|
| `--erodibility K` | 可蚀性 [1/yr] | `3e-6` |
| `--dt-years` | 每步年数 | `1000` |
| `--steps` | 步数（总时间 = dt × steps） | `12` |
| `--uplift-mm-per-kyr` | 抬升速率 U [mm/kyr] | `0` |

`K=3e-6 /yr`、`dt=1000` 等价于之前标定的 `k·dt=0.003`，输出完全一致。
抬升会改变地表与上下游关系，所以 `U>0` 时自动改成每步重建汇流网络
（`network_refresh=1`），代价约为每步一次建网。

冒烟验证：`--steps 4 --uplift-mm-per-kyr 500`（总抬升 2.00 m）让最高点
从 71.57 m 升到 73.58 m，与 `U×总时间` 吻合。

`--passes two` 是**可选**的形态选项（D8 宽谷 + MFD 细沟），当初是为了贴近
Houdini 的谱形加的，不属于标准 LEM 流程：

| 配置 | delta rms | max | 5–11 m | 11–26 m | 26–52 m |
|---|---|---|---|---|---|
| Houdini 参考 | 0.084 | 1.87 | 13.3% | 18.5% | 40.8% |
| 单段 MFD k=9e-5 | 0.081 | 1.75 | 22.3% | 11.5% | 3.6% |
| 单段 D8 k=6e-5 | 0.135 | 3.80 | 13.6% | 21.4% | 39.8% |
| 两段 D8 2e-5 + MFD 2.5e-5 | 0.097 | 3.15 | 12.7% | 20.8% | 41.0% |

### Houdini 四次侵蚀链（`--chain pegasus`）

`sop_pe_landscape` 是 4 次侵蚀，每次参数不同。solver 没有水层/bank 模型，
按下表映射到现有旋钮：

| 映射 | 来源 |
|---|---|
| K 倍率 | `erodability × erosionrate / 1.1`（以 erode1 为基准） |
| thermal | `thermal_erodability`（erode4 为 0 → 跳过迭代） |
| talus | `thermal_cutangle` |
| deposition | `1 − removalrate` |
| flow | `bankangle ≤ 30 → MFD`（宽谷），否则 D8（窄谷） |

实测（`--chain pegasus --chain-k 0.35`，1024²）：

| 指标 | 四次链 | Houdini 参考 |
|---|---|---|
| 高度差 rms | 0.075 | 0.084 |
| 高度差 max | 1.24 | 1.87 |
| std | 1.01× | — |
| slope_p95 | 1.00× | — |
| curv_norm | 0.61× | 1.00× |

链结构与 per-pass 参数已复刻，但增量能量集中在 <8 m，p95 曲率反而比单次低。
因此**出货仍用单次**（curv 0.80×），`--chain pegasus` 保留为对照与后续标定入口。

### 派生层（S4）

`erode_pegasus_global.py` 在写回高度后输出四张掩膜：

| 文件 | 模型 | 实测 |
|---|---|---|
| `mask_gn_water.png` | `0.015 m + 1.5 m × (A/Amax)`，再归一化到 0..1 | 深度 mean 0.0196 / max 1.51 m（参考 0.0214 / 1.535） |
| `mask_gn_flow.png` | `A / mean(A)` 的 log 压缩 | p50 0.2×、p99 16.7× |
| `mask_gn_bedrock.png` | 常量 1（对应 `remap_bedrock` 的 outputmin=outputmax=1） | — |
| `mask_gn_debris.png` | 求解器累计沉积（`--deposition`，默认 0.5），按最大值归一化 | rms 0.0145 m / max 1.68 m |
| `mask_gn_sediment.png` | 累计被带走 = 净侵蚀 − 沉积，按最大值归一化 | rms 0.0741 m / max 2.03 m |

水的单一幂律 `c·A^p` 复刻不了参考：参考 rms/mean=3.44，p≤0.6 只能到 2.17。
改成「薄水膜 + 河道填充」后均值与峰值都对上（rms 0.030 vs 0.074，中间量偏少）。

**单位注意**：Houdini 的 debris/sediment 是 0..1 的层值，我们的台账是米再归一化，
rms 不能直接对比。`--deposition` 只影响平坦处的回填，对最终高度几乎无影响
（实测范围 -52.12..71.57 → -52.11..71.57）。

这些掩膜目前只是**资产产物**：`M_terrain` 还没有水体/碎屑的着色分支，
把 water 接进 TR 的 layer 集合需要先加材质特性，不是纯配置改动。

### 为什么不把 water 加回高度

参考的 `water_addtoheight=on`，但实测把 `water = c·(A/Amax)^0.5` 加到
侵蚀结果上会让 rms 从 0.081 掉到 0.048、`>=5 cm` 从 18% 掉到 2.5%——
水正好填在切掉的地方，两者互相抵消。参考里「宽而浅」的那部分在最终
高度上已经被我们的侵蚀量覆盖，再叠一层就是重复计算。`mask_gn_flow.png`
因此只作为河网掩膜，不参与高度。

### 求解器的两个关键约定

1. **汇流网络复用**：`ErosionParams.network_refresh`（默认 0）表示整次侵蚀只
   填洼/累积/分层一次。每迭代重建占总耗时 88%（填洼 3.3 s、累积 2.0 s、
   分层 1.6 s，而隐式求解只有 0.4 s）。浅侵蚀下网络几乎不变；有抬升时设 1。
2. **默认不填洼**（`fill_sinks=False`）：填洼后的 `filled` 与真实高度不一致，
   而隐式求解的 `value >= max_downstream` 钳制按 filled 的上下游关系做，
   会把整片洼地抬到下游高度（实测 rms 2.78 m、均值 +0.75 m，且与 k 无关）。
   洼地当局部汇即可，不产生下切也不会被抬高。

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

`test_hf_library.py` 会把各组串成两条链（基础链 + 侵蚀链）并求值。
它会**断言**：空几何、缺失属性、NaN/inf、以及 `flow > 总格数`（汇流累积出现
环流或未归一化）都会直接失败，不再只是打印。

当前基线（`Samples=64`、`1 km`）：

```
TEST chain.height        -0.14467 .. 0.19177
TEST erode_group.height  -0.14288 .. 0.18866
TEST erode_group.water    0.00000 .. 0.00405
TEST erode_group.flow     1.00000 .. 271.38
TEST preset.height       -0.11422 .. 0.16207
TEST preset.flow          1.00000 .. 253.97   （≤ 4096 格，断言通过）
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
| `build_pegasus_library_global.py` | 造型烘焙（`--no-erosion` 只出 `heightmap_shape.png`） |
| `erode_pegasus_global.py` | 侵蚀阶段：读造型、跑 CPU 求解器、写回 `heightmap_gn.png`，带汇流网络缓存 |
| `terrain_erosion.py` | CPU 水文求解器（填洼 / MFD·D8 汇流 / 隐式 stream power / thermal） |
| `split_pegasus_tiles.py` | 4×4 切片与 tile manifest |
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
6. **Distort 的 Amp 是米**：它移动的是世界坐标采样点，不能再除以 `Height Scale`
   （那样 140 m 的扭曲只剩 0.28 m，等于没有扭曲）。
7. **`VL_HF_Erode` 只改属性、不改几何位置**：早期在组内把 height 写成 Z 位移，
   下游按平面探针做 `SampleNearestSurface` 升采样时会取到错误顶点。
