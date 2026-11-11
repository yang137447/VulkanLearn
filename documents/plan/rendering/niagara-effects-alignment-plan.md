# UE Niagara 特效全量对齐计划

> 状态：49 个目标已全部建立 VL 案例；本文记录目标、完成度和验收口径，不是当前能力契约。

## 目标范围

参考工程：`D:\YYBWorkSpace\UnrealProject\effect\Content\NiagaraExamples`。

- 工程内 `NS_*.uasset` 共 67 个。
- 去掉编辑器标记、Boundary 调试、SpriteGeneration 与 Reference 工具后，实际需要对齐的特效为 **49 个**。
- `NE_*.uasset` 共 21 个，作为发射器结构参考，不单独计为特效。

## 完成度

已全部建立案例：**49 / 49**。

早期 9 个：

| UE 系统 | VL 案例 |
| --- | --- |
| `NS_Explosion` | `SC_particle_ue_explosion` |
| `NS_Dirt_Explosion` | `SC_particle_ue_explosion` |
| `NS_Smoke_Plume` | `SC_particle_ue_explosion` / `SC_particle_ue_smoke` |
| `NS_FireworkBurst` | `SC_particle_ue_firework` |
| `NS_Fire` | `SC_particle_ue_fire` |
| `NS_Chimney_Smoke` | `SC_particle_ue_smoke` |
| `NS_Impact_Concrete` | `SC_particle_ue_impact` |
| `NS_MuzzleFlash` | `SC_particle_ue_muzzle` |
| `NS_TeslaCoil` | `SC_particle_ue_tesla` |

新增 40 个，按 gallery 批次实现：

| 批次 | UE 分类 | 目标 NS | 数量 | VL 目录 |
| --- | --- | --- | ---: | --- |
| A | `FX_Explosions` | `NS_Dirt_Explosion_Medium`、`NS_Dirt_Explosion_Small`、`NS_Explosion_Medium`、`NS_Explosion_Small` | 4 | `SC_particle_ue_explosion_variants` |
| B | `FX_Weapons/Impacts` | `NS_Impact_Glass`、`NS_Impact_Metal`、`NS_Impact_Wood` | 3 | `SC_particle_ue_impact_variants` |
| C | `FX_Footstep` | `NS_Footstep_Bubbles`、`NS_Footstep_Fire`、`NS_Footstep_Gravel`、`NS_Footstep_LW` | 4 | `SC_particle_ue_footstep` |
| D | `FX_NDC` | `NS_NDC_Footsteps`、`NS_NDC_Footsteps_Bubbles`、`NS_NDC_Footsteps_Fire`、`NS_NDC_Impacts` | 4 | `SC_particle_ue_ndc` |
| E | `FX_Misc` | `NS_Bubble_Burst`、`NS_HitDissolve` | 2 | `SC_particle_ue_misc` |
| F | `FX_Misc` | `NS_SkeletalMeshBones_Burst`、`NS_SkeletalMeshBones_Loop`、`NS_SkeletalMeshTris_Burst`、`NS_SkeletalMeshTris_Loop` | 4 | `SC_particle_ue_misc` |
| G | `FX_PickUp` | `NS_Pickup_Idle`、`NS_Pickup_Spawn`、`NS_Pickup_Success`、`NS_Pickup_Timeout` | 4 | `SC_particle_ue_pickup` |
| H | `FX_Player` | `NS_Player_Buff_Looping`、`NS_Player_DeBuff_Looping`、`NS_Player_Electricity_Looping`、`NS_Player_Teleport_In`、`NS_Player_Teleport_Out`、`NS_Weapon_Buff_Looping`、`NS_Weapon_DeBuff_Looping` | 7 | `SC_particle_ue_player` |
| I | `FX_SkeletalMesh` | `NS_Dino_Tri_Loop`、`NS_Dino_Tris_Color_Burst` | 2 | `SC_particle_ue_skeletalmesh` |
| J | `FX_Sparks` | `NS_Spark_Burst`、`NS_Spark_Continuous`、`NS_Spark_Impact_Looping` | 3 | `SC_particle_ue_sparks` |
| K | `FX_Weapons/Trails` | `NS_BulletTracer`、`NS_RocketTrail`、`NS_SimpleRibbonTrail` | 3 | `SC_particle_ue_trails` |

## 验收口径

每个 NS 满足：

- 有独立 `PS_*.json`，不得只复制同一资产改名。
- 所属分类有可直接启动的 gallery `SC_*.json`，包含该 NS 的 `particleSystem` 实例。
- `main.exe --initial-scene ... --no-dev-ui` 加载成功，无 `Scene.LoadFailed`。
- 生成 1280x720 截图，能看到该 NS 的主体形态。
- README 记录 UE 系统名、VL 资产名、发射器角色和近似/替身说明。

## 自动验收

工具：`tool/validation/particle/particle_niagara_acceptance.py`，PowerShell 入口
`tool/validation/particle/run_particle_acceptance.ps1`。用法见同目录 `README.md`。

```powershell
# 静态：目标推导 + README 映射 + PS/gallery JSON + texture/mesh/MI 引用
py -3 tool/validation/particle/particle_niagara_acceptance.py

# 静态 + 全量 gallery headless 冒烟
py -3 tool/validation/particle/particle_niagara_acceptance.py --smoke
```

目标推导固定为：扫描 `NS_*.uasset`，排除 `FX_Markers/**`（编辑器标记）、
`NS_Boundary*`（Boundary 调试）、`Utilities/**`（SpriteGeneration 工具）和
`Materials/**`（SubUV 参考）。当前 67 个扫描项排除 18 个，得到 49 个目标；
数量与 `--expect-targets` 不一致时脚本失败。

映射优先读 gallery README 中同一行的 `NS_*` / `PS_*.json` 对；没有显式对时按
名称相似度在同目录选择，最后才退化为目录级映射。矩阵里的 `mapping_source`
记录实际规则。

输出（gitignored）：`artifacts/particle-niagara-acceptance/` 下的
`acceptance-matrix.md`、`acceptance-result.json`、`smoke/*.stdout.txt` /
`*.stderr.txt` 与截图副本。矩阵每行包含 UE 源、VL PS、gallery、截图、状态、
Budget、README 近似/替身摘录和 VUID；另有 17 个 gallery 的性能预算表、
48 个 PS 的静态指标表。

性能预算口径：

- 每个 gallery 从第 120 帧起连续提交 3 次 `particlestats`，截图在第 123 帧提交。
- `particles` / `drawCalls` / `vertices` / `gpuMs` 取三次峰值；
  `gpuAvgMs` / `gpuMaxMs` 取最后一次累计值。
- 预算判定使用 `particles` / `drawCalls` / `vertices` / `gpuAvgMs`。
  `gpuMs` 是单帧瞬时值、`gpuMaxMs` 是整轮最坏单帧，只展示、不参与
  PASS/OVER，避免启动上传尖峰误报。
- 预算 = baseline × 1.20；`--write-budget` 按逐指标最大值合并多次测量，
  预算文件为 `tool/validation/particle/particle_performance_budget.json`。
- 静态指标检查 emitter 数、`renderMode` 分布、`maxParticles` 总和/单 emitter
  最大值、纹理引用；异常大/空/重复 renderer 记 WARN。

2026-11-10 基线：49 target / 48 PS / 17 gallery；静态 **0 FAIL**、0 静态
WARN；全量 smoke **49 PASS / 0 WARN / 0 FAIL**，17/17 截图落盘，VUID **0**；
17/17 gallery 在预算内。明细以每次生成的 `acceptance-matrix.md` 为准。

## 已知替身与近似

- UE `NS_Dino_*` 已导入真实 `SKM_DinoDragon` LOD1 作为骨骼粒子源；当前使用绑定姿态，不加载动画片段。
- Player Teleport 已改为真实 Manny/Quinn LOD1 的 `skeletalMeshTris` 骨骼表面
  dissolve（共用 `particleDissolve.glsl`），程序化 shard 降为第二层辅助细节。
- Footstep / NDC 的脚印与弹孔使用程序化/复用贴图；NDC 事件用连续粒子流近似。
- 缺失 UE 贴图的特效使用程序化纹理/圆盘/噪声 fallback；后续拿到导出图后可替换，不改变发射器结构。
- `burstCount`、`burstDelay`、`burstPhase` 与 `spawnPerUnit` 已进入运行时；现有 gallery 的
  UE stateless spawn count / loop duration / spawn spacing 仍不可从二进制直接读取，继续使用
  视觉近似值。拿到 UE 编辑器导出参数后可只改资产，不再改运行时。
- 粒子 pass 的 `VUID-vkCmdDraw-None-09600`、`VUID-vkCmdFillBuffer-dstBuffer-00029`、
  `VUID-vkUpdateDescriptorSets-None-03047` 已修复；2026-11-10 全量 smoke 的
  17 个 gallery 均无 VUID。`particledump` 诊断命令仍可能触发
  `VUID-VkBufferMemoryBarrier-size-01188`，不在正常 smoke 路径上。
