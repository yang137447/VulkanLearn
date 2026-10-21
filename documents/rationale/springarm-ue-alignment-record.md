# SpringArm UE 行为对齐记录

状态：执行记录，非当前契约。2026-10-21。承接
[基础记录](springarm-baseline-record.md) 与工作区已有的 SpringArm 扩展。
当前接口和边界以 [C# 蓝图契约](../contracts/csharp-blueprint-runtime.md#springarm-组件) 为准。

## 对齐范围

参考 Epic 的 [USpringArmComponent API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/USpringArmComponent)：
TargetOffset 使用世界空间，SocketOffset 位于臂末端；目标旋转、逐轴继承、位置/旋转 lag、
子步上限及碰撞诊断采用同类职责。本轮没有运行 UE 二进制做逐帧数值差分。

- 在物理、LateTick 与动画指令同步之后求臂端并同步查询。新增的是桥接内部阶段，
  不是脚本回调或 runtime test flag；没有增加物理时间步。
- 控制旋转、继承开关与绝对旋转同时决定臂端和镜头朝向。禁止继承的轴保留 arm 局部角；
  修正 YXZ 四元数转 Euler 的混合轴公式。父缩放仍影响臂根位置，不伸长臂或 SocketOffset。
- 位置 lag 平滑臂根，旋转 lag 平滑方向，然后才做球扫。单步使用
  `clamp(speed*dt,0,1)`，速度 0 直接到目标；位置与旋转独立开关。
  子步沿前后原始目标插值，保留最大 lag 距离与显式传送复位。
- 从未滞后的臂根（含 TargetOffset）扫到 lag 后的完整臂端（含 SocketOffset）。
  Jolt 返回的是接触球心，不再重复减半径；静止相机也逐帧查询，避免移动障碍物的旧命中缓存。
- 多个相机共用臂端、保留各自作者朝向；最近的 arm 接管嵌套相机。
  相机初始优先级属于 VL 扩展，显式 Activate 仍拥有活动选择。
- 参数、运行时描述、导出、实例覆写、编辑保存与 UI 刷新接通；修正 collisionMask 被当成
  collider 编辑的前缀分支，并把组件权限/覆写上限由 16 提到 32。
- 回调异常恢复 arm 参数与 ControlRotation；程序集重载保留平滑历史和作者相机相对旋转。
  调试绘制按有界组件代际区分 key，结构换代清理移除相机的历史键。

## 验证证据

全部路径相对仓库根，`artifacts/` 为 gitignored 本机证据。

| 范围 | 结果 | 证据 |
| --- | --- | --- |
| Debug MinGW main / script_tests / editor_ui_tests | 构建通过 | `artifacts/springarm-ue-final-build.log` |
| SpringArm 运行时 34 + 编辑保存 1 + 实例覆写 1 | 36 项连续两遍通过 | `artifacts/springarm-ue-tests-final.log` |
| 时钟、回调掩码、增量、重载、物理、Gameplay、运行时、作者、资产与 DrawDebug | 195 项通过 | `artifacts/springarm-ue-regression.log` |
| UI 全量 | 670 项：665 通过，5 项符号链接权限跳过，零失败 | `artifacts/springarm-ue-ui-tests-final.log` |
| SpringArm 真实窗口冒烟 | exit 0、stderr 空、config 逐字节恢复、无残留 main | `artifacts/springarm-ue-smoke/capture.json` |
| 截图 | 1280×720，地面/方块/近墙与臂调试线可见 | `artifacts/springarm-ue-smoke/springarm.png` |

脚本当前注册 436 项，本轮没有完整运行这 436 项，也没有重跑三项 Vulkan runtime gate 或
C# OFF 构建。30/60/120 Hz 对照属于恒速移动的 CLR/Jolt 模块测试，不是 GASP 全场景实窗矩阵。
冒烟只验证已有 smoke 场景，没有修改资源仓库或迁移正式 GASP 控制器。
主程序 SHA256：`890D646E7E01FED7B595ACCD73FEAC7166E2284C8B98346614D0EECC9D7025B6`。

UI 用例发现 RmlUi select 的同步 SetValue 可以重入派发同值 change；测试验证字段/数值与
作者操作幂等语义，不把同值通知条数当作产品契约。刷新仍要求零编辑请求。
新增用例触及 MinGW COFF section 上限，沿用项目既有的单文件 `-Wa,-mbig-obj` 设置。

## 保留边界

这是相机臂行为子集。碰撞仍用 Jolt mask，没有 UE TraceChannel/Profile、PlayerCameraManager、
镜头混合、BlendLocations 扩展点或物理最大 dt 联动。没有编辑器臂线预览。
SceneComponent.WorldMatrix 保留作者父链，没有通用 socket 表，非相机子组件不跟随虚拟臂端。
相机局部 TRS 由臂接管；未裁剪位置与碰撞诊断在 LateTick 中读取的是上一帧结果。
