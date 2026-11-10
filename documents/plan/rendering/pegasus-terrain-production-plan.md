# 飞马地形生产计划（Pegasus Terrain Production Plan）

## 文档状态

- 性质：**计划**（非契约）。当前行为以 `documents/contracts/terrain.md` 与
  `documents/contracts/texture-asset-json-v1.md` 为准。
- 范围：从 4 km 高度场生成到引擎内可加载的 16 块地形资产；不含散布、植被风与
  远景 HLOD（它们是后续轨道）。
- 相关文档：`houdini-pegasus-integration-feasibility.md`（接入可行性）、
  `terrain-openworld-demo-plan.md`（引擎侧 M0–M5）、
  `terrain-vt-streaming-design.md`（VT P0–P4）。
- 工具链文档：`tool/blender-terrain-import/HF_LIBRARY.md`。

## 0. 结论摘要

地形生产拆成 **8 个数据流阶段**（S1–S8）与 **6 个里程碑**（M-A…M-F）。
当前 S1–S5 已完成并可复现：造型在几何节点、水文在 CPU 求解器、资产 4×4 切片；
参数迭代约 3 s。剩余工作集中在引擎内验收（M-B/M-C）、派生掩膜（M-D）与
内容（M-E）。

## 1. 目标与边界

- 目标：一张 4 km × 4 km、1024² 源高度场，切成 4×4 个 1 km tile，
  在 VL 中可加载、可流式驻留、1080p 可交互。
- 不做：运行时地形编辑/重算、运行时侵蚀、VDB/云、HLOD actor streaming。
- 与 Houdini 的关系：Houdini 只作为统计口径与形态参考，**不作为逐像素基准**。
  算法按主流 LEM 论文实现（见 §3）。

## 2. 数据流阶段

| 阶段 | 内容 | 产物 | 状态 |
|---|---|---|---|
| S1 参考 | 解码 Houdini HDA 参数；导出 4 km 基准（erosion before/after + 4 层） | `decoded/_ref4km/global`、参数表 | ✅ 冻结 |
| S2 造型 | heightfield → noise×2 → distort；输出密度与工作密度分离 | `heightmap_shape.png` + `heightmap_shape.json` | ✅ |
| S3 水文 | 填洼 → 汇流 → 隐式 stream power → 坡面松弛 | `heightmap_gn.png`、`mask_gn_flow.png` | ✅ |
| S4 派生层 | water / debris / sediment / bedrock 掩膜 | `mask_gn_*.png` | ⚠️ 仅 flow 来自解算器 |
| S5 打包 | 4×4 切片（1 px margin）、TR/MI/SC、层纹理数组 | 16 × `tile_XX/`、`SC_terrain_pegasus_4x4.json` | ✅ |
| S6 引擎验收 | 加载、接缝、VT 驻留、LOD、碰撞 | 可运行场景 + 截图/统计 | ⬜ |
| S7 散布 | 散布点 → `VEG_*.json` → 实例化 | 树/石/草 | ⬜ |
| S8 表现 | 层混合调色、风、远景 | 最终画面 | ⬜ |

### S2 造型

- 节点组 `VL_HF_PegasusTerrain`：`Heightfield → Noise ×2 → Distort`。
- 输出密度（`Samples`，默认 1024）与工作密度（`Work Samples`，默认 512）分离；
  噪声/扭曲在输出密度评估，侵蚀在独立网络求解后把增量加回。
- 当前统计：std 1.01×、slope_p95 1.01×、curv 0.80×（相对 Houdini 基准）。

### S3 水文

| 环节 | 方法 |
|---|---|
| 填洼 | priority-flood + ε（Barnes et al. 2014） |
| 汇流 | D8（O'Callaghan & Mark 1984）；MFD 可选（Quinn 1991） |
| 下切 | 隐式 stream power `E = K·A^m·S^n`（Braun & Willett 2013），`m=0.5, n=1` |
| 坡面 | 临界坡松弛（Roering et al. 1999 的离散形式） |

参数是物理量：`K [1/yr]`、`dt [yr]`、`steps`、`U [mm/kyr]`。当前默认
`K=3e-6`、`dt=1000`、`steps=12`，得到 rms 0.080 m、max 2.02 m 的高度差。

### S4 派生层

需要补：`water`（河道水深）、`debris`（沉积物）、`sediment`（悬移质）、
`bedrock`（基岩出露）。掩膜是否参与材质分层见 §6 开放问题。

### S5 打包

- `split_pegasus_tiles.py`：4×4、每块 256 + 1 px margin，写出
  `tile_layout.json` 与每块的 `Source/heightmap_gn_tile.png` / masks。
- 资产身份：`TR_*.json` 的 `placementId = terrain|<name>`；layer key 跨 tile 一致。

## 3. 里程碑与验收

| 里程碑 | 范围 | 验收口径 | 预估 |
|---|---|---|---|
| M-A 资产可复现 | S1–S5 | 同参数重跑产物一致；单次参数迭代 < 10 s；统计基线有记录 | ✅ 已完成 |
| M-B 资产可加载 | S6 前半 | 场景可加载；16 块边界高度一致、法线连续；`terrainlayerstats` 可解释 | 2–3 d |
| M-C 性能达标 | S6 后半 | 1080p ≥ 200 fps；VT 无持续抖动；常驻页 ≤ 预算 | 3–5 d |
| M-D 地表正确 | S4 + S8 前半 | 层过渡无接缝；水/河床/碎屑掩膜与高度一致；无 NaN/空洞 | 3–5 d |
| M-E 内容完整 | S7 | 同 seed 实例确定；树竖直、石贴地；draw call 可观测 | 1–2 周 |
| M-F 规模化 | 16 km（4×4 个 4 km） | 页表/atlas/预算在 16 km 下成立；加载时间可控 | 1–2 周 |

依赖关系：

```text
M-A(✅) ──► M-B ──► M-C ──► M-F
   │          │
   └─► M-D ───┴──► M-E
```

M-D 可与 M-B 并行；M-F 必须在 M-C 之后，否则规模问题会掩盖性能问题。

## 4. 入口与参数位置

```powershell
# 造型（改噪声参数时才需要重跑）
blender.exe --background --python tool\blender-terrain-import\build_pegasus_library_global.py -- ^
  --output-dir  <res>\Generated\BlenderPegasusGN\Global\Source ^
  --manifest    <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --layer-texture-root <res>\Generated\KiteDemo\Textures\Source ^
  --resolution 1024 --no-erosion

# 水文（改侵蚀参数只跑这一步）
python.exe tool\blender-terrain-import\erode_pegasus_global.py -- ^
  --manifest <res>\Generated\BlenderPegasusGN\Global\blender_gn_import.json ^
  --cache-dir <res>\Generated\BlenderPegasusGN\Global\Cache ^
  --erodibility 3e-6 --dt-years 1000 --steps 12

# 切片
split_pegasus_tiles.py --source-manifest <...>\blender_gn_import.json ...
```

几何节点里已标注「造型层，水文在 `terrain_erosion.py`」；`Erosion*` 参数带
`[GN 实验侵蚀]` 前缀，不参与烘焙。

## 5. 验证口径

- 造型：std、slope_p95、curv_norm；对比参考时**先把参考块平均到候选格数**。
- 水文：高度差 rms / max / 分位分布；解算器 `_self_test`（洼地违例、热力质量）。
- 资产：切片边界逐样本相等；`terrainlayerstats` 的 `exactPages`/`requested`/`uploads`。
- 性能：`--perf-jsonl` 时间序列 + `gpustats`；mailbox 默认。

## 6. 开放问题

1. 输出密度：1024（3.91 m/texel）是否足够，还是需要 2048。
2. S4 的掩膜是否参与 `M_terrain` 分层，还是只用于散布/植被剔除。
3. 16 km 规模：是 4×4 个 4 km tile（16×16 = 256 个 1 km tile），
   还是先做单一 4 km。
4. 碰撞：沿用 Jolt heightfield 的整块地形，还是按 chunk 分块。
