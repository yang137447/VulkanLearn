# GASP P2 蒙皮验收记录

日期：2026-10-18（本地）。这是执行证据，不是实现契约。
接续 [P1 CPU 导入](gasp-p1-cpu-import-record.md) 与
[GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。
实现边界见 [动画播放与 GPU 蒙皮](../contracts/animation-runtime-skinning.md)。

## 结果与正式资源

**P2 技术渲染验收通过，预览控件真实鼠标交互仍待补验。**
角色可在正式关卡循环播放、切换片段并 seek；主画面、法线与阴影使用相同姿态。
支持单/双线程，原生动画时钟与 C# Play/Pause/Stop 分开。
本轮没有实现 C# 动画状态机、角色运动闭环或 Motion Matching。

按用户授权，在资源仓库建立 `Maps/SC_gasp/SC_gasp.json`：

- `Meshes/`：骨骼 mannequin 与静态地面描述。
- `Materials/`：VL PBR 审阅材质，不宣称 UE 材质等价。
- `Source/`：隔离导出的 mannequin、12 段 FBX、侧车及地面 OBJ。
- 审计清单：资源根 `Generated/Import/SC_gasp/audit-p0.json`。

正式目录共 44 文件，约 47 MB。`config/config.json` 初始场景未改。
厘米/Y-up、角色 scale=1，EV100=8，方向光 2000 lux，阴影距离 2000 cm。
默认片段 0 为 Idle，Walk 为片段 1；四秒约 800 cm 的 Walk 根位移保留在骨架内。
固定相机不跟随，连续 Walk 可离开镜头；此阶段没有 Actor/Jolt 根运动消费。
正式资源不依赖导出工作目录，数值参考与截图保留在 Generated/artifacts。

原项目 `D:/YYBWorkSpace/UnrealProject/gasp/gasp.uproject` 未作为写入目标，
只使用 `artifacts/gasp-p0/ue-audit-06` 的固定隔离导出。
收尾只读盘点仍为 3902 文件，输入 BLAKE3 与 P0 原目录报告相同：
`8f2fba8d398635082bd06428d6d971c828573f9613c4cd905a8b9fc204e40afe`。
对照报告为 `artifacts/gasp-p0/gasp-inventory-20261018.json` 与
`artifacts/gasp-p2-original-inventory.json`；此摘要范围不是 P1 导出集摘要范围。
技术通过没有补齐 P0 内容使用范围或 UE 行为录像的验收。

## 可复验操作

从引擎仓库根执行：

```powershell
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp.json --dev-ui
```

Content Browser 双击 `SM_mannequin` 使用骨骼模型预览标签。
Console 可执行：

```text
animation get
animation clip 1
animation seek 0.25
animation play
animation pause
```

seek 会暂停；换片段重置时间并保留播放开关。
静态网格不显示动画控件；预览刷新只读冻结值，不发业务请求。
这是实现操作入口，鼠标拖 slider/选片段/点击播放没有在本轮桌面自动化中实测。

CPU 参考与出图命令（输出路径需未存在）：

```powershell
.\build\bin\vl_animation_probe.exe --skin-gasp artifacts/gasp-p0/ue-audit-06 --time 0.25 --output artifacts/gasp-p2-cpu025.json
py -3 tool/animation/build_gasp_skin_reference.py --reference artifacts/gasp-p2-cpu025.json --resources ../VukanLearnResources --name GaspCpu025
& "C:/Program Files/Python314/python.exe" tool/animation/capture_gasp_p2.py --output artifacts/gasp-p2-captures02
```

参考为独立逐影响位置累加后生成的静态 OBJ，
与 GPU 路径共用 P1 导入/采样；因此可检查 GPU 蒙皮及光栅消费，
不能代替独立 UE 对照。UE 源姿态对照沿用 P1 记录。
OBJ 导出撤销最终 CW 索引适配，静态导入时再统一适配。
截图工具需要 numpy/Pillow；本机 `py -3` 的环境没有这些依赖，
最后一条使用已具备依赖的 Python。

## 画面证据

最终报告 `artifacts/gasp-p2-captures02/comparison.json` 的 `passed=true`。
共 10 张 1280×720 完整 BMP，每张 2764854 bytes，stderr 均为空。
已查看 `editor.bmp` 与 `gpu025t1.bmp`，角色、地面、阴影及编辑器布局正常出图。

对照 ROI 为 x=[500,1150)、y=[100,600)，共 325000 像素，
包含角色和阴影，排除不同场景名字的 runtime HUD。
预先固定阈值为误差大于 4 RGB bytes 的像素不超过 ROI 的 0.1%。

| 对照 | 实测 | 结论 |
| --- | --- | --- |
| CPU 静态参考 / GPU Walk 0.25 Beauty | 最大/平均字节误差 0 | 通过 |
| CPU / GPU Normal | 最大/平均字节误差 0 | 通过 |
| 单 / 双线程相同暂停姿态 | 最大/平均字节误差 0 | 通过 |
| 两个新进程不同启动延迟后 seek 同一时刻 | 最大/平均字节误差 0 | 确定性通过 |
| Walk 0.25 / 0.75 | 72326 像素误差 >4 bytes | 姿态/阴影确有变化 |
| 暂停时几何非零 velocity | 0 像素 | 通过 |
| 播放 Idle 时几何非零 velocity | 5610 像素 | 通过 |

确定性 seek 不是同进程暂停后等待多帧的截图；
暂停不推进由模块测试另行覆盖。
velocity 使用正式 `debugview 90` 直接显示，不经过曝光、bloom 和 tone mapping。
天空保留 GBuffer clear 值，不能当作几何零速度，验证只统计实际几何。
截图工具结束自己启动的进程；这些进程不构成正常 teardown 证据。

## 构建与生命周期证据

配置为 MinGW Debug、C# ON、动画工具 ON，全部构建目标通过。
根 CTest 共 1360 项，1354 通过、6 项符号链接权限跳过，零失败，449.53 秒。
UI 目标 648 项、动画目标 42 项。
动画/资产/UI 101 项选择集连续两遍零失败，其中 1 项权限跳过；
最终 scalar bounds 优化后，骨骼/渲染历史 24 项又连续两遍通过。
覆盖九影响、精确 bounds、暂停/seek/循环/冻结、
丢弃快照不推进历史、隐藏后重现清理历史及 generation-gated 控件请求。

Vulkan 检查全部串行运行，退出码均为 0，日志确认 `EngineLoop teardown complete`：

| 检查 | 场景与线程 | 日志 |
| --- | --- | --- |
| Surface/Shadow reload | 正式 GASP，双线程 | `artifacts/gasp-p2-shader-runtime.log` |
| Compute reload | 正式 GASP，双线程 | `artifacts/gasp-p2-compute.log` |
| World/graph transaction | 默认场景，单线程，no-dev-ui | `artifacts/gasp-p2-world-t1.log` |
| World/graph transaction | 默认场景，双线程，no-dev-ui | `artifacts/gasp-p2-world-t2.log` |

Graphics 与 World stderr 为空；
Compute stderr 包含故意改变 workgroup size 的预期 ABI 拒绝，
随后原源恢复、退休和退出检查通过。World 结果不代替 GASP 切换场景的鼠标操作验收。
无新 runtime test 标志，无并行共享 SPV 的测试。
65 条架构边界、39 个正式场景 schema、文档分层和 diff 空白检查通过。
日志存于 `artifacts/gasp-p2-build-all.log`、`gasp-p2-bounds-build.log`、
`gasp-p2-ctest-all.log`、`gasp-p2-focused-03.log`、`gasp-p2-focused-04.log` 等。

## 失败修正与限制

初版正式生成器遗漏厘米声明和完整曝光字段，正式 loader 拒绝；补齐作者配置后启动通过。
默认 CSM 距离 300 cm 不覆盖审阅相机，正式场景显式写 2000 cm。
首次截图集 `artifacts/gasp-p2-captures01` 的速度检查失败：
Walk 播放后移出镜头，工具又把天空 clear 值视为非零速度。
改用默认 Idle、实际几何掩码后重新采集完整 10 张，未放宽像素误差阈值。
Debug 精确 bounds 曾大量执行逐顶点 Eigen 表达式；
改为相同权重次序的列优先 3×4 标量运算，保留奇异/非有限拒绝与数值回归。
没有测量独立性能基准，不宣称角色数量或 FPS 达标。

Computer Use 的 Node 内核在执行前报 `failed to write kernel assets`、`os error 3`，
重置后仍失败。实窗证据使用引擎自身截图；模块 RmlUi 控件验证不冒充鼠标验收。
未增加骨骼材质的静态球 MI 预览；当前模型类型与材质必须相符。
C# OFF 构建本轮未重跑，没有 C# API 变更。

后续 P3 接 C# 高层状态机、角色控制与动画请求反馈，
解决模型前向和实际位移约定；轨迹/查询数据库与 Motion Matching 依计划后续执行。
