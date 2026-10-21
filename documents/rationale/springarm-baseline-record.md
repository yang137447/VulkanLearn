# SpringArm 相机臂基础记录

状态：执行记录，非当前契约。2026-10-21。记录本轮 SpringArm 组件的基础能力、验证证据和
明确未做的扩展。当前行为以 [C# 蓝图运行时](../contracts/csharp-blueprint-runtime.md)、
[测试与验证](../contracts/testing.md) 和源码为准。

## 1. 目标与边界

目标是让场景作者能在 Blueprint 组件树里放一个 `springArm`，把 `camera` 作为它的直接子节点，
由运行时代替手工摆相机：

- 相机沿臂的局部 +Z 方向摆到 `targetArmLength`；
- 臂和相机之间可以有 `socketOffset`；
- 默认做球扫碰撞，遇到墙时缩短臂长，避免第三人称镜头穿墙；
- 保持现有 `camera.autoActivate`/`Activate()` 的镜头选择语义，不引入第二套相机栈。

本轮只做“能挂、能摆、能缩、能存”的基础功能；相机滞后、平滑、旋转控制、调试绘制等留给
后续，避免一次把 SpringArm 做成完整的 UE SpringArmComponent。

## 2. 实现

### 2.1 组件 schema

`springArm` 是 `SceneComponent` 的子类，只有数据字段，不自己发查询：

| 字段 | 默认 | 约束 |
| --- | --- | --- |
| `targetArmLength` | 300 cm | 0..100000，有限 |
| `socketOffset` | (0,0,0) | 臂局部 cm，三分量有限 |
| `probeRadius` | 12 cm | 0..100000，有限 |
| `doCollisionTest` | true | 布尔 |
| `collisionMask` | 65535 | 0..65535 的整数 |

schema 在 `source/script/scriptComponents.h` 校验；编辑器 Details 在
`source/editor/ui/scriptComponentControls.h` 暴露这些字段，`scriptComponentOverrides.h`
允许继承 Blueprint 的 Scene 实例覆写；`EditComponent` 接受同名 JSON 字段，向量 `socketOffset`
支持逐轴编辑。

### 2.2 每帧时序

1. `tick`：C# Tick/LateTick 之前，先把 Blueprint 组件树同步到 C#，owner/arm 的世界矩阵按本帧
   Tick 前的状态计算；
2. Tick 与结构指令排空后，`QueueSpringArmProbes` 对每个启用 owner 的每个 arm：
   - 先用完整臂长把相机摆到 `socketOffset + (0,0,targetArmLength)` 的臂世界点；
   - `doCollisionTest=false` 或臂长为 0 时不发查询；
   - 否则从 `socketOffset` 世界点到臂末端发一次球扫，半径 `probeRadius`，掩码 `collisionMask`，
     并忽略 owner 自身；
3. 原生物理 `StepPhysics(dt)` 执行；
4. `late`：`CompleteRays` 先消费本帧射线结果，命中时相机停在
   `origin + segment * ((hitDistance - probeRadius) / length)`，再执行 LateTick；
5. 导出相机局部位置，原生 `UpdateScriptCamera` 用父链矩阵重建脚本相机并冻结进 WorldSnapshot。

结果：相机位置在本帧物理后更新，不会留在墙里；无命中/关闭碰撞/超预算时保持完整臂长。
活动镜头仍由 `camera.autoActivate` 或脚本 `Activate()` 决定；场景作者相机只在该 owner 没有
活动脚本相机时接管。

### 2.3 状态与编辑

- `ComponentDescription` 新增 `springArm` kind 和字段，`CreateComponent`/`ReadSpringArmNode`
  支持运行时描述创建；
- 组件状态导出带 `targetArmLength/socketOffset/probeRadius/doCollisionTest/collisionMask`，
  变化检测纳入 `ComponentChanged`，切镜头/重载时不会丢字段；
- `EditComponent` 的字段白名单、向量轴白名单、布尔/数字控件和默认值回退更新；
  其中 `doCollisionTest` 缺省为 true，编辑面板的布尔回退值必须与运行时缺省一致，
  否则在未显式存字段的组件上把 false 当成 no-op；本轮修正了该回退；
- 场景 schema 仍要求一个作者相机；运行时脚本相机激活后覆盖作者相机，这是既有相机语义，
  不是 SpringArm 引入的第二套选择。

## 3. 验证

| 项 | 结果 | 证据 |
| --- | --- | --- |
| SpringArm 运行时测试 | 7/7 | `artifacts/p28/springarm-tests2.log` |
| SpringArm 编辑保存往返 | 1/1 | 同上 |
| 相关既有脚本/场景测试 | 40/40 | `artifacts/p28/springarm-related-tests.log` |
| 全量 `script_tests` | **408/408**（1,259,763 ms） | `artifacts/p28/springarm-full-tests.log` |
| 运行时冒烟 | exit 0、stderr 空、config 逐字节恢复、无残留 main | `artifacts/p28/springarm-smoke/capture.json` |
| 冒烟截图 | 1280×720，SpringArm 子相机视角可见地面与方块 | `artifacts/p28/springarm-smoke/springarm.png` |

冒烟场景 `Maps/SC_springarm_smoke/SC_springarm_smoke.json`：owner 在原点、arm 在 (0,90,0)、
臂长 300、墙在 z=150、球半径 12；截图里的镜头由 SpringArm 子相机接管，而不是场景作者相机。

## 4. 本轮明确未做 / 待完善

1. **一帧延迟**：arm 世界矩阵取自本帧 Tick 前状态，射线结果在本帧物理后应用。角色快速移动时
   相机会比角色晚一帧到位；UE 的 SpringArm 在移动组件之后同步求值。要消除需要把 arm 刷新
   放到物理后、或增加物理后的同步查询通道。
2. **没有相机滞后/平滑**：没有 `bEnableCameraLag`、`CameraLagSpeed`、位置/旋转阻尼或插值；
   当前是硬贴臂长。
3. **没有臂旋转控制**：没有 `bUsePawnControlRotation`、`bInheritPitch/Yaw/Roll` 或
   `bAbsoluteRotation`；镜头朝向只由作者相机局部旋转 + 父链（含 arm 旋转）决定。
4. **没有调试绘制**：不画臂线段、球扫轨迹或命中点；排查穿墙只能看相机结果。
5. **没有运行时改 arm 字段的 API**：C# 字段可读写，但本轮没有像相机 FOV 那样的
   Set 命令/迁移保证；编辑器改字段走作者数据路径。
6. **多 arm/多相机优先级**：每个 arm 找自己的直接子相机；多个 autoActivate 相机仍按首个选中，
   没有优先级/混合。
7. **只支持直接子相机**：相机必须直接挂在 arm 下；隔一层 Scene 组件不会被识别。
8. **编辑预览**：Details 有字段，但没有 viewport 内的臂长/碰撞可视化预览。
9. **场景作者相机仍必填**：场景验证要求至少一个作者相机；纯脚本相机场景也不能省。

这些是后续按需扩展，不应在本轮基础功能里假装已实现。

## 5. 证据与提交

- 证据：`artifacts/p28/springarm-build-main.log`、`springarm-build-main2.log`、
  `springarm-build-script-tests.log`、`springarm-csharp-build.log`、`springarm-related-tests.log`、
  `springarm-tests.log`、`springarm-tests2.log`、`springarm-full-tests.log`、`springarm-smoke/`。
  最终 main 构建 `springarm-build-main2.log` 为 0 error/0 native warning，二进制 SHA256
  `6F9CD73C91ACEF7E835F7C7A0CCC9F53EA2D48B100A42047D5E5AB8F437F1ADE`。
- 提交：引擎 `a76ba40` `【Mod】add spring arm camera component`、资源
  `7f8b101` `【Mod】add SpringArm smoke scene`、文档 `【Doc】record SpringArm baseline`
  （提交哈希即本记录所在提交）。
