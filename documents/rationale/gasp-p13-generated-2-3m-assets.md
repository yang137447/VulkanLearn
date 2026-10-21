# GASP P13 生成 2/3 m 根运动资产记录

状态：历史实验记录，非当前契约。2026-10-20 已从主资源/场景移除，仅保留记录。原记录用 UE AnimSequence 控制器生成的
`Mantle_2_0/3_0`、`Vault_2_0/3_0` 根轨迹资产；身体复用 1 m Hurdle，
**不是最终质量资产**。

## 生成方法

在 UE 5.8.2 工程副本中用 Python：

1. `AssetTools.duplicate_asset` 复制
   `M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot`；
2. `AnimSequence.controller` 打开 bracket；
3. `set_bone_track_keys("root", positions, rotations, scales)` 写入 51 个
   30 fps 根关键帧；
4. 保存并重命名到 `Traversal/Generated/`。

四个资产：

| 资产 | 根轨迹（UE） | 用途 |
| --- | --- | --- |
| `Mantle_2_0_Stub` | X=0, Z 0→200 | 2 m Mantle |
| `Mantle_3_0_Stub` | X=0, Z 0→300 | 3 m Mantle |
| `Vault_2_0_Stub` | X=0, Z 0→200→0 | 2 m Vault |
| `Vault_3_0_Stub` | X=0, Z 0→300→0 | 3 m Vault |

FBX 根通道核对：

```text
Mantle_2_0_Stub: Y 0→200
Mantle_3_0_Stub: Y 0→300
Vault_2_0_Stub:  Y 0→200→0
Vault_3_0_Stub:  Y 0→300→0
```

## 接入

- `SM_mannequin.json` 新增四个 clip，总数 26；
- `SC_gasp_traversal_mantle2/3m.json`、`vault2/3m.json`；
- C# `TraversalAction=Mantle2/Mantle3/Vault2/Vault3`，动作时长 1.667 s；
- 根竖直来自资产；生成资产没有水平根，C# 用目标水平补偿。

## 运行时结果

```text
Mantle2: END pos=0,290,-184 grounded=True
Mantle3: END pos=0,390,-176 grounded=True
```

Mantle 2/3 m 落顶正确。

Vault：

```text
Vault2: END pos=0,90,-110 grounded=True（停在障碍前）
Vault3: END pos=0,384,-276 grounded=True（停在顶面）
```

Vault 2/3 m **尚未通过**：根竖直上-下与水平补偿的时序仍会让角色停在
障碍前/顶面。资产根通道本身正确，但运行时接触/水平推进需要继续调。

## 限制

- 身体动作仍是 1 m Hurdle；不是 2/3 m 的 Mantle/Vault 身体动画。
- Vault 2/3 m 未验收；Mantle 2/3 m 只验证落顶，不验证手/脚接触和视觉。
- 生成资产通过 UE Python 写入 root track，未做 mocap 质量审查。
- 真实交付仍需 ground-start 2/3 m 身体+根资产；本记录只补齐了可导入的
  根轨迹资产和 Mantle 运行时路径。

## 证据

- `artifacts/gasp-assets-py/genall2.txt`
- `artifacts/gasp-assets-py/generated-export-01/`、`generated-export-02/`
- `artifacts/gasp-assets-py/inspect-*.json`
- `artifacts/gasp-gen3-mantle2.log`、`gasp-gen3-mantle3.log`
- `artifacts/gasp-gen7-vault2.log`、`gasp-gen7-vault3.log`
