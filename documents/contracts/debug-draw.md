# 公共 DrawDebug

> 状态：当前契约。C++/C# 共用世界空间图形收集、不可变快照、RmlUi 叠层和过滤控制。

## 接口与坐标

`source/debug/drawDebug.h` 的 `VL::Debug::DrawDebug` 是独立 CPU 模块，
不依赖 World、CLR、UI 或 Vulkan。每个 World 独占一个 GT 收集器，
通过 `World::GetDebugDraw()` 访问；入口负责验证与几何展开，显示侧只消费冻结值。

| C++ / C# 方法 | 几何约定 |
| --- | --- |
| `DrawDebugPoint` | 世界位置，屏幕像素边长 |
| `DrawDebugLine` | 世界起点、终点 |
| `DrawDebugPolyline` | 至少两个点，可闭合 |
| `DrawDebugArrow` | 世界起终点，厘米箭头长度，四条箭头边 |
| `DrawDebugCircle` | 局部 XZ 平面，四元数旋转到世界 |
| `DrawDebugSphere` | 三个正交大圆 |
| `DrawDebugBox` | 中心、半尺寸、局部到世界四元数 |
| `DrawDebugCapsule` | 局部 Y 轴；参数为圆柱半高，球冠另加 radius；半高零等于球 |
| `DrawDebugCoordinateSystem` | 世界原点、长度、朝向；X 红、Y 绿、Z 蓝 |
| `DrawDebugString` | 世界锚点，8～48 像素文字，RmlUi 字体回退与转义文本 |

厘米、Y-up；朝向使用单位四元数。颜色为线性 RGBA，分量在 [0,1]；
厚度为 0.5～32 屏幕像素。覆盖层执行显示颜色转换，不经过场景曝光。
`Style.depth` / `DebugDrawStyle.Depth` 默认 `Overlay`，穿透场景显示；
可选 `DepthTest` 按场景深度逐像素遮挡，`SupportsDepthTest` 为 true。
编辑器和沉浸显示使用同一渲染适配。
关闭整个 UI 子系统时没有可见 DrawDebug 叠层。

## 生命周期与身份

`Style` / `DebugDrawStyle` 的 duration/Duration：

- `0`：单个完整 GT 帧，该帧内所有提交在该帧累积。
- `-1`：保留到显式清理。
- 正数：按游戏秒计时，Playing 使用 Time Scale，Editing 使用真实帧 delta。
  Pause 不清单帧项、不推进到期；Time Scale=0 冻结计时，仍可执行游戏回调。

Engine 在控制命令之后、游戏更新之前调用 `BeginFrame`（Pause 跳过），
只清理 `EndFrame` 已标记完成的单帧项。这样 Console、BeginPlay 在同帧新提交的项
不会误删；帧结束即使没有 UI 也调用 `EndFrame`。
独立使用收集器的调用者同样负责这对帧边界与 `AdvanceGameTime`。

非空 key 按 owner + publisher + category + key 替换，重置到期时间并保留原生句柄。
空 key 每次追加；原生句柄在不同 World 之间也不复用。
支持按句柄删除、key 删除、全部/owner/publisher/category 清理。
`Clear()` 仅删除图形，保留显示过滤设置和游戏时钟。
关闭主开关或类别、切换 owner 过滤只改变可见集合，不删除图形或暂停其计时。
空 ownerFilter 表示全部；UI 的 owner 选项只列非空身份。

Stop、脚本运行时 Shutdown 与 World 换代清理旧 World 图形；
成功的程序集迁移只清脚本 owner 的图形，原生图形保留；失败迁移保持活动图形。
已冻结的快照不受清理、换代或后续提交影响。

## C# 所有权

Blueprint 与 Component 通过 `DebugDraw` 服务生产请求。
仅允许所属实例的 GT 游戏回调；OnCreate、OnReload、EndPlay、后台线程、
旧上下文和其它实例的服务调用被拒绝。owner 由原生固定为 `Script/<blueprint-id>`，
逻辑组件以组件 id/代际作为独立 publisher。
`Remove(handle)` 和 `Clear(category?)` 限于本次 Play、当前根/组件发布者。
自动 key 使用保留的 `~` 前缀，用户 key 不得使用它。

回调异常撤销本次绘制请求；故障、禁用、移除、销毁清理对应旧图形。
每个实例导出的绘制批次先在原生候选收集器中验证，再整体发布。
批次验证/World 预算失败只报告调试警告，不中断游戏，也不部分发布新几何；
自动生命周期清理独立提交，不能被同批新几何拒绝回滚。
跨桥绘制消息为一次性消费，空回调不重复发送图形。

```csharp
protected override void Tick(float dt)
{
    DebugDraw.DrawDebugArrow(Transform.Position, Transform.Position + new Vector3(0, 100, 0),
        style: new() { Category = "AI", Key = "heading", Color = new(0, 1, 0, 1),
            Depth = DebugDepthMode.DepthTest });
}
```

该例使用 `System.Numerics.Vector3`；默认单帧请求每次 Tick 重画。
对一次性探测结果设置 `Duration = 2`，对持续标记设置 `Duration = -1` 并保留句柄，
然后在同发布者的回调调用 `DebugDraw.Remove(handle)`。

## 快照与显示

`Freeze()` 深拷贝几何、标签和过滤状态为 `shared_ptr<const Snapshot>`。
`UiViewModelSnapshot` 连同相机、World generation 冻结到 GT 显示边界，
RT 不访问可变收集器。线段做近远面和视口裁剪；点与文字检查锚点可见性，
最终几何和标签裁剪到场景视口。命令放在场景图片之后、工具条/菜单/HUD 之前。
文字为独立、无交互的 RmlUi context，复用有界标签槽位。

DepthTest 复用当前图的单采样 resolved `sceneDepth`，不回读 CPU，也不写场景深度。
线段按裁剪后的屏幕端点插值 device Z；点和文字使用各自锚点的恒定深度。
因此文字是以锚点深度展开的屏幕标签，其每个字形像素分别接受遮挡。
调试图形彼此不遮挡；透明物体只有写入场景深度时才参与遮挡。
MSAA 使用图已有的 SampleZero 深度 resolve，轮廓按单个深度样本比较。
没有可采样、已 STORE 的二维 `sceneDepth` 的自定义图或离线显示隐藏 DepthTest 项。

RT 在 acquired image fence 后更新该 slot 的深度 descriptor；场景图像、深度的借用
均限于本次录制，换图/resize 不缓存旧资源。PassBarrier 根据图末访问计划转换为
depth-read-only，UI 录制后恢复图末布局。UI shader 的 set 1 binding 0 为深度采样，
16 字节 fragment push constant 为逻辑场景视口；set 0 继续绑定普通 UI 纹理。
每个 UiVertex 的 `debugDepth=-1` 表示普通 UI/Overlay；非负值为相机投影深度。

轨迹/关节通过 `World::CaptureDebugDraw` 作为 owner/category `Motion`、
publisher `Engine.Motion` 提交；动画只生产几何值，
公共显示端没有动画依赖或专用轨迹渲染器。
足部 IK 的 pre/post 参考点连线在同一 publisher 下按红→绿分段显示；
只要 FootPlacement active 就提交，不依赖 Motion 轨迹开关。
Motion 仍保留自己的采样观察 DTO 和开关，语义见
[动画轨迹与特征](animation-motion-features.md)。

## 控制与预算

视口工具条 DrawDebug 弹窗含主开关、类别开关、owner 筛选、可见/存储项数和清空。
显示刷新不提交命令；所有操作只生产带 World generation 的值请求，
GT 在正式命令执行边界验证，离线模式拒绝。
Console 用法见 [运行时调试命令](debug-runtime-commands.md)。

单 World 上限：1024 项、16384 线段、1024 点、128 标签、128 活动类别；
显式类别过滤记录上限 128。坐标绝对值 ≤1e7 cm，尺寸 ≤1e6 cm，
点大小 ≤128 px；折线 ≤1024 点；圆分段为 8～128 内的 4 倍数。
文字 1～512 UTF-8 字节且无 NUL，duration 为 -1 或 0～3600。
名称按 UTF-8 字节限制：category/key 64，owner/publisher 128，无控制字符。
四元数平方模误差 ≤1e-4。几何预算在替换旧项前检查，拒绝不破坏旧项。
C# 单批最多 512 消息，另以保守 JSON 展开估算限制在 512 KiB，
自动清理不受生产预算阻断。Motion 预算不足则丢弃本组轨迹，不中断渲染。

验证入口：`debug_draw_tests`、`ScriptDebugDrawTest`、
`DebugDrawOverlay`、`DebugDrawLabelTest`、`EditorUiTest.DebugDraw*`；
完整验证映射见 [测试](testing.md)。
