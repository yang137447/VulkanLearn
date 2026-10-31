# 投射物与试玩关卡

> 状态：当前契约（实现已落地）。描述 `ProjectileRuntime` 的可见箭矢、输入发射、
> 命中冻结姿态，以及气球/玻璃共享同一个 `FractureImpactWorld` 的试玩关卡纵切。
> 它不描述弓的蓄力/弹药/计分/存档，也不描述 HUD 文本；这些尚未接入。

## 共享冲击世界

`EngineLoop` 持有唯一的 `Physics::FractureImpactWorld` 和
`DestructibleRuntimeRegistry`，并把引用传给 registry 中的
`SoftBodyRuntime`、`GlassRuntime` 和 `ProjectileRuntime`：

- 每帧只在 `EngineLoop` 调用一次 `Step(deltaSeconds)`；
- 事件列表交给 registry，再分发给所有可破坏运行时；每个事件携带目标注册时
  的 `FractureMaterialProfile`（`balloon` / `glass` / `generic`）和
  `FractureBodyRole`，两个运行时只消费自己的 profile，不再靠 `objectName`
  猜归属；
- World 换代时先 `Clear()` 共享世界（销毁所有 body 并重建调试地面），
  再让各运行时重新注册代理；
- 因此同一支箭可以命中气球或玻璃，不再存在“两个世界各打各的”的限制。

目标注册时必须声明 profile：气球球体 proxy 和气球软片使用 `Balloon`，
玻璃板盒 proxy 和玻璃碎片使用 `Glass`。`FractureImpact` 使用三个正交字段：
`targetRole`（`StaticTarget` / `AttachedPart` / `FreeShard` / `Projectile` /
`Ground`）、`contact`（`Impact` / `Resting`）和 `materialProfile`。material
profile 是选择破坏策略和音频的主键，role/contact 只描述结构角色和接触形态；
事件不再使用材料名 `FractureImpactType`。未接入专用运行时的目标使用
`Generic`，事件会被两个运行时都忽略，不会误播玻璃或气球音。

`FractureImpact::projectileId` 携带命中的投射物句柄；`LaunchProjectile` 返回同一个句柄。
`HasProjectile`、`GetProjectilePose`、`GetProjectileVelocity` 供可见层查询。

## 箭矢

`ProjectileRuntime` 在 GT 创建一支可见箭：

| 属性 | 当前值 |
| --- | --- |
| 网格 | 运行时生成：3 cm 见方箭杆（局部 Z -30..22）+ 四棱锥箭头（22..30） |
| 材质 | 场景顶层 `projectile.materialInstance` 显式指定；不再扫描其他对象猜材质 |
| 速度 | 3500 cm/s，方向取渲染相机前向 |
| 发射点 | 相机位置 + 前向 45 cm |
| 刚体 | 半径 1.5 cm、质量 0.08 kg 的动态球体，LinearCast 连续碰撞 |
| 冷却 | 0.15 s，防止输入异常时一帧多箭 |
| 生命周期 | 6 s TTL；可见箭最多 48 支；命中或超时都主动 `CancelProjectile` |

飞行中每帧用投射物刚体位置 + 速度方向更新 `WorldMeshObject::model`。命中
带 `projectileId` 的静态目标、attached 板片或动态碎片后，箭不再跟随刚体
（刚体已回收），而是冻结在
`impact.point - direction * 30`，2.5 秒后移除。动态碎片命中会继续触发
`GlassRuntime` 的二次碎裂检查。命中 `Ground` 的箭直接移除；射向天空/
未命中目标的箭由 6 s TTL 回收，不能无限累积 Jolt body 和 arrow mesh。

准星是一个屏幕空间大小的 debug draw 点，位于相机前向 450 cm，Overlay 深度，
不依赖场景网格或 UI 文档。

场景通过顶层 `projectile` 配置速度、半径、质量、冷却和材质实例；缺少
`projectile` 时投射物运行时只报告 warning，不创建箭矢。`SC_fracture_playground`
显式配置为 `Common/Materials/MI_pbr_preview.json`。

## 输入

默认 gameplay action `Fire` 绑定 `MouseLeft`（`source/input/gameplayInput.h`）。
`EngineLoop` 每帧读取 `actionState.gameplay["actions"]["Fire"]["pressed"]`；
只在按下沿发射一次，鼠标被 UI 占用时 `pointer` 权限会阻断该 action。

调试命令 `shoot` 走 `RuntimeCommandType::FireProjectile`，与鼠标路径调用同一个
`ProjectileRuntime::Fire`，用于自动化截图和脚本化验证。

## 试玩关卡

`Maps/SC_fracture_playground/SC_fracture_playground.json`：

- 3 个气球（中心、左后、右后）和 2 块玻璃（左前、右前）；
- 气球 `pressure: 0`，保持稳定完整，由箭命中触发爆裂；
- 场景声明 `runtimeMode: "gameplay"`，没有 C# Play 状态也直接推进 native
  破碎/投射物物理；authoring 场景仍受 Play/Pause/Editing 门控；
- 场景顶层 `projectile` 显式给出箭矢材质与弹道参数。
- 相机在射击线后方，默认准星指向中心气球；
- 操作：`Esc` 切换鼠标捕获（相对鼠标模式），`WASD` 移动，左键射箭。

## 当前边界

- 没有 HUD 文本、准星贴图、命中反馈或计分；只有 debug draw 准星和控制台提示。
- 没有弓、蓄力、箭袋/弹药和换弹。
- 投射物命中第一个目标后即被消耗，不会穿透玻璃或继续飞行。
- 箭可命中气球、玻璃整板、attached 板片和地面动态碎片，并在命中点停留 2.5 秒；
  与调试地面本身仍不产生可停留碰撞。
- 关卡没有关卡流/胜负条件/存档；它是单个场景。
- 气球与玻璃仍各自维护碎片/软体状态，只是共享接触世界。
