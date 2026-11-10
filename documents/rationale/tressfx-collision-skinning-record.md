# TressFX 碰撞跟随骨骼：实现选择与测量记录（2026-10-23）

状态：`rationale/`，记录本分支的选择、证据与测量，不是契约。当前行为以
[tressfx-asset-import.md](../contracts/tressfx-asset-import.md) 为准。

## 背景

旧实现有两个已文档化的缺陷（`contracts/tressfx-asset-import.md` 修订前 410–413、
732–734、743–746）：

1. 作者胶囊始终在绑定空间，头部/身体转动时不移动；
2. `collision.mesh` 代理是全身网格，却只按根骨 `Head_M` 的单一矩阵刚性变换，
   头部转动时整份身体代理绕头骨旋转。

本记录覆盖两处修复、资源映射依据、CPU/GPU 成本与验证证据。

## Phase A：胶囊绑定骨骼

- schema 为每个胶囊增加可选 `bone`。加载期通过 `simulation.skinning.source`
  的目标骨架解析；未知骨是硬错误（消息带 `collision.capsules[i].bone`）。
- 运行期每帧用该关节的全骨架 bind→pose 变换 `a`/`b`，radius 原样保留：
  当前导出没有独立骨骼缩放提取，把缩放吸收进半径会把球/胶囊碰撞失真。
- 未声明 `bone` 的胶囊保持作者绑定空间，未修改的资产行为不变。
- collider SSBO 改为每个 frame-in-flight 一份；绑定胶囊逐帧重写，避免覆盖
  仍在飞的上帧边界。

### Female 资源映射依据

在 `skin-v1/female-bind.json` 的绑定姿态身体上，对胶囊轴线采样并统计 4 cm
内身体顶点的权重份额（`artifacts/capsule_weight_probe2.log`）：

| 胶囊 | 采样处主导骨（4 cm 内权重份额） | 选择 |
| --- | --- | --- |
| 头球（0,168,0.5）r8.5 | 最近身体顶点 `Head_M` = 100% | `Head_M` |
| 颈（149→159）r6.5 | `Neck_M` 43.2%、`Chest_M` 37.7%、`Neck1_M` 15.8%、`Head_M` 3.2% | `Neck1_M`（胶囊中心 y≈154 的表层权重主导） |
| 肩线（±12,141,-5）r8 | `Scapula_L` 40.9%、`Scapula_R` 37.6%、`Shoulder_L/R` 17.2%、`Chest_M` 4.4% | `Chest_M`（单一对称胶囊跨两侧肩链，Chest 是两支共同父级） |
| 上背（135→150）r6 | `Chest_M` 78.0%、`Neck_M` 20.8% | `Chest_M` |

a/b/radius 全部保持作者原值；资源变更只增加 `bone` 字段。

## Phase B：代理逐顶点权重

### 权重转移

- 代理 OBJ 无作者权重。加载期对每个代理顶点在绑定姿态身体顶点上做空间哈希
  最近点，复制最近顶点的完整正权重列表并归一化。
- 身体位置先乘 `section.meshToModel`，与代理同处渲染绑定空间；2,572 个代理
  顶点到最近身体顶点的最大距离 0.9256 cm、均值约 0.22 cm，证明两者同空间。
  超过 2 cm 视为资产错位，加载期硬错误。
- GPU `SkinInfluences` 是 CSR 布局，没有 4 骨上限，因此保留全部影响而不截断；
  本资产身体单点最多 4 个正权重，实际结果 <= 4。
- 等距时取全局下标更小的身体顶点；输出只取决于输入顺序。
- 转移只在加载期做一次，缓存为 CSR word 流：2,572 顶点、53,788 bytes、
  最多 4 影响。运行期不重新求解。

### palette：为什么不是直接复用身体 VS 的 per-section palette

`SkinPose::matrices[joint]` 是逐 section 的
`models[joint] * inverseBind[joint]`。FBX 身体被拆成 3 个 body section，
`inverseBinds` 对“未参与该 section 的关节”保留单位阵；这些关节的矩阵会退化
为绝对 model 矩阵。代理权重跨多个 section（头、颈、胸、肩、脊柱），直接复用
第一份 section palette 会把胸/肩顶点推到百米级错误位置（实测域边长从
约 56 cm 变成 298 cm）。

修复：`SkinPose` 增加全骨架 `bindToPoseMatrices[joint] =
models[joint] * inverse(restModel[joint])`，与 section 无关。hair 的根骨、
胶囊和代理都使用这份矩阵。SDF splat 使用 hair 对象自己的逐 frame-in-flight
palette（每关节 128 bytes，current/previous 填同一矩阵，105 关节 =
13,440 bytes/槽），Host 每帧拷贝。

### 域与 GPU 路径

- CPU 每帧用与 GPU 完全相同的逐顶点 LBS 计算代理包围盒（2,572 顶点），
  再按原公式构造立方体域；`SDF params.counts.y=1` 通知 splat 走逐顶点路径。
- `tressfxSimSdfSplat.comp` 从 binding 12（代理影响流）和 binding 13（全局
  palette）按权重混合 `mat4`，`counts.y=0` 时回退 `params.skinning` 单骨矩阵。
- SdfCollide 仍采样同一个有符号场，空间约定未变。

## 验证与测量

- 场景/姿态：`SC_tressfx_female_hair_animated.json`，Walk clip（`animation clip 1`）
  `animation seek 0.4333`（Walk 4.0 s，30 fps）；相机 `camera pose 500 165 0 0 151 0`。
  该帧 Head local 旋转 7.2°、neck_01 5.9°、spine_02 8.45°（`artifacts/walk-fbx-inspect.json`）。
- 截图（1280×720）：`artifacts/evidence/comparison/`
  `capsules_walk0433.png`、`full_walk0433.png`、`full_idle203.png`、
  `side_idle203.png`。像素差（>8/通道）：胶囊 only 21,641 px、胶囊+SDF 21,489 px
  （walk）；Idle 2.033 s 分别为 60,750 / 75,057 px（侧视 66,752 px）。
- 数值探针（临时 instrumentation，`artifacts/probe/walk0433.log`）：
  - Walk 0.4333 s 时头胶囊端点 `a=(-2.2519,167.735,94.6382)`，作者值
    `(0,168,0.5)`；位移 94.17 cm（主要是该片段的根位移）。旧实现该胶囊位移为 0。
  - 同一姿态逐顶点 LBS 与“绕 Head_M 单骨”位置差：最大 7.3066 cm、均值
    1.4715 cm，2,572 顶点中 1,524 个 > 1 cm。旧实现整份代理使用后者。
- 性能（Release，`--frame-limit 60`，`--perf-jsonl`，15 个有效秒，
  `artifacts/evidence/perf/`）：

  | 变体 | hairSimulation 均值 | gpuTotal 均值 |
  | --- | --- | --- |
  | before capsules only | 0.8167 ms | 4.581 ms |
  | after capsules only | 0.8091 ms | 4.441 ms |
  | before capsules+SDF | 1.1286 ms | 4.845 ms |
  | after capsules+SDF | 1.1339 ms | 5.087 ms |

  SDF 部分（full - capsules）：before 0.312 ms、after 0.325 ms。改动后
  hairSimulation +0.005 ms（+0.5%），帧时间在噪声范围内。
- CPU 成本：`renderRecord` 阶段 before capsules 0.5441 ms / before full 0.5549 ms /
  after capsules 0.5437 ms / after full 0.6008 ms；SDF 代理路径新增约
  0.046 ms/帧（逐顶点包围盒 2,572 点 + 13.4 KB palette 上传），逐顶点蒙皮仍
  在 GPU。
- Debug + validation：stderr 为空，日志无 `VUID`；`hair_asset_tests` 155 项通过。

## 已知边界

- 权重是最近身体顶点的整份复制，不是表面点插值；开放边/非流形边附近仍可能
  有弱约束。
- 每帧 CPU 仍需一遍代理包围盒归约；代理数量到数万顶点时应改 GPU 归约
  （TressFX `ICMBox` 路径）。
- 发丝根点仍是单根骨矩阵；碰撞代理的多骨权重不改变 guide 根点语义。
- 没有摩擦、碰撞速度冲量、`.tfxmesh`、follow hair、深度阴影、OIT、LOD。
