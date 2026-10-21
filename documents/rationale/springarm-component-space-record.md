# SpringArm 与组件挂接空间

2026-10-21。背景：GASP BP 页的相机图示停在角色中心，用户进一步要求明确 mesh/component local
space 和父子关系，并核对 UE 的处理方式。本记录解释选择，不代替
[当前运行时契约](../contracts/csharp-blueprint-runtime.md#springarm-组件)。

## UE 接口依据

核对 Epic 官方 API 文档：

- [USceneComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/USceneComponent)：
  AttachParent、AttachSocketName 与 RelativeLocation/Rotation/Scale 分开；
  CalcNewComponentToWorld 接收 NewRelativeTransform、Parent 和 SocketName，
  GetSocketTransform 接收 socket 名称与 ERelativeTransformSpace。
- [USpringArmComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/USpringArmComponent)：
  继承 SceneComponent，公开末端 SocketName 并覆盖 GetSocketTransform；
  SocketOffset 在臂端、TargetOffset 是臂根世界偏移。
  官方特别说明需要参与碰撞检测的偏移应放 SocketOffset，而不是附着组件的 Relative Offset。
- [Spring Arm 教程](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-spring-arm-components-in-unreal-engine)：
  相机附着到 SpringArm 的末端 socket。

以上是官方接口与文档语义核对，不声称已获取用户所用 UE 版本的完整私有引擎源码。

## 空间关系

列向量记法：

```text
ChildToWorld = ParentSocketToWorld * ChildRelative

Actor
  Mesh component -> imported section/model space -> skeletal pose/skin space
  SpringArm component (arm root)
    default socket (evaluated arm endpoint)
      Scene component -> Camera component
```

Mesh 是 SceneComponent 的一种，它的组件空间不等于 bone local space。
SpringArm 的组件原点在臂根，供子级使用的挂接空间在臂端；二者不能合并。
相机通常 relative location 为零，因此恰好在臂端；非零 relative location 仍是合法的子局部偏移。
中间 Scene 的位移、旋转及缩放也应保留。

旧 VL 实现逐个把 Camera 的最终世界位置换算回父空间，再反写相机局部 TRS。
这只能模拟相机跟随，会丢失作者局部位置，非相机子级也没有臂端空间。
只挪 UI 相机图示会继续保留这两个错误，因此改为组件共同消费父挂接空间。

原生 ScriptComponentSpace 同时表达组件世界空间和默认挂接世界空间；
CLR SceneComponent.GetSocketTransform 返回默认挂接空间，SpringArm 覆盖它。
碰撞/lag 结果作为内部 socket 状态发布，作者局部 TRS 不再承载派生的运行时位置。
嵌套臂等待父臂完成碰撞后求值，避免声明顺序或上帧落点影响子臂。

## 有意保留的范围

- VL 世界 Y-up、厘米、相机后向 +Z；UE 的轴约定不直接复制。
- 保留完整矩阵中的 shear，不照搬 FTransform 的非均匀缩放组合数值。
- 当前是每个组件一个默认挂接点，没有命名 socket、骨骼 socket、KeepWorld/Snap 规则表。
- Blueprint 编辑预览显示作者静态姿态；没有脚本、ControlRotation、物理或 lag 求值。
- 臂长/SocketOffset 不缩放；子组件的 relative position 仍由挂接矩阵的缩放变换。
