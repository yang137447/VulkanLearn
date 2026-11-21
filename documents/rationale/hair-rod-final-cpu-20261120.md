# 最终选定 Hair Rod CPU 模型：2026-11-20 有限结论

本文是数值模型与实测证据记录，**不是 runtime 契约或 GPU 生产放行**。
只记录明确冻结的 axial004 CPU 身份及本 worker 执行的真实 groom 验证；
后续源码若变化，必须另记身份，不能继承这里的全部结论。
原 M0/C3 研究见 [M0 实验](hair-rod-m0-experiments.md)、
[C3/落地历史](hair-landing-20261119.md)，本文不修改该历史汇总。

当前有限结论：选定的上拉伸 1% 联立约束模型，在固定八根实际 groom、
N32、240 Hz、静态 3 s / 实际 20 m/s 风 0.5 s / 恢复 10 s 的 CPU
历史上完成 **8 × 3,240 = 25,920 个接受步的多 run 覆盖**。
N8/16 另完成八根、三 h 的 48 格 / 672 个短步；不是低分辨率长程。
39210 完整长程、最差两根短步及 remaining 中六完整历史已有独立力学/
功账重构；fresh120与低N最终覆盖独立审计正在进行，
不能把“执行完成”写成全部审计完成。

## 与 M0 / C3 原目标的对齐

| 原目标 | 当前 CPU 证据与边界 |
| --- | --- |
| 作者形状在重力下保留 | 从原作者 rest 建立 root-local 支撑，静态最大偏移 .450607–5.706652 mm；不是将作者形状反解成精确重力平衡、也不是零偏移保证。 |
| 风下有界、可见、可回正 | 八根固定空气历史动态 2.896984–48.349076 mm、同捕获相机中心线投影 2.501498–41.270726 px；永久恢复后缀 .033333–.104167 s。CPU 中心线投影不等于实际渲染、遮挡或全头运动已经验证。 |
| 材料帧、扭转与惯量 | x 与完整段材料帧 q 联立；保留三维剪切/拉伸及 bend/twist，纠正截面 J；没有删掉 q 或用切向覆盖材料帧。 |
| 拉伸上限 | 新模型加入显式上拉伸 1% 不等式及反力乘子；真实反力进入动量与功账，有限 EA 内力仍存在。旧有限 EA 无约束模型的失败保留。 |
| clump / 整体体积、NN | 当前八根独立求解，没有 guide-guide 物理耦合、密度或体积约束。单根/八根有界不能证明全头体积、互穿或 NN 目标完成。 |
| C3 完整联合参考 | 旧 world-fixed vector-rest、45DOF、13.7 s / 13,700 步参考属于另一模型；其有限 physics/behavior 证据及原 NNIdentity FAIL 均保留，不推广生产或继承为当前模型 PASS。 |
| CPU 到生产 | 本文提供 CPU 方程、实际覆盖和可复现身份；GPU 同方程执行、强风 head/root、SDF/body 接触、完整 follow、时钟/恢复及最终性能另有 owner 与门禁。 |

## 方程来源及明确的本地选择

Kugelstadt–Schömer 2016
[Position and Orientation Based Cosserat Rods](https://animation.rwth-aachen.de/media/papers/2016-SCA-Cosserat-Rods.pdf)
提供位置/段材料帧的建模入口。这里的完整 log-vector 差能量、
非等长权重、截面惯量、动量积分及 1% 不等式分别是明确声明的本地选择，
不能宣称该论文所有公式或算法被逐式复现。
早期 XPBD 柔度/局部投影历史不等于当前完整动量 Newton。

`source/hair/rod/rodEnergy.cpp` 与 `rodAnalytic.cpp` 的约定：
x/v 为世界 SI，R 为 material→world，ω 为 body 角速度，
`R <- R Exp(η)` 为右扰动。作者段长 ℓ，相邻平均段长 ℓ̄：

```text
ν_i = R_iᵀ (x_{i+1} − x_i) / ℓ_i
Uss = Σ .5 ℓ_i (ν_i − ν0_i)ᵀ S_i (ν_i − ν0_i)
φ_i = Log(R_iᵀ R_{i+1})
Ubt = Σ .5 (φ_i − φ0_i)ᵀ (B_i / ℓ̄_i) (φ_i − φ0_i)
S = diag(GA, GA, EA), B = diag(EI, EI, GIp)
```

这里是 `φ−φ0`，没有悄悄换成 `Log(Q0⁻¹Q)` 或 quaternion 虚部近似。
右 Jacobian、body 几何项、q/−q 等价与 `π−1e-4` 分支域一起构成身份。
TFX 中心线不能唯一决定材料 twist；缺省材料帧构造是补充假设，
不称为从 TFX 测得真实毛发截面朝向。

### 作者 rest、采样与质量

固定源 TFX SHA256：
`73FA1F3BCE4A060F4876C3286A5EC29DA5F985C793C9E03F81F5F16340DC39C7`。
inventory IDs 固定为 `584,33486,39210,8924,4006,5651,5660,120`。
N 表示点数；N32 为原始 32 点，N8/16 保留原 x0/x1 与 tip，
从 x1 到 tip 按原折线弧长采样。坐标 cm→m 仅一次；
低 N 折线段长之和可能变化，质量由该离散 rest 几何重新积分，
不伪称各 N 质量逐位相同。

`BuildSectionInertiaRestState` 使用原作者位置、rest strain/joint angle；
TFX w0 仅是 pin 标记，不是物理 kg⁻¹。两个根位置与首段材料帧规定，
自由点 m 由 ρAℓ 分配；规定位置仍有物理质量，inverseMass 仅用于自由度。
pin 判据是 `SampleRootSupport(root,rootLocal)` 的精确构造结果，
不能换成有不同舍入的原 rest 坐标再保持零容差比较。

### 截面 J 与有限 EA

工程有效 bundle：`r=.0005 m, ρ=1300 kg/m³, E=1e6 Pa, G=.4e6 Pa`，
不是逐根真实人发测量材料。`A=πr², I=πr⁴/4, Ip=2I`，
`EA=.7853981633974483 N`。每段 `ms=ρAℓ`，一半质量分到各端点；
段材料帧仅承担截面 rotary inertia：

```text
Jdirector = ms diag(r²/4, r²/4, r²/2), kineticMathVersion = 2
```

截面质心积分使平移与截面旋转的交叉项为零；
中心线长度转动已由 x 的运动承载，不能再向 q 添加 `msℓ²/12`。
旧工程 J 的原输出保留，不能称最终物理 J 验证。
端点 lump 的平动质量仍是 O(ℓ²) 求积，非 exact consistent mass。

### Root-local 作者支撑与工程额定

以实际 root rigid frame 驱动作者点：
`x* = pRoot + RRoot a_i`，其速度包含 root 线速度及 `Ω×RRoot a_i`。
这是外部作者支撑/actuator，不是杆内部弹性或体积势。
`rodRootSupport.*`、`rodProgressiveSupport.*`：

```text
d_i = x_i − x*_i
Ut_i = .5 k1_i |d_i|² + .25 k2_i |d_i|⁴
fSpring = −(k1_i + k2_i |d_i|²) d_i
fDamp = −c_i (v_i − v*_i)
k1_i = m_i gref / (.01 L), c_i = 2 × .5 × sqrt(m_i k1_i)
```

同一工程横向圆柱阻力：
`β=ρair Cd/(πρr)`，`ρair=1.225, Cd=1.5, aeroGain=1`。
以 Udesign=20 m/s、dRating=.08L 决定：

```text
k2_i = max(0, (m_i β Udesign² − k1_i dRating) / dRating³)
```

`.08L` 是单节点、横向、静态额定尺，**不是所有耦合段动态峰值的上界定理**。
本次六根的旧 Ddyn≤.08 工程 flag 仍失败，不通过增大 k2 刷 PASS；
实际有界/回正与该旧 flag 分别报告。

空气为世界速度，不是 +20 m/s² probe。当前接口保持 old 几何切向，
端点速度 `vNew=(xNew−xOld)/h`：

```text
P = I − tOld tOldᵀ
w = P (Uair − vNew)
fAir = m β |w| w
−∂fAir/∂xNew = (mβ/h) (|w|P + wwᵀ/|w|), w=0 时取 0
```

内点 old tangent 为相邻各自单位切向之和的归一化；端点用相邻边。
不将 GPU 曾使用的 outgoing tangent 默认称为同一力离散。
`input.externalForces` 只装非空气其他力，避免重复 oldv drag。
无风阶段也计算静止空气对运动杆的阻力；没有直接空气 body torque。

### 显式 1% 上拉伸约束与数值方法

`rodAxialLimit.*` 定义 `g_i=1.01ℓ_i−|x_{i+1}−x_i|≥0`，
`λ_i≥0, λ_i g_i=0`，成对节点反力
`(+λ_i t_i, −λ_i t_i)`。有约束时 10 分量包括位置/材料旋转、
contact slots 与 incoming axial multiplier；没有删掉方向自由度。
当前实际 groom 验证不添加 body contact，不用 inactive contact 宣称碰撞已过。

这是一项明确的新模型决策 `internalAxialMathVersion=1`，
不是提高 E/EA/EI、不是修改旧有限 EA 模型标签，也不是解后 postclamp。
保留真实有限 EA 内力，额外张力由乘子提供。
`Wstrain=Σ fPair·ΔxPair` 单独导出并只计入功账一次；
不是新储能或再次加入的支撑阻尼。该不等式不限制压缩，压缩原值单列。

完整 BE 平动/最终 body 角动量残差、真实力矩及解析切线联立求根。
新版已审策略为 direct≤24 更新；失败从原 old state 重启
fixed-old/same-h continuation，fallback 总≤108、单 stage≤24、合计≤132。
虚拟 load fraction 不推进物理时间；只有最终全载荷根可发布 x/v/q/ω。
这是新策略身份，不冒充旧 12 更新 solver 的相同成本。

原门槛保持：component norm≤1e-8、物理向量 rx/rq≤1e-2、
精确 mapped root pins/rootQ、qnorm≤1e-12、旋转域、
残差功上界≤5e-11Eref、原 balance/identity≤1e-10。
新约束 gap tolerance=1e-9L、complementarity≤1e-9；
每 edge 的允许数值拉伸超量 `1e-9L/ℓ_i` 与实际超量一并导出，
不舍入成“恰好 1%”，不修改物理 epsilon=.01。

### Profile 语义

作者选择同一明确模型/profile、root/placement 与环境空气速度，
不靠暴露 raw k2、角逆质量、迭代次数、阻力 gain 或逐资产救援旋钮达到目标。
effective bundle 的 E/G/r/ρ 是工程假设，支撑和静态空气额定由同一公式派生。
实际生产字符串及 schema 由配置 owner 维护；本记录不另造 token，
也不因存在 profile 解析就宣布该 runtime backend 质量通过。
数学/kinetic/air-time/tangent/root-frame 版本必须参与 trace/cache/reset 身份，
避免旧 rest/frame/follow 缓存跨模型误用。

## 实际每 guide 长程结果

固定 240 Hz，fresh authored，静态 3 s，世界 +X 20 m/s 风 .5 s，
随后静止空气恢复 10 s。每根 3,240 步。
永久恢复要求回到 static snapshot 的 .005L 内并保持至少最后一秒；
原静态资格为 2–3 s 的速度/L≤1e-3、drift/L≤1e-4，
静态 Dmax≤.015L、原恢复≤.5s、末秒 tail≤.005L。

| ID | 静态 mm | 动态 mm | 投影 px | 恢复 s | 最大上拉伸 % | 最小压缩 % | 最大 λ N | 原 Ddyn [.02,.08]L flag |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 120 | .721862 | 4.854581 | 4.011870 | .04166667 | .0503720547 | −.07791673 | 0 | PASS |
| 584 | .450607 | 2.896984 | 2.501498 | .03333333 | .0110423936 | −.11211713 | 0 | PASS |
| 4006 | 5.706652 | 48.349076 | 41.270726 | .10416667 | 1.0000000003 | −.91424883 | .00195671610 | FAIL |
| 5651 | 2.590704 | 22.650598 | 20.070777 | .07500000 | .3006673947 | −.40170447 | 0 | FAIL |
| 5660 | 2.127395 | 18.781383 | 16.274966 | .07083333 | .2890468465 | −.39413976 | 0 | FAIL |
| 8924 | 4.506437 | 39.596439 | 32.814735 | .09583333 | .7093032619 | −.78687245 | 0 | FAIL |
| 33486 | 1.075854 | 14.825358 | 11.890961 | .06250000 | .8681513687 | −.13107522 | 0 | FAIL |
| 39210 | 3.564115 | 30.751981 | 25.421365 | .08333333 | 1.0000000321 | −.11344454 | .01006579461 | FAIL |

投影来自实际捕获固定 1280×720 camera/model 的 CPU 中心线；
没有光栅遮挡、动画蒙皮、完整 40k follow 或整头体积保证。
全部八根 long 的静态资格满足，末秒 tail/L=1.9843e-9..8.7480e-9。
39210 最大拉伸发生 tick743/segment7，fractional 超量 3.2118931e-10，
该 edge 声明的 gap-derived strain allowance=3.5520321e-8；
峰值 edge 有限 EA 轴力 .007853981834 N。约束补充力有独立 raw λ。

全部 long 最大 component norm=9.9500597e-9、
work bound=4.8067178e-11、complementarity=1.1402642e-12；
最大 direct 更新数 23，fallback 接受步均为 0。
39210 long wall16.7698278s，fresh120 long10.9019504s；
它们包含完整 trace/CSV，不是纯 solver benchmark，更不是 GPU 性能。
4006 原 settled 风首 tick721 新模型 direct9/fallback0/backtracks6，
不删除原隐式空气模型的预算失败。

N8/16 固定短共 48 格 / 672 接受步、wall1.4312352s；
N32 最差两根另有 84 immediateair grid + 1,452 settled prefix 接受步。
短4006较细 h 压缩可达约1.54–1.92%，单列，不因只有上拉伸约束而隐藏。
没有从这些短窗推出低 N 全长或全时域精度结论。

## 原失败、时间误差与工具事故

旧有限 EA / sectionJ2 / endpointair 的39210完整 long
真实伸长2.114576386%、轴向应变.02114483169、
轴力.01660711197N，旧4006风首721 norm6.414419e-6预算失败。
一次冻结失败步52轮重放表明末更新确已复核，不能甩给“缺末次检查”。
这些都是旧模型的有效失败，不因新约束通过而消失。

原 explicitair 24fixture 长程是20原完成+4新WorkGuard完成的多身份
77,760步coverage，原14行为FAIL+补4FAIL保留。不能说一个 EXE 跑完77,760。
新 endpointair 两 N32 固定240/480/960/1920/3840Hz共372步数值/功账已独立核；
240Hz时间误差没有改善，33486/120相对各自有限3840参考的
max-node/L=.0152783214/.0134652834，maxSO3=.30601936/.07907943rad。
1920-vs3840仍有实测差，不是严格误差界；
动态 .025s 误差不直接套静态3s .015L线。
最终轴限模型没有继承这些旧轨迹为自己的时间精度 PASS。
管理冻结这项局限，本轮不再盲扫 h、引入积分器或作谱搜索。

新约束 short001 的 accepted0 是工具 pin oracle 构造误报：
八个首步 solver status0/work accepted，但比较 rest 坐标与 mapped pin 的
3.47e-18/5.55e-18m舍入差。原 FAIL 保留；short002改回规定 root 映射，
没有改 exact-zero 门槛或物理，随后1536/1536完成。

remainingN32long001六完整历史19440步后，在最后120部分历史达到1GiB预算。
工具未捕获 `runtime_error OUTPUT_BUDGET` 导致 exit−1073740791；
不是数值 FAIL。旧stdout/stderr、所有 raw/hash均保留。
各流缓冲尾部不同，全流共同完整120至tick725；不推断实际最后执行tick。
trace每步局部销毁，源码无累计trace/history；未测peak memory，不臆测OOM。

新run前登记覆盖修订：每步物理状态/实际力/λ/gap/EA牵引/功账全保留，
完整10残差及矩阵仅 first/last/every120/active-set-change/failure 存储；
其他步可由 raw 重算。原441MB final-blocks是10分量残差，不是全矩阵。
增加outermain catch、每120flush/progress后，仅fresh120补完整3240；
没有重复六已完成历史、没有拼接旧120partial、没有增大物理或求解预算。
raw120 case字符串沿用long39210的命名缺陷明确保留；
guide列120、`--long120`命令及路径共同唯一标识其实际身份。

## 复现入口、raw closure 与 SHA256

下列路径均相对仓库根；历史 raw 在 gitignored `artifacts/`，
**本文保存关键结论和身份，不声称仅 Git clone 就能恢复未入库的历史raw**。
冻结 bundle/driver/object/raw 需随证据归档；其缺失时必须报告缺失，
不能引用未存在路径或拿当前 live archive 冒充历史构建。
标准模块源码/测试在 `source/hair/rod/`、`tool/hair-tests/`；
最终 actual runner 是冻结 artifact driver，不能声称旧
`vl_rod_groom_validation` CMake CLI 已支持这些 axial flags。

统一 artifact 前缀 `artifacts/hair-landing-20261119/`：

| 身份/文件 | SHA256 |
| --- | --- |
| `axial-step-release-004.json` | `960ED0EAC13275D6B8AE68C80FCC0B32D3CD015E0F16AB7ECFA8F099D2235A81` |
| `axial-cpu-frozen-004/manifest.json` | `B5D8B2FB7BFF74F3FBE852DDCB3A2232383E9586D7C5BA69C29F43A87672D7E4` |
| `validation-final-cpu-api-deps-001.json` | `9928C230B4DD792CEF16C77270AB3865EDCFC62A7064CEB9F0EC1C1C68E83066` |
| driver002 EXE：short002 /39210long001 | `02725B5E8F2DCB63AF562798EF857AFB8180D27A7315EB5C99303DB737E5DA8B` |
| driver003 EXE：remaininglong001 /N8N16short001 | `D95BD459AB191D4C6E111EE15940C02A8D339045A670F86038C195155642660A` |
| driver004 EXE：fresh120long001，采样矩阵/catch修复 | `88B544595A4C2881599D7CBE578AE9732E0F3FABA07393AE91E7F427F4CD3FE5` |
| `validation-axial-n32-coverage-analysis-001/result.json` | `3B83BDDE2A3FB426148FBC0BB9E3CF1706A266280522E97C71D6845BBC430DE1` |
| `validation-axial-n8n16-short-001/summary.json` | `746B5D7D93B4CD9A259DBAEEA834DCD754A8AB240CB30355E64FB3D5CCE8616D` |
| `validation-axial-n32-budget-exit-001.json` | `67E8542ED289FC06B0FA3CE71D5883C875A2542AA6CA56884FD13A9BF2C05E1B` |

25,920最终覆盖的逐guide来源：

| 原始目录 | 接受完整guide | 原始证明入口 |
| --- | --- | --- |
| `validation-axial-39210-long-001` | 39210 | `summary.json`、全部3,240步raw |
| `validation-axial-n32-remaining-long-001` | 4006/584/33486/8924/5651/5660 | `checkpoint.json` 的六case；进程无summary，不伪造summary |
| `validation-axial-120-long-001` | 120 | `summary.json`、`progress.json` tick3240全flush |

每个run的相邻 `*-prerun.json` 保存运行前命令/EXE身份。
`validation-axial-build-001/frozen-identity.csv` 保存完整CPU源码/对象，
002/003/004的`identity.csv`、`link.rsp`保存driver与实际链接闭包。
`validation-final-cpu-api-deps-001.json`确认15个unit源码逐一等于
CPU冻结bundle004；不是将live共享lib与更新header混用。
独立审计已存在：
`audit-validation-axial-actual-short-002-result.json`、
`audit-validation-axial-39210-long-001-result.json`、
`audit-validation-axial-n32-remaining-long-001-result.json`（明确六例19,440步，
排除尾部120及截断行）、`audit-axial-actual-checkpoint.md`；
全8最终审计返还后另增记录，
本文不预写其PASS。

在仓库根、现有冻结证据闭包完整时，可向**全新**目录重放这些命令；
它们是复现说明，不表示写本文时又重复执行：

```powershell
$a = "artifacts/hair-landing-20261119"
$inventory = "$a/groom-selected-strands.raw.json"
& "$a/validation-axial-build-002/validation-axial-actual.exe" $inventory "$a/REPRO-NEW-short"
& "$a/validation-axial-build-002/validation-axial-actual.exe" $inventory "$a/REPRO-NEW-39210" --long39210
& "$a/validation-axial-build-003/validation-axial-actual.exe" $inventory "$a/REPRO-NEW-N8N16" --short-n8n16
& "$a/validation-axial-build-004/validation-axial-actual.exe" $inventory "$a/REPRO-NEW-120" --long120
```

历史 driver003的 `--long-n32-remaining` 命令会触发原固定输出预算问题；
不把该旧命令推荐为一条“完整八根复现”。
修复后的 driver004也保留 `--long-n32-remaining` 分发，可在全新目录
按已登记采样策略重放4006及其余六根；该七根合并的修复后命令
本轮没有再次执行，不能借它伪造另一份已实跑summary。
实际八根来源仍严格以上表为准；其历史driver003命令及中断退出码
完整保留于原 `validation-axial-n32-remaining-long-001-prerun.json` /
`validation-axial-n32-remaining-long-001-exit.txt`。
需要重建时，使用相应 frozen driver源码、001冻结CPU源码/对象和其
`link.rsp`，编译 `-O3 -DNDEBUG -std=gnu++17`；
编译器SHA `9C2577EACB115C305D7A1A0B10AA0B16D0B7B697824E151D190C62B21DD2EB8C`。
不能把当前可变 artifact driver覆盖早期driver再冒称同EXE。
CPU bundle的 `build-reference.ps1`可重编自有driver，但须保留实际driver
自己的JSON/helper include闭包；不要同时编全部源码又重复链接同archive。

标准数学回归可经既有 CMake target 复现，使用共享构建窗口：

```powershell
cmake --build build --target hair_asset_tests -j 4
build/bin/hair_asset_tests.exe --gtest_filter=RodAxialLimit.*:RodImplicitAir.*:RodSectionInertia.*:RodRootMotion.*
py -3 tool/validate-docs.py
```

这里的filter用于明确数学子集，不宣称整个标准套件通过；
本记录新增后实际执行的是docs校验，没有为写文档重跑全部groom。

## 尚未继承的 head/root 与生产证据

既有 `artifacts/root-motion-20261119/`有sectionJ2下SE(3)/Galilean
性质与周期root功账独立重构，源码回归入口 `rodRootMotionTests.cpp`。
它们使用旧独立动量模型，并非最终axial1+强风active约束的小clump；
周期报告也明确仅 accounting PASS，不是物理动态质量保证。
已有的性质/功账无需重复作同一身份证明。
若补最终root/head强风联合门禁，应另预登记有限case、实际root/pin速度、
外部actuator/约束功、activeλ与回正窗，不能默认本次固定root长程已覆盖。

没有 guide-guide耦合、没有 body contact的当前八根验收，不足以宣称
整体体积、SDF接触、全头跟随、GPU执行及生产落地完成。
本文冻结有限CPU结论，供主线程结合独立最终审计与GPU/contact/runtime
证据作整体决策；不再扩新物理方案或通过调参抹去旧失败。
