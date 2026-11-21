# G0-only-v1：单粒子 exact zero-rest ghost 数值实验

2026-11-18。独立本地能量模型实验记录，不是 AMS 作者实现、运行时契约或材料参数标定。

## 规格与历史隔离

依据 `artifacts/hair-ams-20261118/phase2-audit-g0-release.md` 的 G0-only 数值协议 v1。
审核候选 `phase2-g0-review-spec.md` 的 SHA256 为
`63a5ca88e223cba7cdb4056a888bda5e12b43804e10bd72aea0f47e8582fbf81`。
release 的 15 个 oracle 是研究审核结果，不预授予本 CPU PASS。

新源 `tool/hair/ghostEnergyPrototype.cpp`，新 target `vl_hair_ghost_energy`，
只链接 Eigen3，无 CTest 注册。CMake 仅追加该目标两行。
不修改旧 `amsGhostPrototype.cpp`、旧 EXE、旧 rationale 或 M0。
旧源码 SHA256 `6c35eb08da8c9710bba36255c82ba4f65ea91d18bba8146787bff3c9fd360c73`，
旧 EXE SHA256 `6d31486719a78f160b49743d4d2eff8d4437d9473d45af1f6c9051345e4b8d17`。
旧 L2 的完整轨迹数值 FAIL、行为诊断 FAIL 保持原判。

## 模型、步进与能量账

SI：m、kg、s、N、J。输入是单 real 点 x/v、m>0、h>0、k>=0、
环境阻尼 gamma>=0、显式外力 F、给定 ghost 端点 y_n/y_next。
r=y-x，U=k*|r|²/2；real 力 k*r，假想 ghost 反力 -k*r。
精确负力 Jacobian 为 k*I，包括重合；无方向归一化或 frozen rank1。

解 `(m+h*gamma+h²*k)*v_next=m*v+h*F+h*k*r+h*k*deltaY`，
然后 `x_next=x+h*v_next`。ghost 使用实际端点位移 deltaY；
若用 y_next 直接装配 RHS，不重复添加 h²*k*W。
这是本地线性力的后向 Euler 精确代数步，不是连续解析轨迹。

H=m*|v|²/2+k*|r|²/2。离散能量账为：

```text
H_next-H_old = externalWork - dampingLoss + ghostWork
              - numericalVelocityLoss - numericalElasticLoss
externalWork = h*F dot v_next
ghostWork = (k*r_next) dot deltaY
dampingLoss = h*gamma*|v_next|²
numericalVelocityLoss = m*|v_next-v|²/2
numericalElasticLoss = k*|r_next-r|²/2
```

ghostWork 正号表示外部驱动装置输入的功，不是假想 ghost 反力的功。
不要求 real-only 能量或动量守恒。后两项是数值耗散，与 gamma 物理阻尼分开。
H 不包含外力势能，F 的功只计一次。

## 固定输入与判据

全部输入编译时固定，无参数扫描或 CLI 材料参数。
17 个有效单步例：release 的 stationary、coincident-moving、moving-3d、
comoving、zero-stiffness、9 个 integrity-only 平衡（h=.001/.1/2，
gamma=0/.3/10）、非单位质量三维例；
另加 coincident-static-zero 和改变 ghost 端点后的 k=0 独立性例。
对应输入、手算期望速度与零平衡分支标记都写入 `inputs.csv`。
integrity-only 单点重力补偿不是 angular 联合 preload 或论文 sag-free。

9 个非法输入拒绝：零 h、零/负质量、负 k、负 gamma、非有限位置、
非有限 ghost 端点、非有限 h、非有限外力。
无效输入的期望拒绝单列；意外异常、非有限结果或检查失败退出 2。

协议门槛固定：

- 同 A/b 后向残差 <=1e-10，分母为 `|A|_F*|v|+|b|`；
- 非零解析/dense 解相对误差 <=1e-9；
- 预声明 v=0、deltaY=0 的平衡例先检净力（<=1e-10），
  再以输入非相消速度尺度 V_in 检零速度（<=1e-9）；
- 能量账以全部同量纲账项绝对值之和归一化，<=1e-10；
- 分母严格为零时要求分子严格为零，不加任意 epsilon/绝对 atol。

逐项输出 numerator、denominator、scaled error 和 tolerance，不事后换分支。
独立 dense 3x3 装配使用 `mI+h*gammaI+h²*kI`、
`mI*v+h*(F+kI*(y_next-x))`，与分拆 RHS 的闭式速度交叉检查。
该 isotropic 系统的二范数条件数恒为 1。

## 执行与证据

工作目录：
`D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/tressfx5-hair-port/VulkanLearn`。
原始新目录 `artifacts/hair-ams-20261118/g0-execution-001/`。

```powershell
cmake -S . -B build
cmake --build build --target vl_hair_ghost_energy -j 4
.\build\bin\vl_hair_ghost_energy.exe --verify artifacts/hair-ams-20261118/g0-execution-001/verify-001
py -3 tool\validate-docs.py
git diff --check
```

输出目录必须全新；已存在即退出 2，防止覆盖证据。
`inputs.csv` 提供完整参数与预声明期望值，`results.csv` 提供 A 对角、b、
速度/位置、弹力/反力、能量各项及尺度，`checks.csv` 提供分项判定。
新 scope 的 angular、relative dashpot、local block7 组合、联合 preload、
ghost dynamics、完整 AMS、材料 twist、volume、碰撞全部 UNSUPPORTED。
无 helix、长轨迹、参数语义映射或额外模型；本轮单步能量账足够独立复算。

## 冻结执行结果：等待独立 CPU 验收

新源 SHA256：
`89fb4e0c0abf33558a77ba6f6158d10de22f5c187c315459148ba103d6c95072`。
新 EXE SHA256：
`250e4c29e4786fdc937a94d4a74600caa7415d22bff4cfc3ad4a59fcedc8628a`。
编译器 GCC 16.2.0 / MinGW x86_64-ucrt-posix-seh、Eigen 3.4.0；
Release flags `-O3 -DNDEBUG -std=gnu++17`，加根工程路径映射 flags，无 fast-math。
源码、EXE、CMake、规格/release、参数 CSV 与结果 SHA 写入新目录清单。
源码和输入快照保留，复跑必须使用新输出目录。

执行侧结果：

| 项目 | 结果 |
| --- | --- |
| configure / 新 target build | exit 0，未构建旧 target |
| `--verify .../verify-001` | exit 0；17 个有效单步输入，118 项检查，0 失败 |
| 同 A/b 残差最大值 | 0 |
| independent dense 相对解差最大值 | 1.4835979218054372e-16 |
| 手算速度相对误差最大值 | 8.8817841970012523e-16 |
| energy identity scaled error 最大值 | 1.0751760815863953e-16 |
| 独立 RHS 装配尺度差最大值 | 1.483597921805437e-16 |
| 预声明零平衡速度/V_in 最大值 | 8.5877399800832014e-19 |
| 已有目录拒绝 | exit 2，结果 CSV 前后 SHA 相同，没有覆盖 |
| 旧 L2 源/EXE | `identity-before.csv` 与 `identity-after.csv` 哈希保持冻结值 |

零平衡只适用于提前声明的静止例；未用于非零 fixture 放宽相对速度门槛。
非单位三维例、三轴 moving 等非零 oracle 均按相对误差判定。
重合 moving 的 H_next=.01 J、ghost work=.02 J、两项数值耗散各 .005 J
按手算值核对；不把 one-way 功当能量不守恒错误。

`g0-recompute.py` 是执行侧标准库 Python CSV 复算，命令：

```powershell
py -3 artifacts/hair-ams-20261118/g0-execution-001/g0-recompute.py
```

`recompute.log` 显示 17 例、118 项原始检查一致；独立重算速度最大相对差
5.169773973588027e-17，能量账最大尺度误差 1.0751760815863953e-16，
导出能量各项最大尺度差 9.912705577010327e-17，位置更新差 0。
该复算是执行侧自检，不替代独立验收代理的最终判定。

实际 CLI 只有 `--verify NEW_OUTPUT_DIRECTORY`，意外错误/超阈值退出 2；
已有目录拒绝试验的预期 exit 2 不计数值失败。
原始输出路径为
`artifacts/hair-ams-20261118/g0-execution-001/verify-001/{inputs,results,checks,invalid-inputs,summary}.csv`。
构建、执行、防覆盖、复算日志保留在上一级目录。
没有调整任何 fixture 参数、协议门槛或能量定义，没有运行短轨迹之外的行为实验；
本轮实际只做单步 fixtures，无多步轨迹。

文档与 diff 门禁记录为同目录的 `validate-docs.log`、`diff-check.log`；
独立单步 G0 验收最终 PASS：17 例、118 checks，独立复跑 exit 0，源码与 EXE
哈希一致；最大解析速度绝对误差 1.57e-16 m/s、归一化速度误差 2.22e-16、
归一化能量误差 1.08e-16。依据主线程确认的独立结果，审计报告归档为
`artifacts/hair-ams-20261118/phase2-audit-g0-result.md`（由审计代理写入）。
此 PASS 仅覆盖单粒子单步 G0 fixtures，不包含多步轨迹或整根发丝验收，
不升级完整 AMS、angular、ghost dynamics、材料 twist 或 volume。
此次收尾不修改 CPU 源或 EXE、不重建；新文档门禁与 diff 检查日志为同目录
`g0-closeout-validate-docs.log`、`g0-closeout-diff-check.log`，保留原日志。
