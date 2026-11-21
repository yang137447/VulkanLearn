# Hair landing 执行记录

2026-11-19。研究与证据记录，不是运行时契约。

本文按执行次序保留旧身份、失败与后续纠正。文末“管理收尾基础快照”汇总当前模型、
公式来源及未放行项；前文早期“待实现/待执行”不表示最新状态。

## C3：固定三根联合异步长轨迹

实际工作树为
`D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/tressfx5-hair-port/VulkanLearn`。
预登记与审核文件在该树 `artifacts/hair-landing-20261119/`：
`c3-spec.md`、`c3-prerun-manifest.json`、`audit-preregistration.md`。
此前 C2 实际证据在 `artifacts/hair-clump-20261119/`，
最终尺度 E771 与 D280 已独立 PASS；早期 E267 是另一冻结身份。
不得把旧 D 短诊断或 C0 三根独立长轨迹称作 C3。

新增 `tool/hair/hairClumpLongRun.cpp`、
`hairClumpLongRunMath.h` 与 `vl_hair_clump_long_run` 目标。
adapter 逐字段保持冻结 C2 定义，通过新目标只读链接一份 C2 数学
translation unit；C3 自行推进完整 45DOF Newton，未调用 C2/C0 runner。
旧源码与旧 EXE 不改。无 runtime/shader/assets 修改或 commit。

本次唯一长轨迹预登记为 h=.001s、13700 联合步至13.7s，
三根分别在 [3,3.5)、[3.1,3.6)、[3.2,3.7) 接受 +X20m/s²；
沿用原参数、尺度、准静态材料 frame 与全部真实残差/功账门槛。
每步全45 chart、物理力/矩、域、root/qnorm、初末目标、功账均检查。
最终两档原 Hessian 仅在预登记固定集合采样，其余明确
`H_NOT_SAMPLED`，不能宣称每步最终 SPD；每个 Newton 方向仍用完整
45×45 原 H，nearroot 另外保留两档 base 原矩阵。

每根分别测原五项行为与静止资格；固定 authored 弧长截面、
XY 面积和固定/重选最近邻只作诊断，没有体积质量 PASS。
NN 沿用平方距离 64eps 稳定低 ID；面积退化阈值为64eps×.00156m²。
正式运行前先测无推进 H 成本、冻结源/EXE/spec 身份并经独立审核；
墙钟预算900s、输出6GiB，预算耗尽保留原始证据并报告未完成。

本段初始状态为 PRE_REGISTERED；实际执行及独立结论在后续追加。
worldfixed vector-rest 支撑非客观、可非 central；G0 是外部世界支撑；
准静态 frame 无角惯量。数值/行为通过不替代生产模型选择及真实
groom、GPU、碰撞或规模证据。生产实现等待主线程另授 write scope。

### C3 唯一实际执行 checkpoint

独立 `audit-c3-release.md` 审核实现与预算后放行。
无推进10次 H pair 成本均值 .00190251s，按每步6对保守估算156.386322s；
固定最终 H 集合实算152点。源 SHA256：
`1adea74ce4fead480f6559efb6118268d20e0b0f83c39800c28e7d39c6dcf84e`；
adapter：
`9cb30d689901cb4b0f3155f2e3fea9cf7487ec43cd6f464c87c66e464a12e136`；
新 EXE：
`59bb3c2cf08bfe80a26bfec31f2f97ad738c1e8a7196d1cb35cd6d07a2e43c99`。
运行前身份见 `identity-before.csv`，源、adapter、EXE/spec/manifest
副本均保留在该 evidence 目录；运行的是冻结 `exe-c3-001.exe`。

唯一 `run-001/` 实际 exit0，13700个完整联合步、0失败，
动力推进22.2975s，含全程指标输出25.3017765s，原始输出347355121字节。
727次 nearroot 接受；三真实残差最大依次
5.27208162e-11、9.92886025e-11、1.56460638e-11。
全步功账比最大4.94981404e-16，Rrel精确余项误差比5.18968952e-17。
152个固定最终原 H 点最小 lambda_min/SH=.000683333289，
非对称/两档差最大5.74259396e-13/8.11636961e-11。
其余最终状态仍为 H_NOT_SAMPLED，未提高覆盖声明。
pair误差最大 .00670081921m、Urel最大9.46573595e-6J。

| guide | Dmax/L | Ddyn/L | own Trec | Rtail/L | stretch |
| --- | --- | --- | --- | --- | --- |
| 0 | .010259176309059726 | .022786319011855759 | .033s | 1.65058945e-11 | .005176089434811626 |
| 1 | .010259176309059466 | .023333922784791628 | .032s | 1.65893405e-11 | .005275379448877127 |
| 2 | .010259176309060206 | .023917060894225468 | .031s | 1.22377925e-11 | .005377803381215296 |

三根静止资格与各自五项行为执行侧通过，group recovery=.031s，
仅报告。完整状态、残差、force/root/pair、能量、方向/trial、原 H、
metrics/sections/NN均保存；`c3-execution-checkpoint.json` 汇总实算。
`frozen-before.csv` 与 `frozen-after.csv` 证明列出的 C0/C2源/EXE
及公共 Math 哈希不变。独立最终重算尚 PENDING，本段不替代验收。

### C3 独立第一轮：物理通过，NN identity 差异保留

实际 `audit-c3-001/result.json` 第一轮完整重算保留总状态 FAIL，
唯一 failure 为 `NNIdentity`。独立覆盖13700步真实残差/域/连续性/
能量、41103行逐根 metrics、41103行截面、287721行 NN，
并重求全部152个固定最终 H；三根五项行为分别 PASS。
独立最大 chart/force/torque 为5.27208159e-11 /
9.92886022e-11 / 1.56460726e-11，功账最大4.94981597e-16。
727次 nearroot 的 base H/merit另审中，不据第一轮提前作完整验收。

proxy 面积比范围 .8883715673302013..1.111621239369661，
固定 NN 比 .8883255084523843..1.1116701890650813，
重选 NN 比 .8883255084523843..1.0601211011279923；
翻转/退化均0。这些只是诊断，没有质量门槛。

`audit-nn-diagnosis.json` 定位首轮唯一重建差异：
tick179、section1、guide2，CPU/exported section points选择ID0，
独立从冻结states按normalized arc重建则选择ID1。
重建点差5.55111512e-17m；距离平方差减64eps门槛分别为
+3.92103017e-19与-4.15778523e-20，落在既定判别线两侧。
执行侧另有 `c3-nn-rounding-diagnostic.md` 记录tick181的不同
求和方式边界例子，不能混成首轮tick179问题。
原始失败、全部CSV、源/EXE均保留；未放宽tie、未重跑轨迹。
当前准确结论为：执行侧数值/行为通过，独立物理/行为及固定H
符合原线，proxy identity存在舍入敏感差异，整体独立结论未通过。
vector-rest/worldfixed coupling仍仅是reference，不推广production。

### C3 独立有限放行 checkpoint

后续正式结论见 `audit-c3-checkpoint.md`；首次
`audit-c3-001/result.json` 整体 FAIL 原样保留。
独立验收明确支持 physics/behavior 有限 PASS，全部13700步物理、
域、连续性、能量及152个固定最终 H符合原门槛。
首轮脚本的numerical=FAIL是聚合标签，唯一失败断言是NNIdentity。

`audit-nn-arithmetic-v2.json` 对实际导出的123309行截面NN，
分别采用left/right/NumPy平方求和，identity mismatch均0。
tick179导出binary点的70位精度计算同样选择ID0。
因此该问题被独立判为近tie诊断在不同插值算术下的可复现性差异；
不是物理失败，也没有由重新选择掩盖固定邻居。
未改本次轨迹、导出、源/EXE或64eps门槛，不授无条件all-fields PASS。
后续版本须预先统一arc/interpolation及平方归约算术，或明确报告
无法分辨的近tie身份；不能回写本次证据。

生产模型数学gate另见 `audit-production-gate.md`。
本C3记录不授权正式资产默认enable，不替代GPU/碰撞/规模验收；
本worker继续等待主线程CPU stress write scope。

## CPU groom validation：新有限惯量模型独立验证

主线程后续明确授予 `tool/hair/rodGroomValidation.cpp`、
tool/hair CMake独立target及本段记录范围；不改CPU库/配置/schema/资产。
新增工具 `vl_rod_groom_validation` 调用 `source/hair/rod` 纯CPU库，
不链接旧C3源，C3源/输出继续冻结。
统一Material默认由CPU owner确认对应effectiveBundleV1。

预登记见 `artifacts/hair-landing-20261119/validation-prereg.md`。
固定源inventory IDs为584/33486/39210/8924/4006/5651/5660/120，
N8/16/32指点数。保留原始x0/x1 pin，N32保留所有原点；
低分辨率只在x1至tip作固定作者弧长重采样。物理总质量依重采样
折线实际长度计算，不将TFX w=1当物理逆质量，也不掩盖折线缩短。
先验24个groom几何/rest/质量/FD/objectivity与3个straight twist，
全部零时间推进；具体扰动state和逐DOF FD原始值均导出。

群体proxy记录八根样本包围盒/extent/质心、同层跨guide最近距离，
只代表这八根，不能称全头groom体积。包围盒乘积是occupancy proxy，
无guide-guide物理耦合，不因单根通过宣称体积/NN目标完成。
完整groom整体轮廓/体积保持仍未验证。

StageP已独立放行；唯一构建窗口向GPU Lagrange预约，
没有并发make。StageT原24/48草案在未运行前被CPUowner建议替换为
h=1/240,/480,/960与16/32/64 sweeps全9格、固定.025s短诊断；
该建议尚待独立门槛冻结，未执行short/perf或长run。
有限J residual由Fref=sum(m)*9.8+max(k)*L、Tauref=Fref*L报告，
不套用C3 dense最终1e-10，也不继承C3动态PASS。

### 实际 StageP 与真实groom短9格：失败保留

GPU全应用构建占用共享窗口；按主线程授权，使用同一MinGW
`-O3 -DNDEBUG -std=gnu++17` 对独立artifact目标直接编译，
不触碰共享CMake生成/obj/lib。CPU owner确认共享lib已落后；
本工具冻结全部rod头及五CPP于 `validation-build-001/hair/rod`，
命令 `compile.rsp`、源身份 `source-identity.csv` 均保存。
原工具源SHA256为
`20b1a713763f19e6cbe398a19452c462479c6e9588a315fb8425b1fd505710bd`；
EXE为 `4052d8f58887d6276c4d8cb31ec1d77a0a7a475a06c929b4ec6d07bfdf3d96aa`。

唯一 `validation-prepare-001` 零推进执行：24groom+3twist，
22项 `rigid_objectivity` 超出原1e-10门槛，其余质量/rest/FD/twist
检查通过。最大质量相对差4.14225731e-16、rest energy比
2.62578472e-32、一般扰动完整能量FD误差比1.03780292e-6。
刚体等变性最大误差比1.23065699e-9；原FAIL完整保留，
没有降低归一尺度、修改物理参数或放宽门槛。

主线程与验收明确允许在StageP FAIL保留、无enable前提下继续
短步纯诊断。运行前 `validation-short-manifest-001.json` 与
`validation-short-identity-before.csv` 冻结。唯一
`validation-short-001` 完成24groom×9格共3024步，3.1615333s；
每格authored/v0/omega0重启，固定gravity及首步起+X20m/s²。
3024步均返回Success，但不表示收敛，最终为
`NUMERICAL_NOT_QUALIFIED`。未跑长程、未做昂贵参数扫描。

| 点数 | 最大rx/Fref | 最大rq/Tauref | 最大ss缺陷 | 最大bt缺陷 |
| --- | --- | --- | --- | --- |
| 8 | .3829490291 | .0212559026 | .00394576153 | .00945608272 |
| 16 | 6.567589883 | .1353163215 | .0345807259 | .0250199738 |
| 32 | 41.00491044 | .2540567045 | .1445151218 | .0817414041 |

`validation-distribution-001/fixture-h-sweep.csv` 给出216格逐fixture/
h/sweep分布。110格逐步candidate门槛满足；1133个物理步至少一项
超标，其中rx983步、rq354步、ss1073步、bt944步，support0步。
144组端点细化中122递减、6不递减、16低于原floor而不可辨；
1133步+6不递减为工具summary的1139 unqualifiedChecks。
例如584号N32、16sweeps，h细化位置差 .01874335→.02313834，
SO3差 .03583261→.03932831；240Hz budget细化也不递减。
不能以细格局部通过掩盖粗格失败。分布已发送CPU owner及验收，
下一次同协议重验依赖算法修复的新身份。

新增只读 `validation-shape-check.py` 可比较同ID/node固定姿态CSV，
不做刚体拟合，导出质心/bbox/固定投影视廓位移、主轴/切线/转角、
NN与pair距离及由原Dmax推导的界。原短步N32粗格已实际生成
`validation-shape-short-n32-v2.json`；明确SHORT_NOT_SETTLED、
八根子集，未发静态行为或全头体积PASS。普通py缺numpy的首次
执行日志保留，实际成功使用bundled Python。无coupling只限制
物理解释，不自动判定用户静态占据体积需求FAIL；实测要按
`audit-authored-shape-metrics.md` 和完整覆盖/静止资格判断。

### Objectivity精度分解，仅静态诊断

独立build002只增加诊断导出，沿用同五CPP冻结身份和原门槛，
新 `validation-objectivity-002` 零推进仍22FAIL。
`states.csv` 包含实际transformed_general x/q，
`evaluations.csv` 包含两侧force/tau，`objectivity.csv` 保留
各分量original/transformed/expected/absolute与归一误差。
最大body torque绝对差1.70528061e-16Nm，对极小EI/L尺度的分量
归一差1.22731631e-9；force绝对差3.89129701e-13N，
归一差4.95455323e-13；能量绝对差1.87295925e-17J，
归一差1.77622973e-16。高精度独立归因尚待验收，
不能凭这些小绝对值抹掉原双精度门槛FAIL。
CPU owner另建implicitMomentumV2算法；工具不改v1库或原结果，
后续只在V2静态/短阶验证通过后接新API重验，不提前运行groom长程。

### 高精度客观性归因与后续接口准备

独立 `audit-objectivity-highprecision-v2-result.json` 已实际用70位计算
4006N16、584N32、4006N32。归一化输入q后进行精确共同刚体变换，
body torque归一差<6e-65，数学客观性 PASS。
对工具实际double transformed x/q重新用70位求能量/力矩，
差分别为1.13867e-16/1.72331e-16/9.75592e-17Nm，
归一1.24404e-9/1.61332e-10/1.07015e-9，
复现原双精度门槛失败量级。主要归因为共同变换输入量化后，
短段world位置差分与极小EI归一放大，不能类比worldfixed非客观能量。
数学PASS与原严格double gate的22FAIL分开记录，原证据不改；
如需root-local中心化精度修复由CPU owner另作实现并独立验收。

固定姿态脚本已验证authored自身输入，质心/bbox误差严格0；
对原短步N32粗格的 `validation-shape-short-n32-v3.json`：
质心位移 .00237763471m、bbox极值误差 .00406302829m，
占据bbox体积proxy由 .02115575844m³ 至 .02117325942m³。
此时Dshape不是所有guide满足原静态尺度，且没有静止资格；
不能因体积proxy接近就宣称形状保真，也不把短期激励状态当静态失败。
脚本新增numpy bool序列化修复，原失败日志保留；成功结果为v3，
此前v2名称的文件不当作有效最新结果。

工具已预留 `--solver implicitMomentumV2`，调用新CPU API，
单独记录Newton/backtrack/internal/linear residual；XPBD defect导出NA，
不作0或FAIL。V2没有XPBD sweep预算，拟只沿原h三档/非零激励/
相同24fixtures运行，需先完成CPU静态Jac/block及N3/N8短阶gate。
该接口准备尚未运行V2，也不授权长run。

## V2真实groom与固定细参考实际结果

`validation-build-003` 冻结新implicit六CPP/头与工具后，
`validation-v2-short-001` 实际完成24fixtures×三h共1008步，1.5437289s。
所有status/pin/domain与true vector残差过原线，最大rx/Fref
1.38129020e-8、rq/Tref1.35331972e-9，internal9.98642856e-9，
最多4Newton；旧XPBD stationarity为NA。
原总体 `NUMERICAL_NOT_QUALIFIED` 保留：仅4006号N8位置相邻
细化差7.39785080e-5→7.54762265e-5不递减，SO3递减。
独立 `audit-groom-v2-short-result.json` 全1008步重算确认真实残差/
能量/速度；没有第二次轨迹优化。

按用户新指令先登记 `validation-v2-phase-prereg.md` /
`validation-v2-phase3840-prereg.md`，只补该固定fixture的
1920Hz48步及3840Hz96步，完全同旧物理/初态/载荷，
不重跑240/480/960。完整误差向量、共同六时点、root轴分量、
右log旋转、速度/转到同一body的omega与world动量均导出。
相对3840参考的位置L2/L为 .0002352666493/.0001627218377/
.00008848664357/.00003481575666，SO3也依次递减；
1920-vs3840只是参考不确定度估计，不是严格误差上界。
独立 `audit-v2-reference-result.json` 核对，保留原nondecreasing，
不将其称作算法发散，也不放宽原判别或拟合相移。
没有运行线性V2完整长程，最终新profile不继承其动态结果。

## Progressive V3：实际新profile验收

### 身份、短步与真实空气失败

统一 `BuildProgressiveSupport(rest,Material{})` 明确创建新数据：
core线性k1与原Material默认不变，额外k2/Fw由冻结20m/s工程额定
公式派生；不把20m/s² probe冒充风。`validation-v3-prereg.md`
登记新profile/解析切线/短步/长程功账及预算。
`validation-build-006` 冻结八CPP、所有rod头、共享
`hair/tressfxSolverMath.h` 与工具；实际EXE SHA256：
`beddfb7b4d9f554ae2efe168de3c12e359c4af70113ad99e5c7d7a3749a473de`。
CPU小规模84步与pure/analytic门禁独立放行后才执行真实groom。

`validation-v3-prepare-001` 仍保留原double客观性22FAIL，
不把70位数学结论冒充严格双精度门槛通过。
`validation-v3-probe-short-001` 完整1008步，1.3176105s，
真实rx/rq/status/domain/pins均满足，仍只有4006N8位置相邻差
5.93795351e-5→6.86289445e-5不递减。
运行前已登记的1920/3840参考分别48/96步，同profile同probe。
相对3840位置误差依次 .0002095775842/.0001519200015/
.00008441632847/.00003360518578，SO3也递减；原标签保留。

独立的 `validation-v3-air-short-001` 采用真实world+X20m/s空气，
每步old velocity/old tangent及共享unsaturated drag helper。
首次尝试799步，其中798接受；8924号N32、240Hz首步返回status3，
9Newton、49累计backtracks、internal=.003805288956，
linear solve residual1.7053e-13。异常停止，未跑空气长程。
早退diagnostics的physical零值是未填充，不是合格残差；
state按库契约回滚。CPU owner随后使用完全同固定一步/external
CSV作 `air20-failure-trace-001.json`，导出真正failed candidate、
完整块矩阵、方向、trial；不改物理/阈值或以rollback冒通过。
空气globalization修复与独立验证仍是后续依赖。

### 唯一+20acc正交长程：数值过、15项可见响应不足

主线程明确允许空气故障修复期间执行该独立行为诊断；
独立 `audit-progressive-probe-long-release.md` 同时审过代码功账。
运行前 `validation-v3-probe-long-manifest-001.json` /
identity冻结。唯一 `validation-v3-probe-long-001`：
24fixtures共77760步、111.085417s，原始958586441bytes，
在900s/4GiB预算内。exit2是行为失败，非数值中断。

数值全部通过：maxrx/Fref1.41475001e-8、
maxrq/Tref1.98209005e-9、internal9.99839953e-9，
功账比1.62606512e-11，账与实际全DOF残差功差比1.08505491e-16。
Iangular明确为signed离散项，未称正耗散或机械能逐步下降。
每步old/new、真实残差、外力、能量、root/domain/qnorm均保存。
Windows打开文件时目录Length元数据可能滞后；本次关闭后的实际
输出已核在预算内，工具后续版本改用stream.tellp累计在写字节，
不覆盖该已完成身份或改变物理。

24项均通过静止资格、Dmax、Trec、Rtail和stretch；
15/24仅因Ddyn低于原.02L下界而FAIL，未改门槛。
三个分辨率均为同5个ID不足：584/33486/5651/5660/120；
39210/8924/4006各三个分辨率的完整五项通过。
例如584号N32 Ddyn=.01496406375L，4006号N32=.02117019532L。
这描述+20m/s²工程探针，不代表最终20m/s风响应。

只读 `validation-long-pose-extract.py` 单次扫描原raw提取共同
3s static、13.5s final和每fixture自己的peak。
peak_per_fixture不是同一物理时刻的群体姿态，不能拿它做全束截图。
3s的八根子集在N8/16/32按原Dmax及导出静止资格支持静态几何
执行侧有限PASS；需独立重算，不代表全头/follow。

| 点数 | 质心移位/m | bbox极值误差/m | 占据bbox体积比 | 最大主轴角/rad |
| --- | --- | --- | --- | --- |
| 8 | .0016080014 | .0048597813 | 1.0095470768 | .0106133401 |
| 16 | .0019468582 | .0050306508 | 1.0111091864 | .0098843793 |
| 32 | .0020609232 | .0050721682 | 1.0113959415 | .0102619826 |

这些占据形状数据不等于物理体积定律；没有因为缺乏guide coupling
而自动判用户静态形状目标失败。局部短段切线/turn误差也原样导出，
不只报bbox掩盖局部变化。数据位于
`validation-v3-probe-static-shape-n8/n16/n32.json`。

### CPU性能基线，仅诊断

按新manifest唯一运行 `validation-v3-perf-001`：2000个N32实例，
循环使用固定8模板，各一步+20acc，未测GPU或runtime图形/碰撞。
全部status与物理门槛过，wall6.7727226s，
solver phase .6725684s，即336.2842us/guide-step。
该冻结build006的phase包含一行solver telemetry格式化；
manifest运行前已注明，不冒称纯kernel时间。
工具后续版本把计时截止移到日志前，后续修复若重测须新身份；
不能据此宣称2k GPU预算或生产性能达标。

## 运动学惯量纠正：旧工程J不作最终物理通过

独立物理复核发现旧director `ms*(ell²/12+r²/4)` 将长度方向
中心线转动能重复加入q，而x节点质量已经承载该部分运动学。
此前仅核圆柱惯量公式而未核DOF分配的认可撤回。
旧源/EXE/输入/数值功账保留，其结论限定在declared engineering
kinetic model，不作正确物理惯量的最终PASS。

生产候选显式新工厂 `BuildSectionInertiaRestState`，版本2，
`Jsection=ms*(r²/4,r²/4,r²/2)`。旧BuildRestState默认不偷换。
rho/r/EA/EI/masses/rest/support/profile均不改；
平动lumped mass的O(ell²)积分误差另列，不称consistent mass。
纯q轴向Jz不变，因此不能预先宣称该纠正必然修复twist/pi故障。
独立推导与实际5tests见 `audit-section-inertia-release.md`、
`audit-inertia-quadrature-result.json`，原文/KB实际阅读溯源见
`audit-source-traceability.md`，不虚构已提前读过尚未读的节点。

旧J continuation AIR短1008步全部求到真实root，但11项h误差
非递减，原标签保留。旧J真实air长管理授权诊断后实际接受10448步，
8924N8 tick729失败；旧模型高Hz11case计划只登记/构建，未执行，
停止继续旧J长程/剩余case。见
`validation-old-kinetic-retirement.md`。

### 新工厂静态与短步实际执行

`validation-section2-prereg.md`、build009新source/EXE身份冻结；
命令必须显式 `--kinetic-math-version 2`，调用新工厂，
不是在工具中直接修改J并换标签。
`validation-section2-prepare-001` 逐段J/mass/l原值和独立公式导出，
所有sectionJ与刚体动能分配检查过线；原严格double objectivity
22FAIL仍保留。独立consistent centerline+section rotary等于圆柱
连续体能量，lumped与consistent差精确符合
`sum ms*|v1-v0|²/12`，没有把mass quadrature误差混入q。
`validation-section2-preservation-001.json` 确认除Jxy/invJxy外，
所有rest字段与progressive profile逐值不变；Jz保持原值。

corrected小fixture84步独立先行通过后，新24groom每项三h各跑
1008步probe与AIR，均status成功，真实残差/域/pins符合。
原相邻h refinement分别1项与10项nondecreasing原样保留；
独立 `audit-validation-section2-probe-short-001-result.json` /
`audit-validation-section2-air-short-001-result.json` 全步重算，
AIR每步实际oldv/old normalized-edge tangent力也独立核对。
这些短结果支持诊断执行，不是最终时间精度或生产准入。

### 新J空气长程：优先case与完整24项诊断

主线程明确授权先跑8924N8（旧tick729故障）与8924N32，
再其余22项；不等待更细h分析或以形式release空转。
新build010只改工具case顺序、独立case失败隔离与失败状态导出，
数学源仍冻结build009。每case独立authored初态，h240与
3s静态/.5s真实20m/s空气/10s恢复完全不变，累计900s/4GiB。
numerical失败停止该case，不推进失败状态，其他fixture继续。
已完成case不重跑；精确失败candidate/old/external与summary路径
写入逐casecheckpoint，优先两项完成立即写priority-checkpoint。

`validation-section2-air-long-priority-001` 实际尝试全24项，
95.4819391s、876193642bytes，接受67929步；20项完整3240步，
4项因原能量精度门槛失败保留。无遗漏case或原参数更改。
优先8924两分辨率均完整通过数值，旧N8tick729失败未重现：
新该步max adjacent log1.38057125rad、net step .0610054rad，
全程max step .10121rad；N32全max step .340727rad。
这只是实际观测，不将Jxy纠正对Jz未变的twist问题作一般保证。

两优先项仍有原行为FAIL：N8 Ddyn=.08947544L、
stretch=.02052552；N32 Ddyn=.09192167L、stretch=.00883730，
两者恢复时间 .0958333s。完整20项中6项五项行为全过，14项失败，
具体上界/伸长分布在 `validation-section2-air-distribution-001/`；
不改.08或1%门槛、不把这些数值诊断提升最终品质PASS。

| 失败fixture | tick | Step status | internal残差 | 原B/SB |
| --- | --- | --- | --- | --- |
| 39210 N8 | 723 | Success | 7.99326877e-9 | 2.16330253e-10 |
| 5651 N8 | 724 | Success | 8.00647905e-9 | 2.37387900e-10 |
| 39210 N16 | 843 | Success | 3.68205638e-9 | 1.18305579e-10 |
| 39210 N32 | 843 | Success | 4.58511564e-9 | 1.29213291e-10 |

这四项均满足root/域/vector residual，但现internal1e-8停止精度
不足满足已经登记的功账1e-10；B与实算全DOF残差功吻合至
3.6e-17归一尺度，不是账漏项，也不是pi域失败。
原FAIL与candidate完整保留，CPU新work-guard修复另审，
不能直接放宽B门槛。全部候选路径在
`validation-section2-air-long-checkpoint-001.json` 和case summaries。

### 新J加速度probe长程正交证据

同一新物理身份按先前主线程授权独立执行
`validation-section2-probe-long-001`，完整77760步、109.4674126s，
数值通过，15项原行为失败保留。它是+20m/s²外加速度，
不冒充20m/s空气，不将旧J长程结果迁移到新工厂。
独立审计/完整质量、真实body contact、full-head/follow、
GPU精度与生产性能仍分别验收；本worker未修改runtime/shader/
配置/schema/资产，也没有commit。

### 新J后续只读与CPU吞吐诊断

固定3s静态姿态从24case原raw逐一提取，包含后续因功账失败的
四case（它们都已接受至3s）。新
`validation-section2-air-static-poses-001` 恰有24fixtures/448点，
所有点同物理时刻3s，非每根自选peak拼成群体。
N8/16/32作者对应形状均满足原Dmax尺度；aircase占据bbox体积比
约1.00954708/1.01110919/1.01139594，质心移位约
.0016080/.0019469/.0020609m。只代表八根子集，
没有以此取代完整头发/follow或动态精度验收。

`validation-section2-probe-poses-001` 另提取完整probe轨迹，
配合导出的静止资格做3s对应几何检查，三个分辨率均为执行侧
subset静态几何PASS；单姿态本身不发静止行为PASS。
`validation-section2-air-ledger-report-001` /
`validation-section2-probe-ledger-report-001` 是只读导出账代数核对，
明确不是独立force重建；失败candidate从accepted账统计排除。
独立force/动量/真实air重建由验收agent另行输出。

按独立新manifest执行correctedJ CPU诊断2000个N32实例，
八模板循环、各一步gravity+20m/s²probe：
`validation-section2-perf-001` 实际无状态/物理门槛失败，
solver-only4.6511835s、2325.59175us/guide-step，
全raw诊断I/O含内wall11.4981891s。编译身份build010，
计时截止在solver telemetry之前。该数字不是GPU、fullhead、
碰撞或runtime2k性能宣称，不用它修改h/profile求好看预算。

### 残差功精度修复准备，四失败case不擦除

独立 `audit-section-air-work-failures-result.json` 全量核对四原始
old/candidate/airforce/ledger，确认上述B失败是原component残差
停止精度对功账预算不足，不是模型换风或pi失败。
新 `ResidualWorkCertificate` 保留原component norm1e-8、
12Newton/stage、最终B/SB1e-10与identity1e-10，
并要求finalfullforce候选
`sum_axis |rx_axis dx_axis|+|rq_axis theta_axis| <=5e-11 Eref`；
乘积/累加nextafter向上只对已计算double残差/增量作保守界，
不冒称原force评估的完整区间证明。仅t=1stage作物理work检查。

新显式WorkGuard入口与旧norm-onlyContinuation分开。
review曾发现draft旧wrapper误接新guard，CPU在编译执行前修为
传nullcertificate；旧已运行EXE/源snapshot不变。
工具新build011/012包含显式新solver名、每步certificate字段、
固定四case新目录与300s/1GiB预算。
build012只额外编译candidate数组边界校验修复，
其余复用obj路径在link.rsp逐一记录，EXE SHA256：
`41320fd9b14b3288b2b3a2e577df484b481246e04fdcefb827b6a3d9937a651a`。
当时状态仅“编译就绪”，尚待CPU四exactstep重放与独立audit，
未以pure4tests通过替代真实失败步验证，未提前补跑四长case。
后续若放行，四项均从authored按原13.5s重启，新identity不嫁接旧
prefix；20个已完成case继续冻结，综合覆盖须保留多身份来源。

### WorkGuard四项真实长程闭环与多身份覆盖

CPU四exactstep真实重放及独立物理/ledger核对后，
`audit-workguard-four-release.md` 放行，不以pure tests替代。
唯一 `validation-section2-work-air-long-001` 从四authored初态
重新执行12960步，17.7069095s；四项完整3240步数值均通过，
原B失败消除但旧失败目录不动。maxB/SB2.55730844e-11、
ledger与残差功差比6.48775487e-17，internal仍原1e-8以内。
独立 `audit-validation-section2-work-air-long-001/result.json`
全12960步真实air/动力学/能量/原行为重算通过。
新四项行为均FAIL，未为修复账目调整材料/空气或行为线。

`validation-section2-combined-air-coverage-001/result.json` 明确只读组合：
原20完整norm-onlyContinuation histories + 新4WorkGuard histories，
24项各3240步，共77760实际物理步的完整覆盖。
没有伪称一次同EXE的77760步新run，没有把新结果回写原四FAIL。
原20项中14behaviorFAIL、新4再加4FAIL，合计6PASS/18FAIL。
旧短probe1/AIR10 nonmonotonic flags与时间精度未准入状态保留。

### 全24实际空气：毫米、相机投影与伸长

报告 `validation-section2-air-all24-report-002.md` 包含逐fixture表；
原始JSON/CSV与压缩全轨迹在
`validation-section2-air-actual-metrics-001/`。
001版Markdown首次因Windows默认GBK写入失败留空，
002显式UTF8成功，不引用空文件冒已有报告。
相机使用实际 `fullhead-wire-001/current.bin.json` 的VP/model及
startup1280×720尺寸，CPU固定source pose中心线投影；
不包含occlusion、不称GPU已观察的全头/动画可见性。

| 实际量 | 24项范围 |
| --- | --- |
| 静态作者偏移 | .442260..5.706652 mm |
| 静态固定相机投影偏移 | .399227..4.839234 px |
| 动态最大位移 | 2.800653..48.202419 mm |
| 动态固定相机投影位移 | 2.431641..40.644799 px |
| 永久恢复后缀Trec | 0.033333–0.104167 s |
| 最后一秒tail最大位移 | 1.4433e-7..3.8376e-6 mm |

全部24静止资格、静态Dmax/原恢复/tail门槛满足；
原动态工程尺度与伸长FAIL保留，不用增加k2将报告刷为PASS。
.08L是冻结单节点静态额定尺，不是瞬态必不超的动力学定理；
用户“风下有界可见、回正”的实测判断需上述毫米/像素、时间误差
与最终runtime实际观察共同支持，不能用旧工程单护栏代替。

N32真伸长FAIL有两根：

| guide | 最大伸长 | long peak | segment |
| --- | --- | --- | --- |
| 39210 | 2.15388194% | 3.0125s | 1 |
| 4006 | 1.22786965% | 3.05s | 24 |

最坏39210N32在既有corrected AIR短240/480/960Hz中最大伸长
分别2.0559621%/2.1309480%/2.1508722%，物理root残差都过线。
短grid从authored立刻加风，long从3s静止态加风，明确不同历史，
不将短窗口当全long时间收敛证明；但峰值随细化未降至1%以内，
该事实必须由CPU/model审查实际轴向force/EA与工程材料/时序，
不能只甩旧Ddyn护栏，更不能凭数值残差小说真实伸长目标完成。
`validation-section2-air-short-stretch-001` 保存全部72格伸长原值。

### 固定两根高Hz时间误差与语义profile依据

主线程要求先关键长程后做固定参考，优先及完整24项完成后，
只对预选33486N32与120N32增加1920/3840Hz短窗，共288步，
完全同newJ/力/材料/continuation，不重复原240/480/960。
数据 `validation-section2-air-ref1920-001` /
`validation-section2-air-ref3840-001`，完整误差向量/旋转/速度/
能量在各guide的 `validation-section2-air-phase-*-002`；
001旧分析保留，新002额外明确max-node与L2不混比。

33486N32相对3840的位置L2/L依次
.0615190/.0478346/.0286275/.0116162，SO3亦递减；
120N32为 .0427595/.0315792/.0189815/.0079308。
不能把32点L2直接与max-node的.015L静态线比较：
240vs3840实际max-node误差分别 .01420404L/.01294565L，
1920vs3840的max-node不确定度估计 .00268489L/.00245091L。
参考差不是严格误差上界，且此为.025s动态startup而非静止窗口。
这些数据支持“绝对误差朝细h下降而有限h未渐近”，不解除240Hz
时间精度疑点或原nonmonotonic标签，不继续盲扫h求PASS。

`validation-final-profile-evidence.md` 汇总实施依据而非新增方案：
作者选择统一语义profile，物理参数、correctedJ版本、quartic、
真实drag与frame/cache/reset语义须一起固定；没有raw救援knob，
不让旧生成rest/frame/follow缓存冒新model身份。
不擅自改schema/token/default资源enable，最终模型/风/碰撞/
fullhead/GPU准入由主线程与独立验收分别作实证判定。

### 独立伸长与时间精度裁决

独立 `audit-section2-model-and-time-decision.md`、
`audit-section-stretch-result.json` 已实际核对最坏N32候选，
不是只相信本工具的summary。
39210N32材料轴向strain=.02153787639、轴向resultant=.01691580856N；
4006N32分别.01227869644与.00964366563N。
冻结EA=.7853981634N，1%应变量对应.00785398163N。
director剪切分量相对小，实际几何伸长主要就是有限EA轴向伸长，
不是q frame/norm误用或把component residual当力矩的实现错误。
物理root/账过线不解除这项原1%要求。

独立288高Hz参考全步也核对了actualair、完整force/torque、
correctedJ/能量/速度与误差向量。它确认绝对误差朝细h下降，
同时确认240Hz时间精度尚不合格，不能拿float局部root追精度来
治约1e-2L量级的时序误差。

这两类问题必须分开：如果最终仍要求该20m/s额定下≤1%伸长，
需要独立有依据的材料识别或明确另一个strain-limit模型及反力/
能量/CPU-GPU门禁；当前没有默许为过图调EA/r/k2。
CPU提出隐式同一drag律仅获得独立公式/FD小oracle许可，
不自动构成时间误差或真实有限EA伸长的修复，也不替代接触、
fullhead固定相机实际运动、profile默认准入裁决。
本worker仅整理既有模型事实与单语义profile依据，
未扩写新模型、未把旧FAIL抹为PASS。

## 管理收尾基础快照：模型、证据与未放行边界

2026-11-19，CR 窗口释放后的资料快照，**不是最终 runtime 准入**。
以下把关键公式、选择和实际失败写在可版本管理的文档中；`artifacts/` 是原始证据定位，
不是这些结论唯一的正文。文中相对 `artifacts/` 路径均从代码仓库根解析。
后续实际运行须追加新身份和结果，不能改写前面已冻结的轨迹。

### 推导树及来源边界

| 层次 | 来源与本地选择 | 可重建入口 |
| --- | --- | --- |
| 中心线/材料帧运动学 | Kugelstadt–Schömer 2016 [Position and Orientation Based Cosserat Rods](https://animation.rwth-aachen.de/media/papers/2016-SCA-Cosserat-Rods.pdf) 提供位置/段材料帧结构。下述完整 log-vector 差能量、非等长权重、截面惯量及积分器是分别声明的本地选择，不冒充论文逐式复现。 | `source/hair/rod/rodMath.*`、`rodTypes.h`；本仓库早期 [R1 记录](hair-rod-ghost-r1-20261118.md)。 |
| 退役局部投影路线 | Macklin–Müller–Chentanez 2016 [XPBD](https://mmacklin.com/xpbd.pdf) 的柔度/累计乘子结构；Müller 等 2020 [刚体 XPBD](https://matthias-research.github.io/pages/publications/PBDBodies.pdf) 的位置/方向投影结构。有限局部 sweeps 不等于完整隐式动量求根。 | `rodXpbd.*`；前文 V1 短格失败和 `artifacts/hair-landing-20261119/repair-formulas-v2.md`。保留 `cosseratXpbdV1` 历史候选，不能把后来的 Newton 藏进这个名字。 |
| 当前 CPU 结构化动量路线 | 完整 BE 平动与最终 body 表达的角动量差，完整弹性切线；每根 guide 的非对称 6×6 块三对角 Newton。fixed-old/same-h continuation 是全局化 seed，不是九个物理子步。 | `rodImplicit.*`、`rodAnalytic.*`；下文列出实际方程与判据。 |
| 作者形状支撑 | 从 [M0 实验记录](hair-rod-m0-experiments.md) 的软核心/渐进支撑思路形成当前 root-local 外部 actuator；各向同性 quartic 的力、Hessian、驱动功在本地直接求导。不是杆内力、密度或体积约束。 | `rodRootSupport.*`、`rodProgressiveSupport.*`，`artifacts/hair-landing-20261119/progressive-support-formulas.md`。 |
| 截面转动惯量 | 对截面点 `r(s)+R(s)a` 的速度平方积分，截面质心使交叉项为零。中心线长度运动已由 x 质量承载，q 只补截面 rotary inertia。 | `rodSectionInertia.*`；下文 J 纠正与解析反例。不是按行为拟合惯量。 |
| 空气与下一候选离散 | 仓库既有工程 `Cd=1.5`、空气密度 1.225 kg/m³，加上局部圆柱横向投影阻力的本地推导。旧状态采样已实跑；同一阻力律的端点隐式采样只有公式/小 oracle 授权。 | `rodProgressiveSupport.cpp::ProgressiveAirDragForce`；`artifacts/hair-landing-20261119/audit-section2-model-and-time-decision.md`。 |

C3 的 world-fixed vector-rest 耦合、准静态材料帧和 45DOF 联合轨迹属于另一模型。
其 13,700 步 physics/behavior 有限 PASS 与原 NNIdentity FAIL 同时保留；
它不为当前独立 guide 的有限惯量、空气、全头体积或碰撞提供继承 PASS。
本次未改知识库、Cloth、C3 或任何历史冻结源码。

### 当前方程、工程材料与语义身份

`x,v` 使用 world SI，`R` 是 material→world，`omega` 为 body 角速度，
右扰动 `R <- R Exp(eta)`。以原作者段长 `l`、相邻平均段长 `lbar` 定义：

```text
nu = R^T (x_next-x) / l
Uss = .5*l*(nu-nu0)^T S (nu-nu0),  S=diag(GA,GA,EA)
phi = Log(R_left^T R_right)
Ubt = .5*(phi-phi0)^T (B/lbar) (phi-phi0), B=diag(EI,EI,GIp)
```

其中 bending 使用 `phi-phi0`，不是 `Log(Q0^-1 Q)` 或 quaternion 虚部近似。
扭转输入不能由中心线唯一确定；TFX 默认材料帧的平行传输/零 rest twist 是声明的
补充假设。角分支域、q/-q 等价、完整右 Jacobian 和 body 几何项必须保持一致。

版本化作者字段仍是 `solver="cosseratImplicitV3"`、
`solverProfile="effectiveBundleV1"`；Rod runtime 配置要求显式 GPU backend，
CPU/missing-backend 不能掉进旧 GuideSimulation。历史 `tressfx` 默认及 baseline JSON
保留；Rod 拒绝原 stiffness/damping/iteration 等 raw override，含继承的 group raw 混用。
未知 solver/profile 值拒绝。kineticMathVersion 是内部模型/cache/reset 身份，
不是新增 artist knob；新 J 结果不继承旧 kinetic 身份的 PASS。
工具输出中的 `effectiveBundleProgressiveV3`、continuation/WorkGuard 名称标识诊断身份，
不是新增 JSON profile aliases。已测试的解析/保存边界见
[导入契约](../contracts/tressfx-asset-import.md) 与
[资产修改契约](../contracts/asset-modify-api.md)；以下动态选择不写成这些契约的已放行行为。

`source/hair/rod/rodTypes.h::Material` 当前工程假设为
`r=.0005 m, rho=1300 kg/m³, E=1e6 Pa, G=.4e6 Pa, delta=.01, zeta=.5, gref=9.8 m/s²`。
这是 effective bundle，**不是人发实测材料识别**。`A=pi*r²`、
`I=pi*r⁴/4`、`Ip=pi*r⁴/2`；不开放裸 delta/zeta/EI 旋钮救援失败。

root-local 作者支撑以 `y=a+Rroot*r_local`、
`ydot=adot+Omega_world×(Rroot*r_local)` 定义。软核心与渐进项为：

```text
d=x-y
k1_i=m_i*gref/(.01*L)
c_i=2*.5*sqrt(m_i*k1_i)
Ut_i=.5*k1_i*|d|²+.25*k2_i*|d|⁴
fs_i=-(k1_i+k2_i*|d|²)*d
Ht_i=(k1_i+k2_i*|d|²)*I+2*k2_i*d*d^T
fd_i=-c_i*(v_i-ydot_i)
Pdrive=sum_i (fs_i+fd_i)·ydot_i
Dsupport=sum_i c_i*|v_i-ydot_i|²
```

`beta=rhoAir*Cd/(pi*rho*r)`、`aDesign=beta*20²`、
`Fw_i=m_i*aDesign`、`dDesign=.08*L`，
`k2_i=max(0,(Fw_i-k1_i*dDesign)/dDesign³)`；固定位置 pin 的 k2 为零。
`.08L` 是单节点静态额定尺，不能证明整杆瞬态位移必小于它。
阻尼沿软核心派生，不随瞬态伸长重拟合。20 m/s 空气与 20 m/s² probe 是不同载荷。

### J 纠正、BE 已实现部分与 implicit drag 待验部分

每段 `ms=rho*A*l`，各半 lump 到相邻 x 节点；生产显式入口为
`BuildSectionInertiaRestState`，内部 `SectionInertiaKineticMathVersion=2`：

```text
T=.5*sum_nodes m*|v|²+.5*sum_segments omega^T Jsection omega
Jsection=ms*diag(r²/4,r²/4,r²/2)
```

旧 `BuildRestState` 的 transverse q 惯量另含 `ms*l²/12`，在该 x/q 自由度分配下重复
中心线长度转动，故撤回其“生产物理惯量”认可，保留为 engineering kinetic 历史身份。
单段绕中点横转时，x 端点 lump 给长度惯量 `ml²/4`，连续值是 `ml²/12`；
再给 q 加旧长度项会变为 `ml²/3`。纠正 q 后，x 的 lumped quadrature 仍有误差，
固定总长细分时为 O(l²)，不能宣称 consistent mass 已实现。
轴向 `Jz=ms*r²/2` 没变，不能许诺旧 twist/时间误差自动消失。

新 J 独立五项惯量测试和七项短步、七项 contact 小 oracle 分别有实际通过记录；
`audit-section-inertia-replay.log/xml`、`section-corrected-short-tests.log/xml`、
`audit-section-contact-replay.log/xml` 定位原证据。root 三项独立测试使用新入口，
共同固定 SE3、Galilean 及 1 Hz 非零支持功诊断通过；CPU owner 已把单个
`tool/hair-tests/rodRootMotionTests.cpp` 接入 hair CMake。它们不等价于全头 runtime 根运动通过。

**完整动量 BE 已实现；即将核验的是同一 drag 律的端点隐式采样，而不是把 BE 说成尚未实现。**
令 `theta=Log(Rold^T Rnew)`、`vnew=(xnew-xold)/h`、`omeganew=theta/h`：

```text
rx=m*(vnew-vold)/h - felastic - fs + c*(vnew-ydot) - mg - fdrag - fother
rq=(J*omeganew-Rnew^T*Rold*J*omegaold)/h
   - tau_elastic_body - Rnew^T*tau_external_world
```

rq 已包含最终 frame 表达下的动量输运，不能再额外加一次 gyro。
残差切线一般非对称；CPU 带状 Newton 不是 SPD/Gauss–Newton 或旧 XPBD。
固定 old 的九级 continuation 只改变虚拟载荷并 seed x/q，最终 fraction=1 解完整目标，
只推进一次 h；各级保留原十二次 Newton 预算，不能把累计迭代写成物理子步。
能量账保留 BE 平动缺陷、signed 角离散余项、支撑/外力功和实际残差功；
不宣称任意受驱杆机械能逐步下降或离散账天然精确为零。

已执行空气轨迹采样 old velocity/old tangent。下一候选冻结单位切向 `t`，
`P=I-t*t^T`、`w=P*(u-vnew)`，同一力律及导数是：

```text
fdrag=m*beta*|w|*w
Dfdrag/Dv=-m*beta*(|w|*P+w*w^T/|w|)       (w!=0)
D(-fdrag)/Dx=(m*beta/h)*(|w|*P+w*w^T/|w|)
w=0 时连续导数为零
```

它来自速度耗散势 `(m*beta/3)*|P*(v-u)|³`，不改变 Cd/rho/r/风速/材料，
只给局部 x 对角增加对应导数。当前切向若也取新位置，必须另推邻点导数和功账；
本次尚无这种准入。端点 drag 与冻结切向仍是一级离散，不能当二阶积分器，
也不能先称已修复观察到的相位/有限 EA 伸长。
截至本快照，仅放行独立公式/FD 小 oracle；实现、真实轨迹、CPU/GPU 一致性结果等待另次追加。

### WorkGuard 修复和仍不满足的实测目标

固定 root、无接触时，既有账满足
`B=sum_free rx·dx+sum_free rq·theta`。小的最大 residual component 不自动保证全 DOF 功小。
WorkGuard 对计算得到的分量积作 nextafter 向外舍入，
`Wabs=sum |rx_axis*dx_axis|+sum |rq_axis*theta_axis|`，
只在最终 continuation fraction=1 要求 `Wabs<=5e-11*Eref`；
原 `maxComponent<=1e-8`、`abs(B)/SB<=1e-10` 和原迭代预算不变。
这界定计算输入的残差功，不是整个浮点 force evaluator 的严格认证。
移动 root/contact 的约束反力功须另推，不能继承此固定-root certificate。

四个实际失败旧状态 39210/N8/t723、5651/N8/t724、
39210/N16/t843、39210/N32/t843 已由独立执行/重算确认修复：
最大 component `9.29099e-13`、`abs(B)/SB=5.47619e-17`、
`Wabs/Eref=5.99724e-14`。四项新身份长程 12,960 步也完成独立账核。
原二十项与新四项只是多身份只读覆盖，共 77,760 物理步，**不是一次新 run**；
原四 FAIL 不删除。最终行为仍是 **6 PASS / 18 FAIL**，原动态尺度/伸长要求未通过。

真实 N32 峰值伸长：39210 为 2.15388194%、4006 为 1.22786965%。
独立重算的轴向力 `.01691580856/.00964366563 N` 与
`EA=.7853981634 N` 的有限轴向应变相符；1% 对应 `.00785398163 N`。
这不是 q frame 错误，也不能用“数值 residual 小”替代原 1% 目标。
33486/120 的 240-vs-3840 动态 max-node 位置差分别
`.01420404L/.01294565L`；1920-vs-3840 差只是参考不确定度估计。
隐式 drag 是否改善这些量仍待实测，不调 E/r/k2 或放宽线来刷 PASS。
全头静态 silhouette/centroid/volume 仍待真实覆盖；无 guide coupling 不等于该用户目标必 FAIL，
也不自动扩成完整自碰撞需求。

### 几何选择：1 cm 矛盾、.5 mm profile 与 SDF-primary

源 guide584/point2 的原 trilinear `C=d-.01 m` 为 `-.00863547558310539 m`。
作者允许球半径 `epsilon=.015L<=.0006893167813515388 m`；
覆盖两 cells 的保守梯度界 `K<=1.03452391707` 给
`C0+K*epsilon=-.00792236088636 m<0`，并有精确 Fraction 核对。
所以**该原 1 cm 场与该作者偏移上界不相容**，不能靠 J/Newton/支撑调参修复几何矛盾。
源 TFX SHA256 `73fa1f3bce4a060f4876c3286a5ec29da5f985c793c9e03f81f5f16340dc39c7`，
原 SDF SHA256 `025583f09a728417c9cad6d5f917aebd23a211e2ad6f3ab072285242ffce01a2`，
证书 `artifacts/hair-landing-20261119/sdf-authored-shape-certificate-584.json`。

管理选新隔离几何：profile 的物理 r=.0005 m 对应 scene `mesh.margin=.05 cm`。
raw SDF d/g/H 不预减 radius，统一 gap `C=d-r` 只减一次；
capsule 保留 body radius，用 `distanceToAxis-Rbody-r`。旧 baseline 1 cm 与 combined 失败保留。
source39210 的粗 Head capsule 另有作者 .015L 矛盾，允许新 SDF-primary 场景只去除四个粗胶囊；
这不证明其他 capsule 数值失败无解，也不把新场景改称“旧场景修好了”。

离线原身体场 `female_head.sdf` 与 runtime proxy/skinned grid 是不同近似。
77,092 个源表面顶点的采样 |d| 最大 2.376 mm、p95 约 1.187 mm，大于 .5 mm radius；
这是采样误差，既非均匀界也非允许穿皮量。surrogate-field contact PASS 不能等同真实皮肤零穿透。
新静态/动画 SDF-primary 场景 hash 分别为
`106d084f9f5bda325d85f4f24c1b8cfde97a56db9a0f373e47b1c32f72d48267` /
`e5244db023899629b88d351a5291e2f2fc9fb2ab7d5236e64e2a10d589ec5372`。
共享 TF 描述符仍是未放行的 V1 诊断身份，不能据场景生成提前宣称 V3 production 已启用。

实际 SDF-only 八根各八步和八根各 3,288 静态步均成功，
后者 26,304 accepted steps、最大 normalized residual `9.853e-9`、Dmax/L `.01237865`，
但全程 active count=0，因此只是 inactive controls。
一致平移的 head 八步 controls 同步变换 root/pins/support/SDF，
pin/body 相对坐标误差 `5.554e-18 m`；未验旋转、骨骼变形、风或真实 active response。
动态 pin 的 BE secant velocity 与解析端点速度相差 O(h)，必须在同一功账中声明，不能要求二者同时精确相等。

curved-slide active-set 的原两周期失败已修正为保留正反力 active identity，
独立运行八个回归测试通过；两点反力 `.00259347/.00126025 N`、
gap 至多 `2.92e-15 m`，是小 oracle 的真实 active force 证据。
**真实 groom/body active、原三根 capsule 失败重放、runtime SDF 接触/恢复仍未准入**；
小 oracle 与全程零 active 的身体 controls 不可互相代替。

### GPU：新 J 局部正确性、实际预算与 CR 未放行

新 J GPU 采用 rest-relative 位移 u 和 material delta quaternion，避免先还原大 world float 再相减。
六步×十根 same-rounded 独立重算：最大 scaled component `8.630132704e-5`，
在原 float `1e-4` 内；物理力向量/Fref `1.235171019e-4` 单独保留，
它不是同一个最大分量范数。实际 J relative error `3.79593e-7`，accepted clock error
`1.11759e-9 s`。2,000-guide 导出端点逐字节重复十个源 fixture，
不是两千个独立作者输入；零 imposed-air 局部结果不提供 20 m/s 空气/接触/full-head PASS。

先前 21–54 ms/h 来自 persistent HOST_VISIBLE|COHERENT probe，不能全归因 6×6 逆。
同核 residency A/B 的真实 DEVICE_LOCAL 2k 六步为
`20.917824,20.681856,20.277856,18.895296,20.381536,44.283808 ms`；
同 executable host control 为
`25.304128,24.725792,24.100704,23.064064,25.116736,42.408384 ms`。
导出 state/reference/delta 与 clock 相同；device allocation type1/flags1/local heap，
staging type3/flags6/nonlocal heap，上传一次，transfer 在 dispatch timestamp 外。
真实 device-local 路径仍约 19–44 ms/h，尚不满足目标；六步不是持续 runtime 预算或统计置信区间。
满 20 m/s 单步另有 GPU/CPU status3 的原失败及 continuation 计数 instrumentation 缺陷待复核，
不能拿零风六步 PASS 盖过它。

独立 fast3 Schur 逆因历史实际 full-band 方向误差
`7.11583e-5 -> .00327574` 被拒绝集成，即使微核更快。
后续独立 CR 是 nonsymmetric block cyclic reduction：
stride1/2/4/8/16 每级消去半数行，保留消去行再反向回代，原 scalar6 partial pivot、
无 shift/fast3/隐藏串行 Thomas fallback。全局可逆但消去块奇异时明确失败。
审计补了末尾统一 barrier，原修订也保留。

对当前 stride 的 `L_i*x_left+D_i*x_i+U_i*x_right=b_i`，
保留偶数行时令 `alpha=L_i*inverse(D_left)`、
`beta=U_i*inverse(D_right)`；其新对角/RHS 是
`D'=D_i-alpha*U_left-beta*L_right`、
`b'=b_i-alpha*b_left-beta*b_right`，新侧块为
`L'=-alpha*L_left`、`U'=-beta*U_right`。
最后解 row0，再以保留的消去行
`x_i=inverse(D_i)*(b_i-L_i*x_left-U_i*x_right)` 逐层并行回代。
乘法顺序适用于一般非对称块；不是 PCR 每级更新全部行，也不是 lane0 串行 Thomas。

CR revision002 使用六步×十根**新 J 实际 GPU rounded bands**：
scalar/CR 均 60/60 finite/原 residual PASS；最大原 residual
`1.29970e-9 / 2.25552e-9`，方向 relative error `4.29239e-5 / 1.37055e-4`。
同一 rounded 192×192 CPUdouble solve 作 oracle，不重组另一张解析矩阵；
有限 residual 不证明 nonlinear state error 或动态质量。
2k 重复六十个 band、十次 warmup/五十 alternating 样本，
常规 median/p95：scalar `5.304976/6.601632 ms`，CR `2.713648/3.494880 ms`；
CPU submit/wait median `5.621450/2.958200 ms`。host JSON 原 upper-median 字段照留，
上述数由 CPU 分析按偶数样本中心二者均值重算。

主线程授权窗口中实际 start `22:13:07.2330703+08:00`，
release `22:13:38.6761999+08:00`，此后无额外 GPU 执行。
start/end 除预期 CPU make/编译外，还出现 `main_editor PID52424`，
CPU time `4.578125 -> 13.531250 s`；稍后查询已退出，未取得原命令行/GPU utilization。
因此保存观测但 **clean isolated performance acceptance PENDING、CR 不自动集成**。
没有杀用户进程或自行重跑。连续调用/NaN/Inf 专项 GPU coverage 和实际驱动 shared-resource
占用也不能从编译通过推成 PASS。

### 证据身份与重建命令

下表保存关键实际 SHA256；中间每步矩阵/状态 hash 仍见对应 manifest。
如果取得本次 landing 的版本管理源码而没有本地原始文件，可由上面的公式、实现/测试/工具重新构建新身份，
但不能伪称重现了旧 hash 或覆盖旧记录。CR/fast 的独立 probe 源当前在 gitignored
artifacts 中；因此本段已经记录算法、输入布局、原始数值与未放行原因，不能仅链接其日志代替结论。

| 证据身份 | SHA256 |
| --- | --- |
| J 纠正推导 `artifacts/hair-landing-20261119/section-inertia-correction-prereg.md` | `A21CDC2882FFEC6B474FC24CC77A80DECC547DCA412659F9F8D55A2E5B869339` |
| quartic 额定 `artifacts/hair-landing-20261119/progressive-support-formulas.md` | `8021C4DC5F46F663A2F66167F417E9E0BB33B5E5D1D28550D893857F65CAF12E` |
| 实际伸长/时序及 implicit-drag 公式审核 `audit-section2-model-and-time-decision.md`（同 evidence 根） | `93D7FAC3BCF9F676F26509BCB091E719FB11B74A522E536C0A8C78E4E37125B2` |
| 四失败步独立 WorkGuard 重算 `audit-section-air-workguard-replay-result.json`（同根） | `E28506FEBAA45D0952B152220395E421159F420D2FD7B2D305581D85B56B653B` |
| corrected-J GPU `rodV3ImplicitSection.spv`（同根） | `D0D254CB6CEAABF02E0D2834B55AE6ABDBA22F381E530933206FBA8AA468AF16` |
| corrected-J 实际 band step0 / step5 的 binding28（同根） | `D822EE129EF6C43881D6B517FDE7EE901AA4F7BCE92E49DD3B03235A3E40E157` / `285657795C56B8D7DF33BAAB1FF6C14191C5DCCFA364CFF70CEC8544B128B14E` |
| CR helper revision002 `shader/glsl/hair/rodCrBand.glsl` | `A2D01C4764076BE9FDC07666AD5629BD7D10223FC5A29F0466D752834FD215C0` |
| CR probe revision002 `artifacts/cr-band-20261119/cr-probe-002.spv` | `41C4115F8B5338962C0D024D3CF4810B1C31BACB74D5715E498E8C01AA364723` |

corrected-J 每步 binding28 为 `10 × 932 float4`，每 guide 前 928 float4 是
32 个节点，每节点 lower/diagonal/upper 各 36 个 row-major float，加 RHS linear/angular
各一个 float4；尾四个 float4 保存尺度/h、origin/tolerance、gravity/h、status。
binding29 保留 candidate/old u、material delta、速度/omega、targets、载荷与诊断；
逐项说明与 SHA 在 `artifacts/hair-landing-20261119/section-matrix-001/README.md`、
`manifest.json`，CPU references 另有 step/guide JSON。不可把旧 engineering-J 矩阵替代此 corpus。

复核入口（以下是命令记录，不表示本次文档整理执行了构建、长程或 GPU）：

```powershell
cmake --build build --target hair_asset_tests -j 4
build/bin/hair_asset_tests.exe --gtest_filter=RodSectionInertia.*:RodRootMotion.*:RodWorkCertificate.*:RodContactImplicit.*
py -3 tool/hair/prepare_rod_sdf_scenes.py --resource-root ../VukanLearnResources --report artifacts/NEW-sdf-scenes.json
artifacts/cr-band-20261119/vulkan-probe.exe artifacts/cr-band-20261119/correctness-002 2176 0 fullband
artifacts/cr-band-20261119/vulkan-probe.exe artifacts/cr-band-20261119/timing-002 64000 50 fullband
py -3 tool/validate-docs.py
git diff --check
git -C ../VukanLearnResources diff --check
```

共享 build 与 GPU 必须由主线程另安排唯一窗口；旧 probe 命令会写其目录，保留证据时须复制
冻结输入到新输出目录，不能为复核覆盖本次002。CPU NumPy oracle 使用现成 bundled Python，
系统 `py -3` 若无 NumPy 的原失败仍留存。生产源码/测试可以正常重建，
但历史 CMake 配置、编译 flags、source/EXE hash 要以各 frozen manifest 为准。

最终还需追加：同一最终模型的 implicit-drag 公式/FD及实际轨迹，
原伸长与时间误差裁决，真实 body active/contact/recovery，完整 40,334-strand follow、
2k guide accepted-time/debt/root/pause/reset/风停恢复和正常 runtime 退出实测。
当前场景生成、局部测试、线性微核与 CPU 多身份轨迹都不能替代这些最终验收。

### C3 worker追加：端点空气固定372步实际时间诊断

此前24项actual-air仍是旧速度采样的多身份完整历史；上面的
20原完成+4WorkGuard覆盖、原14+4行为FAIL、两根N32真实伸长FAIL
保持原身份。没有用新接口给旧结果改标签。

CPU独立命名的AirContinuation入口在冻结old tangent下使用vnew，
同一drag律、材料、section inertia2、root/support及20m/s世界风不变。
4项公式/FD/纯drag/84短步测试已有实际PASS，随后本worker按预登记
仅运行33486/120 N32的240/480/960/1920/3840Hz、.025s窗口：
共372步，exit0，总7.6434531s，无status失败/未达门槛项。
所有372项work certificate accepted，最大norm9.8668717e-9，
work上界4.7944769e-11、balance2.4081751e-12、
identity1.0200823e-16。最大总Newton40，不是每步固定12的性能承诺。

实际源码/编译/EXE在 `validation-build-014` 冻结，
EXE SHA256 `194989929A6726F181A37BC6AA5C7C421334EC118A90898A14C3CC1067E3C476`。
运行前身份 `validation-section2-endpoint-air-time-001-prerun.json`；
原始 `validation-section2-endpoint-air-time-001` 每h目录导出全状态、
actual final空气力、其他外力、old tangent、Wair/Wother、完整物理残差
及功证书。输入externalForces没有再次加入oldv drag。

全向量与能量分析 `validation-section2-endpoint-air-analysis-001`：

| N32 guide | h240相对3840 max-node/L | h240 max SO3 | 1920相对3840 max-node/L | 1920 max SO3 |
| --- | --- | --- | --- | --- |
| 33486 | .0152783214 | .30601936 rad | .0028037990 | .06585280 rad |
| 120 | .0134652834 | .07907943 rad | .0024400090 | .01334772 rad |

各自有限3840参考下位置/SO3绝对误差随h减小，但这一端点力改动
未消除240Hz时间误差，不能据此作质量PASS。这是动态共同时间误差，
不拿它直接套静态.015L线；1920-vs3840仅为实测差，非严格误差界。
不拟合相位、不继续搜索h、不为FAIL调整EA/EI/k2/风。
本两例也不覆盖39210/4006的最坏伸长，物理1%判据仍待model裁决。
独立审计实际力/残差/功账与时间向量已请求，当前不宣称审计完成。
语义profile补充见 `validation-final-profile-endpoint-air-amendment.md`；
生产schema、cache/reset身份和runtime准入仍由对应owner合并裁决。

### C3 worker追加：最差两根端点空气完整长程尝试

管理在372固定参考后明确授权仅39210/4006 N32的新鲜长程诊断，
不等时间精度最终准入。保持原h240、静态3s/20m/s风.5s/恢复10s、
sectionJ2/WorkGuard/材料/EA/EI/k2，未新增扫描或应变限制。
`validation-build-015` 仅重编本工具TU，链接build014冻结CPU对象；
运行前完整linked-object SHA/命令记录于
`validation-section2-endpoint-air-long-worst-001-prerun.json`。
EXE SHA256 `655510651D3B8286D8F802D29B163E3EF43531AF165AC5A3DD9CAF0B016268A4`。

实际 `validation-section2-endpoint-air-long-worst-001` 总28.9487493s：
39210完整3240步数值PASS；4006接受720静态步，风首步721失败。
合计3960accepted/3961attempted，不宣称两根6480步完整通过。

39210的几何最大伸长仍为2.114576386%、t3.0125s、segment1；
实际轴向材料应变.02114483169、轴力.01660711197N，EA=.7853981634N。
动态30.746725mm/.0934963742L、固定相机投影25.421526px，
永久恢复后缀.0833333s、tail2.7236627e-6mm；pins/rootQ均精确0，
最大accepted真实norm9.8254636e-9。原行为FAIL保留，
端点空气仍没有消除真正有限EA的1%伸长问题。

4006在t3.0041667s的未发布内部候选status3、norm6.4144190e-6，
trace原因 `continuation-stage-8-newton-iteration-budget`，
最后load fraction1；原步及一次相同trace重放均保留。
`failed-old-state.csv`、`failed-internal-candidate.csv`、
`failed-system.csv`、`failure-trace.json`已交CPU与独立auditor。
其accepted720步只证明静态历史，不能给动态位移/伸长/恢复结论；
失败行导出的回滚态不是有效air轨迹，不纳入accepted统计。

`validation-section2-endpoint-air-long-worst-analysis-001` 保存准确
checkpoint、两根accepted姿态、逐段几何/材料应变及轴力摘要。
原始CSV每步另有实际端点空气力、其他外力、分开功、完整动量残差、
能量功账/证书与root反力。独立复核已请求；原先所有输出继续保留。

CPU随后要求仅冻结4006第721单步重放、导出全部52次norm检查，
以区分收敛慢、回溯与预算末次检查遗漏。本worker使用原build014对象、
原old state、原12更新/stage，不重跑长程，实际输出
`validation-section2-endpoint-air-replay4006-001` 的逐stage/iteration
alpha/merit/maxnorm、完整块矩阵/残差/方向/线性缺陷及候选。
EXE SHA `1A014ECF91E1671EC7238C37D44603D1C74B840C8B9F541FECF3D8919B40629E`。
finalstage有13次检查/12次更新，早段连续alpha.125，merit降低而
maxnorm约.001平台；末两full更新后norm为6.414419e-6，末条明确重新
检查，重组残差同值。不是遗漏末更新验收，实际finalstage线性缺陷
最大9.85e-16。不存在system的末次check行0是未求解哨兵，README明确。
原预算失败保持，定位与完整raw已交CPU/auditor，不由本worker改预算或物理。

随后独立 `audit-validation-section2-endpoint-air-long-worst-001/result.json`
已实际重构全部3960accepted前缀，核对端点真实力、材料应变/traction、
全动量/能量功账及39210行为；4006仅720前缀，不补造缺失长程。
`audit-endpoint4006-local-result.json` 的未发布局部FD/单方向oracle
也保留原失败，不构成预算变更或物理时间接受。
准确路径/hash集中于 `validation-section2-endpoint-air-long-audited-checkpoint-001.json`，
同样未宣称6480步PASS或生产准入。

### C3 worker：最终选定轴限模型的真实groom覆盖

CPU `axial-step-release-004.json` 的新模型明确加入上拉伸1%互补约束，
`StepAxialContactImplicit` 联立incoming10、sectionJ2、progressive、
端点air与WorkGuard。它不是旧finiteEA轨迹的数值重命名；
旧39210的2.1146%真FAIL、4006原721求根FAIL全部保留。
实际编译15个CPU unit的源码/对象SHA及精确API统一记录
`validation-final-cpu-api-deps-001.json`，全部源码与独立冻结bundle
`axial-cpu-frozen-004` 相同。未使用live共享archive或共享make。

工具short001最初8个首步均solver status0/work accepted，但错误地
将exact pin与rest.positions比较，而非原协议SampleRootSupport映射，
造成3.47e-18/5.55e-18m假FAIL。原数据保留；short002只修正目标构造，
未放宽exact-zero阈值、未改物理，实际1536/1536接受：
39210/4006 N32各三h immediateair .025s及各726步settled prefix。
4006原721触发步现在direct9/fallback0/backtracks6，norm7.6986e-13。

管理随后授权39210优先、再4006及全部8N32原3+.5+10s协议：
39210long001完整3240，remaininglong001完整六例19440后，在120
部分历史达到1GiB输出预算。工具未捕获runtime_error OUTPUT_BUDGET，
进程退出-1073740791，不是CPU求根失败。旧所有raw及SHA、stdout/stderr
保留，逐文件完整tick统计见 `validation-axial-n32-budget-prefix-inventory-001.json`；
120全流共同完整至725，不能由缓冲尾部推断实际最后物理执行tick。
trace是每步局部对象，源码无累计trace/history容器；未采集peak memory，
不臆测OOM。原441MB final-blocks实际是10分量残差行，非全矩阵。

新run前 `validation-axial-output-coverage-amendment.md` 固定输出coverage：
每步原状态/实际力/activeλ/gap/EA牵引/反力功/物理残差/功证书全保留，
完整10残差与矩阵仅first/last/每120步/active-set改变/失败存储，
其他步仍可由raw独立重算。不改方程/阈值，增加outermain异常捕获、
每120步flush/progress。六完整历史不重复，仅fresh120long001补3240，
10.9019504s、约114MB、退出0。raw case字符串沿用long39210，
但guide120/--long120 prerun与每行身份准确；此命名缺陷明确记录，
不改旧raw。最终8N32是显式三run25920步coverage，非单一25920原run。

完整派生表/accepted poses在 `validation-axial-n32-coverage-analysis-001`：

| N32 ID | 静态mm | 动态mm | 固定相机投影px | 永久恢复s | 最大拉伸% | 最小压缩% |
| --- | --- | --- | --- | --- | --- | --- |
| 120 | .721862 | 4.854581 | 4.011870 | .04166667 | .0503720547 | -.07791673 |
| 584 | .450607 | 2.896984 | 2.501498 | .03333333 | .0110423936 | -.11211713 |
| 4006 | 5.706652 | 48.349076 | 41.270726 | .10416667 | 1.0000000003 | -.91424883 |
| 5651 | 2.590704 | 22.650598 | 20.070777 | .07500000 | .3006673947 | -.40170447 |
| 5660 | 2.127395 | 18.781383 | 16.274966 | .07083333 | .2890468465 | -.39413976 |
| 8924 | 4.506437 | 39.596439 | 32.814735 | .09583333 | .7093032619 | -.78687245 |
| 33486 | 1.075854 | 14.825358 | 11.890961 | .06250000 | .8681513687 | -.13107522 |
| 39210 | 3.564115 | 30.751981 | 25.421365 | .08333333 | 1.0000000321 | -.11344454 |

39210最大实际拉伸的fractional overshoot为3.2118931e-10，
该edge声明gap容差对应strain3.5520321e-8，原值不舍入、不改门槛；
最大lambda .01006579461N，峰值edge的有限EA轴力 .007853981834N。
4006最大lambda .00195671610N。约束是实际成对反力进入动量/功账，
不是postclamp；压缩不受此上拉伸模型约束，单列原值。
全部8long原静态speed/drift为0，tail/L范围1.9843e-9..8.7480e-9；
原.08动态工程flag仍有六例FAIL，不据此调k2。
投影只是同固定1280x720捕获相机的CPU中心线，不宣称fullhead遮挡/渲染。

所有long最大norm9.9500597e-9、work bound4.8067178e-11、
complementarity1.1402642e-12；最大direct23、fallback均0。
CPU wall cost含每步trace/CSV，不作GPU性能外推。
N8/16另完成全8IDs固定三h短48格672/672、1.4312352s，没有增加长跑。
新模型不继承旧时间误差/生产PASS；全轨迹独立audit已请求，
runtime/GPU/SDF/fullhead准入仍归对应owner。最新全量身份与误报澄清
始终放 `validation-final-checkpoint.md` 顶部，原FAIL一律保留。

## 冻结前最小重放清单与 evidence 源码保留

2026-11-19 23:05 后的管理资料补充。本节是入口/保存约束，不表示新执行、
最终模型已经冻结或主线程需要 commit。前面的“implicit drag 待实施”是当时快照；
后续端点空气 372 步、最差两根长程和 4006 单步诊断已实际存在，分别见前文追加。
当前 production 源继续变化，**用最新源码跑旧参数只能得到新身份，不能当作旧 EXE 的同版本重放**。

### 最小保留的常规入口

| 用途 | 仓库入口/目标 | 最小输入与输出，实际限制 |
| --- | --- | --- |
| CPU 模块标准回归 | `tool/hair-tests/CMakeLists.txt` → `hair_asset_tests`；当前包含 `rodSectionInertiaTests.cpp`、`rodRootMotionTests.cpp`、`rodWorkCertificateTests.cpp`、`rodImplicitAirTests.cpp`、`rodContactImplicitTests.cpp` | 首先运行无 filter 全套，输出 GTest XML/日志/退出码，再作定位子集；不通过排除历史不合格反例来构造“默认全过”。旧阶段曾有带排除的回归，不能移植其计数。CMake 使用逐 GTest discovery，不能假定 `ctest -R hair_asset_tests` 会选中全部测试；直接执行 EXE 是明确入口。 |
| 真实资产盘点 | `tool/hair/main.cpp` → `vl_tfx_inspect` | 输入真实 `female_guides.tfx`，输出版本/40334×32点/geometry。它不导出全部所选 x/y/z/w，不替代下面 fixture extractor。 |
| 实际 groom CPU 工具 | `tool/hair/rodGroomValidation.cpp` → `vl_rod_groom_validation` | 输入 `groom-selected-strands.raw.json`；八个固定 ID × N8/16/32，新 J 必须显式 `--kinetic-math-version 2`。输出 rest/states/steps/dynamics/能量/summary，失败保存真实 rejected candidate。`--prepare` 零推进，`--wind-short` 为 .025s 三 h，`--implicit-air-time` 固定两根五 h 共372步；长程仍须另次授权。 |
| 真实 body/SDF CPU 工具 | `tool/hair/rodContactValidation.cpp` → `vl_rod_contact_validation` | 输入资源根、实际 scene 与 `--geometry sdf/combined`；`preflight` 零推进、`probe` 默认八步；`long` 要传实际同几何成功 probe 的 `--probe-report`，不能凭存在目录放行。raw φ/g/H、lambda、reaction residual、trace/rollback、scene/margin身份均保留。 |
| 场景与基础 schema | `tool/hair/prepare_rod_sdf_scenes.py`；`tool/validate_model_descriptors.py`、`tool/validate_scene_schema.py`、`tool/hair-tests/tressfxSemanticSchemaTests.py` | generator 保留 baseline/combined，仅去掉新 SDF-primary 的四胶囊，记录反向语义 delta/哈希。schema 通过不代表运行/物理通过；全局既有 gun/scene failures 仍单列。 |
| 正常 runtime 场景 | `main_editor` / `main_game`，`Maps/SC_tressfx_rod_validation/SC_tressfx_rod_sdf_static.json`、`SC_tressfx_rod_sdf_animated.json` | 加载前记录实际共享 TF 的 solver/backend/profile 与 descriptor/scene 哈希，不能把仍指向 V1 的诊断资产称 V3。帧数只用于退出，物理 accepted time/debt 另读回。 |
| 常规 trace 分析 | `tool/hair/analyze_placement_trace.py`、`tool/hair/verify_hair_physics.py` | 前者比较两份 VLHT 根/尖轨迹；后者消费 capture 的 report/trace。版本布局以 `hairTraceFormat.h` 与实际 parser 为准；旧 parser 不支持最终格式时明确失败，不能换掉 source identity 或省字段。 |

以下为从代码根运行的**新身份**最小命令模板，`NEW` 必须换成未存在目录。
共享 build、窗口和更长实验仍由 main 排程；此文档整理没有执行这些构建/推进命令：

```powershell
$replayRoot = 'artifacts/hair-replay-NEW'
New-Item -ItemType Directory -Path $replayRoot -ErrorAction Stop | Out-Null
cmake --build build --target hair_asset_tests vl_tfx_inspect vl_rod_groom_validation vl_rod_contact_validation -j 4
build/bin/hair_asset_tests.exe --gtest_output=xml:artifacts/hair-replay-NEW/hair-all.xml
build/bin/vl_tfx_inspect.exe ../VukanLearnResources/Maps/SC_tressfx_female/Source/female_guides.tfx
build/bin/vl_rod_groom_validation.exe --prepare artifacts/hair-landing-20261119/groom-selected-strands.raw.json artifacts/hair-replay-NEW/prepare --solver implicitMomentumProgressiveContinuation9 --kinetic-math-version 2
build/bin/vl_rod_groom_validation.exe --wind-short artifacts/hair-landing-20261119/groom-selected-strands.raw.json artifacts/hair-replay-NEW/air-short --solver implicitMomentumProgressiveContinuation9 --kinetic-math-version 2
build/bin/vl_rod_groom_validation.exe --implicit-air-time artifacts/hair-landing-20261119/groom-selected-strands.raw.json artifacts/hair-replay-NEW/endpoint-air-372 --solver implicitMomentumProgressiveAirContinuation --kinetic-math-version 2
build/bin/vl_rod_contact_validation.exe --mode preflight --geometry sdf --scene Maps/SC_tressfx_rod_validation/SC_tressfx_rod_sdf_static.json --scenario static --resource-root ../VukanLearnResources --output artifacts/hair-replay-NEW/contact-preflight
build/bin/vl_rod_contact_validation.exe --mode probe --geometry sdf --scene Maps/SC_tressfx_rod_validation/SC_tressfx_rod_sdf_static.json --scenario static --resource-root ../VukanLearnResources --output artifacts/hair-replay-NEW/contact-probe
```

`vl_rod_groom_validation` 的输入不是 inventory summary，也不是 `groom.fixture.json`。
工具实际校验源 SHA256 `73fa1f3bce4a060f4876c3286a5ec29da5f985c793c9e03f81f5f16340dc39c7`，
读取 `samples[].guide_id/positions_xyzw/length_cm`；JSON 输入本身 SHA256
`156405043C66E8FFEC835023664EEF0ED6C97FF7CC115D2E28571D21A0B31948`。
cm→m 一次；N32保留原点，N8/16固定首两点再按作者弧长重采样；
w=0只是 pin 标记，真实 kg mass 从工程截面/密度生成。
八个 ID 为584/33486/39210/8924/4006/5651/5660/120。extractor 源
`artifacts/hair-landing-20261119/inventory_groom.py` 必须与输入一起保留，
其 SHA256 `E0E11CA5A59130433F668331938AD67419EF79E59C82719380728B0A9D4F364D`；
只保留 `vl_tfx_inspect.exe` 或 summary 无法重建此输入。

长程入口清单仅作定位：old-state AIR 使用 `--wind-long ... --solver implicitMomentumProgressiveContinuation9 --kinetic-math-version 2`；
四 WorkGuard remainder 使用 `--wind-long-work-remainder ... --solver implicitMomentumProgressiveContinuationWorkGuard --kinetic-math-version 2`；
端点空气最差两根使用 `--implicit-air-long-worst ... --solver implicitMomentumProgressiveAirContinuation --kinetic-math-version 2`。
它们各有固定 case 集合、预算和 force-time 身份，不是通用换 solver 后自动复用的 run。
CPU 空气协议为3s无风+.5s空气+10s恢复=13.5s；
用户普通 runtime 的3s无风+3s2000cm/s+9s无风=15s是另一协议，必须分开记录。

普通场景烟测命令（另获 GPU 窗口后，确认实际 descriptor 身份）：

```powershell
build/bin/main_game.exe --initial-scene Maps/SC_tressfx_rod_validation/SC_tressfx_rod_sdf_static.json --exit-after-frames 120 --worker-thread-count 1 --no-dev-ui
build/bin/main_editor.exe --initial-scene Maps/SC_tressfx_rod_validation/SC_tressfx_rod_sdf_animated.json --exit-after-frames 120
```

必须另存 stdout/stderr、启动选项、配置/scene/TF/资源/source/SPV/EXE 哈希、perf与接受时钟。
普通 Console `wind get`、`wind strength 2000`、`wind strength 0` 提供非持久 live 强度；
三段15s测试由外部明确注入并记录命令 receipt/真实 wall与accepted-time，不在命令里暗加 timeline。
`hair trace <fresh-path> <frames> <guide-stride>` 是读回入口；正常退出不自动等于正确物理。
`capture_hair_physics.py` 是既有特定 Female 场景捕获器，当前没有 `--scene` 泛化参数，
不能给它编造该参数或把它的旧固定场景报告冒新 Rod 场景证据。

### 必须同行保留的 CPU/GPU oracle 源与版本闭包

这里的“保留”指源、头、输入、构建 recipe/依赖 manifest、输出一起保留，
不是仅保 EXE/SPV 或只记 hash。现有文件均继续原地冻结；
不清理、不覆盖旧 data、不覆盖旧脚本、不把最新版 production 源拷回旧 snapshot。
拒绝的 fast3/M0/C3 等实验无须全部提升为正式 production source，
但其原失败与已引用的 oracle 源也不能删除。

| 闭包 | 必须保留的位置与依赖 | 重建/状态边界 |
| --- | --- | --- |
| CPU section2 / root 三测试 | `artifacts/root-motion-20261119/frozen/source/hair/rod/`、冻结 `tressfxSolverMath.h`、三个 frozen CPP snapshot、`source-identity-before.json`、`properties-001-identity.json`、`periodic-003-identity.json`、编译日志/`compiler-identity.txt`、原 state64/JSON/CSV | 重新编译 snapshot 验旧模型；常规 CMake只加生产 `rodRootMotionTests.cpp`，不能把三个 snapshot 都加进去。独立源码 `recompute-properties.py`、`recompute-periodic.py` 和输入也保留。 |
| CPU endpoint drag 及最差两根 | `artifacts/hair-landing-20261119/validation-build-014/` 的完整 `hair/rod/`、`rodGroomValidation.cpp`、`obj/`、`build-manifest.json`、`source-identity.csv`、`frozen-identity.csv`、`link.rsp`；build015同类文件及其 `cpuSnapshot=validation-build-014` 依赖 | build015不是独立自足 EXE：它复用014CPU对象。014 manifest SHA256 `206D99C86357CF5A4B60959CBBE8954F46EE2FAC5D5F07B30F2D3D46B234A49F`。同时保留 endpoint-time/long 两份 prerun、实际输出、4006单步replayCPP/trace及 `validation-section2-endpoint-air-long-audited-checkpoint-001.json`。 |
| CPU WorkGuard 原四步与长程 | `replay-four-work-failures.cpp`、`replay_four_work_failures.exe`对应构建/identity；`validation-build-011/012`及被link.rsp引用的冻结CPU源/对象；`audit-workguard-four-replay-001/002`、原20项与新4项输出 | 首次错误输入001也保留。独立 `audit-section-air-work-failures.py` 依赖 `audit-math.py`；长程 `audit-section-air-long.py` 同样依赖此数学模块和实际 CSV/summary。 |
| CPU body / active failures | `cpu-contact-sdf-primary-frozen-004`、`cpu-contact-sdf-head-frozen-005`、retention006/007、response008/009/010、`cpu-contact-exclusion-frozen-011` 的完整 `source/`、`tool/`、`manifest.json` 或对应冻结源树/构建日志 | 011内有真实源树，不只一个EXE。对应preflight/probe/first-active/rejected-candidate/9-band/trials/raw grid/body/scene身份一并保存；retention、exclusion与4006自由globalization失败不能混成同一修复。 |
| GPU residency / corrected-J actual bands | `residency-ab-001/`、`section-matrix-001/` 的 `gpu-kernel-probe.cpp`、输入fixture/weights、manifest、metadata、SPV及完整transitive GLSL include源、对应CPU source/lib身份 | README记录 `gpu-residency-probe.exe 10 6 v3sectiondump device` 的参数，但该probe写固定 evidence 路径；不能直接在旧目录执行。manifest所列原 `.comp/.glsl` 版本必须可取得，不能用今天的 kernel 编译冒旧SPV。 |
| CR 线性核独立 oracle | `artifacts/cr-band-20261119/` 的 `frozen/`、`frozen-helper-002/`、所有 `.py/.cpp/.comp/.glsl/.ps1`、build-identity001/002、host-baseline identity、corpus/labels/double-directions与correctness/timing002；首版001也保留 | 不依赖一份旧EXE：源码含新J corpus准备、CPUdouble与Thomas对照、GPU wrapper/host及分析。被拒绝的 `artifacts/fast-block-20261119/` 保持另一个原实验身份。 |
| 后续 GPU CR 全非线性诊断 | `cr-fullsolver-freeze-001.md`、`gpu-cr-fullsolver-probe.exe`匹配的hostCPP/编译recipe、`rodV3ImplicitCr.spv`及所有includes、cr-device十根/2k JSON/log/raw buffers | 这是 owner 后来另行集成的六步诊断，不回写独立 CR 未放行结果。全2k接受、每步2Newton，实际5.084448..27.249760ms/h，仍超过4.167ms预算；非 clean性能/fullhead/contact PASS。缺源码/依赖recipe时标交付缺口，不能拿EXE替代。 |
| fullhead / 相机与pose oracle | `analyze_fullhead_shape.py`、`fullhead-reference-pose-oracle.py`、current.bin及metadata/bones/palette、`v3-runtime-fullhead-analysis/report.json`、原TFX/source-order身份 | `reference` 必须与captured pose一致；原始camera/viewport、选择guide表与lagged accepted-clock metadata都保留。不能仅给一张图或旧EXE。 |

`audit-math.py` 当前核验 SHA256
`BDB591317B398C4C510EB6553387E12967DB425BC2CA4A22C202B7F96071718C`。
这是被多份独立重算脚本实际 import 的依赖，不可遗漏。
分析还需保留 `validation-v2-phase-analyze.py`、
`validation-endpoint-air-analyze.py`、`validation-endpoint-air-long-report.py`、
`validation-shape-check.py`、`audit-gpu-section-state.py`、`audit-gpu-residency.py`、
`audit-contact-failure-traces008.py`、`audit-contact-real-retention.py`、
`audit-endpoint4006-local.py` 及各自实际读取的 raw 文件。
Python/NumPy版本与脚本哈希要写进新 identity；已有成功重算使用 bundled NumPy2.3.5，
不能把系统 Python 缺 NumPy的原失败日志删除。

### analysis 命令及覆盖保护

参数化分析可在旧输入上写**新输出**，例如：

```powershell
py -3 tool/hair/analyze_placement_trace.py artifacts/hair-replay-NEW/sweep.vlht artifacts/hair-replay-NEW/control.vlht --json artifacts/hair-replay-NEW/placement-analysis.json
py -3 tool/hair/verify_hair_physics.py artifacts/hair-replay-NEW/capture/report.json --profile capture
```

使用带NumPy的 Python解释器时，实际全头工具入口是
`artifacts/hair-landing-20261119/analyze_fullhead_shape.py --current <current.bin> --metadata <current.bin.json> --output <new-directory>`；
eight-guide shape工具为
`validation-shape-check.py <authored.csv> <pose.csv> <new.json> --points 32 --coverage selected-eight`，
必要时再加实际kind/tick/h与behavior文件，不能省略选帧身份。
它们的 coverage/settled/quality_pass字段不能因CLI正常退出改名为PASS。

多数独立 audit 或 inventory 脚本从 `Path(__file__).parent` 计算固定输入和输出，
部分无防覆盖，且response文件有绝对路径。因此不可在原 evidence 路径直接照抄运行：
先在独立新 evidence 副本中保留相对目录关系，复制其完整源/manifest/输入闭包，
只对副本作显式路径映射，记录映射和新hash；再执行复制后的脚本/重新编译。
`inventory_groom.py` 的代码/资源根也由目录层级派生，移动后须验证路径，
不能误读共享master资源。原 `.rsp` 及 `.py` 不改。

每次重放最少交付：代码/资源 HEAD加未提交补丁和新增文件清单、完整source/include
SHA256、编译器/flags/链接输入、实际CLI、solver/profile/kinetic/force-time身份、
固定fixture/schema/scene/TFX/SDF/权重hash、参数/h/预算/窗口上下文、原始输出、
退出码与接受/失败计数、未发布candidate与原失败结论、独立analysis版本/命令/结果。
仅 `git diff` 不包含未跟踪新增源，必须连同这些源文件交接；
也不能用一个目录名中的“frozen”替代检查manifest闭包。

最终源码接线与可重放 recipe 要随 landing 交付；本清单不把gitignored artifacts
变成Git会自动携带的数据，不以“本机还留着EXE”关闭可重建性要求。
退回实验可继续保留为独立源包而不进入正式production target。
本次不做任何清理、数据迁移/覆盖、全量无关构建、暂存或commit；
main在最终冻结后仍须核对上述文件实际存在和依赖闭包，再安排唯一窗口复跑需要的项。
