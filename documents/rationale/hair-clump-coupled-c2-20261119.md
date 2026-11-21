# C2 E0/E1/E2 联合能量与交叉装配执行记录

2026-11-19。本记录只描述新 C2 静力 fixtures，
不继承旧 C0 数值结果作为 C2 通过依据。独立最终审计 PENDING。
没有实现或运行 D 单步/startup，没有耦合长跑、runtime/shader/assets 修改或 commit。

## 依据与身份隔离

读取 `artifacts/hair-clump-20261119/c2-spec.md`、
`c2-audit-protocol.md` 与原 C0/C1 固定模型。两份文档的尺度有显式差异：
本轮主线程批准的 E fixtures 以 audit protocol 第3节为尺度依据，
使用 Fc2=F0+2*krel*Lref、Ec2=Fc2*Lref+Tauc2，
所有 krel=.1 与 krel=0 结构对照都用同一预登记 C2 公共尺度，
同时导出原 C0 的 F0/E0。没有按实测残差选尺度。

新增 `tool/hair/hairClumpCoupled.cpp/.h` 和独立
`hairClumpCoupledMain.cpp`；CMake只追加 `vl_hair_clump_coupled`，
链接 Eigen3。命名空间 C2，既有 C0/C1 main/source/EXE 保持冻结，
仅引用冻结 `rodGhostPrototypeMath.h`，不 include 旧 cpp/main。
从 C0 窄复制必要的单根能量/物理力矩/旧q chart 逻辑，
新模块自行累计所有三根和全部 pair，不调用 C0 runner。

证据目录为 `artifacts/hair-clump-20261119/c2-execution-001/`。
首次直接build新target因旧构建图尚无该target退出2，保留build.log；
随后显式configure刷新构建图。此构建入口失败不是数值fixture结果，
没有覆盖该日志或修改物理参数/门槛。

## 45维目标与真实导数

guide-major顺序每根9位置+6材料角，总27位置+18材料角。
PositionIndex/FrameIndex检查guide/node/segment/axis范围；
root x0/q0不进入未知量。SingleAt从固定old state重建
x=Lref*z、q=qold*ExpR(eta)，计算杆/G0/gravity及BE惯性/阻尼/probe目标。
E fixtures固定h=.001、old=authored、v=0、probe=0；
没有推进时钟，也不宣称静力平衡或收敛。

At先还原三根全部候选态，一次计算三根目标加 Urel，
梯度拼接后加完整coupling梯度，物理力残差减去所有incident pair力。
导出45 chart、27物理力、18物理body torque的完整90标量merit；
一般扰动上的大残差是fixture输入，不冒称动力学求解结果。
总杆目标的xq/qx/qq仍由原Evaluate/RightJacobian计算。

PairAt只累加三条无向边、每j=1..3一次，
力为两端等大反向、直接材料矩为零；物理H的对角2kI、
同层跨guide-kI，chart H乘Lref²/Ec2。
完整联合H由实际g45中心FD逐列构造；
独立对照从三份15维SingleAt梯度中心FD构造blockdiag，
再加解析pair H，比较实际联合矩阵。
raw矩阵先导出，不用对称化掩盖非对称项。
完整45谱另导出；18维qq按FrameIndex显式gather，
没有取guide-major矩阵最后18行列。

## 固定 fixtures 与输出

E0含authored和一般非零两基态，krel=0时：
联合目标与三根原目标和比较，原C0梯度换算到公共C2尺度后逐坐标比较；
两档完整H与独立blockdiag比较，并核全部跨guide块。
能量/gradient门槛1e-10，FD装配1e-6，FD零跨块1e-10，
不要求k0时保留pair非零谱。

E1按audit protocol第4节：
各根所有可动节点分别加(.001,-.002,.003)、
(-.002,.001,.0005)、(.0007,-.001,.002)m；
每根自由frame s=1,2右乘Exp((.001,-.002,.003)*(s+1))。
这与较早spec的角扰动条目不同，本次明确选择获批E协议的固定值，
不在结果出来后变更fixture。
全部45坐标由同一基点重建中心能量FD，epsilon为1e-4/1e-5/1e-6；
位置导数换算为物理力误差，角导数经Jr转置逆映射为body torque误差，
细两档分别按Fc2/Tauc2检查1e-7，粗档仅记录。

独立pair物理FD使用Lc=.06、Fc=.006、Ec=.00036、Tauc=.00036，
完整27位置导数/27×27H均从真实PairAt求值差分，
后两档沿用1e-7误差门槛。解析跨guide块另按非零khat尺度比较1e-9，
不借总惯性H的大尺度弱化pair检查。

E2核pair-only45谱为27零、18非零（chart值为.3*Lref²/Ec2），
9个对应层共同位移零模与18个直接材料角零块；
绝对节点坐标计算全部9个pair-node moment，
与k*r×r0比较，允许其本身非零；净力按Fc核对。
保留单层hand能量1e-7J与三根力(-.0002,.0001,.0001)N、
同步位移零能量/力，以及负k/非有限位置/非法索引拒绝测试。

原始inputs、guide-major dof-map、能量分项、45梯度、
每node累计pair力/物理残差、body torque/root reaction、
per-pair力/绝对moment、全部坐标FD、完整H与qq/45谱均导出CSV。
checks每行由实算误差/固定尺度与门槛比较生成，不硬编码PASS。
最终状态只表示执行侧E fixtures结果；独立audit另行出具，
D单步、D startup、C3为NOT_RUN，volume为UNSUPPORTED。

## E fixtures 首次实际执行 checkpoint

新target显式configure与build成功，冻结新EXE后执行
`--verify c2-execution-001/verify-001`，exit0，
267项实算checks、0失败。执行侧E0/E1/E2门槛满足，
独立审计仍PENDING，不据此放行D或宣称联合时间步已通过。
首次无target构建失败日志保留，后续构建日志为build-002/003。

冻结 `hairClumpCoupled.cpp` SHA256：
`2479115c7f5a869408538006f49e8b5cfe9bf671ad57e46cf8e15d71bb018a0d`。
冻结EXE SHA256：
`74964123bad395f9fb827438566e187b1dfb010ced8d1e105447eabf33306167`。
完整入口/头/CMake/Math/spec/protocol身份见identity-verify-001.csv；
exe-verify-001.exe和源码快照与运行证据同目录。

实际公共尺度为Lref=.10062305898749055m，
F0=1.3566012358907333N，Fc2=1.3767258476882314N，
Tauc2=.12051150319255192Nm，E0=.25701686937408774J，
Ec2=.25904186937408774J。
45坐标完整能量FD细两档最大物理误差比分别
6.88812664e-11、5.16007032e-10，均小于1e-7；
独立27位置pair FD细两档分别1.95781614e-12、2.65453169e-11。
27×27物理pair H细档Frobenius归一差约3.44819341e-10。

一般扰动完整H1最小/最大特征值约
.029191399795963257 / 42.713907182991754；
按iq gather的qq最小值约.029191714364325005。
这些是该固定非平衡fixture的谱报告，不是D优化器最终原H门槛验收。
pair-only45谱最小约-1.54272409e-18（数值零），
非零chart特征值约.011725903643837146，
27零/18非零全部按预登记误差检查。

旧C0入口/core/header、Math、旧rodBehavior及两份旧EXE前后哈希未变。
新CMake只新增coupled target，不重建旧CPU目标。
git diffcheck与文档分层门禁均通过。
原始CSV/输出SHA256见verify-001及output-verify-001-sha256.csv，
执行汇总见E-checkpoint.json。

## 最终尺度新身份：E回归与D短算子实际执行

本节追加后续获批执行，不改写上节早期E267身份或结果。
主线程依据 `c2-audit-d-release.md` 正式批准D实现及一次三档短算子执行。
该release与 `c2-audit-protocol-d-amendment.md` 明确最终沿用原单根
Fref/Tauref/Eref，不再加2*krel*Lref；最终一般角fixture采用
eta1=(.01,-.02,.015)、eta2=(-.015,.01,.02)。
早期source2479115c/EXE74964123及verify-001全部保留，
它们只代表各自旧manifest的早期E实例，不能冒称最终尺度结果。

### 最终冻结身份与执行顺序

目录仍为C2专属 `c2-execution-001`，使用全新输出
`verify-finalscale-001/` 与 `startup-001/`，拒绝覆盖已有目录。
构建日志为build-D-001.log，exit0；源码/EXE/spec/release/amendment
运行前SHA256见identity-D-001.csv，源码与EXE快照为
source-D-001.cpp.txt、main-D-001.cpp.txt、header-D-001.h.txt、
exe-D-001.exe。最终核心与EXE：

- hairClumpCoupled.cpp：
  `c7eee97f6cf3588f473ed09d65bde1ae9ba69b87bb85181144566d168a3e221d`
- EXE：
  `0bf26427a07f874d0ae7505f6d00f1ff4d67ba90629fde118c25618eb0cfe6dd`

该同一EXE先实际 `--verify verify-finalscale-001`：
771项检查、0失败、exit0。E现在覆盖三个预登记h、
old=authored/v0、probe=+20X/0/-20X的固定状态目标/梯度/H对照，
一般角按最终spec；没有额外k0动态轨迹。
随后只执行一次 `--startup startup-001`，exit0，
h=.001/.0005/.00025分别40/80/160个联合45DOF步，
总280联合步。首步包含在粗档40步内，不是另跑一次首步。

### 求解器与原始证据

从冻结C0窄复用Newton/v3近根规则及三个原StableDelta项；
新的pair项用long-double累加
.5*k*(e_trial-e_base)·(e_trial+e_base)，按固定最终Eref除一次。
每次全45矩阵求一个方向，统一alpha回溯、三根同时提交。
merit为完整45chart+27force+18bodyTorque平方和；
近根要求完整未乘alpha方向norm，不逐guide缩放门槛。
不使用顺序guide solve、lagged force或修改predictor。

每步raw H1/H2、45谱、按iq gather的18维qq矩阵/谱均保存；
nearroot用到的base H1/H2亦保存，不仅记录flag。
三档分别接受8/24/88个nearroot trial，
其base状态、g、方向、shift、rho/M、原谱、两档H一致性及全部trial
均在directions/trials/hessians原始CSV中。
每个物理步输出old/new x/q/v、完整45真实残差、
逐node potential/pair力、body torque/root反力矩、
全部9条pair-node绝对moment、分guide外功及coupling节点功。
states中old行采用物理旧时刻tick-1，accepted行采用新时刻tick。

U含全部杆/G0/gravity与Urel；重力不重复计外功，
pair势能不重复计外力功。每步Rrel与
-.5*sum k*|dx_a-dx_b|²精确二次余项比较，
归一尺度固定.00036J。没有将未平衡余项塞入账目来掩盖漏力。
运行scope严格是首步即+20/0/-20X的.04s短算子，
不是原3s异步载荷窗口；每档从同一authored/v0重新开始。

### 执行侧 checkpoint，独立审计仍PENDING

首联合步h=.001三残差最大约1.18177520e-13，
Urel=3.19476216e-10J、max pair error=3.77167924e-5m。
全部280步最坏chart/force/torque归一残差分别
4.90360286e-11 / 9.23848154e-11 / 2.25909071e-11，
最小最终原H lambda_min/SH=4.5924311833092195e-5。
最大H非对称比6.08260391e-13、两档差8.26475495e-11；
最大能量账残差比4.34922593e-16，
Rrel二次余项差比3.51287522e-17。
root x0/q0误差严格零，qnorm最大1.11022302e-16。
门槛全部沿最终spec原值，没有失败后放宽。

| guide | position阶 | SO3阶 |
| --- | --- | --- |
| 0 | .95210181175431463 | .8593598996243309 |
| 1 | .95290084590247626 | .91346668067739523 |
| 2 | .95211749708821791 | .85879761734239823 |

全部两级细化误差递减且细档误差>1e-9，逐guide分别满足[.5,1.5]；
全束汇总仅辅助，不平均掩盖单根失败。细化原值见refinement.csv。

| h | 末Urel/J | 窗内max pair error/m |
| --- | --- | --- |
| .001 | 2.291047945612921e-5 | .011216973148822177 |
| .0005 | 2.3456094980416148e-5 | .011331035722223465 |
| .00025 | 2.3740418643923967e-5 | .011389449968459897 |

这些非零量说明本诊断实际激发pair响应，不是只输出enabled标志，
不构成clump/volume质量门槛。细档三根外功依次为
.00030507595467283615、0、.0003050733227030153J；
净外力为零不抹掉两端各自外功。

全部接受步两档45×45 raw H共1134000元素行，
另有90720个gather后qq元素行和486000个nearroot base H元素行；
45残差共12600行。三档求解与I/O累计3.9897355s。
执行checkpoint见D-checkpoint-001.json。
本节只记录执行侧门槛与实测覆盖，summary保持independentAudit=PENDING。
不增加算法、不重跑E/D、不扩C3；最终结论由独立审计给出，
coupled longrun/C3为NOT_RUN，volume为UNSUPPORTED。
