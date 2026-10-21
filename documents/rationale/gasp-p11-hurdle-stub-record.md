# GASP P11 2/3 m Hurdle 程序化扩展记录

状态：历史实验记录，非当前契约。2026-10-20 已按严格 UE GASP 范围从主资源/场景移除，仅保留记录。原记录在缺少 ground-start 2/3 m Mantle/Vault
资产时，用 1 m `Hurdle_1_0` 身体动作做的 gameplay stub；**不是资产验收**。

## 背景

[med/high 资产审计](gasp-medhigh-asset-audit.md) 已确认：UE GASP Traversal
目录没有 ground-start 的 2 m/3 m Mantle/Vault；所有 med/high 都是
`Catch_*`，根轨迹从 12.8–14.5 m 高空开始。

唯一完整地面“上-过-下”根轨迹是 1 m：

```text
M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot
duration 1.667 s
root (Y,Z): first (0,-75), last (0,110)
Y 0→100→0, Z -75→110
```

## 实现

- 新 FBX：`Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.fbx`
- `SM_mannequin.json` 新增该 clip，clip 总数 22。
- `TraversalAction=Hurdle`，高度窗口 70–320 cm；检测球从局部 `y=120` 出发。
- 根水平仍来自 Hurdle；竖直按目标高度缩放：

```text
verticalScale = (targetY - startY) / 100
2 m → 2.0
3 m → 3.0
```

- 3 m 时再补一个朝目标的有界水平速度（≤120 cm/s），避免根水平不足卡在前沿。
- 结束后清空 root override；Mantle/Vault/Climb 路径不变。

场景：

- `SC_gasp_traversal_hurdle_2m.json`：障碍顶面 200 cm；
- `SC_gasp_traversal_hurdle_3m.json`：障碍顶面 300 cm。

## 结果

仓库：

```text
2m: START height=200 action=M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot
    END pos=0,290,-233 grounded=True
3m: START height=300 action=...
    END pos=0,90,-297 grounded=True
```

2 m 落在顶面（y=290 = 顶 200 + 胶囊 90）；3 m 越过顶面后回到地面，
落点在障碍后沿之外（z=-297 < -260），符合 Vault 式落点。

独立包 `artifacts/gasp-p11-hurdle-package-01`（1724 文件，约 1.59 GB）：

```text
2m: END pos=0,290,-246 grounded=True
3m: END pos=0,90,-306 grounded=True
```

stderr 为空。

## 明确限制

- 身体动作仍是 **1 m Hurdle**；2/3 m 只缩放根竖直并补水平，不是
  2/3 m 的 Mantle/Vault 身体动画。
- 无手/脚接触、无 2/3 m 的视觉验收；不能宣称“已支持 2/3 m Mantle/Vault”。
- 该路径是 gameplay stub；assets 审计的结论仍是“缺 ground-start 2/3 m clip”。
- 真实 2/3 m 交付需要新 clip；本记录只解除运行时无路可走的阻塞。

## 证据

- `artifacts/gasp-p11/hurdle-export-02/`
- `artifacts/gasp-p11/hurdle-fbx-inspect.json`
- `artifacts/gasp-hurdle2-2m.log/.err`、`gasp-hurdle2-3m.log/.err`
- `artifacts/gasp-p11-hurdle-package-01.log`
- `artifacts/gasp-p11-pkg-2m.log/.err`、`gasp-p11-pkg-3m.log/.err`
