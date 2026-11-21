# R1：材料杆 + G0 的有界 CPU 实验

2026-11-18。独立本地模型，不是完整 AMS 或 DER。源为
`tool/hair/rodGhostPrototypeMath.h`（能量、full-angle SO3、解析虚力矩）与
`tool/hair/rodGhostPrototype.cpp`（固定 fixtures、证据、唯一小N时间步）。
新目标 `vl_hair_rod_ghost` 只链接 Eigen3。旧 M0/L2/G0 源、EXE、记录均不修改。
本文件先登记运行输入与算法，随后按日志追加结果，不事后调参数或门槛。

## 公式与约定

依据 `artifacts/hair-ams-20261118/phase3-r1-minimal-spec.md`、
`phase3-audit-static-release.md`、`phase3-audit-timestep-release.md`。
段中心独立材料 q，从材料到世界；右乘完整 rad 增量。
nu=R^T*edge/ell，rest nu0/phi0 来自 authored；
Uss=.5*sum ell*(nu-nu0)^T*S*(nu-nu0)，
Ubt=.5*sum (phi-phi0)^T*B*(phi-phi0)/ellbar。
选择 log-vector 差，不是 Log(Q0^-1 Q)。S 单位 N，B 单位 N*m²，能量 J。
固定 rest ellbar，不重复除长度。
joint 虚矩使用 w=Jr(phi)^(-T)*B*dphi/ellbar、tau_s+=Q*w、tau_t-=w；
剪切/拉伸矩为 ell*nu cross S*e。
Jr 在近零处用其解析 Taylor 极限，不用 I 替代一般有限角。
域 margin=.1 rad，单位 q 输入误差<=1e-12；无效输入拒绝，不回单位四元数。

root x0/q0 固定，x1 不固定。导出完整未消元力/矩，可取 root 约束反力=-f0、
body 约束反矩=-tau0；world 矩须乘 R0。位置与材料自由度分开。
G0 只作用可动节点，不直接依赖 q；gravity 在势能内，不重复加功。
材料 frames 准静态，无旋转动能、角惯量、omega 推进或 twist waves，无额外 Q/C。

## 静态固定输入与判据

全部 SI，N=3 段。非轴 authored 节点：
(0,0,0)、(.05,.02,.3)、(.1,-.02,.6)、(.08,.04,.9) m；
authored 材料 q=ExpR((.1,-.15,.05))、ExpR((.25,.05,.2))、
ExpR((-.1,.2,.3))。S=(3,4,5) N，B=(.2,.3,.4) N*m²。
非轴当前位置偏移依次为根0、(.01,-.02,.005)、(-.015,.01,-.01)、(.02,.005,.015)；
当前 q_s=q*_s*ExpR((.04*(s+1),-.03,.05))，用于完整未消元虚功检测，
该人为扰动夹持坐标只作梯度测试，不称满足根边界的静态解。

静态 FD 以同一基态作 ±epsilon：位置尺度 Lref=max ell，完整 body 角尺度1rad；
三档 1e-4/1e-5/1e-6 全导出，后两档每坐标误差/Fref 或 Tauref<=1e-7。
authored 零力/矩阈值1e-10，零弹性能1e-12；
共同刚体变换、q符号、internal force/global moment 门槛1e-10。
Fref/Tauref/Eref 按 static release 的输入参数公式，不取实测残差作尺度。
共同变换用 ExpR((.3,-.2,.4))、平移(.2,-.3,.1)。

直杆纯 twist：3段每段1m，S=(3,4,5)，B=(1,1.5,2)，
frame中心间 Leff=2m，不是节点弧长3m；T=.2Nm，加载增量 psi=(0,.1,.2)rad，
期望 Ubt=.02J、两joint内矩 .2Nm。另一个 rest psi*=(0,.07,.16)，
加载后=(0,.17,.36)，验证增量总角 .2，而不是总角 .36。
非零解析相对门槛1e-9，加载自由矩门槛1e-10；
2x2 矩阵[[4,-2],[-2,2]]、RHS=(0,.2)，独立 dense 对照。
这里中心线固定是纯轴解析边界，不能当一般三维优化器。
额外 tip 势 -T*psi_last 只在此 pure-axis 图中解释，不构造一般 SO3 力矩势。

G0 静态例：节点 m_i=.01*(i+1)kg，可动 k=7N/m，
g=(0,-9.8,0)m/s²。默认 y=x* 输出 mg；显式支撑 y=x*-mg/k
只检零弹性应变 authored 点的自由净力/矩，是外部支撑，不是 rest 反解或自发 sag-free。
该支撑有非零 G0 能量，不要求其为零。
非轴外载 FD 仍用相同三档。期望域拒绝：零/非单位/非有限 q、
零段、非有限位置、joint cut、chart cut、无效材料系数。

## 动态运行前固定算法与唯一输入

严格 timestep release v1：N3，authored节点(0,0,0)、(.01,0,.1)、
(.02,.005,.2)、(.025,.015,.3)m；q0=I，
q1=ExpR((.03,-.02,.04))、q2=ExpR((.05,.01,.08))。
S=(.3,.4,1)N，B=(.001,.0015,.002)Nm²，m=.001kg每点，
可动 gamma=.002kg/s、k=.05N/m，g=(0,-1,0)，
可动 y(t)=x*+t*(.002,-.001,.003)，根静止。
初态 authored、v=0；三档 h=.01/.005/.0025 独立从同一初态开始，
T=.04s，共4/8/16步，档内连续，不跳失败步、不重置q。

Phi 包含平移 backward Euler 惯性、U(y_next)、环境阻尼项；
每步以旧q为固定图基准，15自由 z=(x1..3/Lref,eta1..2)。
固定图梯度对角增量拉回为 -Jr(eta)^T*tau/Eref，不能仅用-tau。
dense Newton最多40轮，chart梯度 FD Hessian eps=1e-5，对称化后
solver-only shift=max(0,1e-6-lambda_min)。Armijo c1=1e-4，
从alpha1折半，最多25次。无自定义替代优化器、材料正规化或扫参。
同时 chart梯度无穷范数、物理力/Fref、物理矩/Tauref<=1e-10 才停止。
不以微小步幅或优化器返回成功代替真实残差。

最终未shift Hessian两档1e-5/5e-6，记录完整矩阵与全部谱；
反对称比/两档差<=1e-6，lambda_min/max(1,||Hsym||2)>1e-6，
另记q-q主块最小值。每步目标不得上升超过1e-12*Eref。
各能量项、ghost功、阻尼、平移动能、D_U/D_nl与余额逐步记录；
余额按release账项尺度<=1e-10。D_nl是精确余项记账，不是独立稳定性证据或物理阻尼。
初步失败保存最后合法试探及真实残差，不发布成成功帧。
位置/姿态细化以同一T作差，误差>1e-9时预期阶 .5..1.5；
与独立oracle末态按Lref/1rad比较<=1e-7，由外部复算完成，不导入oracle到求解。

## 当前执行记录

原始新目录 `artifacts/phase3-r1-execution-001/`。
configure exit0；静态最小核心首次 build exit0，
`verify-stage1` 51checks/0失败/exit0。该阶段不是完整静态或动态验收。
两源码的后续阶段新增仅为上述已放行scope；以最终冻结SHA为交付身份。
禁止 extrapolate 到 N32、长轨迹、风/回正行为或完整惯性杆。

## 冻结交付：执行侧检查，等待独立验收

最终两源码 SHA256：

- `rodGhostPrototype.cpp`：
  `97f4fbfe85fadc054332a2e1d7a6cd8a10c94fc111985f49bcda181850c98f87`；
- `rodGhostPrototypeMath.h`：
  `e1071ccb2feabc31b91e08ce468df9703ce43fcda102d6127b99a44ffc015bd9`。

EXE `build/bin/vl_hair_rod_ghost.exe` SHA256：
`8abfffd80fe7ea42ce80565356e992ca8181c9031a81d524d3dce84a73663281`。
编译环境为 MinGW GCC 16.2.0、Eigen 3.4.0、Release C++17，
`-O3 -DNDEBUG -std=gnu++17` 加根工程路径映射；无 fast-math。
初次补丁驱动替换被工具拒绝，未改变源码；随后按小阶段补丁落盘。
所有实际 build/verify/trajectory 首跑均成功，无材料参数或门槛调整。
最后一次源码变化只追加参数/authored/root约束反力导出和修正阶段标签，
没有改变数学、算法或输入；旧阶段证据保留。

在 worktree 根目录实际命令：

```powershell
cmake -S . -B build
cmake --build build --target vl_hair_rod_ghost -j 4
.\build\bin\vl_hair_rod_ghost.exe --verify artifacts/phase3-r1-execution-001/verify-frozen
.\build\bin\vl_hair_rod_ghost.exe --trajectory artifacts/phase3-r1-execution-001/trajectory-frozen
```

configure/build均exit0。冻结静力148checks、0失败、exit0；
冻结动态完成28步、exit0。每次输出目录全新，已有目录拒绝，
意外输入/域/solver/二阶/能量gate失败均退出2并保留记录。
完整物理步只在三残差与二阶/能量gate同时通过后发布；失败不跳步。
失败时 `states.csv` 负tick表示最后合法试探，不能作为成功时间帧，
其velocity列仍为旧已接受速度，失败残差以 `step-failure.txt` 为准。
本轮没有失败步，因此没有负tick状态。

执行侧动态实际最坏值：

| 项目 | 实测 |
| --- | --- |
| chart gradient infinity norm | 3.2171854574854377e-11 |
| max physical force/Fref | 7.04783392332139e-11 |
| max body torque/Tauref | 9.048461836796606e-12 |
| 最小未shift Hessian特征值 | 0.034080261897810056 |
| 最小lambda_min/S_H | 0.003866777810527336 |
| Hessian反对称比例最大 | 1.3997185924245784e-11 |
| 两档Hessian差比例最大 | 7.367477373760469e-11 |
| 最大Newton更新数 | 2 |
| 最大余额绝对差 | 1.4726799371093984e-17 J |
| 最大余额协议尺度差 | 6.640254915591755e-17 |

以上为固定chart无量纲Phi的二阶证据，不宣称全局最小、全局唯一或长期稳定。
能量余额是残差与定义余项的一致性检查，不另授予物理守恒。
记录完整两档Hessian、全部特征值与q-q主块最小值，人工solver shift不进入最终证据。

同一T=.04s的细化：

| 对比 | 位置差/Lref | 材料SO3角差rad |
| --- | --- | --- |
| h=.01 与 .005 | .0010138657660351735 | .00071496520845839575 |
| h=.005 与 .0025 | .000555441098630105 | .00035736700315485781 |
| 估阶 | .86816081957850433 | 1.0004666075112947 |

执行侧末态对照独立NumPy oracle `phase3-audit-timestep-oracle-002/results.json`，
只读其已冻结末态，不把参考状态输入求解、不重跑oracle优化器：

| h | 位置差/Lref | SO3角差rad |
| --- | --- | --- |
| .01 | 7.049848618629783e-16 | 1.9584668040213094e-16 |
| .005 | 2.9914822298369737e-16 | 9.987475096924954e-17 |
| .0025 | 1.3723957976270482e-11 | 1.5906736936427883e-12 |

均低于本例1e-7门槛。复核脚本
`artifacts/phase3-r1-execution-001/oracle-comparison-frozen.py`，
使用已有 `D:/YYBWorkSpace/Tools/blender-5.2.3-windows-x64/5.2/python/bin/python.exe`
运行，日志/JSON同名。这是执行侧对照，不替独立验收。
工具自己的trajectory summary把外部oracle comparison标NOT_RUN，
表示求解程序不内嵌oracle；外部结果以该单独日志为准。

原始可复算证据：

- `verify-frozen/inputs.csv`：current/authored x/q、质量、k、ghost、rest nu0/phi0/ell；
  `parameters.csv`：每例S/B/g/margin及默认root边界；
  pure-twist例中心线另固定，只作为解析测试；两joint dual均1m、Leff2m。
- `verify-frozen/evaluations.csv`：分项能量、全部未消元力/body矩、
  root_constraint行是负完整root力/矩；world矩由q0转换；
  `checks.csv` 和 `finite-differences.csv`：每档FD原始能量与门槛。
- `trajectory-frozen/manifest.txt`：运行前算法、参数、尺度；
  `inputs.csv/parameters.csv`：动态authored输入；
  `states.csv`：每个接受状态x/v/q/R，含初态与末态；
  `iterations.csv`：每轮目标/三残差/solver shift/alpha；
  `steps.csv`：各步真实残差、二阶、能量/功和余项；
  `hessians.csv/eigenvalues.csv`：两档未shift二阶原始值。
- `identity-before.csv/identity-after.csv`：旧M0源、旧L2/G0源与L2/G0 EXE身份；
  `stage-static-sha256.csv`：前一阶段哈希，不冒称是最后导出补丁版本；
  `frozen-code-sha256.csv`：本次最终两源码/EXE哈希。

本交付只覆盖本地R1静态能量虚功和唯一N3/.04s的有界28步。
无额外Q/C、转动惯量/角动量/材料扭转波、N32、长期行为、
风停持续回正、runtime/shader/资产或commit。
独立最终验收已有限 PASS，报告为
`artifacts/hair-ams-20261118/phase3-audit-r1-result.md`：
G-E 静态148checks、G-T/G-M唯一N3/.04s三档共28步均通过，独立复跑均exit0。
最大真实力/矩归一残差约7.05e-11/9.05e-12，oracle末态差约
1.37e-11 Lref/1.59e-12 rad，位置/姿态细化阶约.868/1.000。
此结论仅覆盖准静态材料frame模型的上述有限验证；G-B行为NOT_RUN，
不升级为完整惯性材料杆、扭转波、完整AMS或真实发丝资产验收。
收尾仅更新本文，不改两源码或EXE、不重建；新门禁日志位于原始目录的
`closeout-validate-docs.log`、`closeout-diff-check.log`，不覆盖原日志。
