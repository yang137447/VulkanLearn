# Phase5 C0/C1：首轮 C1 失败冻结记录

2026-11-19。首轮只落盘并编译了独立 `vl_hair_clump_cpu` 工具；
未运行 C0 三guide startup/control，也未运行任何完整13.7s轨迹。
旧phase4 rodBehavior/rodGhost Math/EXE 未修改。

## 首轮身份与范围

新工具候选：
`tool/hair/hairClumpPrototype.cpp`，target `vl_hair_clump_cpu`，
引用冻结 `tool/hair/rodGhostPrototypeMath.h`，不包含旧main。
首轮输出目录：
`artifacts/hair-clump-20261119/execution-001/verify-001/`。

首轮 C1 是三guide、三根无向 complete graph、对应节点 N3 的
world-oriented vector-rest energy：

```text
U=.5*k*sum_(a<b,j=1..3)|((x_a,j-x_b,j)-(x*_a,j-x*_b,j))|²
k=.1 N/m
```

本记录不把它称为 clump physics、volume 或材料 constitutive law。
C0/control、C1 v2、coupled dynamics、volume、clump behavior 均未通过/未运行。

## v1 执行结果

configure 与新 target 首次 build 成功；v1 `--verify` 退出 `2`：

```text
C1 checks=FAIL failures=5
```

保留：

- `execution-001/verify-001.log`
- `execution-001/verify-001/checks.csv`
- `execution-001/verify-001/summary.csv`
- `execution-001/source.cpp.txt`
- `execution-001/exe.exe`

v1 C1 失败项为 hand guide1/guide2 力、world-oriented moment、
两档细 FD。没有通过调 k、放宽阈值或删掉失败项。

首轮诊断发现两个待修 oracle/实现核对点，尚未运行修复版：

1. 三guide位置扁平数组必须使用每根4节点布局；若把guide/节点索引
   当作每根3个可动点，会读错对应节点，直接表现为hand guide1/2力错误。
   正确完整pair loop应按每根 `4*guide+node` 寻址，roots仍排除在能量但不能
   从存储布局中消失。
2. FD中心差分必须从非零手例基态计算，并比较该基态解析力沿同一扰动方向的
   directional derivative；不能在 authored零梯度基态上硬编码解析值，
   也不能把正负能量差的符号与 `-gradient` 约定混淆。

`pair.csv` 的每pair/node实际力导出已在待修复源中准备，但没有运行该版本，
因此不把它倒写成 v1结果。world moment 必须以实际全部pair force和绝对节点
坐标求和，并与全部pair的 `k*r×r0` 解析和比较；不要求总矩为零。

## 身份与冻结边界

v1执行快照哈希保存在 `identity-before.csv`；当前首轮执行快照尚未包含后续
未运行的pair诊断改动。当前工作源/目标的哈希另行读取后保存，不冒称为v1
执行身份。旧冻结身份仍保持：

- `rodGhostPrototype.cpp`：
  `97f4fbfe85fadc054332a2e1d7a6cd8a10c94fc111985f49bcda181850c98f87`
- `rodGhostPrototypeMath.h`：
  `e1071ccb2feabc31b91e08ce468df9703ce43fcda102d6127b99a44ffc015bd9`
- `vl_hair_rod_ghost.exe`：
  `8abfffd80fe7ea42ce80565356e992ca8181c9031a81d524d3dce84a73663281`

本轮不继续编辑、构建或运行修复版；等待主线程依据 C1 失败闭环重新授权。
C0/control、异步probe、NN identity/reselect、固定XY signed area、
13.7s/41100 guide-steps、C1 coupling通过、volume与clump PASS 均保持
`NOT_RUN` 或 `UNSUPPORTED`。不从单根phase4 PASS推导三guide能力。

## C1 v2 correction-only result

依据 `artifacts/hair-clump-20261119/phase5-c1-v2-spec.md`，v2只修正
oracle/source诊断，不改变vector-rest能量、k、阈值或C0范围：

- 所有guide按4节点stride索引，roots保留在布局中而从C1求和范围j=1..3排除；
- `pair.csv`输出三条无向pair、每个可动node的error及两端实际pair force；
- hand fixture保留完整graph的guide0 `-2kd`、guide1/2 `+kd`期望；
- FD从已登记非零general perturbation base计算，比较中心差分能量方向导数
  与该base解析gradient投影，并记录三档epsilon；
- world moment使用绝对节点坐标和全部pair force，与完整
  `sum k*r×r0` 逐项比较；不要求总矩为零。

v1失败证据 `execution-001/verify-001/` 保留，未覆盖或追判。
修复版使用全新 `artifacts/hair-clump-20261119/execution-002/`：
build exit0，`vl_hair_clump_cpu --verify .../verify` exit0，
`C1 checks=PASS failures=0`。C0/control、13.7s异步control、proxy、
coupled dynamics、volume与clump物理PASS均仍NOT_RUN/UNSUPPORTED。
本次没有运行C0，也没有修改phase4或旧冻结EXE。

## C0 startup 实现：execution-003

本节追加记录新的执行批次，不改变以上历史结果。新增
`tool/hair/hairClumpControl.cpp/.h`，通过 `C0` 命名空间隔离。
`vl_hair_clump_cpu --startup NEW_DIRECTORY` 顺序执行三根独立
15DOF guide 的三档 startup；`--verify` 保留原有 C1 静力入口。
control 尚未实施或执行，等待 startup checkpoint 后的单独放行。

求解函数从冻结 `rodBehaviorPrototype.cpp` 的已审 v3 必要函数窄复制：
Context、At、StableDelta、Hessian、Norm、Converged、NearRoot 与单步
Newton/backtracking。步长是每个 Context 的值，没有包含旧 main，
没有修改旧 solver、Math 或旧 EXE。位置为九维、材料角为六维；
每根的 x0/q0 固定，x1 自由，无 guide coupling、无 45DOF 求解。

先在未平移 authored guide 上构造同一 rest strain/rest rotation、
段长与尺度，再将 authored/model ghost/初态一起平移至规定的三根
root offset，避免通过平移的浮点减法重新标定 rest 或尺度。
未知位置仍采用旧求解器的绝对位置除 Lref chart。
S、B、m、k、gamma、重力、静止未补偿 G0 与原 v3 相同。

每根 h=.001/.0005/.00025 各连续推进至 .04s，分别 40/80/160 步，
共 840 个独立 guide-steps。每个已接受物理步导出真实状态、完整梯度与
物理力/材料矩残差、两档未 shift H、谱、root reaction、能量账、
初末目标值。每次试探导出 merit 与 StableDelta/Armijo 判据、
domain-valid 状态和方向；数值失败停止并保留 tick、三残差与失败 H。

所有原门槛保持不变。近根 merit 使用完整 15+9+6 残差平方和，
不是单独位置残差。未 shift H 的两档一致性、非对称性与
lambda_min/SH 逐步检查，最终目标界仍为 phi_final<=phi_initial+1e-12。
不声明逐迭代能量单调，不以 shifted H 代替物理 H。

三档末态细化误差保持 phase4 的定义：position 是三个自由节点位置差
平方和开根号后除 Lref；SO3 是两个自由材料 frame 相对旋转 Log 的最大范数。
两组相邻档误差必须递减且细档误差>1e-9，
log2(error_h_h2/error_h2_h4) 必须在 [.5,1.5]，
逐根逐指标写入 refinement.csv，并计入 startup 退出码。
工具内部数值结果与独立验收结论分开；summary 的 independentAudit 为 PENDING。

成本仅按实测三根 h=.001 各 40 步总耗时乘 342.5，
等价于每 guide 平均 w40 乘 1027.5，估算未来 41100 guide-steps；
不减少 Hessian 检查，也不把外推当作长期实测。

### execution-003 实测 checkpoint

新目标 Release 构建 exit0；冻结快照 `exe-startup.exe` 实际执行
`--verify execution-003/verify` 得到 13 checks、0 failures、exit0；
`--startup execution-003/startup` 完成九组共 840 步、exit0。
执行侧 startup 门槛全部满足，独立验收仍为 PENDING，不在此授最终 PASS。
原 execution-001/002 的 C1 身份与结果未覆盖。

三根 position 阶依次为 .9529008943、.9529008943、.9529008899；
SO3 阶为 .9134571461、.9134571461、.9134571603，
两档误差均大于 1e-9。所有 840 步的三残差最坏值为：
gradient_inf=5.28441719e-11、force_scaled=9.95052448e-11、
torque_scaled=1.74679759e-11。最小 lambda_min/SH=4.59254672e-5；
最大 H 非对称比=5.57638227e-13、两档 H 差比=8.26481805e-11；
最大能量账残差比=4.34347250e-16，root x0/q0 误差为零。

三根 coarse w40 合计 .1808392s；41100 步预算外推约 61.94s，
不是长期运行结果。汇总见 `execution-003/checkpoint.json`、
`startup/refinement.csv`、`startup/summary.csv`。
冻结 source/EXE 及哈希见 `identity-startup.csv` 和同目录快照；
旧 rodBehavior/rodGhost/Math 与两份旧 EXE 的前后 SHA256 完全一致。
文档门禁通过。control 未实施/未运行，coupled dynamics 与 volume 的边界不变。

## C0 control 实现与执行协议

独立 startup 审计结果位于
`artifacts/hair-clump-20261119/phase5-audit-c0-startup-result.md`；
其冻结旧 startup EXE 身份独立保留。该报告与主线程随后放行一次
control，新 EXE 必须自己回归 verify/startup，不继承旧 EXE 的执行结果。

`--control NEW_DIRECTORY` 逐根执行独立 15DOF，连续初态到共同13.7s，
每根13700步；三根分别使用3001..3500、3101..3600、3201..3700步
的+X20加速度，其余为零。更新步使用区间起点载荷，故3001步对应
[3.000,3.001)。所有等待区间继续真实模拟，没有状态重置或复制旧轨迹。
实现仅缓存新轨迹状态和归一化速度供共同时间 proxy，Hessian 始终为
单步局部矩阵，不缓存全程大矩阵；逐根计算与每tick轮流计算在无coupling
的模型上等价，真实各根轨迹仍分别求解。

startup 的默认求解路径、门槛及导出语义保留。control 使用同一内核，
逐步计算两档真实 H、谱、三残差、域、初末目标界和能量账；
raw H 只存 first/every100/全部六个开关边界及其前后一步/final，
准确集合由运行前 manifest 及 RawCheckpoint 固定。数值失败即停止整个
control，保留失败tick、三残差、失败H；行为失败按根独立报告，
不修改物理输入或数值/行为门槛。

每根静态资格使用2..3s全部样本的速度与相对2s drift，snapshot固定3s。
Dmax保留原3s相对authored定义，另报0..3s全部样本最大值。
Ddyn从自身probe开始前端点至共同末取最大，另报从全组3s起的最大值。
Trec取自身off之后最后一个带外样本的下一步作为完整带内后缀起点；
要求该后缀至少1s，恢复时间从自身off计。共同tail为12.7..13.7s。
共同group恢复从3.7s计，单独report-only。三根和最坏guide均输出，
不以平均值替代失败根。

proxy 的u=1/3、2/3、1先按authored三段非等长弧长查找固定segment/fraction，
之后全部时刻沿同一映射插值。截面三点坐标、固定XY有符号/绝对面积、
两种A/A0、翻转、数值退化标记、centroid与三条pair距离均逐tick导出。
固定身份NN由各截面及对应node的authored点一次定义，并同时导出当前
重选ID/距离和各自authored距离/比值。距离平方在64倍机器epsilon相对
范围内按较小稳定ID破同分；该规则不作为物理压缩阈值。
数值退化只标abs(area)<=64*epsilon*A0，亦不是质量门槛。
所有面积/NN只report，coupled dynamics未实施，volume仍UNSUPPORTED。

### 最终新身份回归与唯一一次完整 control

execution-003 的 `exe-control.exe` SHA256：
`11309a89ea960c9ac5cdd80d56b199e1785d5461ce1d9314f3d5aa7fcb7f5660`。
control 模块 SHA256：
`20136579e0f02a86364ad17c1c942240fbb2aaca9ac507b8fe9ca790615fa9f9`。
完整源码/头/入口/CMake/EXE 身份见 `identity-control.csv`，
源码快照另存 `hairClumpControl-control.cpp.txt` 等，旧 startup 快照保留。

该同一 EXE 先实际执行 `--verify verify-control-identity`：
13 checks、0 failures、exit0，再执行
`--startup startup-control-identity`：九组840步、细化阶均通过、exit0。
八类数值 CSV 乘九组共72份文件与独立验收的旧 startup 冻结结果
逐字节一致，记录于 `startup-regression.log`；计时/身份另行保存。

之后只执行一次 `--control control`，exit0，各根13700步、
总41100步连续完成。执行侧数值与行为门槛均满足；该记录不是最终
独立审计 PASS。summary 保持 independentAudit=PENDING。

| guide | Dmax/L | Ddyn/L | own Trec/s | common Rtail/L | max abs stretch |
| --- | --- | --- | --- | --- | --- |
| 0 | .010259176309059382 | .023990273204110252 | .033 | 3.72841428e-11 | .005376894079810679 |
| 1 | .010259176309059127 | .023990273204107987 | .033 | 3.72856711e-11 | .005376894079814676 |
| 2 | .010259176309059528 | .023990273204107165 | .033 | 3.72831016e-11 | .005376894079813566 |

各根2..3s速度与drift均为零，静态资格满足。另报静态全样本最大
位移约.01186648060834L，没有用它替换Dmax定义。共同group恢复
从3.7s起为.033s，仅report。三根probe均恰500步，开关步与预登记一致，
各根独立probe功约.000304295983737J，gravity已包含在U。

全步执行报告复核：最大chart/force/torque归一化残差分别约
5.27207842e-11、9.92885270e-11、3.56167707e-11；
最小lambda_min/SH约6.83466397e-4；最大H非对称比6.33828784e-13，
两档差8.12736085e-11，最大能量账残差比5.14045634e-16。
每根150个预登记raw-H时刻、两档共67500矩阵元素行；
每根residuals/spectra各205500行。末态13.7s全部覆盖。
这些是执行侧 CSV 核对，不冒称已独立重求全部H。

固定截面映射实测为：
u=1/3落在segment1、fraction=.00082355394831884119；
u=2/3落在segment2、fraction=.00041177697415900697；
u=1落在segment2末端（浮点fraction=.99999999999999989）。
它们不是直接取node1/2/3。proxy-sections共41103行，
proxy-neighbors共287721行，包含对应root与全部可动node及三个截面。

三根求解含逐步I/O累计18.476212s；早期w40预算外推61.94s保留，
没有改写为事后预测。完整轨迹/metrics/energy/proxy见
`execution-003/control/`，执行核对汇总为 `control-checkpoint.json`。
C1仅静力回归；coupled dynamics NOT_RUN，volume UNSUPPORTED，
未提交commit，最终结论留给独立审计。

### 冻结后 proxy 流式 checkpoint

不增加功能、不重跑模拟，只逐行读取既有 control CSV，生成
`execution-003/proxy-freeze-checkpoint.json`。状态明确为
EXECUTION_CHECKPOINT_PENDING_INDEPENDENT_AUDIT；
不设 proxy 验收阈值，不将本节当独立审计或 volume PASS。
source/EXE 继续使用上述 `identity-control.csv` 的冻结身份。

| authored u | 固定XY signed/unsigned A/A0 最小 | 最大 | flip行数 | 数值退化行数 |
| --- | --- | --- | --- | --- |
| 1/3 | .9296157282519963 | 1.0703950846834085 | 0 | 0 |
| 2/3 | .8935852902937939 | 1.1064170286727577 | 0 | 0 |
| 1 | .8794158380297035 | 1.120581412090742 | 0 | 0 |

每截面13701样本，覆盖0..13.7s。三个截面合并的固定身份NN距离范围
为.05276153455872925..06723840049672053m，
比值为.8793589093121543..1.1206400082786756；
重选NN距离范围为.05276153455872925..06395365246239539m，
相对各自authored pair距离的比值为.8793589093121543..1.0653025372709322。
对应node（含root）统计仅有末位舍入差，明细保存在JSON。

对每个固定(kind,index,guide)序列，将当前reselected ID与上一tick
比较，三个截面合计98次ID切换；对应node序列另计98次，二者是两套
采样统计，不合并为独立物理事件。两套各有4054个sample-guide行的
重选ID不同于authored固定ID。JSON包含逐index/guide计数与范围；
这些计数不抹掉固定邻居压缩，也不增加NN质量阈值。

逐行核对每根raw H实际索引恰为：
`{1,13700} ∪ {100,200,...,13700} ∪ {b-1,b,b+1 | b∈{3000,3100,3200,3500,3600,3700}}`。
去重后每根150个时刻，epsilon=1e-5/5e-6，两档各15×15，
共67500元素行。JSON另保存每根完整排序索引，便于审计定位。
没有重复长期优化，coupled dynamics仍NOT_RUN，volume仍UNSUPPORTED。

### 独立最终验收

独立报告 `artifacts/hair-clump-20261119/phase5-audit-c0-control-result.md`
确认本次 C0 数值与三根各自五项行为有限 PASS：全41100 guide-step
物理/功账/指标重算、450个预登记H点重求、41103截面行与287721 NN行核对通过。
三根 Dmax约.01025918L、Ddyn约.02399027L、own回正.033s、stretch约.5377%。
proxy面积/NN只报告变化，无质量阈值；这是无coupling的比较control，
不是clump耦合或volume PASS。C1 v2仅静态PASS，C2/C3仍NOT_RUN，
volume UNSUPPORTED；未重复长优化、未改CPU或门槛。
