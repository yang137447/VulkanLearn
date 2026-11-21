# 发丝杆求解器实验记录

> 状态：**实验记录**，不是契约，也不代表运行时能力。记录假设、代码、参数、原始输出和失败结论；原始输出保存在 `artifacts/`，不覆盖旧记录。理论推导见知识库 `rendering/topics/hair-solver/`。

## 记录约定

- 实验编号：`M0-00x`；M0 指位置 + 拉伸 + 弯曲 + 标量扭转的 CPU 验证版。
- 每条记录必须写：日期、假设、代码路径、构建/运行命令、参数、原始输出路径、结论、限制、下一步。
- 失败结论与成功同样保留；不得因为“后来改了参数”删除旧输出。
- 不把实验记录写成当前运行时契约；只有进入 `source/` 并经契约/测试验收后才升级。

## M0-001：raw fixed-point rest 反解的条件数

**日期**：2026-11-14

**假设**：在位置 + 拉伸 + 弯曲 + 标量扭转的 XPBD 链上，直接对 rest 做固定 `ω` 迭代

```text
r ← r + ω (x* − F_r(x*))
```

可以在少量迭代内让作者造型成为静态平衡态。

**代码**：`tool/hair/rodM0Prototype.cpp`（CPU，只依赖 Eigen；不进入运行时）。

**构建**：

```powershell
cmake -S . -B build
cmake --build build --target vl_hair_rod_m0_prototype -j 8
```

**运行**：

```powershell
build\bin\vl_hair_rod_m0_prototype.exe --mode position --omega 0.5 --iterations 20 --settle 120
build\bin\vl_hair_rod_m0_prototype.exe --mode position --omega 0.02 --iterations 20 --settle 120
build\bin\vl_hair_rod_m0_prototype.exe --mode bend --omega 0.5 --iterations 20 --settle 120
```

**参数**：32 点螺旋（半径 3、高度 20、1 圈）；`EA=1e4`、`EI=100`、`GJ=100`、`g=-9.8`、`dt=1/240`、`substeps=4`、XPBD 迭代 8、`rootFixed=2`、阻尼 0.002、每轮稳态 120 步、反解 20 轮、rest twist 步长 0.02。归一化单位，不是正式资产参数。

**原始输出**：

- `artifacts/hair-rod-m0-20261114/position-w05.log`
- `artifacts/hair-rod-m0-20261114/position-w002.log`
- `artifacts/hair-rod-m0-20261114/bend-w05.log`

**结果**：

| 参数化 | ω | 20 轮残差 | 最终 sag-free 发梢偏差 |
| --- | ---: | --- | ---: |
| 基线（无 sag-free） | — | 0.903253 | 0.90325 |
| rest 位置 | 0.5 | 0.903 → 7.137（中途多次反弹，整体发散） | 3.36283 |
| rest 位置 | 0.02 | 0.903 → 0.899511（几乎不动） | 0.89936 |
| rest 曲率 | 0.5 | 0.903 → 0.922282（缓慢变差） | 0.92404 |

**结论**：在当前参数化下 `I − ωJ_F` 不收缩。固定 `ω` 的 raw fixed-point 不是可用算法；把 `ω` 调小只会让残差停在平台，调大则发散。这个失败与推导页“只靠投影非扩张不足以证明收敛”的警告一致。

**限制**：

- 原型用标量扭转、没有完整四元数材料框架与剪切耦合；它只能验证 rest 反解的条件数，不能代表完整 M0。
- 螺旋、归一化参数、固定碰撞缺失；没有测试碰撞 active set、摩擦或发束接触。
- 每轮稳态只跑 120 步，未证明已到速度归零；结论只针对这个离散流程。

**下一步**：

1. 在 N=8 小链上做数值 Jacobian，估计 `J_F` 的谱，确认是负特征值、近奇异还是参数化问题。
2. 用阻尼 Newton / Levenberg–Marquardt + 线搜索替代固定 `ω`，记录每轮 `R_x`、`R_θ`、段长/扭转残差。
3. 收敛后再扩到 N=32、2k guides；最后才接 GPU。

## M0-002：静态残差 + Newton/LM（rest 位置参数化）

**日期**：2026-11-14

**假设**：不用“跑 200 步后的位移”做残差，而用零速度、一次 Step、除以 `dt²` 的净加速度作为静态残差

```text
R(r) = (x_after_one_step - x*) / dt²
```

再对 rest 位置做数值 Jacobian + LM + 线搜索。

**代码**：同一 `tool/hair/rodM0Prototype.cpp`，`--mode newton`；N=8，`rootFixed=2`，settle 200，12 轮，`lambda=1e2`。

**原始输出**：`artifacts/hair-rod-m0-20261114/newton-static-n8.log`

**结果**：残差 23.912553 → 21.702519；线搜索 alpha 从 0.5 降到 0.004；`J^T J` 奇异值范围约 `[2.6e-2, 7.7e3]`（条件数 ~3e5）。最终 settle 200 步的发梢偏差 2.13826。

**结论**：静态残差比“长跑位移”干净，但 rest 位置参数化仍有近奇异方向；LM 只能把残差压到平台，不能收敛到零。

## M0-003：静态残差 + Newton/LM（rest 弯曲参数化）

**日期**：2026-11-14

**假设**：重力塌陷主要是弯曲模式，只解 rest bend 向量，rest 长度保持不变，应该比 rest 位置条件更好。

**代码**：同一目标，`--mode newton` 改为更新 `Rest::bend`；N=8，未知数 18，100 轮，`lambda=1e2`。

**原始输出**：

- `artifacts/hair-rod-m0-20261114/newton-static-bend-n8.log`（12 轮）
- `artifacts/hair-rod-m0-20261114/newton-static-bend-n8-100.log`（100 轮）

**结果**：12 轮 23.912553 → 20.873812；100 轮最终停在 **6.905477**，线搜索 alpha 降到 0.004–0.125，`J^T J` 最小奇异值降到 `1e-7` 量级，出现明显零空间。最终 settle 200 步的发梢偏差 **25.93315**，比基线更差。

**结论**：只解 rest 弯曲不能表达这个平衡态。静态残差里至少还有 rest 长度/边界/整体模式没有被参数化覆盖；继续加 Newton 迭代只会停在平台，甚至把长跑结果弄坏。

**共同限制**：原型是标量扭转、无碰撞、单根 N=8 螺旋；静态残差取“一次 XPBD Step 的净位移/dt²”，可能包含约束投影顺序的伪影。结论只说明这个原型/参数化不可用，不等于完整 Cosserat 模型无解。

**下一步（修正方向）**：

1. 从约束梯度/力直接构造静态残差，而不是从一次 XPBD 投影反推；把拉伸、弯曲、扭转、碰撞的力/力矩分开对账。
2. 参数化改为 rest 长度 + rest 弯曲角 + rest 扭转，并显式处理根边界和整体刚体模式；先做秩分析，确认零空间来自哪里。
3. 用 SVD/截断伪逆 + Tikhonov 正则做 Newton，报告 rank、条件数和每类残差；再决定是否需要路线 B 的完整四元数耦合。

## 记录边界

M0-001/002/003/004/005 都是**失败或部分记录**，不是可交付算法。它们排除了“固定 ω”“rest 位置 Newton”“仅 rest 弯曲 Newton”“3-DOF bend Newton”“力平衡即稳定”五条捷径，并把下一步收敛到 2-DOF 物理弯曲、gauge 剔除、Hessian 正定与信赖域。

## M0-005：2-DOF bend + rest 长度 + SVD 截断伪逆

**日期**：2026-11-14

**假设**：把 rest bend 限制在切线垂直平面的 2 DOF，加入 rest 长度作为未知量；用 SVD 截断伪逆求力平衡，应该能同时消除规范零空间并收敛。

**代码**：`tool/hair/rodM0Prototype.cpp --mode svd`；N=8，未知数 19（7 段长 + 6 关节×2），方程 18。

**原始输出**：

- `artifacts/hair-rod-m0-20261114/svd-n8-null3.log`
- `artifacts/hair-rod-m0-20261114/svd-n8-hessian.log`

**结果**：

- 力残差 24.004999 → 0.378561 → 0.000010 → 0（3 轮，alpha=1）。
- 秩 18/18（19 未知数，1 个零空间方向）。
- 零空间方向是**首段 rest 长度**（两端都在 `rootFixed=2` 内，不可观）；迭代后混入首关节 rest bend，但仍是不可观组合。
- 最终 settle 200 步：发梢偏差 0.02314，但点 2/3/4 偏差 1.3923/0.7520/0.3809。
- 静态 Hessian（对可动位置）：最小特征值 **−8.947877e+03**，最大 9.466359e+01。

**结论**：SVD 找到了**力平衡点**，但它是不稳定鞍点，不是稳定平衡。力残差为零只是必要条件；Hessian 必须正定（至少非负）才能让长跑不发散。近根点的漂移与负特征值一致。

**下一步**：

1. 从未知量中剔除两端都固定的首段 rest 长度（以及任何不可观组合），让参数化满秩。
2. 把 Hessian 正定作为验收条件；在 Newton/LM 的候选解上检查 `λ_min > 0`，不能只看力残差。
3. 加 Tikhonov/信赖域，优先选择靠近作者 rest 且不压缩根段的解；如果约束模型本身在该造型下不存在稳定解，必须改约束/材料模型，而不是继续调 SVD 截断。

## M0-004：解析约束力残差 + rest bend Newton

**日期**：2026-11-14

**假设**：直接从弯曲约束力与重力构造静态残差，不再从一次 XPBD 投影反推：

```text
G(b0) = Σ F_bend(x*; b0) + m g
```

只解 rest bend 的 3 个分量，N=8，未知数 18，方程 18。

**代码**：同一目标，`--mode energy`；`lambda=1e-2`，12 轮。

**原始输出**：`artifacts/hair-rod-m0-20261114/energy-newton-n8.log`

**结果**：24.004999 → 21.031786（第一轮），随后完全停住；`J^T J` 最小奇异值为 **0**，最大约 22.93；最终 settle 200 步发梢偏差 2.10702。

**结论**：3 分量 rest bend 参数化秩亏。物理弯曲只有 2 个自由度（垂直于切线的平面），第三个分量是规范/不可观方向；解析力残差也说明 bend-only 的列空间覆盖不了全部力残差。下一步必须：

1. 把 rest bend 投影到垂直平面，使用 2 DOF/关节；
2. 把 rest 长度（以及后续 rest twist）加入未知量，检查列空间是否满秩；
3. 对 `J` 做 SVD/截断伪逆，报告 rank 与零空间方向，而不是继续加迭代。

## M0-006：gauge 剔除 + 静态 Hessian 稳定性门禁

> 2026-11-14 复核更正：本节的 XPBD 200 步数字、vector `C_ss` 的 PD
> 结论、旧 torque 与 rest quaternion proxy 已在下方「M0-006 复核更正」
> 节降级；当前证据以 `m0-006-corrected-*` 和 M0-007 为准。

**日期**：2026-11-14

**假设**：M0-005 的力平衡点是不稳定鞍点，原因之一是首段固定边的 rest 长度
进入了未知量。把两端都固定的 rest 长度当作 gauge 固定掉，并强制检查
可动位置上的静态 Hessian：

```text
G(r) = F_stretch(x*; r) + F_bend(x*; r) + m g
λ_min(H(r)) > 0,  H = -∂G/∂x 在可动位置、x = x* 处
```

只有同时满足 `G = 0` 与 `λ_min(H) > 0` 的 rest 才算稳定解；否则明确报告
鞍点/无稳定解，不把力残差为零当作成功。

**代码**：`tool/hair/rodM0Prototype.cpp`，新增 `--mode m0-006`；
保留 M0-001–005 的旧入口。构建：

```powershell
cmake --build build --target vl_hair_rod_m0_prototype -j 8
```

运行（六组）：

```powershell
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene straight --points 8 --settle 200
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene straight --points 32 --settle 200
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene helix --points 8 --settle 200
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene helix --points 32 --settle 200
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene initial-bend --points 8 --settle 200
build\bin\vl_hair_rod_m0_prototype.exe --mode m0-006 --scene initial-bend --points 32 --settle 200
```

**参数**：`EA=1e4`、`EI=100`、`GJ=100`、`g=-9.8`、`dt=1/240`、
`substeps=4`、XPBD 迭代 8、`rootFixed=2`、阻尼 0.002、每轮 settle 200 步。
归一化单位，不是正式资产参数。场景：

- `straight`：竖直直链，总长 20，根在顶端，前两点固定；重力沿链轴。
- `helix`：半径 3、高度 20、1 圈的螺旋，与 M0-001–005 相同。
- `initial-bend`：平面圆弧，总长 20、总转角 π/2，根在原点，初始切向 +x，
  向下弯曲；前两点固定。

### 1. gauge 剔除与秩分析

把两端都在 `rootFixed=2` 内的边（本组只有边 0）从 rest 长度未知量中剔除，
固定为作者长度。其余参数化保持不变：每条可动边的 rest 长度 + 每个关节
2 个 rest bend 分量（`along`/`binormal`）。

| N | 未知数 | 方程 | 参考 rank | 候选 rank | 最小/最大奇异值（除以 \|g\|） | 条件数 |
| ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 8 | 18 | 18 | 18/18 | 18/18 | 4.57e-2 / 1.90e3（helix） | 4.16e4 |
| 32 | 90 | 90 | 90/90 | 90/90 | 5.15e-2 / 2.03e3（helix） | 3.95e4 |

机器精度阈值与 `1e-6·σ_max` 阈值下都满秩；六组参考/候选的最小奇异值在
`4.6e-2`–`9.1e-2`（已除以 `|g|`），条件数约 `2.2e4`–`4.2e4`。
M0-005 中“首段 rest 长度 + 首关节 rest bend”混合的零空间方向消失，
说明该方向主要来自固定边 gauge；剔除后没有发现新的结构性零空间。

### 2. 稳定性处理

静态 Hessian 使用可动位置的中央差分：

```text
H_ij = -(G_i(x* + h e_j) - G_i(x* - h e_j)) / (2h)
H ← (H + H^T) / 2
```

`h = 1e-5·L_segment`；`λ_min(H) > 0` 才算通过。M0-005 打印的是
`∂G/∂x`，其最小特征值为 `-8.947877e3`；物理势能 Hessian 是
`H = -∂G/∂x`，同一个 helix 鞍点的 `λ_min(H) = -9.466498e1`。
两者都说明不定，但数值不能跨符号约定直接比较；不稳定模态的量级是
约 `9.5e1`，不是 `8.95e3`。

求解流程：

1. 解析 bend 梯度 + 阻尼 Newton/LM（Tikhonov 对角项、信赖域、线搜索）。
2. 重力连续：`λ` 从 0 到 1，预测-校正；每一步都检查 `λ_min(H)`。
3. 从作者 rest 直接做 `λ=1` Newton，作为交叉检查。
4. 刚度连续：在 `λ=1` 下把 `EI` 从 `1e5` 降到目标 `100`，跟踪同一条
   力平衡分支；每一步检查 `λ_min(H)`，寻找高刚度稳定分支能否连到目标。
5. 只在候选 `λ_min(H) > 0` 时接受；否则保留鞍点证据并报告失败。

扭转不进入位置 Hessian：原型里的 `phi` 是独立标量状态，rest twist 也不在
位置未知量中；本次没有计算角向 Hessian，因此不声称扭转稳定。

### 3. 结果

`G_norm = ||G||∞/(m|g|)`，`m=1`；`L` 是作者总长度。200 步偏差是
第 200 步结束时的最大点偏差（`/L`）；段长拉伸是 200 步内的最大值。

| 场景 | N | `G_norm` | `λ_min(H)` | 200 步最大偏差 | 发梢偏差 | 段长拉伸 | 首次失稳 | 刚度路径失稳 | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| straight | 8 | 4.80e-11 | **+7.93e-1** | 8.45e-6 L | 8.01e-6 L | 0.59% | 无 | — | **PASS** |
| straight | 32 | 1.67e-13 | **+7.34e-1** | 2.25e-4 L | 2.14e-4 L | 3.07% | 无 | — | **PASS** |
| helix | 8 | 5.47e-11 | **−9.47e1** | 5.15e-2 L | 8.55e-4 L | 1.40% | λ=0.05 | EI=3.125e3 | FAIL |
| helix | 32 | 1.86e-13 | **−2.27e3** | 3.65e-1 L | 3.60e-1 L | 24.2% | λ=0.05 | EI=6.25e3 | FAIL |
| initial-bend | 8 | 5.22e-12 | **−4.41e0** | 2.61e-3 L | 2.99e-4 L | 0.28% | λ=0.65625 | EI=1.953125e2 | FAIL |
| initial-bend | 32 | 1.86e-13 | **−3.37e1** | 4.61e-1 L | 4.61e-1 L | 42.3% | λ=0.125 | EI=7.8125e2 | FAIL |

补充诊断（候选 rest）：

| 场景 | N | `max \|dL\|/L` | `max \|db\|` | `max \|b_rest\|` | 候选来源 |
| --- | ---: | ---: | ---: | ---: | --- |
| straight | 8 | 5.85e-3 | 2.6e-23 | 2.6e-23 | 重力连续 |
| straight | 32 | 2.86e-2 | 5.5e-32 | 5.5e-32 | 重力连续 |
| helix | 8 | 4.37e-3 | 1.43e1 | 1.38e1 | 重力连续（鞍点） |
| helix | 32 | 2.19e-2 | 1.37e1 | 1.36e1 | 重力连续（鞍点） |
| initial-bend | 8 | 2.76e-3 | 1.23e1 | 1.21e1 | 重力连续（鞍点） |
| initial-bend | 32 | 1.10e-2 | 1.50e1 | 1.50e1 | 重力连续（鞍点） |

**解读**：

- **直链**：去除 gauge 后，rest 长度只做小幅调整（N=8 约 0.58%，
  N=32 约 2.86%），rest bend 保持为零；Hessian 正定，200 步偏差和发梢
  偏差都远低于门槛。竖直链的重力沿链轴，拉伸张力提供稳定刚度，这是本次
  唯一通过全部门槛的场景。
- **helix**：力残差可以压到 `1e-13` 量级，但根是鞍点。重力连续在
  `λ=0.05` 就失稳；从高刚度稳定分支降 `EI` 也在 `3.1e3`（N=8）/
  `6.25e3`（N=32）失稳，远高于目标 `EI=100`。从作者 rest 直接 Newton
  得到同一个鞍点。N=8 的 200 步最大偏差 `5.15e-2 L` 略高于 `0.05L`，
  N=32 则明显发散（`0.365L`，段长拉伸 24%）。
- **initial-bend**：N=8 是“动态门槛通过、稳定性门槛失败”的近例：
  200 步最大偏差 `2.6e-3L`、发梢 `3e-4L`、段长拉伸 0.28%，但
  `λ_min(H)=-4.41`，仍是鞍点，不能算成功。刚度连续在 `EI≈195` 失稳，
  目标 `100` 已在其下方；N=32 在 `λ=0.125`、`EI≈781` 失稳并长跑发散。
- **rest bend 量级**：目标 `EI` 下的力平衡根需要 `max |b_rest|≈12–15`。
  物理 cross product `t0×t1` 的模长不超过 1；这个 rest bend 不对应任何
  可实现的相邻切向组合。也就是说，失败不仅是 Hessian 负定，力平衡根
  本身还超出了当前 2-DOF bend 参数化的物理范围。

### 4. 结论

在“2-DOF bend + rest 长度”当前模型和这组归一化材料参数下：

- gauge 剔除成功，力平衡系统在参考点和候选点都满秩，M0-005 的固定边
  零空间不再出现。
- 直链存在稳定的 sag-free rest，六项门槛全部通过。
- helix 和 initial-bend **没有找到正定解**。重力连续、作者 rest 直接
  Newton、刚度连续三条路径都只得到鞍点；helix N=8/32 与
  initial-bend N=32 的长跑也失败，initial-bend N=8 只是动态指标碰巧
  通过。结论按任务要求明确记为“模型下无稳定解（已追踪分支）”，
  不把力残差为零的鞍点当作成功。

**限制**：

- 分支搜索不是全局证明；只覆盖重力连续、直接 Newton 和 EI 刚度连续
  三条路径，可能存在未找到的孤立稳定根。
- 静态残差是解析约束力，不是 XPBD 固定点残差；200 步 XPBD 是独立
  验证。Hessian 正定是解析平衡稳定的必要条件，不自动保证离散 XPBD
  固定点稳定。
- 扭转/角向 Hessian 未纳入；没有碰撞、摩擦、发束间接触；单根 guide、
  归一化单位。
- rest bend 没有限制在 `|b|≤1` 的物理范围；本次只是把超界量作为诊断
  证据。若要继续，应先决定是否对 rest bend 加物理约束或改用完整材料
  框架，再重做力平衡与稳定性搜索。

**下一步**：

1. 对 rest bend 加 `|b|≤1`（或改用旋转向量/材料框架）后重跑；如果
   有界模型连力平衡都不存在，就应修改约束/材料模型，而不是继续调
   SVD 截断或 Newton 迭代。
2. 把直链的稳定分支作为 M1（碰撞/扭转）基线；helix/arc 需要更高
   `EI`、不同预弯分布或额外自由度才可能稳定。
3. 在运行时实现前固定 Hessian 符号约定（`H=-∂G/∂x`），并用本记录的
   直链/helix 数值做回归。

**原始输出**：

- `artifacts/hair-rod-m0-20261114/m0-006-straight-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-straight-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-helix-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-helix-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-initial-bend-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-initial-bend-n32.log`

## M0-006 补充：论文口径修正、额外自由度、向量 C_ss 与不可行判定

**日期**：2026-11-14

**触发**：论文核验线给出三条直接影响 M0-006 的结论：离散旋转定义必须在
段中心（Kugelstadt/Hsu），不能把节点上的 `2 log(q⁻¹q)/L̄` 写成论文原式；
向量约束 `C_ss` 的柔度必须是 `diag(1/(GA₁L), 1/(GA₂L), 1/(EA L))`，
不能把标量 `α_s=L/EA` 塞进向量约束；求解顺序应至少对比单向/反向/交错。
此外，Hsu 的 sag-free 还要求零力矩、单位 rest 四元数与翻转裕量，单看
`G(r)=0` 不够。

本补充不删除上一节的标量结果，而是把它标记为“节点标量 stretch 模型”的
证据；下面所有新结论都绑定具体模型和日志。

### 1. 口径修正

- **离散定义**：本原型仍是节点位置 + 节点切向的小角度模型。3-DOF rest
  bend 的第三个分量是作者切平面的切向/扭转分量，不是段中心四元数。
  Kugelstadt 2016 的段中心形式是
  `Ω_s = (2/L̄_s) Im(q̄_s q_{s+1})`；Hsu 用完整四元数
  `2 log(q_s⁻¹ q_{s+1})/L̄_s`；Bergou 用 `2 tan(φ/2)`。本记录不声称
  节点公式与它们等价。
- 若保留节点四元数，必须显式写映射：段中心
  `c_s=(x_s+x_{s+1})/2`，段中心四元数 `q_s` 用
  `slerp(q_s,q_{s+1},1/2)`（或相对旋转中点）得到，
  `L̄_s=(L_s+L_{s+1})/2`。本轮没有实现这个段中心四元数变体；它是下一步，
  不是当前数值结论的一部分。
- **柔度**：标量约束 `C_s=|e|-L` 的 `α_s=L/EA` 只对
  `C_s` 成立。向量 `C_ss` 必须用
  `diag(1/(GA L), 1/(GA L), 1/(EA L))`。日志中的数值检查：
  helix N=8 的 `L=3.865288`，轴向 `1/(EA L)=2.587e-5`，
  标量 `L/EA=3.865e-4`（比值 `6.69e-2`），剪切
  `1/(GA L)=6.727e-5`。本轮新增了正确柔度的向量 `C_ss`
  静态力/Hessian 和向量 XPBD。
- **求解顺序**：新增 `forward` / `reverse` / `interleaved` 三种扫描；
  `interleaved` 是先正向逐约束、再反向逐约束的双边交错。

### 2. 新增测试

- 2-DOF rest bend + rest 长度（标量 stretch）：上一节基线。
- 3-DOF rest bend + rest 长度：第三分量作为小角度切向/扭转 rest 分量，
  使 N=8 的 `J` 从 18×18 变为 18×24（零空间 6），N=32 为 90×120
  （零空间 30）。这是“额外自由度/rest twist”的最小扩展。
- 多初值：N=8 每组 8 次（helix 3-DOF vector 另跑 16 次），N=32 每组
  4 次；长度扰动 ±3%，bend 扰动 ±0.3，固定种子。
- 曲率修正 QP：对负模态方向 `v` 用有限差分求
  `∂(vᵀHv)/∂r`，投影到 `ker J` 后沿负梯度走一步，再重解力平衡。
  2-DOF 方阵的零空间为空，数值上 `‖g_null‖≈1.2e-8`，所以
  `Δr=0`；3-DOF 的零空间非空，`Δr` 可以非零。
- 向量 `C_ss`：静态力/Hessian 与 XPBD 都使用
  `diag(1/(GA L),1/(GA L),1/(EA L))`。
- 力矩/quaternion：每关节弯曲力矩残差（绕关节中心），
  rest quaternion `q=(1,b/2)` 归一化前的模长误差、旋转角与
  `π-angle` 翻转裕量。

### 3. 结果

**表 A：2-DOF + 标量 stretch（基线，N=8/32）**

| 场景 | N | `G_norm` | `λ_min(H)` | 200 步最大偏差 | 发梢 | 段长拉伸 | `max \|b_rest\|` | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| straight | 8 | 1.41e-13 | +7.93e-1 | 8.45e-6 L | 8.01e-6 L | 0.59% | 3.6e-17 | PASS |
| straight | 32 | 1.68e-13 | +7.34e-1 | 2.25e-4 L | 2.14e-4 L | 3.07% | 3.3e-19 | PASS |
| helix | 8 | 7.56e-14 | −9.47e1 | 5.15e-2 L | 8.55e-4 L | 1.40% | 1.38e1 | FAIL |
| helix | 32 | 1.44e-13 | −2.27e3 | 3.65e-1 L | 3.60e-1 L | 24.2% | 1.36e1 | FAIL |
| initial-bend | 8 | 7.00e-14 | −4.41e0 | 2.61e-3 L | 2.99e-4 L | 0.28% | 1.21e1 | FAIL |
| initial-bend | 32 | 2.68e-13 | −3.37e1 | 4.61e-1 L | 4.61e-1 L | 42.3% | 1.50e1 | FAIL |

标量 2-DOF 的 helix/initial-bend 根是鞍点；initial-bend N=8 的动态指标
碰巧通过，但 `λ_min(H)<0` 仍判失败。2-DOF 方阵的曲率修正
`‖g_null‖≈1.2e-8`，只能得到 `Δr≈0`，与“满秩方阵无法在保持
力平衡的同时修曲率”的理论判断一致。

**表 B：3-DOF + 向量 C_ss（N=8/32）**

| 场景 | N | `G_norm` | `λ_min(H)` | 200 步最大偏差 | 发梢 | 段长拉伸 | `max \|b_rest\|` | 数值判定 | 物理判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| straight | 8 | 2.45e-13 | +7.87e1 | 4.88e-14 L | 4.88e-14 L | 0.59% | 2.77e-1 | PASS | PASS |
| straight | 32 | 4.33e-13 | +1.62e1 | 2.63e-13 L | 1.37e-13 L | 2.94% | 2.92e-1 | PASS | PASS |
| helix | 8 | 2.71e-11 | +6.05e1 | 7.95e-6 L | 7.95e-6 L | 0.44% | 5.91e0 | PASS | FAIL |
| helix | 32 | 1.47e-13 | +1.20e1 | 3.58e-4 L | 3.58e-4 L | 2.15% | 7.82e0 | PASS | FAIL |
| initial-bend | 8 | 1.46e-13 | +7.81e1 | 6.45e-6 L | 3.13e-6 L | 0.28% | 1.21e1 | PASS | FAIL |
| initial-bend | 32 | 3.23e-13 | +1.53e1 | 8.47e-4 L | 8.31e-4 L | 1.23% | 1.50e1 | PASS | FAIL |

向量 `C_ss` 是正确的分水岭：同样的力平衡根，加上剪切柔度后
`λ_min(H)` 从负变正，200 步 XPBD 偏差也降到 `1e-13`–`1e-3 L` 量级。
但非直链的 PD 根需要 `max |b_rest|=5.8–15.0`，远超物理
`|t0×t1|≤1`；直链只需要 `0.28–0.29`，所以只有直链物理可行。

**2-DOF + 向量 C_ss 对照（N=8）**

| 场景 | `λ_min(H)` | 200 步最大偏差 | `max \|b_rest\|` | 数值 | 物理 |
| --- | ---: | ---: | ---: | --- | --- |
| helix | +6.01e1 | 1.22e-5 L | 1.38e1 | PASS | FAIL |
| initial-bend | +7.81e1 | 6.45e-6 L | 1.21e1 | PASS | FAIL |

2-DOF 在向量 `C_ss` 下也能正定；3-DOF 把 helix 的 rest bend 从
`1.38e1` 降到 `5.91e0`，但 initial-bend 仍约 `1.21e1`。多初值没有
找到 `|b|≤1` 的正定根：helix N=8 的 16 次全部 PD 但
`max |b|=5.66–6.10`；initial-bend N=8 的 8 次全部同一个根
`max |b|=12.11`；N=32 的 4 次也全部 unphysical。

### 4. 负模态能量分解

`v` 是 `H` 的最小特征向量（`|v|=1`）；`vᵀHv` 分解为拉伸、弯曲、
重力。直接重力 Hessian 为零；重力通过预应变/几何刚度进入弹性项。

| 模型/场景 | 总 `vᵀHv` | 拉伸 | 弯曲 | 重力 | 拉伸 material/geometric | 弯曲 material/geometric |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| 2-DOF helix N=8 | −9.466e1 | −2.492e1 | −6.975e1 | 0 | +6.35e-1 / −2.555e1 | +1.342e1 / −8.316e1 |
| 2-DOF helix N=32 | −2.271e3 | −8.275e2 | −1.444e3 | 0 | — | — |
| 2-DOF initial-bend N=8 | −4.407e0 | +1.117e1 | −1.558e1 | 0 | — | — |
| 2-DOF initial-bend N=32 | −3.371e1 | +2.438e1 | −5.809e1 | 0 | — | — |
| 3-DOF helix N=8 | −5.578e1 | −2.094e1 | −3.483e1 | 0 | +8.85e-2 / −2.103e1 | +1.076e1 / −4.559e1 |

helix 的负能量约 26–38% 来自拉伸、62–74% 来自弯曲，直接重力 0；
几何项是负的，material 项是正的。initial-bend 的拉伸项为正、
弯曲项更负，净值为负。这个分解说明负模态不是“重力 Hessian 为负”，
而是预应变带来的几何刚度超过了 material 刚度；向量 `C_ss` 的剪切
material 刚度正好补上了这个缺口。

### 5. 力矩、rest quaternion 与扫描顺序

- 2-DOF 标量 helix N=8：每关节弯曲力矩残差 `3.096e2`
  （归一化 `1.168`，joint 0）；rest quaternion 最大角 `2.854 rad`
  （163.5°），翻转裕量 `0.288 rad`（16.5°）。
- 3-DOF 向量 helix N=8：力矩 `1.413e2`（归一化 `0.533`）；
  最大角 `2.472 rad`（141.6°），翻转裕量 `0.670 rad`（38.4°）。
- 3-DOF 向量 initial-bend N=32：力矩 `2.328e3`
  （归一化 `11.88`）；最大角 `2.876 rad`（164.8°），翻转裕量
  `0.265 rad`（15.2°）。
- 直链 3-DOF 向量 N=32：力矩 `7.4e-18`，最大角 `0.290 rad`，
  翻转裕量 `2.851 rad`，`max |b|=0.292`。

即使力残差压到 `1e-13`，弯曲力矩残差仍可到 `1e2–1e3`；`G(r)=0`
确实只覆盖力平衡，不覆盖零力矩。非直链的 rest quaternion 已接近
翻转边界，和 `max |b|` 超界一致。

扫描顺序（forward/reverse/interleaved）对同一 rest 的 200 步结果差异
小于约 1%：3-DOF 向量 helix N=8 为 `7.952e-6 / 7.956e-6 / 7.963e-6 L`；
2-DOF 标量 helix N=8 为 `5.146e-2 / 5.132e-2 / 5.165e-2 L`。
扫描顺序不能把标量鞍点救回来，向量模型的数值通过也不是靠某一个
扫描顺序。

### 6. 结论

- 标量 stretch 的 2-DOF 模型：helix/initial-bend **无正定解**；
  方阵满秩，曲率 QP 只能 `Δr=0`。这是上一节结论的严格化。
- 正确的向量 `C_ss`（`diag(1/(GA L),1/(GA L),1/(EA L))`）会改变
  稳定性：所有测试场景的 `λ_min(H)` 都变正，向量 XPBD 的 200 步
  数值门槛也通过。因此“负 Hessian 不可救”不能归因于“2-DOF 本身”，
  而是标量长度约束缺少剪切刚度。
- 但正定根是非物理的：helix/initial-bend 需要
  `max |b_rest|=5.8–15.0`，超过 `|t0×t1|≤1`；多初值没有找到
  `|b|≤1` 的 PD 根；力矩残差和 rest quaternion 角也显示同一个问题。
  所以按“物理可实现的 rest”判定，当前节点 2-DOF/3-DOF bend +
  向量 `C_ss` 模型在 helix/initial-bend 造型下**不可行**；直链可行。
- 这不是继续调 SVD 截断或 Newton 迭代能解决的问题：需要段中心
  四元数、单位 rest quaternion、零力矩约束和翻转裕量，或对有界
  rest bend 重新定义材料/约束模型。

### 7. 下一步

1. 实现段中心四元数变体：`c_s=(x_s+x_{s+1})/2`，
   `Ω_s=(2/L̄_s) Im(q̄_s q_{s+1})`（Kugelstadt）或
   `2 log(q_s⁻¹q_{s+1})/L̄_s`（Hsu），显式写 `q_i→q_s` 与
   `L̄_s` 索引；同时检查单位 rest quaternion 和翻转裕量。
2. 对 rest bend/rest quaternion 加物理边界（`|b|≤1` 或单位四元数），
   求力+力矩联合平衡；如果无解，优先改材料/约束或作者造型，
   而不是继续调数值截断。
3. 段中心模型通过后，再把向量 `C_ss` 柔度和扫描顺序移到运行时；
   当前节点原型的数值通过不能直接当作运行时能力。

**原始输出**：

- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-straight-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-straight-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-helix-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-helix-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-initial-bend-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-scalar-2dof-initial-bend-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-straight-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-straight-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-helix-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-helix-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-initial-bend-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-3dof-initial-bend-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-2dof-helix-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-006-vector-2dof-initial-bend-n8.log`

## M0-006 复核更正：代码级不一致与结论降级

**日期**：2026-11-14

独立复核发现 M0-006 的 XPBD 与静态模型不是同一离散模型，以下旧结论必须降级。
修正后的日志统一放在 `m0-006-corrected-*`，旧日志保留但不再作为当前证据。

### 1. 修正项

1. `Step` 原先把 `alphaStretch/alphaBend/alphaTwist` 全部写成
   `rest.lengths.front()`，而静态 `StretchForceAt`/`BendForceAt` 用各自局部
   长度。已改为每条约束使用 `rest.lengths[i]`。
2. 向量 `C_ss` 原先用固定的作者 rest frame，整根刚体旋转会被误判成横向
   应变。已改为 co-rotated frame：
   `R_current = Q(t_auth, t_current) R_auth`；静态力、向量 XPBD 和
   material Hessian 都使用同一 `R_current`。
3. `applyBend` 原先对 3-vector 约束用标量分母，而静态力用精确梯度。已改为
   3×3 分母 `α̃ I + Σ G_a^T G_a`，用 `Δλ = -denominator⁻¹ (C + α̃λ)`。
4. `MaxBendTorqueResidual` 原先跳过固定点反力，最坏关节总是 joint 0。
   已改为包含关节三点（含固定点）的完整弯曲力矩。修正后：
   helix N=8 最大力矩 `6.43e1`（归一化 `2.43e-1`，joint 2），
   helix N=32 `7.55e1`（`2.80e-1`，joint 9）；
   initial-bend N=8/32 约 `1e-13`（joint 1）。
   旧记录的 `1e2–1e3` 和 joint 0 不再有效。
5. `PrintRestQuaternion` 的 `(1,b/2)` 不是真单位 material quaternion；
   已改名为“rest quaternion proxy”，只作小角度诊断，不参与验收。
   M0-007 使用真正的单位 rest quaternion。

### 2. 降级结论

- **撤回“2-DOF/3-DOF vector C_ss 数值 PD”**。co-rotated 修正后：
  - 2-DOF scalar：helix N=8 `λ_min=-9.466e1`，N=32 `-2.271e3`；
    initial-bend N=8 `-4.407`，N=32 `-3.371e1`。
  - 3-DOF vector（co-rotated）：helix N=8 `-5.522e1`，N=32 `-2.965e3`；
    initial-bend N=8 `-4.394`，N=32 `-3.371e1`。
  旧 `+6.01e1/+7.81e1` 是固定 rest frame 的伪稳定；只有直链仍 PD
  （scalar N=8/32 `+7.93e-1/+7.34e-1`，vector 3-DOF `+7.88e-1/+6.99e-1`）。
- **旧 200 步数字不可比**。修正后 scalar 2-DOF：
  helix N=8 `3.49e-4 L`（tip `5.91e-6`，stretch `0.435%`），
  initial-bend N=8 `4.62e-5 L`（tip `3.19e-6`，stretch `0.277%`）；
  这两个 N=8 动态指标其实通过，但静态 Hessian 仍为负，结论仍是 FAIL。
  helix N=32 `0.351 L`、initial-bend N=32 `0.436 L` 仍明显失败。
  旧 helix N=8 `0.0515 L` / initial-bend N=8 `0.0026 L` 不再引用。
- **旧力矩结论按形状拆分**：initial-bend 的完整关节力矩约 `1e-13`，
  所以“force-only 连力矩也不平衡”对 initial-bend 撤回；helix 仍有
  `6.4e1–7.6e1` 的非零弯曲力矩，“力平衡 ≠ 完整力矩平衡”只对 helix 成立。
- **旧 rest quaternion flip margin 只是 proxy**，不能作为真单位四元数证据。
- **仍然成立**：scalar static Hessian 对 helix/initial-bend 为负；直链通过；
  Hessian 必须是独立验收门禁，长跑偏差小不能替代正定性。
- **M0-005 的动态数字也降级**：同一个 SVD 静态解在修正 XPBD 下
  `svd final max=0.00943`、`tip=0.00016`；旧 `1.39227/0.02314` 和
  点 2/3/4 的 `1.39/0.75/0.38` 主要来自全局长度/标量弯曲分母的离散
  不一致，不再作为“失稳”证据。静态 Hessian 仍为负
  （旧约定 `∂G/∂x` 最小 `-8.947877e3`；`H=-∂G/∂x` 最小 `-94.66`），
  所以“力平衡点是鞍点”仍成立，但长跑漂移论据撤回。

### 3. 修正后日志

- `artifacts/hair-rod-m0-20261114/m0-006-corrected-scalar-2dof-{straight,helix,initial-bend}-n{8,32}.log`
- `artifacts/hair-rod-m0-20261114/m0-006-corrected-vector-3dof-{straight,helix,initial-bend}-n{8,32}.log`

## M0-007：段中心材料四元数 + rest twist

**日期**：2026-11-14

**代码**：`tool/hair/rodM0SegmentCenter.cpp`；CMake target
`vl_hair_rod_m0_segment_center`。M0-006 的节点小角度原型不再承担这条结论。

### 1. 模型

- 节点位置 `x_i` 保留用于拉伸/剪切与重力；每段中心
  `c_s=(x_s+x_{s+1})/2`，段中心材料四元数 `q_s` 为单位四元数。
- `C_ss,s = R_s^T (x_{s+1}-x_s)/L_s - e3`，使用当前材料框架
  `R_s=R(q_s)`；柔度 `diag(1/(GA L),1/(GA L),1/(EA L))`。
- 关节 `s` 连接段 `s` 与 `s+1`；`r_s=q_s⁻¹q_{s+1}`，
  `r_s.w<0` 取 `-r_s`，`θ_s=2 atan2(|r_s.v|,r_s.w)`，
  `ω_s=θ_s axis_s/L̄_s`，其中
  `L̄_s=(L_s^0+L_{s+1}^0)/2`（rest 长度平均，索引为相邻段 `s,s+1`）。
- 弯曲/扭转能量用 `ω_s-ω_s^0`，`ω_s^0` 来自单位 rest relative
  quaternion；rest twist 是 rest rotation vector 的第三分量。
- 角速度恢复使用同一 log：`ω=(2/h)Log(q_old⁻¹q_new)`，不使用 Im 近似。

### 2. 反解与验收

- 未知量：rest 长度（S）+ 单位 rest relative rotation（3(S-1)）+
  target segment quaternion 扰动（3(S-1)）；target 只做小正则，
  rest quaternion 用 `Exp(v)` 构造，天然单位。
- 物理域：`|v|≤π-0.1`（flip margin≥0.1）；rest 长度改变≤5%；
  target shear≤0.2 rad（报告并作为附加门禁）。
- 求解拆成两个精确解耦的 block：
  - force block：target q + rest 长度；
  - torque block：rest rotation，目标 `q` 与 rest 长度固定。
  因为 C_b 使用 rest `L̄`，rest rotation 不改变作者状态的力残差。
- 验收：force、每关节 torque、rest Jacobian rank/condition、
  完整位置+角向 reduced Hessian `λ_min>0`、200 步偏差、发梢、
  段长拉伸、flip margin、target shear、无 NaN。

### 3. 结果

`force`/`torque` 分别按 `m|g|` 与 `m|g|L` 归一化；200 步与 tip
按 `L` 归一化；`rest dL` 为 `max |L_rest/L_auth-1|`；
`flip` 为 `min(π-|v|)`。

| 场景 | N | force | torque | `λ_min(H)` | 200 步最大 | tip | stretch | rest dL | flip | torque rank / left-null | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| straight | 8 | 4.49e-8 | 4.29e-9 | +6.30e-2 | 3.81e-11 L | 3.66e-11 L | 0.59% | 0.59% | 3.1416 | 18/18 / 2.6e-22 | **PASS** |
| straight | 32 | 4.50e-7 | 2.01e-8 | +2.62e-1 | 8.66e-9 L | 8.66e-9 L | 2.94% | 2.86% | 3.1412 | 90/90 / 1.1e-20 | **PASS** |
| helix | 8 | 1.95e-9 | 2.27e-1 | −1.825e1 | 5.11e-2 L | 5.11e-2 L | 0.46% | 0.44% | 0.1000 | 15/18 / 1.53e0 | FAIL |
| helix | 32 | 2.20e-7 | 1.72e-1 | −6.489e1 | 2.94e-2 L | 2.60e-2 L | 2.12% | 2.16% | 0.1000 | 87/90 / 1.68e0 | FAIL |
| initial-bend | 8 | 8.14e-10 | 3.69e-1 | −6.788e0 | 6.52e-2 L | 5.08e-2 L | 0.49% | 0.28% | 0.1000 | 17/18 / 4.25e0 | FAIL |
| initial-bend | 32 | 9.06e-7 | 1.02e-1 | −4.804e1 | 1.08e-1 L | 1.08e-1 L | 1.64% | 1.12% | 0.1000 | 89/90 / 5.59e0 | FAIL |

补充：

- Force block 在 2–8 次迭代内收敛到 `1e-9–1e-6`，所以“力平衡可达”
  成立。target shear 只有 `1.0e-2–7.6e-2 rad`，rest 长度改变≤2.9%，
  不是失败原因。
- Torque block 是瓶颈：helix N=8 rank 15/18、N=32 87/90；
  initial-bend N=32 rank 89/90，且 left-null residual 等于全部 torque
  residual（5.59），说明该 torque 残差完全不在 rest rotation 的列空间里。
  initial-bend N=8 的某些初值 rank 18/18，但 rest angle 已到
  `3.0416 rad`（flip margin 0.1），解被物理域截住。
- 非直链的 rest angle 全部到达 `3.0416 rad`，flip margin 正好 `0.1 rad`；
  不需要再放大 rest rotation，物理域已经用满。

### 4. 最低模态能量分解

`v` 是最低特征向量；`vᵀHv` 按 stretch/shear/bend/twist/gravity 分解。
直接重力 Hessian 为零，重力通过预应变/几何项进入。

| 场景 | 总 `vᵀHv` | stretch | shear | bend | twist | gravity | 占比（stretch/shear/bend/twist/gravity） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| helix N=8 | −1.825e1 | −1.183e1 | −7.153e0 | +5.79e-1 | +1.52e-1 | ~0 | −64.8% / −39.2% / +3.2% / +0.8% / 0 |
| helix N=32 | −6.489e1 | −3.175e1 | +6.76e0 | −7.82e0 | −3.207e1 | ~0 | −48.9% / +10.4% / −12.1% / −49.4% / 0 |
| initial-bend N=8 | −6.788e0 | +2.52e0 | −1.135e1 | +4.09e-1 | +1.63e0 | ~0 | +37.2% / −167.2% / +6.0% / +24.0% / 0 |
| initial-bend N=32 | −4.804e1 | +1.53e0 | −2.32e0 | −7.963e1 | +3.239e1 | 0 | +3.2% / −4.8% / −165.8% / +67.4% / 0 |

helix N=8 的负模态以 stretch/shear 几何项为主；initial-bend N=32
以 bend 几何项为主，twist 正项部分抵消。直链的最低模态是小的正
twist 刚度（`λ_min=+6.3e-2/+2.6e-1`），不是失稳。

### 5. 判定

- **直链 N=8/32：物理可行且稳定，全部门槛通过。**
- **helix/initial-bend N=8/32：当前段中心四元数 + rest twist 模型下不可行。**
  证据不是“力平衡找不到”，而是：
  1. force block 可收敛到 `1e-9–1e-6`；
  2. torque block 秩亏/左零空间残差非零，或 rest angle 撞到
     `π-0.1` 的物理 flip 边界；
  3. 非直链 reduced Hessian `λ_min<0`；
  4. 动态 200 步/tip 多数超门槛。
- 因此不能再用“加 SVD 截断/加迭代”解决；需要改材料/约束模型或改作者
  造型。当前可交付基线只有直链。M0-007 没有用非物理 rest 冒充成功。

### 6. 下一步

1. 若要保留 helix/initial-bend，需要改模型：例如允许更强的 shear/twist
   耦合、改用不同弯曲能量、或把 target frame 也作为艺术可调量；
   不能靠放大 flip margin 或使用 5.8–15 的非物理 rest。
2. 直链的段中心四元数稳定分支可作为 M1（碰撞/扭转）基线。
3. 运行时应采用 co-rotated `C_ss`、局部长度柔度、3×3 弯曲乘子和完整
   力矩检查；M0-006 旧实现不得直接移植。

### 7. 原始输出

- `artifacts/hair-rod-m0-20261114/m0-007-straight-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-007-straight-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-007-helix-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-007-helix-n32.log`
- `artifacts/hair-rod-m0-20261114/m0-007-initial-bend-n8.log`
- `artifacts/hair-rod-m0-20261114/m0-007-initial-bend-n32.log`

## M0-008：clump/centroid 约束的投影分析与可行性

**日期**：2026-11-14

**代码**：`tool/hair/rodM0SegmentCenter.cpp` 的 M0-008 模式：
`--clump-stiffness`、`--clump-group`、`--clump-whole`、
`--clump-projection`、`--cross-guides`。M0-007 的段中心四元数模型保持不变。

### 1. 模型与理论

对一组点 `G`，动态质心 `c=(1/|G|)Σ x_j`，rest 距离
`d_i=|x_i^0-c^0|`，约束

```text
C_i = |x_i - c| - d_i
U_c = 1/2 k Σ_i C_i^2
J_i = ∂C_i/∂x
```

在 `C=0` 处 `K_c=Σ k_i J_i^T J_i ⪰ 0`，只能加正刚度。对 M0-007 的
负模态 `v`：

```text
λ_new = λ0 + k ||J_c v||²
k_req = -λ0 / ||J_c v||²
```

若 `||J_c v||²≈0`，clump 对该模态完全无效。单 guide 整个 group 的
位置空间 rank=`N`，movable nullity=`2N-6`（加上 6 个刚体模式为
`2N`）。跨 guide 的对应点 group 仍有大量 nullity；整体刚体平移/旋转
和同步模态不能由 centroid 距离约束消除。clump 也不能平衡整根 guide
的净重力，不能替代接触/摩擦。

**rest 距离全部由作者造型派生**：`d_i=|x_i^0-c^0|`，没有新增 rest
变量，也没有用 5.8–15 的非物理 rest。权重为 group 内均匀权重。

### 2. 投影分析

`λ0` 是 M0-007 的 negative/lowest eigenvalue；表中为
`|J_c v|² / k_req`。`k_req` 单位与 clump stiffness 相同；工具用
`1e6` 作为“可实现”参考上限（仅用于分类，不是物理定论）。

**单 guide 内部分组**

| 场景 | N | λ0 | group4 | group8 | whole |
| --- | ---: | ---: | --- | --- | --- |
| helix | 8 | −1.825e1 | 1.244e-1 / 146.7 | 3.250e-1 / 56.2 | 3.250e-1 / 56.2 |
| helix | 32 | −6.660e1 | 3.638e-4 / 1.831e5 | 9.112e-3 / 7.309e3 | 4.537e-2 / 1.468e3 |
| initial-bend | 8 | −6.862e0 | 2.43e-11 / 2.823e11 | 4.714e-10 / 1.456e10 | 4.714e-10 / 1.456e10 |
| initial-bend | 32 | −4.804e1 | 2.874e-12 / 1.672e13 | 7.435e-11 / 6.461e11 | 1.477e-9 / 3.253e10 |

**跨 guide 对应点分组**（M=2/3，间距 0.3）

| 场景 | N | cross2 sync | cross2 anti | cross3 sync | cross3 anti |
| --- | ---: | --- | --- | --- | --- |
| helix | 8 | 0 / inf | 3.808e-1 / 47.9 | 3.5e-23 / inf | 1.692e-1 / 107.8 |
| helix | 32 | 1.0e-21 / inf | 1.159e-1 / 574.9 | 1.8e-20 / inf | 5.149e-2 / 1293 |
| initial-bend | 8 | 0 / inf | 4.593e-1 / 14.94 | 1.1e-21 / inf | 2.041e-1 / 33.62 |
| initial-bend | 32 | 0 / inf | 1.443e-1 / 332.9 | 1.0e-19 / inf | 6.412e-2 / 749.1 |

**rank/nullity**：

- 单 guide whole：rank `N`（8/32），movable nullity `2N-6`（10/58）；
  加上 6 个刚体模式为 `2N`（16/64）。
- 跨 guide 实测：helix N=8 M2 rank9/null27、M3 rank17/null37；
  helix N=32 M2 rank53/null127、M3 rank84/null186；
  initial-bend N=8 M2 rank6/null30、M3 rank15/null39；
  initial-bend N=32 M2 rank30/null150、M3 rank81/null189。
- **跨 guide 的 synchronous 模态投影精确为 0**；它在 cross-clump
  nullspace 内，任何 k 都不能改变该模态。

### 3. 单 guide clump 实算 Hessian

理论预测 original mode 可被 `k_req` 压正，但完整 Hessian 还可能有
clump nullspace 内的其他负模态。实算结果（`clump share` 为最低模态的
`k||J_c v||²`）：

| 场景 | N | 配置 | k | λ_min | clump share | 200 步 | tip | 判定 |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| straight | 8 | whole | 1e3 | +6.304e-2 | 3.3e-14 | 1.54e-11 L | 1.29e-11 L | **PASS** |
| straight | 32 | whole | 1e3 | +2.624e-1 | 4.7e-11 | 8.62e-9 L | 8.62e-9 L | **PASS** |
| helix | 8 | group4 | 1e3 | −1.522e1 | 8.95e-1 (5.9%) | 5.11e-2 L | 5.04e-2 L | FAIL |
| helix | 8 | whole | 1e6 | −1.444e1 | 1.19e-3 (0.0%) | 1.74e-2 L | 1.74e-2 L | FAIL |
| helix | 32 | whole | 1e4 | −3.100e1 | 7.96e-1 (2.6%) | 1.19e-2 L | 1.19e-2 L | FAIL |
| helix | 32 | whole | 1e6 | −3.146e1 | 1.08e-2 (0.0%) | 1.19e-2 L | 1.19e-2 L | FAIL |
| initial-bend | 8 | whole | 1e6 | −6.862e0 | 2.8e-6 (0.0%) | 2.87e-4 L | 2.77e-4 L | FAIL |
| initial-bend | 32 | whole | 1e6 | −4.804e1 | 6.3e-8 (0.0%) | 2.83e-2 L | 2.83e-2 L | FAIL |

结论：helix N=8 的 **original mode** k_req 只有 56–147，确实可被
clump 加刚度；但 k=1e3/1e6 时最低模态已经换成一个 clump-nullspace
模态（share≈0），λ_min 仍为负。helix N=32 类似，k=1e4 时 original
mode 被压下去，但新的 nullspace 模态在 −31 附近；k=1e6 仍在 −31.46。
initial-bend 的 original mode 本身就在 clump nullspace（k_req>1e10），
clump 完全无效。单 guide clump **不能**把弯曲造型救成全正定。

### 4. 跨 guide clump 扫描

跨 guide clump 的 Hessian 只作用于节点位置，不含 quaternion torque。
扫描 `k=1e1…1e8`：

| 场景 | N | M | λ_min 范围 | 稳定 k |
| --- | ---: | ---: | ---: | --- |
| helix | 8 | 2 | −1.825202e1（恒定） | 无 |
| helix | 8 | 3 | −1.825202e1…−1.825216e1 | 无 |
| helix | 32 | 2 | −6.660027e1（恒定） | 无 |
| initial-bend | 8 | 2 | −6.862256e0（恒定） | 无 |
| initial-bend | 8 | 3 | −6.862256e0…−6.862403e0 | 无 |
| initial-bend | 32 | 2 | −4.803750e1（恒定） | 无 |

`λ_min` 不随 k 改变，说明最低模态就是跨 guide 的 **synchronous mode**，
它在 `J_c` 的 nullspace 中；anti-synchronous 模态虽然有有限 k_req，
但 synchronous 模态始终存在。因此跨 guide clump 也不能使整束 PD。

### 5. 判定与成本

- **直链 N=8/32**：clump 残差、力/力矩、Hessian、200 步全部通过；
  clump 不是必需项。straight 的 `λ_min` 本身为正，clump share≈0。
- **helix/initial-bend**：单 guide clump 不足；跨 guide clump 无法
  消除 synchronous 负模态；torque block 仍然秩亏/撞 flip 边界；
  `clump` 不产生 quaternion torque。最终状态：
  **NULLSPACE / NO_EXTERNAL_SUPPORT**。
- 需要的不是继续增大 clump stiffness，而是接触/外部支撑或修改
  材料/约束模型（例如给整束提供外部反力矩、改弯曲/扭转能量、或
  改作者造型）。不应放宽 `λ_min>0`、torque、flip margin 等物理门槛。
- 成本：单 guide whole 增加 `N` 个标量约束；跨 guide M 根增加
  `M*N` 个标量约束。单 guide Hessian 仍是 `P+Q`（36/180）；
  跨 guide Hessian 为 `M*(P+Q)`（M=2/3 时 72/108、360/540）。
  跨 guide Hessian 构造用 `2*M*P` 次 clump 梯度评估，再做 8 个
  stiffness 特征值扫描；它没有引入新的 rest 变量。

### 6. 原始输出

- `artifacts/hair-rod-m0-20261114/m0-008-projection-<scene>-n<N>.log`
- `artifacts/hair-rod-m0-20261114/m0-008-clump-<scene>-n<N>-<tag>.log`
- `artifacts/hair-rod-m0-20261114/m0-008-cross-<scene>-n<N>-m<M>.log`
- `artifacts/hair-rod-m0-20261114/m0-008-summary.log`

## M0-009：多 guide 接触/摩擦网络的投影与可行性

**日期**：2026-11-14

**代码**：`tool/hair/rodM0SegmentCenter.cpp` 的
`--contact-projection` 模式（M0-007 段中心四元数模型 + 2–3 根 guide）。

### 1. 接触模型

- 2–3 根相邻 guide 由作者造型的平移副本生成，间距 `0.3`；
  每根 guide 的半径 `0.2`，半径和 `R=0.4`。
- 接触对是**跨 guide** 的对应点最近点对，**不做单 guide 自接触**。
  接触对几何、rest 法向和 rest 切向都来自作者 rest；不新增
  rest 变量，也没有非物理 rest。
- 单边法向：`C_n=max(0,R-d)`；`d=0.3<R=0.4`，rest 穿透
  `C_n=0.1`，法向刚度 `k_n=1e4`（柔度 `1e-4`），rest 法向力
  `1e3`。
- 摩擦：stick 近似，切向相对位移相对作者 rest 切向偏移；
  `k_t=1e4`（柔度 `1e-4`），`mu=0.3`，Coulomb 上限
  `mu*F_n=3e2`（rest 时切向力为 0，未触发上限）。接触/friction
  残差分别就是 `max C_n` 与 `max |C_t|`。

### 2. 投影分析

与 M0-008 同一公式：

```text
λ_new = λ0 + k ||J_c v||²
k_req = -λ0 / ||J_c v||²
```

同步模态（每根 guide 施加同一个 M0-007 negative mode）：
任何两自由 guide 之间的法向/摩擦约束都是相对约束，
`J_c v_sync = 0`。因此同步模态在接触网络中完全不可观。

表中 `λ0` 为 M0-007 negative eigenvalue；投影为
`|Jv|² / k_req`。`straight` 的 `λ0>0`，接触不需要。

| 场景 | N | M | normal rank/nullity | friction rank/nullity | combined rank/nullity | sync `|Jn v|²` | sync `|Jt v|²` | anti `|Jn v|²/k_n` | anti `|Jt v|²/k_t` |
| --- | ---: | ---: | --- | --- | --- | ---: | ---: | --- | --- |
| helix | 8 | 2 | 8/36 | 16/36 | 24/36 | 0 | 0 | 385.2 / 0.047 | 195.9 / 0.093 |
| helix | 8 | 3 | 16/54 | 32/54 | 48/54 | 0 | 0 | 770.4 / 0.024 | 434.5 / 0.042 |
| helix | 32 | 2 | 31/180 | 64/180 | 95/180 | 0 | 0 | 0.232 / 287 | 0.395 / 169 |
| helix | 32 | 3 | 64/270 | 128/270 | 192/270 | 0 | 0 | 0.463 / 144 | 0.790 / 84.3 |
| initial-bend | 8 | 2 | 8/36 | 16/36 | 24/36 | 0 | 0 | 0.919 / 7.47 | 4.3e-9 / 1.61e9 |
| initial-bend | 8 | 3 | 16/54 | 32/54 | 48/54 | 0 | 0 | 1.837 / 3.74 | 3.4e-4 / 2.05e4 |
| initial-bend | 32 | 2 | 32/180 | 64/180 | 96/180 | 0 | 0 | 0.289 / 167 | 3.1e-8 / 1.53e9 |
| initial-bend | 32 | 3 | 64/270 | 128/270 | 192/270 | 0 | 0 | 0.577 / 83.2 | 6.3e-8 / 7.65e8 |

- 所有同步投影都精确为 0；`k_n`/`k_t` 为 inf，接触法向/摩擦
  对同步模态完全无效。
- anti-synchronous 相对模态有有限 `k_req`（helix 法向很小，
  initial-bend 法向也较小；initial-bend 的切向 k_req 常超过
  `1e6`），但这不能消除同步模态。
- 接触图的 nullity 很大：normal `(M-1)*N` 量级的约束只覆盖相对
  运动；combined 仍有 12/6/85/78 等 nullity，包含同步模态。

### 3. 判定

- **直链 N=8/32**：`λ0>0`，接触不需要；已有 clump/无 clump 结果
  通过。
- **helix/initial-bend**：跨 guide 法向/摩擦网络不能稳定同步
  negative mode。`NO_EXTERNAL_SUPPORT` 仍成立。
- 这不是“再增大 k_n/k_t”能解决的问题：同步模态在接触 Jacobian
  的 nullspace 中，增大刚度只会增加相对模态刚度。需要**固定碰撞体/
  外部支撑**、外部反力矩，或改连续体/MPM 模型；不能放宽
  `λ_min>0`、torque、flip margin 等物理门槛。
- 额外成本：`(M-1)*N` 个法向约束、`2*(M-1)*N` 个切向 stick
  约束、0 个新变量；多 guide Hessian DOF 为 `M*(P+Q)`
  （N=8 M=2/3 为 72/108，N=32 M=2/3 为 360/540）。
  接触图构造不需要新增 rest。

### 4. 原始输出

- `artifacts/hair-rod-m0-20261114/m0-009-contact-<scene>-n<N>-m<M>.log`
- `artifacts/hair-rod-m0-20261114/m0-009-summary.log`

## M0-010：保形 vs 动态策略基准

**日期**：2026-11-14

**代码**：`tool/hair/rodM0SegmentCenter.cpp` 的 `--benchmark`
模式。同一 M0-007 段中心四元数 rest，每个 scene/N 只反解一次；
tether/clump 只作为外部保形项，不改变作者态反解。

### 1. 基准设置

- 策略：A 无 tether baseline；B 全局 tether `k_g=1/10/100/1000`；
  C 局部 tether stride 3/4/8，`k_l=1/10/100/1000`；
  D 已有 clump（whole `k=1e3/1e6`）；E 全局+局部混合
  （10/10、100/100，stride 4）。
- 静态：先重力+策略 settle 600 步（2.5s），测
  `Dtip/L`、`Dmax/L`、质心漂移/L、包围盒高度比、点云 RMS。
  建议门槛：`Dtip<0.10`、`Dmax<0.15`、质心`<0.05L`、
  高度比`>0.8`。
- 动态：从静态形状起施加横向风 `20 m/s²` 持续 0.5s，再恢复
  0.5s；测最大偏移/L、回正时间、残余偏移/L。可见动态门槛
  `maxDyn>0.005L`；`recovery=-1` 表示 0.5s 内未回正。
- 数值：NaN、段长拉伸（静态+动态最大）`<10%`；不再要求
  `λ_min>0`。tether/clump 是外部保形项，不是新的物理 sag-free rest。
  contact 仍按 M0-009：同步模态 nullspace，`NO_EXTERNAL_SUPPORT`，
  未做完整 contact 动态。

### 2. 选出的 Pareto 候选

完整 17 策略/刚度表在 `m0-010-<scene>-n<N>.log`；下表为每个
scene/N 的代表候选（`Dtip/Dmax/centroid` 与 `dyn` 均按 L 归一化）。

| 场景 | N | 策略 | 约束 | Dtip | Dmax | centroid | height | RMS | dyn | recovery | residual | stretch | 说明 |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| straight | 8 | baseline | 0 | 0 | 0 | 0 | 1.000 | 0 | 0.1217 | -1 | 0.1217 | 0.0092 | 已保形，无 tether |
| straight | 32 | baseline | 0 | 0 | 0 | 0 | 1.000 | 0 | 0.1224 | -1 | 0.1224 | 0.0425 | 已保形，无 tether |
| helix | 8 | global-10 | 6 | 0.0334 | 0.0373 | 0.0319 | 0.955 | 0.0336 | 0.0537 | -1 | 0.0362 | 0.0020 | 动态明显 |
| helix | 8 | global-100 | 6 | 0.0032 | 0.0036 | 0.0029 | 0.996 | 0.0032 | 0.0118 | 0.138 | 0.0012 | 0.0019 | 回正最好 |
| helix | 8 | mixed-10 | 8 | 0.0301 | 0.0316 | 0.0247 | 0.960 | 0.0271 | 0.0522 | -1 | 0.0332 | 0.0025 | 保形/动态折中 |
| helix | 8 | clump-1e6 | 8 | 0.0721 | 0.0721 | 0.0483 | 0.996 | 0.0524 | 0.0266 | -1 | 0.0266 | 0.0008 | 已有策略 |
| helix | 32 | local4-100 | 8 | 0.0182 | 0.0931 | 0.0250 | 0.982 | 0.0490 | 0.0739 | -1 | 0.0504 | 0.0465 | 最小局部 |
| helix | 32 | local3-1000 | 10 | 0.0064 | 0.0199 | 0.0031 | 0.994 | 0.0091 | 0.0476 | -1 | 0.0476 | 0.0957 | 保形好，stretch 接近上限 |
| helix | 32 | mixed-10 | 38 | 0.0272 | 0.0898 | 0.0410 | 0.970 | 0.0541 | 0.0693 | -1 | 0.0649 | 0.0219 | 动态明显 |
| helix | 32 | global-1000 | 30 | 0.0001 | 0.0031 | 0.0002 | 1.000 | 0.0011 | 0.0095 | -1 | 0.0095 | 0.0181 | 近刚体保形 |
| initial-bend | 8 | local3-1000 | 2 | 0.0713 | 0.0713 | 0.0376 | 1.014 | 0.0493 | 0.0992 | -1 | 0.0991 | 0.0101 | 最少约束 |
| initial-bend | 8 | global-100 | 6 | 0.0035 | 0.0049 | 0.0040 | 1.005 | 0.0041 | 0.0155 | 0.158 | 0.0016 | 0.0025 | 回正最好 |
| initial-bend | 8 | clump-1e6 | 8 | 0.0003 | 0.0003 | 0.0002 | 1.000 | 0.0003 | 0.1101 | -1 | 0.1100 | 0.0107 | 已完美保形且动态明显 |
| initial-bend | 8 | mixed-10 | 8 | 0.0291 | 0.0401 | 0.0303 | 1.040 | 0.0312 | 0.0674 | -1 | 0.0404 | 0.0042 | 折中 |
| initial-bend | 32 | global-1000 | 30 | 0.0024 | 0.0024 | 0.0001 | 1.000 | 0.0006 | 0.0084 | -1 | 0.0084 | 0.0198 | 唯一 shape+stretch 通过 |

### 3. 策略结论

- **直链 N=8/32**：M0-007 已经保形；baseline（0 约束）是自然
  Pareto 点。tether 只降低风响应，不解决不存在的失稳。
- **helix N=8**：`global-10` 保留强动态（dyn 0.054），
  `global-100` 回正最快（0.138s），`mixed-10` 是 8 约束折中，
  `clump-1e6` 是已有策略中保形最好的（Dmax 0.072，dyn 0.027）。
- **helix N=32**：`local4-100` 只需 8 约束且动态明显
  （dyn 0.074）；`local3-1000` 保形更好（Dmax 0.020）
  但 stretch 9.57% 接近 10%；`mixed-10` 动态 0.069 但 38 约束；
  `global-1000` 是近刚体保形（dyn 0.0095）。
- **initial-bend N=8**：`local3-1000` 只需 2 约束就 shape pass
  且 dyn 0.099；`global-100` 回正 0.158s；`clump-1e6` 近乎
  完美保形（Dmax 0.0003）且 dyn 0.110，是已有策略最佳。
- **initial-bend N=32**：只有 `global-1000` 同时通过形态与
  `<10%` stretch（dyn 0.0084 刚超过可见门槛，已接近刚体）。
  其他策略要么形态失败，要么 stretch 超限：global-100
  `27.4%`、local3-1000 `25.2%`、mixed-100 `27.6%`、
  clump-1e6 `29.5%`。
- tether/clump 是**外部保形项**，不是物理 sag-free rest；
  M0-007 的 torque block 问题仍在。contact 仍
  `NO_EXTERNAL_SUPPORT`。若要求物理可实现，仍需接触支撑/
  连续体模型；tether 只适合作为参数化/制作期保形手段。

### 4. 成本

- 约束数：global `N-2`（6/30），local stride 3/4/8 为
  `2/8/10`（N=8/32 对应），mixed 为 `8/38`，clump whole
  为 `8/32`。
- 每步时间：N=8 约 `0.4 ms`，N=32 约 `9–10 ms`（当前
  finite-difference gradient 主导；tether/clump 本身不增加
  状态 DOF）。整套 benchmark 每 scene/N 约 `0.5–2 min`
  （N=32 更慢）。
- 没有新增非物理 rest；tether/clump 只是外部能量项。

### 5. 原始输出

- `artifacts/hair-rod-m0-20261114/m0-010-<scene>-n<N>.log`
- `artifacts/hair-rod-m0-20261114/m0-010-summary.log`

## M0-011：软核心 + 渐进三次项（authored rest）

**日期**：2026-11-14

**代码**：`tool/hair/rodM0SegmentCenter.cpp` 的
`--benchmark-deadzone` 模式。每个 scene/N 直接用**作者曲线**作为
rod rest（不做 sag-free 反解），tether 承担重力偏移。

### 1. 为什么不再用 M0-010 的 N=32 结论

M0-010 的 N=32 结果使用 `SolveBlockInverse` 的 sag-free rest。复跑
发现该反解在 helix N=32 只有 rank 78/90、initial-bend N=32 只有
rank 89/90，rest angle 顶到 `π−0.1` 的 flip 边界；0.5s 窗口内看不
出问题，但把 recovery 放到 1.5s 后，角速度从 5.1 升到 34.7 rad/s，
stretch 从 1.81% 升到 7.94%，initial-bend N=32 的 stretch 从
1.98% 升到 21.5%。因此 **M0-010 的 N=32 行只在该 0.5s 窗口内
有效，不能当成可落地的保形结论**。

### 2. 纯死区为什么失败

先实现 `E=0.5*k*max(0,d-ε)^2`，结果带内没有恢复力：

- 风停后发丝停在带内新位置，`recovery=-1`；
- 残差 0.04–0.09L，随风历史变化；
- `dz-g-*` 与 `dz-l4-*` 都是 `MASKED_DRIFT`/无唯一平衡。

结论：**死区只能做“允许形变”，不能单独做“回正”**。要同时满足
静态、风响应和回正，必须有软核心。

### 3. 模型：软核心 + 渐进三次项

```text
F = k1*d + k2*d^3
E = 0.5*k1*d^2 + 0.25*k2*d^4
k1 = g / (delta1 * L)
k2 = max(0, (F_w - k1*deltaMax) / deltaMax^3)
c  = 2*zeta*sqrt(k1),  damping = c*h
```

- `delta1/L`：目标重力偏移比例；`c001/c002/c003/c005` 分别对应
  1%/2%/3%/5%。
- `deltaMax/L=0.08`：只用三次项把静态风力上限压到 8% 发长；
  若软核心本身已足够，`k2=0`。
- `zeta=0.5`，`h=dt/substeps=1/1920 s`。
- 全程只对**所有可动点**加约束（global）；local stride 4 只给
  部分点目标，未约束点没有唯一平衡，回正失败，见下表。

### 4. 结果（authored rest）

helix N=8：

| 策略 | k1 | Dtip | Dmax | C | H | dyn | rec | res | stretch | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 0 | 0.2088 | 0.2088 | 0.1915 | 0.718 | 0.1140 | -1 | 0.1140 | 0.0026 | 塌 |
| linear-g100 | 100 | 0.0035 | 0.0035 | 0.0031 | 0.995 | 0.0118 | 0.138 | 0.0012 | 0.0014 | 偏硬 |
| prog-g-c001 | 36.2 | 0.0098 | 0.0098 | 0.0087 | 0.987 | 0.0239 | 0.417 | 0.0027 | 0.0016 | **PASS** |
| prog-g-c002 | 18.1 | 0.0196 | 0.0196 | 0.0179 | 0.973 | 0.0406 | -1 | 0.0122 | 0.0019 | 风感够，回正慢 |
| prog-g-c005 | 7.2 | 0.0482 | 0.0534 | 0.0465 | 0.935 | 0.0710 | -1 | 0.0584 | 0.0018 | 残差大 |

initial-bend N=8：

| 策略 | k1 | Dtip | Dmax | C | H | dyn | rec | res | stretch | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 0 | 0.1426 | 0.2190 | 0.1748 | 1.194 | 0.1325 | -1 | 0.1325 | 0.0087 | 塌 |
| linear-g100 | 100 | 0.0043 | 0.0053 | 0.0046 | 1.007 | 0.0155 | -1 | 0.0016 | 0.0016 | 偏硬 |
| prog-g-c001 | 49.1 | 0.0084 | 0.0105 | 0.0093 | 1.013 | 0.0238 | 0.358 | 0.0039 | 0.0020 | **PASS** |
| prog-g-c002 | 24.6 | 0.0159 | 0.0212 | 0.0181 | 1.025 | 0.0429 | -1 | 0.0042 | 0.0030 | 风感够，回正慢 |
| prog-g-c005 | 9.8 | 0.0349 | 0.0488 | 0.0407 | 1.055 | 0.0792 | -1 | 0.0505 | 0.0049 | 残差大 |

helix N=32（recovery 1.5s）：

| 策略 | k1 | Dtip | Dmax | C | H | dyn | rec | res | stretch | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 0 | 0.2071 | 0.2071 | 0.1733 | 0.716 | 0.1875 | -1 | 0.1875 | 0.0167 | 塌 |
| linear-g100 | 100 | 0.0035 | 0.0035 | 0.0032 | 0.995 | 0.0119 | 0.142 | 0.0003 | 0.0074 | 偏硬 |
| linear-g1000 | 1000 | 0.0004 | 0.0004 | 0.0003 | 1.000 | 0.0013 | 0.242 | 0.0000 | 0.0043 | 近刚体 |
| prog-g-c001 | 35.7 | 0.0099 | 0.0099 | 0.0087 | 0.986 | 0.0244 | 0.396 | 0.0003 | 0.0090 | **PASS** |
| prog-g-c002 | 17.8 | 0.0198 | 0.0198 | 0.0173 | 0.973 | 0.0413 | 0.621 | 0.0008 | 0.0108 | 风感更强，回正 0.62s |
| prog-g-c003 | 11.9 | 0.0292 | 0.0292 | 0.0255 | 0.960 | 0.0540 | 0.812 | 0.0058 | 0.0118 | 回正 0.81s |
| prog-g-c005 | 7.1 | 0.0484 | 0.0484 | 0.0425 | 0.934 | 0.0733 | 1.125 | 0.0115 | 0.0128 | 回正 1.13s |

initial-bend N=32（recovery 1.5s）：

| 策略 | k1 | Dtip | Dmax | C | H | dyn | rec | res | stretch | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 0 | 0.1703 | 0.2389 | 0.1793 | 1.256 | 0.1815 | -1 | 0.1815 | 0.0354 | 塌 |
| linear-g100 | 100 | 0.0048 | 0.0051 | 0.0046 | 1.008 | 0.0156 | 0.154 | 0.0005 | 0.0031 | 偏硬 |
| linear-g1000 | 1000 | 0.0005 | 0.0005 | 0.0005 | 1.001 | 0.0018 | 0.146 | 0.0000 | 0.0005 | 近刚体 |
| prog-g-c001 | 49.0 | 0.0096 | 0.0106 | 0.0092 | 1.015 | 0.0238 | 0.342 | 0.0001 | 0.0050 | **PASS** |
| prog-g-c002 | 24.5 | 0.0184 | 0.0211 | 0.0178 | 1.029 | 0.0428 | 0.508 | 0.0012 | 0.0089 | 风感更强，回正 0.51s |
| prog-g-c003 | 16.3 | 0.0267 | 0.0314 | 0.0260 | 1.042 | 0.0571 | 0.650 | 0.0020 | 0.0123 | 回正 0.65s |
| prog-g-c005 | 9.8 | 0.0412 | 0.0496 | 0.0404 | 1.065 | 0.0790 | 0.887 | 0.0123 | 0.0177 | 回正 0.89s |

### 5. 结论

- **`delta1/L=0.01` 是当前口径的甜点**：N=8/32、helix/initial-bend
  全部 `Dmax<0.011L`、`dyn≈0.024L`、`rec 0.34–0.42s`、`stretch<1%`。
- `delta1/L=0.02` 风感更强（`dyn≈0.041–0.043L`），但回正
  0.51–0.62s；若项目接受 0.6s 回正，这是更“活”的选择。
- `0.03/0.05` 分别把回正推到 0.65–0.81s / 0.89–1.13s，残差也
  增大；不建议作为默认。
- **local stride 4 不合格**：未约束点没有唯一平衡，回正
  1.3s 或直接 -1；不能用它省约束。
- **纯死区不合格**：带内无恢复力，风停后漂移。
- authored rest 是这套路线的正确基线；sag-free 反解在强弯
  N=32 上会把 rest 推到 flip 边界，不能作为 M0-011 的输入。
- tether 仍是**数值保形项**，不是物理锚定；M0-009 的
  `NO_EXTERNAL_SUPPORT` 没有被改变。若要求物理可实现，仍需
  头皮/身体接触或真实附着点。

### 6. 成本

- N=8：约 `0.42 ms/step`；N=32：约 `9.3–10.5 ms/step`
  （finite-difference gradient 主导）。
- 约束数：global 为可动点数 `N−rootFixed`；local4 为其中
  stride 4 的点数。

### 7. 原始输出

- `artifacts/hair-rod-m0-20261114/m0-011-authored-<scene>-n<N>.log`
- 纯死区失败对照：`artifacts/hair-rod-m0-20261114/m0-011-<scene>-n<N>.log`
