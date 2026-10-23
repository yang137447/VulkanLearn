# TressFX 5.0 风、阻尼与步长语义校正记录（2026-10-23）

状态：`rationale/`，记录本分支的审计结论、公式修改、资产校准与测量证据，不是契约。
当前行为以 [tressfx-asset-import.md](../contracts/tressfx-asset-import.md) 为准。

## 背景

`codex/tressfx5-hair-port` 的发丝在 Walk 时会被拉成水平片。审计把根因定位到
三处与 UE 5.4 TressFX 插件不一致的语义/单位，以及一个 VL 自加的阵风项：

| # | 旧实现 | TressFX 5.0 参考 | 后果 |
| --- | --- | --- | --- |
| 1 | `damping=0.995` 被当作“每步保留比例”，运行时 `-ln(0.995)=0.00501` | `DampingCoeff = Guides.Damping`，默认 0.035，UI 0..0.07 | 阻尼小 ~7 倍：60 Hz 衰减 0.995 vs `exp(-0.035)=0.966` |
| 2 | `Wind0..3` 四路填同一个向量 | `SetWind` 把风向绕 Y/Z 各 ±40° 生成四个锥角，每路乘 `wM` | 权重和 2 使有效风翻倍且没有锥形变化 |
| 3 | 三叉积前把段向量归一化 | 源实现直接用未归一化的 `v = pos_i - pos_{i+1}` | 丢掉 `|v|²` 量纲；VL 厘米下又少了 `1e-2` 的位移适配 |
| 4 | `windGust`/`windFrequency` 正弦再乘一次 | 没有这一项；时间调制只在 `SetWind` 的 `wM` 里 | 双重调制 |
| 5 | `maxDt=0.033` | `TimeStep = min(dt, 0.016)` | 大步长下长度约束与碰撞更不稳 |

本资产 32 点 guide 的真实段长均值 1.04 cm（p95 1.54 cm，min 0.10 cm，max 3.86 cm），
不是审计假设的 2 cm；这会影响风力与长度约束的数值，后文按实测重算。

## 公式修改

### 阻尼

```text
decay = exp(-dampingCoeff * dt * 60)
dampingCoeff = asset.damping            // 直接使用，不再 -ln
```

`damping` 的 schema 默认值改为 0.035，范围仍为 `[0,1]`（典型 0..0.07），
校验、模型缓存身份、CPU/GPU 参数打包一起迁移。

### 四角风

Host 每帧从单个作者向量展开：

```text
wM  = |wind| * (sin(simFrame * 0.001)^2 + 0.5)
W0  = R_wind * RotY(+40°) * X * wM
W1  = R_wind * RotY(-40°) * X * wM
W2  = R_wind * RotZ(+40°) * X * wM
W3  = R_wind * RotZ(-40°) * X * wM
```

`R_wind` 是从 X 轴到归一化风向的最短弧。参考实现用
`asin(|cross(X, dir)|)` 构造，只覆盖 `dir.x >= 0`；VL 用精确最短弧，对已校准的
`+X` 风完全一致，且不会把 `-X` 风翻到反方向。shader 仍用

```text
w = a*W0 + (1-a)*W1 + a*W2 + (1-a)*W3,  a = (strand % 20)/20
```

权重和为 2 是 TressFX 的既有语义；四路方向不同，混合结果逐 strand 变化。
源实现每帧还有 `(0.3 + FRand())` 的随机幅值抖动；VL 为保证确定性省略该随机项。
`windGust`/`windFrequency` 从 schema、参数和资产中删除，旧资产出现这两个键时
加载期报错。

### 风力

```text
v     = pos_i - pos_{i+1}                        // 未归一化，厘米
force = -cross(cross(v, w), v) * 1e-2            // 厘米/秒²
pos_i += force * dt^2
```

推导：源在米制下位移为 `|v_m|²·|w|·dt²`。`w` 是 TressFX 的 `WindMagnitude`，
属于力标量而非风速，两种单位下取同一个数。令 `v_cm=100·v_m`，三叉积为米结果的
`1e4` 倍；厘米位移是米位移的 100 倍，所以
`100·force_m = 1e-2·cross(cross(v_cm,w),v_cm)`。乘 1e-2 后作者幅值与 UE 的
`WindMagnitude` 数值可直接对照；段长平方量纲必须保留。

### 步长

```text
dt = min(frameDelta, maxDt)      // maxDt ∈ (0, 0.016]
```

`maxDt` 默认 0.016，校验与 schema 上限 0.016；资产只能收紧，不能放宽。
仍是一帧一个 solver step，没有 accumulator / 子步。`simulationFrame` 每步 +1，
只服务 `SetWind` 的 `sin(frame*0.001)²`。

### 长度约束迭代（校准项，非语义修改）

TressFX 的资产 UI 把 `LengthConstraintsIterations` 限制在 1..4，本资产原为 3。
本资产 32 点 guide 的段长只有 0.4–1.5 cm；3 次 Gauss-Seidel 在根点姿态变化后
留下明显拉伸。校准后两个动态资产使用 8 次（solver 公式与偶数/奇数边顺序未改）。
这是 VL 资产参数，不是对 TressFX 公式的修改。

## 校准

### 有效风力量级

对均值段长 1.04 cm（`|v_cm|²≈1.08`），混合风幅值为
`1.53..1.78·wM`，`wM = 0.5..1.5·|wind|`。因此

```text
a_w = |v_cm|² * 1e-2 * |w_eff|          // 厘米/秒²
v_w = a_w * dt / (1 - exp(-0.035*dt*60))
```

`dt=0.016` 时 `1-decay=0.03304`，`v_w ≈ |wind| * (4.0e-3 .. 1.4e-2)` cm/s。
旧值 120 在修正后只有约 0.48–1.68 cm/s，肉眼不可见，必须重新标定。

### 扫描

截图：`SC_tressfx_female_hair_animated.json`，相机
`camera pose 500 165 0 0 151 0`，Walk clip 1 seek 0.4333 + pause，冻结后
继续步进 300 帧；`tonemap 3`、`bloom strength 0`。像素差为与 0 风速同帧
ROI(100:600, 350:900) 内 >4/通道的像素数。

| `wind` (cm/s) | 有效风终速 (cm/s) | idle 像素差 | walk 像素差 | 观察 |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | 自然下垂 |
| 2 | 0.01–0.03 | 6,710 | 1,269 | 与 0 噪声级，不可见 |
| 10 | 0.04–0.14 | 7,029 | 1,187 | 不可见 |
| 50 | 0.2–0.7 | 6,588 | 1,211 | 不可见 |
| 200 | 0.8–2.8 | 6,365 | 1,324 | 极轻微 |
| 1,000 | 4–14 | 6,899 | 1,455 | 自然轻微摆动 |
| **2,000** | **8–28** | **7,159** | **1,506** | **选定：自然摆动，无抬升/片状** |
| 5,000 | 20–70 | 7,985 | 1,735 | 顶部开始外翻 |
| 10,000 | 40–140 | 9,516 | 1,987 | 明显抬升/扇形，过强 |

最终资产值：

```json
"gravity": [0.0, -980.0, 0.0],
"wind": [2000.0, 0.0, 0.0],
"damping": 0.035,
"iterations": 8,
"maxDt": 0.016
```

`SM_female_hair_skinned.json` 与 `SM_female_hair_dynamic.json` 使用同一组值；
`SM_female_hair.json` 没有 `simulation` 块（静态预览），无需迁移。

## 数值探针

`dt=0.016`、`damping=0.035`：

| 量 | 值 |
| --- | --- |
| 每帧衰减 `exp(-0.035*0.016*60)` | **0.96696**（旧映射 0.995） |
| 重力终速 `g*dt/(1-decay)` | **474.5 cm/s = 4.75 m/s**（旧映射约 32.6 m/s） |
| 有效风终速（wind=2,000） | **8–28 cm/s = 0.08–0.28 m/s** |
| 长度比（3 次迭代，旧资产） | max **3.59–3.64**，>1.5 的 strand 2,282–2,563 / 40,334，guide0 **1.62** |
| 长度比（8 次迭代，最终） | max **1.94–2.15**，>1.5 的 strand **49–160**（≤0.4%），guide0 **1.22–1.27** |
| 长度比（16 次迭代对照） | max **1.57–1.77**，>1.5 的 strand 1–5，guide0 **1.14** |

长度比是整根 strand 的当前路径长 / rest 路径长，在 Walk 0.4333 冻结姿态后
继续步进 500 帧测得。8 次迭代把 99.6% 以上的 strand 压到 1.5 以内；最大值出现
在最短的 4.6 cm strand 上。16 次更好，但超过 TressFX UI 的 4 次上限太多，
视觉收益很小，因此选 8。

## 截图与视觉确认

- 旧实现（HEAD `744bae8`，旧资产，seek+pause 后立即截图）：
  `artifacts/audit/before/idle.png`、`walk.png`。Walk 是明显的长水平片。
- 新实现（最终资产，冻结姿态后继续步进 300 帧）：
  `artifacts/audit/final/idle.png`、`walk.png`。Idle 自然下垂；Walk 发丝从头部
  垂下并带轻微风摆，没有水平片。
- 相机、clip/frame 完全一致：Idle clip 0 seek 2.033，Walk clip 1 seek 0.4333，
  `camera pose 500 165 0 0 151 0`，`tonemap 3`，`bloom strength 0`。

**方法说明（重要）**：启动 Console 脚本在同一帧提交所有命令，`animation seek`
会让根点在一帧内跳数十厘米。旧实现和新实现都会在“seek 后第一帧”出现水平拉伸，
这不是 `animation seek` 目标姿态下的稳态。新实现的截图让冻结姿态继续步进 300 帧，
等长度约束把跳变收回；这一步只影响外部截图验证，不改变运行时行为。TressFX 的
`ResetPositions` 仍未接入（见契约的显式差异），所以 seek 跳变不会被瞬间复位。

## 性能

Release、1280×720、`--perf-jsonl`，取窗口内中位数：

| 变体 | hairSimulation | gpuTotal |
| --- | --- | --- |
| before（旧资产，3 次迭代） | 1.707 ms | 7.123 ms |
| after（wind=2k，8 次迭代） | 1.818 ms | 5.721 ms |

`hairSimulation` +0.111 ms（+6.5%），来自长度 pass 8 次迭代；gpuTotal 的差异主要
来自场景/帧状态，不作为本次改动收益。`--perf-jsonl` 的 before 窗口 20 行、
after 窗口 17 个有效行。

## 验证

- Debug 构建、validation 开启：hair 场景跑 14 s，stderr **0 字节**，日志无 `VUID`。
- 模块测试：`hair_asset_tests` **161/161**、`vl_renderer_backend_tests` 104/104、
  `editor_ui_tests` 761 通过 + 5 跳过、`material_schema_tests` 29/29、
  `animation_preflight_tests` 292/292。
- Vulkan runtime gates（串行、单独进程）：
  `--shader-reload-test` 退出码 0、stderr 空；
  `--shader-compute-reload-test` 退出码 0（stderr 只有预期的 ABI 拒绝诊断，
  无 VUID）；`--world-graph-transaction-test` 退出码 0、stderr 空。
- 文档校验与架构审计通过；`git diff --check` 无空白错误；无残留 `main.exe` 进程。
