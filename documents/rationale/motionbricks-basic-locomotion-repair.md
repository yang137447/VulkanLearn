# MotionBricks 基础运动修复记录

日期：2026-11-19。分支：`codex/motionbricks` 与 `codex/motionbricks-assets`。
本记录描述工程验收，不声明论文指标复现或脚部 IK 已完成。

## 原因与修改

- live 的四个目标原先都取参考姿态，只修改根，且第一目标仅前进
  1.5 cm，却用作整段动画的终点。改为官方线性 Smart Primitive 的
  Idle/Walk/Jog/Run 四帧局部姿态，分别控制一秒目标距离和 speed/30 终点速度。
- Hips 的局部 Y 轴并非地面向上轴。原先右乘局部 yaw，导致小角度输入产生
  大幅地面转向。改用左右髋关节提取 heading，并绕世界 Y 轴放置四个目标。
  初始 Idle 也放置到 -Z 朝向，避免第一段多转半圈。
- SOMA 为 T-pose、UEFN 为 A-pose。源参考的肩、上臂、前臂、腿与脚
  按目标绑定骨段方向校准，解决生成动作的手臂交叉。
- 原先将重定向后的整个 root 恢复为参考，连骨盆起伏与倾斜也删除。
  现在先去除源地面位移与 heading，再将重定向 root 的残余变换交给 pelvis。
- 根反馈已经处于连续模拟空间；蓝图不再额外乘 Actor yaw。
  相机保持固定地面朝向，W/S=-Z/+Z，D/A=+X/-X。
- 异步结果带控制版本，过期结果丢弃；GT 按最新显示姿态重新校正生成起点。
  蓝图流使用脚本倍率时钟，Pause 冻结，Stop 释放实例，重新 Play 重建历史。

## 实机验收

使用 `tool/animation/capture_motionbricks.py`，通过确定性输入蓝图驱动真实
ONNX → external pose → CLR → Jolt 胶囊与渲染链路。
速度为稳定阶段约两秒内的实际 Jolt 水平速度均值，单位 cm/s：

| 动作 | 主要方向速度 | 接地 |
| --- | ---: | --- |
| Walk | Z=-152.36 | true |
| Run | Z=-320.45 | true |
| Right | X=149.60 | true |
| Left | X=-131.99 | true |
| Back | Z=151.23 | true |
| Stop | 水平漂移约 5.64 | true |
| Idle | 水平漂移约 0.47 | true |

Walk 场景还通过 Pause/Resume、Stop 后重新 Play 验证。
截图检查确认手臂不再交叉，转向与实际移动方向一致。
原始日志、截图、`runtime-summary.json` 与拼图位于代码工作树
`artifacts/motionbricks/`，该目录被 Git 忽略。
临时 Verification 资源已由工具清理，正式 Gameplay 程序集恢复。

## 数据与边界

`soma_locomotion_targets.json` 是官方 BVH 的采样数据，单独适用随附的
NVIDIA Sample Data Evaluation License，供内部评估 NVIDIA 技术使用；
不是 Apache-2.0 源码许可下的合成 golden vector。ONNX 权重不进入 Git。

此修复覆盖平地基础运动。当前未加入脚部接触 IK、碰撞后的生成轨迹重规划、
跳跃或翻越控制；这些能力不能由本次七种动作验收推导。

## 专项结果与扩展回归

最终 `ctest --test-dir build -R '^(MotionBricks|ExternalPose)'` 为
49/49 通过，真实 ONNX 测试启用，耗时约 87.6 秒；
包含手臂参考姿态、四方向目标、真实模型走跑转停、零时步冻结、
过期控制结果丢弃及 external pose 提交与暂停。
当前配置的 27 个 GoogleTest 目标全部编译成功；C# 开启的
`main_editor` 与 C# 关闭的 `main_game` 均构建成功。
架构边界审计、文档分层校验与 `git diff --check` 通过。

`ScriptAnimationTest.FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`
仍失败：台阶模式漂移 3.7379527 cm 超过 3 cm，两个支撑误差分别约
10.9987526 / 10.8855782 cm 超过 1 cm。工作树原有
`artifacts/foot_quality_retry.log`（2026-11-18）包含完全相同的数值，
本次未修改 GASP 足部算法或放宽断言。

脚本全量第二轮为 528 项：526 通过、1 跳过、上述 1 项失败。
第一轮另有动态构建测试在并行编译负载下超时，第二轮通过。

World/graph 事务门禁未通过：异步模式在失败回滚期间观察到 buffer
身份数量由 121 增至 124；同步、关闭编辑器 UI 后在 resize 计数检查失败。
静态 `SC_scene01` 也复现，故不能将该门禁计作通过。
无 C# 的 `main_game` 编译与场景运行完成并返回 0，但退出时触发
`VUID-vkDestroyDescriptorPool-descriptorPool-00303`；该诊断在不含
MotionBricks 的静态 `SC_scene01` 同样出现。此次未修改 renderer 资源退休逻辑。
