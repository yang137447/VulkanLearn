# R1-B：phase4 单根行为 startup 数值准备

2026-11-18。独立新driver `tool/hair/rodBehaviorPrototype.cpp`，
target `vl_hair_rod_behavior`，引用冻结 `rodGhostPrototypeMath.h`。
只复制 phase3 主源中必需的固定chart目标/gradient/Hessian/Newton约定，
未复制旧main或静态fixtures，不修改旧两源码/EXE。
准静态材料frame，无旋转惯性、扭转波、Q/C、clump、碰撞或volume。

## 唯一模型与本轮实际scope

依据 `artifacts/hair-ams-20261118/phase4-behavior-spec.md` 与
`phase4-audit-release-v1.md`。仅运行startup基线h=.001s、计划40步至.04s，
不运行细步、13.5s baseline、风/恢复或longrun。CLI只有
`--startup NEW_DIRECTORY`，已有目录拒绝，意外失败exit2，不跳失败步。

N3几何、authored q、S/B/m均与phase3一致，root x0/q0固定而x1自由。
G0 ghost是未偏移的authored节点，root不挂G0；
k=3.24777679370055 N/m，环境gamma=.056989269110075014 kg/s，
每点m=.001kg，g=(0,-9.8,0)m/s²。无gravity补偿、rest重拟合或参数扫描。
后续若获准probe为+X20 m/s²，但本startup始终处于重力阶段，没有probe。
行为归一化L=.30174487418618995m与chart Lref分开。
Fref含最大未来probe力，并在startup不缩小尺度。

算法v1：15维固定旧q的full-angle body图、解析gradient，
旋转gradient=-Jr(eta)^T*tau/Eref；dense Hessian中心差分eps1e-5；
Newton40轮、Armijo25次、c1=1e-4，solver shift=max(0,1e-6-lambda_min)。
三残差均<=1e-10才停止；每接受步两档未shift H eps1e-5/5e-6，
谱/不对称/差异门槛按release，分解info与有限谱显式检查。
绝对Phi不减常量，v1 Armijo采用旧总目标比较。

## v1 已冻结执行结果：与Python实际不同

新目录 `artifacts/phase4-r1b-execution-001/`。
configure/build-v1均exit0；实际运行：

```powershell
cmake -S . -B build
cmake --build build --target vl_hair_rod_behavior -j 4
.\build\bin\vl_hair_rod_behavior.exe --startup artifacts/phase4-r1b-execution-001/startup-v1
```

**C++本次40步全部接受，exit0**。独立Python v1在第8步迭代上限失败的历史不改；
不能把C++结果写成已复现ITERATION_LIMIT，也不能据此倒判Python失败为PASS。
C++第8步迭代1的gradient=1.4421716448484456e-10，
forceScaled=2.7156154050700208e-10、torqueScaled=4.7672028301487031e-11，
Newton斜率=-3.0974020552357321e-21；旧rounded总Phi差为0，
Armijo接受alpha=.25。迭代2接受.5，迭代3再次接受.5后才满足三门槛；
第8步共4次更新。第9步也用4次更新，其余实际迭代数见日志。
存在相同目标相消症状，本次不同舍入路径没有卡在40轮。

40步执行侧最坏值：

- chart gradient_inf=3.6835558334709e-11；
- force/Fref=6.93620346614356e-11；
- body torque/Tauref=1.2214229128943e-11；
- H不对称比=5.5516505234999e-13；
- 两档H差比=8.11723752118883e-11；
- 能量余额尺度差=2.73162441684461e-17。

以上是本次短基线执行侧数值结果，完整startup三h资格仍NOT_RUN，
静态3s资格、probe、持续回正、10s恢复、13.5s行为baseline全部NOT_RUN。
不能据40步PASS续跑13500步，成本也不能冒充完整w40预算验收。

v1源码SHA256：
`6ab84454bf331d6b0e923807cbcc61edefa35310cbd30ffed016acd14165913a`；
EXE SHA256：
`b7048731ced0e8b085c4c7f778240a271754fcb32f424d29537b85fe63b46d15`。
归档 `v1-source.cpp.txt`、`v1-vl_hair_rod_behavior.exe`、`v1-sha256.csv`，
便于后续v2改动仍保留可复现身份。
`startup-v1/manifest.txt`在求解前写参数/算法，`iterations.csv`逐轮三残差、
Phi下降、alpha/shift/方向斜率；`directions.csv`保留z/gradient/direction，
`states.csv`含所有接受状态，`hessians.csv/spectra.csv`含每步双档真实二阶，
`steps.csv`含残差与余额。
失败路径保存最后试探和双档H；当前没有触发该路径，不能宣称所有失败输入已测。

## v2 有限复核：整体 startup FAIL，最终冻结

经主线程与独立公式预审放行，依据
`phase4-audit-solver-v2-spec.md`，只修改新driver的Armijo为同一目标的逐项
平方差稳定增量；不改gradient/Hessian、物理参数、40/25上限或门槛。
不使用residual替代接受、不增绝对atol、不改globalization。
v1源码/EXE和实际40步成功证据保持原样。

v2逐项计算伸剪、弯扭Log差、G0、重力、平移惯性与world阻尼的同目标增量；
G0的delta r直接用 -Lref*(trial_z-base_z)，不通过近等offset相减。
当前startup无probe，不实现尚未授权的行为开关求解。
特征值、q/Log/strain与gradient/Hessian仍用double；特征相减/平方和与账项
转换long double后累计（当前编译器long double有效二进制位64）。
G0位移从可表示trial/base的差计算；没有将不可表示更新当真实改变。
这与Python longdouble特征计算不是同一精度路径，不能强求失败tick一致。
精度在每档运行前manifest登记。无模型或门槛调参。

v2单次新目录复核：

```powershell
cmake --build build --target vl_hair_rod_behavior -j 4
.\build\bin\vl_hair_rod_behavior.exe --startup artifacts/phase4-r1b-execution-001/startup-v2
```

build-v2 exit0。`--startup`顺序执行同一物理初态、相同.04s计划的三档，
分别写 `h001` / `h0005` / `h00025`，每档初态重置只为共同终点h比较；
档内状态连续，不跳步。**最终进程exit2，整体startup FAIL**：

| h | 计划步数 | 实际 | 结果 |
| --- | --- | --- | --- |
| .001 | 40 | 40步接受至.04s | 本档数值PASS |
| .0005 | 80 | 80步接受至.04s | 本档数值PASS |
| .00025 | 160 | 56步接受，第57步失败 | ARMIJO_EXHAUSTED，FAIL |

第57步是t=.01425s、Newton iteration=2（从0计），三残差：

- chart gradient_inf=5.6574687424150911e-11；
- real force/Fref=1.0653167913995497e-10，**超1e-10**；
- body torque/Tauref=1.851039875440006e-11。

不能仅因chart/torque合格、force“差一点”就宣称收敛。
25个alpha试探均未接受；最后实际试探为backtrack24、
alpha=2^-24=5.9604644775390625e-8，
stableDeltaPhi/Eref=4.6221595550570154e-24，
Armijo右端=-2.0191107167732761e-33。
同一试探旧总Phi相减=0，trial gradient与base相同到导出精度。
`iterations.csv`失败行的alpha为for循环结束后又减半的2^-25，
不是第26次尝试；实际25次试探以 `trials.csv` 为准。
这是日志标签的局限，未修改冻结源码，最终失败JSON显式澄清。

最后接受状态是本档step56、t=.014s，失败trial为step57。
失败trial导出velocity是旧已接受速度，不是接受的新v；不能当合法新帧。
每档states保留从t0起的所有状态，累计接受176步；
失败H1/H2、所有iteration/gradient/direction/alpha与三残差均保存。
无下一步、无继续其他尝试、无v3。
Python v2在h=.0005第19步失败是独立实际结果，不能倒套为C++失败；
C++本次在第三档失败，不把前两档成功升级为三档资格PASS。

v2基线40步记录wall=.0623797s，第二档80步=.1176487s，
第三档失败点wall=.0797837s；整个进程外部计时约.4701893s。
计时包含各档输出/求解，不能从本次不完整资格推导longrun已可执行，
也不授予13500步预算/精度/行为PASS。未独立重算所有C++结果，
最终冻结数值失败交由审计短窗复跑。

## 交付身份与原始路径

v2 source SHA256：
`80e1cbe740e7bd402209a1a80b9b24f8be990177f818aacc6108d69683bbbd16`；
v2 EXE SHA256：
`56eaa851e241c202ed7f20de4a58501d0dffdb9c3303d9d3d78c96a40923225a`。
`v2-sha256.csv`另保存采用的v2规格SHA，
`v2-source.cpp.txt`和`v2-vl_hair_rod_behavior.exe`为冻结快照。
所有文件位于 `artifacts/phase4-r1b-execution-001/`：

- `startup-v1.log` / `startup-v1/`：原C++ v1完整40步实际结果；
- `startup-v2.log` / `startup-v2/{h001,h0005,h00025}/`：
  唯一v2三档的manifest、steps、states、iterations、directions、
  trials、hessians、spectra、各档summary；
- `startup-v2/h00025/failure.txt`：第一失败原始上下文；
- `final-failure.json`：精确step/三残差/最后实际alpha与总体范围；
- `final-last-accepted-state.csv` / `final-failed-trial-state.csv`：
  从原始states直接提取，含root与材料q；
- `identity-before.csv/identity-after.csv`：旧phase3两源/EXE身份，
  本轮不编辑或重建旧目标。

新target只链接Eigen3，MinGW GCC16.2.0 / Eigen3.4.0，
Release `-O3 -DNDEBUG -std=gnu++17`及根工程路径映射，无fast-math。
原始spec/protocol的全部版本hash与两套C++source/EXE清单保留，不覆盖历史。
本轮只完成有限startup数值FAIL闭环；细步完整收敛比较未完成。
**3s静态、probe、持续回正、10s恢复、13500步baseline/longrun、clump均NOT_RUN。**
保持旧M0/L2/G0/R1判定，无runtime/shader/资产或commit。

## v3 有界近根全球化：执行侧短窗通过，独立验收待定

在主线程新授权及 `phase4-audit-solver-v3-release.md` 下，仅新增近根残差
接受分支；不是v2稳定求值的同类修补，明确改变globalization。
远根继续stableDelta Armijo；近根不再声称每轮精确能量下降。
物理模型/gradient/Hessian、三类1e-10门槛、40Newton/25回溯、
最终两档未shiftSPD/能量余额与初末目标上界均不变。
v1 C++40步真实通过和v2三档startup真实失败保持原判，不覆盖档案。

base入口四条件：完整rho_max<=1e-8、未shiftH有限/分解成功/两档一致性与
反对称比例合格/sym最小特征值>=0、完整Newton方向norm<=1e-6、域合法。
merit=.5*(chart梯度平方和+全部自由节点归一力残差平方和+
全部自由frame归一矩平方和)，含15+9+6冗余分量，不只看chart梯度。
入口由base及完整方向决定，不能回溯到短步后才算入口。
合法trial需merit<=.5*base merit，或原三门槛同时满足；
否则拒绝并回溯。direction方程残差另导出，不加任意最终容差。

新增每trial记录mode、rho、merit、入口H谱与两档比、完整方向norm、
direction residual、stableDelta、旧总Phi差、最终真实残差和接受理由。
`nearroot_acceptances`只是该模式接受次数，不宣称每一次旧energy Armijo
都一定失败；不能把计数当独立“救回失败”次数。

### 先原失败点单次重放

读取冻结v2 h00025的step56接受态/速度和step57 iteration2的完整z，
不重求前56步、不修改v2档案；新v3计算一次full Newton alpha1，
并核base入口、trial三残差、最终两档真实H与初末目标原数值上界。
重放是同一失败上下文的数值诊断，不计入连续物理轨迹。

`--replay .../replay-v3` exit0：

- base rho=1.0653167913995497e-10；
- full方向norm=3.9357397980562248e-11，base lambda_min=.029424100986167909；
- merit从1.4759636407778194e-20到5.4165695671938413e-27，
  比=3.6698529811610796e-7；
- trial chart=3.5414317062905337e-14、force=6.7068158035225166e-14、
  torque=2.0829452559228078e-17；
- 最终lambda_min/S_H=4.5925780180010912e-5，双档H门槛通过。

这个trial的stableDelta约+1.15758e-21而旧总Phi差为0，
仍可按新merit规则接受；不描述为能量Armijo接受或精确能量非增证明。

### 唯一v3三h startup

随后在全新目录 `startup-v3/` 顺序运行同一初态h=.001/.0005/.00025，
40/80/160步全部完成，进程exit0。代码没有额外物理变体或细步重试：

| 档 | 步数 | 档内墙钟s | 近根模式接受数 |
| --- | --- | --- | --- |
| .001 | 40 | .0623812 | 12 |
| .0005 | 80 | .1196919 | 38 |
| .00025 | 160 | .2425126 | 112 |

整个进程外部计时.7983958s，含启动/输出等开销；
w40*.3375e3=21.053655s是短窗线性外推13500步预算，
不是longrun实测、成功保证或运行授权。后续阶段迭代/I/O可能不同。

280步执行侧最坏值：

| 指标 | 实测 |
| --- | --- |
| chart gradient_inf | 5.284369486612865e-11 |
| force/Fref | 9.95052447887831e-11 |
| torque/Tauref | 1.7467920765046527e-11 |
| H反对称比例 | 5.515387083433767e-13 |
| H两档差比例 | 8.264800496229159e-11 |
| 最小lambda_min | .02942364430846621 |
| 最小lambda_min/S_H | 4.592546722141394e-5 |
| 能量余额尺度差 | 4.4395696434130726e-17 |
| Newton更新数最大 | 2 |

共同T=.04s位置差/Lref为.0004866636247296927、.0002514068660923027，
阶=.9529008943198944；SO3姿态差为4.785317464337071e-5、
2.540579292799917e-5 rad，阶=.9134571460974235。
由导出states/steps汇总的脚本为 `v3-summarize.py`，
`v3-summary.json/log` 保存全量数字；使用已有Blender Python/NumPy，
不运行独立优化器，不替代审计复算。

v3源SHA256：
`ca5dfae634226971c5f3c096d23f5eb600e169e27f4937aa35c24827cf535448`；
EXE SHA256：
`2c80f03e8c16344be8ba1eaec0295f3a64d9e7564e4fb0cb6cb93168d0a53eb0`。
`v3-source.cpp.txt`、`v3-vl_hair_rod_behavior.exe`、
`v3-sha256.csv`保存冻结版本及release hash。实际命令：

```powershell
cmake --build build --target vl_hair_rod_behavior -j 4
.\build\bin\vl_hair_rod_behavior.exe --replay artifacts/phase4-r1b-execution-001/replay-v3
.\build\bin\vl_hair_rod_behavior.exe --startup artifacts/phase4-r1b-execution-001/startup-v3
```

此为执行侧有限数值通过，等待独立审计。当前CLI仅startup/replay，
尚无behavior功能。完整13.5s/13500步、3s静态资格、probe、10s恢复、
持续回正、longrun、clump均NOT_RUN。未来若另批添加behavior导致新源码/EXE
hash，必须在新身份上先复核startup，不能从旧EXE的状态无证据续跑。

## 唯一 behavior baseline：执行侧完成，等待独立长轨迹复算

主线程随后授权最小behavior追加与条件运行；
`phase4-audit-v3-result.md`独立确认startup v3，新增mode的只读审核见
`phase4-audit-behavior-entry.md`。只运行一条13500步baseline，没有第二长跑，
没有full h/2、参数扫描或额外物理变体。

### 三种身份分别记录

原startup-only v3 EXE为上节 `2c80f03e...`；
追加behavior的首版source为
`b77bf95168bc538e640943f9ca162e14abc7a04fa5feeabf9703694ceb27a424`，
EXE为`019e0a7634e69884acdcbc89141ad20c24302f408208d726bededb93ad931ed9`。
该EXE已运行 `startup-behavior-v3` 280步exit0，没有运行baseline。

最终baseline source：
`bf063c386f9a57f55f7b6d7048d49ce36dc7ca90d1addb959c83177456a2c7b0`；
最终baseline EXE：
`9a277b637bbfd1309887e7af2cf05aa25e8e4659afca07059945ab90dbf5181c`。
第二版比首个behavior源**只增加5行t=3的STATIC_GATE打印**，
包含settled/Dmax/尾窗速度漂移/wall/preauthorized继续说明；
`baseline-vs-behavior-source.diff`保存精确diff。
二者EXE大小/hash不同，不能混称同SHA。

在最终EXE上先执行 `startup-baseline-identity` 三档280步exit0，
随后唯一baseline。baseline后又按管理指示执行一次短
`startup-final-baseline-identity` 280步exit0以核最终冻结身份。
后者只是短复核，不是重复baseline，前后源码/EXE哈希清单保留。
新行为source没有修改冻结Math或旧phase3主源/EXE。

### 输入、时程和数值边界

behavior唯一h=.001、初态authored/v0/q*、root静止、ghost未偏移。
每步同一v3 solver，40Newton/25回溯、三残差1e-10、
全步两档未shift Hessian与谱、域、初末目标上界和余额门槛不省。
Context.probe是已知world外力：位置真实残差减m*probe，Phi减m*probe·x，
stableDelta相应减m*probe·deltaX。材料q直接矩不变；H中的U不含probe势。
Wprobe=m*probe·deltaX只计一次，不重复计重力。
正好step3001..3500施+X20，其他子步probe=0。

每个物理步均导出完整15维chart梯度、9平移/6材料矩残差、谱/二阶摘要、
能量/功、节点/材料状态和阶段。完整raw H1/H2仅保存预登记集合：
1、每100步、2999/3000/3001、3499/3500/3501、13500，
合计140个；**其余步照常计算两档H和完整数值gate**，不以减少raw存储偷省计算。
独立审计按预定集合重求H，不冒称已独立重求每一步H。

2s状态记录尾窗基准，2..3s每步检查速度/L和漂移；
3s快照只作指标基准，不改x/q/v。3s全步数值PASS时，主线程预授权同进程
继续probe与恢复；若静态行为筛选FAIL也允许连续诊断，数值FAIL则停止。
Trec使用恢复全后缀固定.005L带且剩余>=1s；
Rtail覆盖12.5..13.5所有状态，stretch覆盖初态及全部子步、全部3条几何edge。
数值失败exit2，数值完成但行为不合格exit3，全部执行侧门槛通过exit0。

### 实际唯一执行

以下命令均从主题worktree根执行；未再次运行旧tools：

```powershell
cmake --build build --target vl_hair_rod_behavior -j 4
.\build\bin\vl_hair_rod_behavior.exe --startup artifacts/phase4-r1b-execution-001/startup-baseline-identity
.\build\bin\vl_hair_rod_behavior.exe --behavior artifacts/phase4-r1b-execution-001/behavior-v3-001
.\build\bin\vl_hair_rod_behavior.exe --startup artifacts/phase4-r1b-execution-001/startup-final-baseline-identity
```

build/startup/baseline均exit0。唯一baseline到t=3的stdout checkpoint：
numerical PASS、settled=1、Dmax=.010259176309059382，
尾窗速度/L及漂移/L在该离散导出精度下为0，运行内wall=1.4427663s。
程序据已批准条件在同进程连续推进，实际完成13.5s，
未reset v/q、未重选settled时刻。内部wall=6.1301407s，
进程外部计时6.3811359s。该实测不是其他场景或N规模的性能承诺。

执行侧行为测量：

| 项目 | 实测 | 既定门槛 |
| --- | --- | --- |
| Dmax | .010259176309059382 L | <=.015 |
| Dwind / Ddyn | .023990273204110252 L | Ddyn在.02..08 |
| Trec | .033s | <=.5，后缀至10s恢复末且剩余>=1s |
| Rtail / Rend | 3.7284142825781274e-11 L | Rtail<=.005 |
| 全程最大几何stretch绝对值 | .005376894079810679 | <=.01 |
| signed压缩 / 拉伸 | -.00072673740353079719 / .005376894079810679 | 不隐瞒压缩 |

执行侧数值测量：

- 全部13500步数值gate通过；最大Newton更新2；近根模式接受571次；
- chart最大5.2720704490545606e-11；
  force/Fref最大9.928840250678596e-11；
  torque/Tauref最大3.561677068573568e-11；
- 最小lambda_min/S_H=.0006834663966460478；
  H反对称比例最大6.173528056057377e-13、两档差最大8.127360847226626e-11；
- 能量余额协议尺度差最大2.2719639559732245e-16。

这是单一离散轨迹的执行侧结果，**完整独立验收仍待审计**；
不能把5个行为数值通过当材料标定、真实风动力学或任意长期稳定证明。
G0是外部authored支撑，probe是加速度，不是物理气动验证。
clump未运行，volume/碰撞/角惯性/完整AMS没有实现。

### 冻结数据与复算

`behavior-v3-001/manifest.txt`运行前登记参数、算法和raw H集合；
来源/spec/exe等hash由 `baseline-identity-before.csv`在运行前登记。
`baseline-source.cpp.txt`、`baseline-vl_hair_rod_behavior.exe`保留实际版本。
`states.csv`有t0与全部接受态：13501时刻、54004粒子行；
`steps.csv`有13500步真实gate；
`residuals.csv`有202500行坐标残差；
`energy.csv`保留phase/probe/各能量项/功/余项；
`metrics.csv`与`settled-snapshot.csv`可重算全部行为指标。
root反力/矩、全步谱、固定raw H与全部迭代/merit理由分别导出。

执行侧标准库 `recompute-baseline.py`只读取CSV，不重跑优化器，
`recompute-baseline.json/log`核对：
13501连续状态、13500步、500个probe区间、140个raw H索引，
全部行为量与summary一致，位置更新误差/root误差0，
q单位范数最大误差2.220446049250313e-16。
全步真实物理力矩和独立H重算由审计代理负责，执行侧不重复Python完整优化。

用户允许可选科学图，但检查已有Blender/bundled/Python3.12/Python3.14环境
均无Matplotlib；按“不引入依赖、不阻塞冻结”约束未安装依赖，
未产出可选behavior-overview.png，不以图替代验收。

## 最终独立结论同步

独立报告 `artifacts/hair-ams-20261118/phase4-audit-baseline-result.md`
（gitignored 的原始证据；当前工作区未保留该文件，结论以本文摘录为准）
确认有限 PASS：全13500步物理残差、指标与能量复算，以及140个预定
Hessian检查点通过。结论仅限唯一N3、h=.001s、13.5s的数值与五项行为筛选；
未做长期h细化（long-horizon refinement NOT_RUN），clump NOT_RUN，
volume UNSUPPORTED；不升级真实发丝、完整AMS或带旋转惯性的材料杆。
v1/v2的独立失败历史及各C++版本实际结果全部保留，不追判PASS。

最终source/EXE哈希仍为本节前述 `bf063c38...` / `9a277b63...`；
同一最终EXE的baseline前后短startup均280步exit0，前后身份一致，
精确diff证实相对前版仅增加5行checkpoint打印。
此次只同步本文，不改源码/EXE、不重建、不重复baseline。
新收尾日志为原执行目录的 `closeout-validate-docs.log`、
`closeout-diff-check.log`，原冻结与执行日志不覆盖。
