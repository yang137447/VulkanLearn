# 投射物与试玩关卡

> 状态：当前契约（实现已落地）。描述 `ProjectileRuntime` 的可见箭矢、输入发射、
> 命中冻结姿态，以及气球/玻璃共享同一个 `FractureImpactWorld` 的试玩关卡纵切。
> 它不描述弓的蓄力/弹药/计分/存档，也不描述 HUD 文本；这些尚未接入。

## 共享冲击世界

`EngineLoop` 持有唯一的 `Physics::FractureImpactWorld`，并把引用传给
`SoftBodyRuntime`、`GlassRuntime` 和 `ProjectileRuntime`：

- 每帧只在 `EngineLoop` 调用一次 `Step(deltaSeconds)`；
- 事件列表再分发给气球和玻璃运行时；每个事件携带目标注册时的
  `FractureMaterialProfile`（`balloon` / `glass` / `generic`），两个运行时
  只消费自己的 profile，不再靠 `objectName` 猜归属；
- World 换代时先 `Clear()` 共享世界（销毁所有 body 并重建调试地面），
  再让各运行时重新注册代理；
- 因此同一支箭可以命中气球或玻璃，不再存在“两个世界各打各的”的限制。

目标注册时必须声明 profile：气球球体 proxy 和气球软片使用 `Balloon`，
玻璃板盒 proxy 和玻璃碎片使用 `Glass`。`FractureImpact` 同时携带
`type`（`BalloonHit` / `PaneHit` / `AttachedHit` / `ShardLanding` /
`ShardHit`）与 `materialProfile`；后者是运行时选择破坏策略和音频的主键，
前者只描述接触形态。未接入专用运行时的目标使用 `Generic`，事件会被两个
运行时都忽略，不会误播玻璃或气球音。

`FractureImpact::projectileId` 携带命中的投射物句柄；`LaunchProjectile` 返回同一个句柄。
`HasProjectile`、`GetProjectilePose`、`GetProjectileVelocity` 供可见层查询。

## 箭矢

`ProjectileRuntime` 在 GT 创建一支可见箭：

| 属性 | 当前值 |
| --- | --- |
| 网格 | 运行时生成：3 cm 见方箭杆（局部 Z -30..22）+ 四棱锥箭头（22..30） |
| 材质 | 复用场景内第一个 glassPane，其次 balloon，最后 mesh 的材质 key |
| 速度 | 3500 cm/s，方向取渲染相机前向 |
| 发射点 | 相机位置 + 前向 45 cm |
| 刚体 | 半径 1.5 cm、质量 0.08 kg 的动态球体，LinearCast 连续碰撞 |
| 冷却 | 0.15 s，防止输入异常时一帧多箭 |

飞行中每帧用投射物刚体位置 + 速度方向更新 `WorldMeshObject::model`。命中
`BalloonHit` / `PaneHit` / `AttachedHit` / 带 `projectileId` 的动态碎片
`ShardHit` 后，箭不再跟随刚体（刚体已回收），而是冻结在
`impact.point - direction * 30`，2.5 秒后移除。动态碎片命中会继续触发
`GlassRuntime` 的二次碎裂检查。

准星是一个屏幕空间大小的 debug draw 点，位于相机前向 450 cm，Overlay 深度，
不依赖场景网格或 UI 文档。

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
