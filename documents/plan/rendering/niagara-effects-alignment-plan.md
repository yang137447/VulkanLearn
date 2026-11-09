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

## 已知替身与近似

- UE `NS_Dino_*` 的恐龙骨骼资产尚未导入 VL；当前使用 `SK_UEFN_Mannequin` 的绑定姿态作为骨骼 mesh 替身，结构对齐但模型外观不等价。
- Player Teleport 使用静态 mesh shard + dissolve，不是骨骼表面三角 dissolve。
- Footstep / NDC 的脚印与弹孔使用程序化/复用贴图；NDC 事件用连续粒子流近似。
- 缺失 UE 贴图的特效使用程序化纹理/圆盘/噪声 fallback；后续拿到导出图后可替换，不改变发射器结构。
- 粒子 pass 的既有 `VUID-vkCmdDraw-None-09600`、`VUID-vkCmdFillBuffer-dstBuffer-00029` 仍未处理。
