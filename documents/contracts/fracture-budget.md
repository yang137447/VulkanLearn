# 全场景碎片预算

> 状态：当前契约（实现已落地）。描述 `FractureBudget` 对玻璃动态碎片、
> 玻璃 attached 板片和气球软片的统一计数、回收排序与降级决策。它不描述
> 断裂拓扑生成、Jolt 求解或 GPU 资源生命周期；这些分别见
> `fracture-core.md`、`glass-fracture-runtime.md` 和 `softbody-runtime.md`。

## 模块与所有权

`source/physics/fracture/fractureBudget.*` 是纯 CPU 策略模块，编译进
`vl_fracture_core`，不依赖 Vulkan、Jolt 或 `World`：

- `FractureBudget` 只保存按 owner 分类的计数和可回收候选，不持有网格、刚体或场景对象；
- `FractureEvictionDispatcher` 是回收接口，由 `EngineLoop` 实现；
- `AcquireFractureBudget` 是“先查询、再回收”的便捷入口；需要先判断是否值得
  回收的路径（初次断裂缩小图案、二次碎裂、气球裁剪）直接调用 `Request`，
  确认额度后再执行 dispatcher。

`EngineLoop` 持有唯一 `FractureBudget` 实例，并在构造 `GlassRuntime` /
`SoftBodyRuntime` 时按引用注入。不存在进程级可变单例；同一进程内的多个
`FractureBudget` 互不影响，测试可直接构造。

## 参数

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `GlobalLimit` | 320 | 全场景碎片总数上限 |
| `GlassPaneLimit` | 96 | 单块玻璃（动态碎片 + attached 板片）上限 |
| `BalloonLimit` | 48 | 单个气球软片上限 |
| `MinimumBalloonShards` | 4 | 气球爆裂的视觉下限 |
| `SettledSecondsToReclaim` | 1.5 s | 动态碎片可被回收前需要静止的时间 |

计数对象是运行时创建的碎片：

- 玻璃：每个动态碎片计 1，每个 attached 板片计 1；
- 气球：每个软片计 1（渲染网格与 Jolt body 同属一片，不重复计数）。

attached 板片与没有 Jolt 刚体的气球软片（neck 锚定片或代理注册失败）只计数，不进入
可回收候选。因此回收永远不会把玻璃拆出一个缺口，也不会把气球底部的
锚定片单独抽走。

## 请求与降级顺序

`FractureBudget::Request` 是纯查询：不执行回收，也不改账本。可用额度为

```
capacity = 剩余全局额度 + 已静止的可回收候选数
```

单对象额度按同样方式叠加该 owner 的已静止候选。额度按以下顺序使用：

1. **先回收旧碎片**：只考虑 `settledSeconds >= 1.5` 的动态碎片。排序键依次为
   屏幕占比（包围半径 / `max(相机距离, 1)`，越小越先回收）、相机距离（越远越
   先）、静止时长（越久越先）、owner 与 shard id（保证计划可复现）。占位是
   相机距离与包围半径的启发式，不读取深度缓冲、不做真实投影或遮挡查询。
2. **玻璃 attached 请求退化**：`allowWholePlateDrop=true` 且额度不足时不新增
   碎片，返回 `dropWholePlate`；`GlassRuntime` 走 `DropAttachedPlate`，把命中
   的板片整块转成动态刚体。该操作净增 0 片，因此只要还有 attached 板片就
   一定打得动。
3. **气球裁剪**：气球爆裂按 `grantedCount` 作为球面 Voronoi 的 `siteCount`
   生成软片；额度不足时碎片更少但形状仍由同一生成器决定。
4. **视觉下限**：气球请求至少保留 4 片，即使全局已满；此时
   `exceededLimit=true`。玻璃初次命中同样保底：先减少环数（3→2→1）和母裂纹
   （每次减 3，最低 6 条）重试，仍不足时按最后图案生成并在日志中报告超出量。

调用方拿到带 `evictions` 的决策后，必须先让 dispatcher 回收，再创建新碎片，
最后用 `OnShardsCreated` 记账；销毁路径用 `OnShardsRemoved` 并
`UntrackReclaimable`。`Request` 不修改账本，重复查询不会产生重复回收。

## 运行时接线

- `GlassRuntime`：初次碎裂、attached 板片切碎、落地二次碎裂都走预算；
  每帧 `Tick` 刷新动态碎片的静止时长、包围半径和相机距离。回收时移除 Jolt
  凸包、`WorldMeshObject` 和碎片记录，网格进入既有 GPU epoch 池。
- `SoftBodyRuntime`：爆裂前按剩余额度裁剪 `siteCount`；每帧只对拥有 Jolt
  刚体的软片刷新回收候选。回收时移除刚体、网格和并行的粒子/姿态/预算数组。
- `EngineLoop::EvictFractureBudgetShards` 把同一批候选同时交给两个运行时，
  各自按 owner 过滤，因此候选可以跨玻璃与气球混合。

## World 换代

`EngineLoop` 在成功提交新 World、且 generation 与上次预算归属不同时调用
`Reset()`；材质定义重载等同代事务不清空账本。`Shutdown` 同样重置。

## 当前边界

- 屏幕占比降级是半径/距离启发式，没有真实屏幕面积、视锥剔除或遮挡信息。
- 回收只释放运行时网格与刚体，不区分玩家是否仍能看到该碎片。
- 视觉下限允许气球短暂超出全局上限 4 片，玻璃最小图案也可能超出；
  超限量写进日志，不静默吞掉。
- 碎片网格池仍按玻璃运行时私有，气球软片不共享该池。
- 没有跨 World 的持久预算、统计上报或可视化调试面板。

## 测试

`tool/fracture-core-tests/fractureBudgetTests.cpp` 覆盖全局上限、跨对象回收
排序、未静止碎片不可回收、attached 整块掉落降级、单板上限、气球裁剪与
视觉下限、账本重置和 dispatcher 只执行一次。
