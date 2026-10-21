# GASP 2 m / 3 m 地面 Mantle/Vault 资产审计

状态：只读资产/根通道审计记录，非当前契约。本轮只导出并检查 FBX 根轨道，
未导入 VL、未运行场景，未验证碰撞、胶囊、手/脚接触、Motion Warping 或视觉表现。

## 结论

在这份 UE 5.8.2 GASP 工程副本的 `/Game/Characters/UEFN_Mannequin/Animations/Traversal`
（78 个 AnimSequence）中，**没有**能支撑地面 2 m / 3 m Mantle 或 Vault 的根运动：

- 所有 med/high Mantle/Vault/Hurdle 的名称都是 `Catch_*`；抽查的 stand 变体
  根轨迹从约 12.8–14.5 m 高空开始下落，不是从地面起跳。
- 唯一从地面开始的 med/high 动作是 `Climb_Start_2_5`（0→250 cm，落顶）；
  它是上攀，不是 Mantle，也没有 Vault 的“过-下”回地面。
- 1 m 地面 `Hurdle_1_0` 有完整 0→100→0 的上-过-下弧线，但高度只有 1 m。
- 1 m `Vault_1_0` 在 0.53 s 到 100 cm 后继续下坠到 -1512.8 cm，是下落/抓边
  路径，不能当地面 Vault；这与既有的 1 m Vault 用 jump+target 的结论一致。

缺的不是另一个 Catch 变体，而是**从地面起跳、非 Catch 的 2 m / 3 m
Mantle/Vault 动画（身体动作 + 根轨迹）**。

## 坐标约定与判定

探针 `vl_animation_probe --fbx` 输出的是原始 FBX 通道；FBX 元数据是
`UpAxis=2`（Z-up）、`FrontAxis=1`、`CoordAxis=0`。VL 的导入基是
`{x, z, -y}`（`source/mesh/loader/animation/assimpSkeletalImporter.cpp`
的 `Vector()`，以及 `source/animation/animationAssets.cpp` 的
`FbxToVlBasis()`），因此本文统一使用：

- **Y = 竖直**（VL 导入后），单位 cm；
- **Z = 前进**（VL 导入后），单位 cm；
- 所有被测 clip 的 root X 恒为 0，表中省略 X。

注意：旧记录里被称为“竖直 Y”的数值，很多是原始 FBX 的前进轴。按 VL 基，
原始 FBX 的 Y 要取反后才是 Z，原始 Z 才是 Y。例如 `Vault_1_0` 原始末帧
`(0, -671.66, -1512.84)` 换算后是 `(Y, Z) = (-1512.84, 671.66)`：
**-1512.84 cm 是竖直下落量，不是水平距离**。

判定口径：

- 地面 Mantle：首帧 Y≈0，随后 Y 抬到 200/300 cm 并停在顶部；
- 地面 Vault：首帧 Y≈0，Y 抬到 ≥200/300 cm，再回落到 ≈0；
- “地面上-过-下”要求这三段都成立；只在空中做上-下不算。

## 命令与产物

导出（输出目录必须不存在；本次两个目录都是首次创建）：

```powershell
$env:VL_GASP_TRAVERSAL_OUTPUT = 'D:\YYBWorkSpace\GitHub\VulkanLearn\artifacts\gasp-medhigh-audit-01'
$env:VL_GASP_TRAVERSAL_CLIPS = 'M_Neutral_Traversal_Catch_Mantle_med_stand,M_Neutral_Traversal_Catch_Vault_med,M_Neutral_Traversal_Catch_Vault_high,M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot,M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot,M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot,M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot,M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot'
& 'C:\Software\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'D:\YYBWorkSpace\GitHub\VulkanLearn\artifacts\gasp-p0\ue-project-20261018\gasp.uproject' `
  '-run=pythonscript' `
  '-script=D:\YYBWorkSpace\GitHub\VulkanLearn\tool\animation\audit_gasp_traversal.py' `
  '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' `
  '-AllowCommandletRendering' '-RenderOffscreen' '-unattended' '-nosound' '-nop4' '-nosplash'
```

对照导出（Hurdle/Cliff 与 1 m 地面 Hurdle 基线；其余参数同上）：

```powershell
$env:VL_GASP_TRAVERSAL_OUTPUT = 'D:\YYBWorkSpace\GitHub\VulkanLearn\artifacts\gasp-medhigh-audit-02'
$env:VL_GASP_TRAVERSAL_CLIPS = 'M_Neutral_Traversal_Catch_Hurdle_low_stand,M_Neutral_Traversal_Catch_Hurdle_med_stand,M_Neutral_Traversal_Catch_Hurdle_high_stand,M_Neutral_Traversal_Catch_Cliff_high_stand,M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot'
```

PowerShell 里 `-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities` 必须整体加引号，
否则逗号会被当成参数分隔符。

逐 FBX 只读探测（每个 `--output` 必须是新 JSON）：

```powershell
$dir = 'D:\YYBWorkSpace\GitHub\VulkanLearn\artifacts\gasp-medhigh-audit-01'  # 02 同样
Get-ChildItem $dir -File -Filter '*.fbx' | ForEach-Object {
  & 'D:\YYBWorkSpace\GitHub\VulkanLearn\build\bin\vl_animation_probe.exe' --fbx $_.FullName `
    --output (Join-Path $dir ('probe-' + $_.BaseName + '.json'))
}
```

根通道解析（`root` 通道；`$vl` 的列依次是 X、Y、Z）：

```powershell
$j = Get-Content $probe -Raw | ConvertFrom-Json
$a = $j.animations[0]
$pos = @(($a.channels | Where-Object { $_.name -eq 'root' }).positions)
$vl = @($pos | ForEach-Object { ,@([double]$_.value[0], [double]$_.value[2], -[double]$_.value[1]) })
$vl[0]; $vl[-1]
$vl | ForEach-Object { $_[1] } | Measure-Object -Minimum -Maximum
$vl | ForEach-Object { $_[2] } | Measure-Object -Minimum -Maximum
```

产物：

- 主审计：`artifacts/gasp-medhigh-audit-01/`（8 个 FBX + `probe-*.json` +
  `traversal-audit.json`）；
- 对照：`artifacts/gasp-medhigh-audit-02/`（5 个 FBX + 同样的探测/审计文件）；
- UE 日志：`artifacts/gasp-medhigh-audit-01.log`、`artifacts/gasp-medhigh-audit-02.log`。
  两次均为 `Success - 0 error(s), 0 warning(s)`；两份 `traversal-audit.json`
  都是 `status=complete`、78 个 clip、0 errors。

## 逐 clip 根通道数据

### med/high 候选（3 个）

| clip | 时长/s | 首帧 (Y, Z) | 末帧 (Y, Z) | Y min/max | Z min/max | 地面上-过-下？ |
| --- | ---: | --- | --- | --- | --- | --- |
| `M_Neutral_Traversal_Catch_Mantle_med_stand` | 3.833 | (1351.0, -468.9) | (300.0, 50.0) | [235.7, 1351.0] | [-468.9, 50.0] | 否（下坠→1.00 s 抓边 2.36 m→1.47 s 到 3.00 m） |
| `M_Neutral_Traversal_Catch_Vault_med` | 3.333 | (1451.0, -468.9) | (-1212.8, 671.7) | [-1212.8, 1451.0] | [-468.9, 671.7] | 否（下坠→1.00 s 3.17 m→1.17 s 4.00 m→继续下坠） |
| `M_Neutral_Traversal_Catch_Vault_high` | 3.833 | (1392.2, -445.4) | (-1192.7, 668.3) | [-1192.7, 1392.2] | [-445.4, 668.3] | 否（下坠→1.00 s 2.59 m→1.83 s 4.00 m→继续下坠） |

### 参照（5 个）

| clip | 时长/s | 首帧 (Y, Z) | 末帧 (Y, Z) | Y min/max | Z min/max | 地面上-过-下？ |
| --- | ---: | --- | --- | --- | --- | --- |
| `M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot` | 3.333 | (0.0, -75.0) | (250.0, 50.0) | [0.0, 250.0] | [-75.0, 50.0] | 否（上-落顶，无下） |
| `M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot` | 4.000 | (0.0, -388.1) | (250.0, 49.1) | [-0.1, 250.4] | [-388.1, 49.2] | 否（上-落顶，无下） |
| `M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot` | 4.233 | (0.0, -354.7) | (250.0, 50.0) | [0.0, 250.0] | [-354.7, 50.0] | 否（上-落顶，无下） |
| `M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot` | 2.000 | (0.0, -118.0) | (100.0, 0.0) | [0.0, 100.0] | [-118.0, 0.0] | 否（上-落顶，1 m） |
| `M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot` | 2.567 | (0.0, -75.0) | (-1512.8, 671.7) | [-1512.8, 100.0] | [-75.0, 671.7] | 否（上到 1 m 后坠到 -15.13 m） |

### 对照（5 个）

| clip | 时长/s | 首帧 (Y, Z) | 末帧 (Y, Z) | Y min/max | Z min/max | 地面上-过-下？ |
| --- | ---: | --- | --- | --- | --- | --- |
| `M_Neutral_Traversal_Catch_Cliff_high_stand` | 5.000 | (1278.8, -445.4) | (300.0, 60.0) | [113.6, 1278.8] | [-445.4, 60.0] | 否（下坠→1.07 s 1.14 m→3.00 m 停顶） |
| `M_Neutral_Traversal_Catch_Hurdle_low_stand` | 4.167 | (1432.9, -454.2) | (300.0, 171.3) | [299.5, 1432.9] | [-454.2, 171.3] | 否（下坠→1.87 s 3.00 m 停顶） |
| `M_Neutral_Traversal_Catch_Hurdle_med_stand` | 4.000 | (1435.8, -433.3) | (300.0, 90.7) | [300.0, 1435.8] | [-433.3, 90.7] | 否（下坠→1.40–1.50 s 4.00 m→1.73 s 3.00 m） |
| `M_Neutral_Traversal_Catch_Hurdle_high_stand` | 4.333 | (1392.2, -445.4) | (300.0, 91.7) | [257.7, 1392.2] | [-445.4, 91.7] | 否（下坠→1.03 s 2.58 m→4.00 m→3.00 m） |
| `M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot` | 1.667 | (0.0, -75.0) | (0.0, 110.0) | [0.0, 100.0] | [-75.0, 110.0] | **是**（0→1.00 m@0.47 s→0@0.83 s，仅 1 m） |

## 关键读数

- `Catch_Mantle_med_stand`：首帧 1351.0 cm，1.00 s 落到 235.7 cm，1.47 s 抬到
  300.0 cm 后停住。整段相对首帧仍是 **-1051.0 cm**；末段只有 **0.64 m**
  的相对抬升，不是从地面抬到 3 m。
- `Catch_Vault_med`：首帧 1451.0 cm，1.00 s 到 316.8 cm，1.17 s 到 400.0 cm，
  之后继续下坠到 -1212.8 cm；整段相对首帧 **-2663.9 cm**，不会回到地面。
- `Catch_Vault_high`：首帧 1392.2 cm，1.00 s 到 259.0 cm，1.83 s 到 400.0 cm，
  末帧 -1192.7 cm；整段相对首帧 **-2584.9 cm**。
- `Climb_Start_2_5` stand/run/walk：首帧 Y=0，分别在 1.50/1.70/2.77 s 到
  250 cm 并停顶；这是 2.5 m 上攀，不是 2 m/3 m Mantle，也没有 Vault 的回地面段。
- `Mantle_1_0`：0→100 cm（1.37 s 到顶），停顶；是有效的 1 m 地面 Mantle 基线。
- `Vault_1_0`：0→100 cm（0.53 s 到顶）→ -1512.8 cm（末帧）；是“上-过-坠”，
  不是地面 Vault。这个数值也说明旧记录里的 1513 cm 是竖直下落，不是水平距离。
- `Hurdle_1_0`：0→100 cm（0.47 s）→ 0（0.83 s），之后沿地面前进到 Z=110；
  是本轮唯一完整的地面上-过-下根轨迹，但只有 1 m。
- `Catch_Hurdle_*` / `Catch_Cliff_high`：同样从 12.8–14.4 m 起跳高度开始，
  末段在 3–4 m 高度上做抓边/上顶或继续下坠；不是从地面起跳的 2/3 m 动作。

补充：`Catch_Mantle_med`、`Catch_Hurdle_*`、`Catch_Cliff_high` 的末帧都是
Y=300.0 cm。这个 300 可能是这些 take 的落点/平台高度，而不是“3 m 高的可攀爬
边缘”；因此本文只用末段相对抬升（0.64 m 等）判断，不把末帧 300 当作 3 m
Mantle 的证据。

## 对 2 m / 3 m 目标的判断

| 目标 | 可用 clip | 结论 |
| --- | --- | --- |
| 2 m 地面 Mantle | 无 | 没有 ground-start、Y 抬到 200 cm 并停顶的 clip |
| 2 m 地面 Vault | 无 | `Vault_1_0` 只到 100 cm 后下坠；Catch 系列不从地面起跳 |
| 3 m 地面 Mantle | 无 | `Catch_Mantle_med` 仅末段 2.36→3.00 m；前置 10 m 下落，不是地面动作 |
| 3 m 地面 Vault | 无 | `Catch_Vault_*` 抓边后继续下坠；`Catch_Hurdle_*` 也不是地面起跳 |
| 2.5 m 地面 Climb | `Climb_Start_2_5` stand/run/walk | 已有，但这是 Climb，不是 Mantle/Vault |

因此，2 m/3 m 的地面 Mantle/Vault 都需要新资产。具体缺的是：

- `Mantle_2_0_*` / `Mantle_3_0_*`（或同义命名）：首帧 Y≈0，根轨迹抬到
  ≈200/300 cm 并停在顶部；水平前进量可按目标 warp，但必须有从地面起的竖直上升；
- `Vault_2_0_*` / `Vault_3_0_*`：首帧 Y≈0，抬到 ≥200/300 cm，再回落到 ≈0；
- 如果团队把 Hurdle 视为 Vault 的替代身体动作，则对应的是
  `Hurdle_2_0_*` / `Hurdle_3_0_*`；当前目录只有 1 m 地面 Hurdle，
  2/3 m 没有 ground 版本。

这些名称在本轮 78 个 Traversal clip 中不存在；med/high 只有 `Catch_*`。

## 推荐下一步

| 方案 | 判断 | 理由 |
| --- | --- | --- |
| 从同一 Traversal 目录再导出 | 不解决 | 78 个 clip 中没有 ground-start 的 2/3 m Mantle/Vault；再导出只会得到同一批 Catch |
| 只给 Catch clip 作者根路径 | 不解决 | 根轨迹可以改，但 Catch 的根前半段是 10 m 下落，名称也是抓边型；仅改根不能把动作变成地面起跳 |
| 制作/引入 ground-start 2/3 m 动作（身体+根） | **首选** | 同时解决身体姿态和根轨迹；导出后重跑本审计即可验证根通道 |
| 程序化 jump+target 扩展 1 m 动作 | 临时兜底 | P8 的 1 m 路线可复用；但 2/3 m 的接触、视觉和根轨迹都未验证，只能标为 gameplay stub，不能声称已支持 2/3 m Mantle/Vault |

如果只能选一个：**先找/做 clip**。在本工程内找不到时，优先在 UE GASP 的其他
版本/目录里找 `Hurdle_2_0` / `Hurdle_3_0`（1 m Hurdle 已证明同族存在完整的
地面上-过-下，只差高度），或用 mocap/手工动画制作同义动作；拿到后仍用
`audit_gasp_traversal.py` 导出并重跑本审计。

短期没有新资产时，可以用现有 1 m 动作做 procedural 扩展，但必须明确：
这是运行时近似，不是 2/3 m Mantle/Vault 的资产验收；不要从 `Catch_*` 的
根轨迹反推地面动作。

## 证据与限制

- 盘点/导出：`artifacts/gasp-medhigh-audit-01/traversal-audit.json`、
  `artifacts/gasp-medhigh-audit-02/traversal-audit.json`（各 78 个 clip，
  `status=complete`，0 errors）；UE 日志见上文。
- 根通道：两个目录下的 `probe-<clip>.json`，共 13 个 clip；原始 FBX 和
  探测 JSON 均保留，未覆盖既有产物。
- 审计 clip 的 UE 元数据均为 `enableRootMotion=true`、
  `rootMotionRootLock=REF_POSE`；导出的 `root` 轨道非常量，说明探测到的是
  作者根轨道，而不是被清零的绑定姿态。
- 本轮**没有**导入 VL、没有运行场景；身体姿态、碰撞/胶囊、手/脚接触、
  Motion Warping、相机与视觉表现都未验证。2/3 m 结论只针对根通道。
- “所有 med/high 都是 Catch”来自 78 个 clip 的名称盘点；根通道只抽查了
  13 个 stand/代表 clip，没有逐一探测全部 run/walk 变体。
- 相关背景：[2.5 m Climb 根运动记录](gasp-p10-climb-root-motion-record.md)、
  [根运动 UE 对照](gasp-root-motion-ue-analysis.md)、
  [动画预检工具契约](../contracts/animation-preflight-tools.md)。
