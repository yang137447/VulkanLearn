# 合成 VT 的全视距 height-blend 保留方案与测量

日期：2026-11-19。本文为实现权衡与验证记录，不是运行时契约。
2026-11-20 补充：下文记录首轮兼容方案；新交付夹具使用显式
`u_heightBlendDistanceFade=0` 的静态模式，全视距读取合成基础属性。
旧默认值 1 保留本文的距离分支。静态模式源码与现状以
[合成 VT 契约](../contracts/terrain-composited-vt.md) 为准。
对应设计：[资源生命周期计划](../plan/rendering/terrain-composited-vt-lifecycle-plan.md)。
工作区：`codex/terrain-openworld`，包含其他线程尚未提交的集成修改；
不把 HEAD 当作本文测量的完整代码版本。

## 为何不把视距混合直接烘成一张页

旧材质使用 `smoothstep(8,45,distanceM)` 调制每层高度分数，再执行最大分数、
clamp、原权重相乘与重新归一化。相机距离改变基础层 albedo、normal、ORM，
并非仅改变后处理。即使保存 fade=0 和 fade=1 两套最终属性页，线性插值也不等价于
中间 fade 的非线性权重与归一化；直接固定作者混合又会改变既有外观。

首轮实现采用保留旧材质外观的兼容方案：

| 条件 | 基础层评价 |
| --- | --- |
| 合成模式关闭 | 保留旧直接数组/层源 VT 分支 |
| 合成模式开启，`heightBlendStrength>0` 且距离小于 45 m | 直接源数组，保留原 `smoothstep(8,45,distanceM)` 和源高度 mip3 |
| 合成模式开启，`heightBlendStrength>0` 且距离不小于 45 m | 合成 VT；Cook 基础混合固定 fade=1 |
| 合成模式开启，`heightBlendStrength=0` | 全视距合成 VT，无高度重加权 |

在 45 m 处旧 smoothstep 已精确饱和为 1，因此上述分支使用同一高度混合公式；
没有把非线性过渡近似为两张结果页的插值，也没有增加第二套页面缓存。
有限分辨率、源纹理过滤、RGBA8 量化及宏法线插值仍会造成直接源与合成页的差异，
不能据此声称任意真实场景的切换均逐像素相等。

`TerrainCompositedUsePage` 由主视图和距离验证 shader 共用。
主视图在距离分支之前计算源 UV 导数与合成目标 mip；直接分支使用
`textureGrad`，VT 分支调用 `MF_TerrainCompositedVTAtMip`。
这避免在非一致距离分支内求导导致未定义 LOD。

## 构建与集成边界

`TerrainCompositedBakeGpu::Validate` 在 `heightBlendStrength>0` 时只接受
显式 `frozenHeightBlendFade=1`。默认 -1、fade=0 或任意中间值均失败。
调用方仍需把实际 strength、固定 fade、源高度内容、catalog、
映射、UV 和过滤配置纳入构建依赖身份；默认不猜测相机位置。
主线程的 service 必须为此模式设置 fade=1。

detail、macro、global colour/AO/wetness 继续走原 fragment 后处理；
源 albedo/normal/ORM、源高度 mip3、覆盖权重/映射因此仍是运行时依赖。
TES 独立使用常驻源 mip2 高度 atlas，不把 mip2 替代 height-blend 的 mip3。
合成页无效时由 `vt.valid` 决定中性回退；45 m 内直接分支仍需要有效覆盖页，
loader 必须保持当前独立完整驻留覆盖数据。

这项实现只把远景基础混合转移到 VT；有 height blend 的近中景继续支付源数组混合成本。
它不满足原计划“所有视距都只读合成基础属性”的性能目标。保留此权衡供主线程审阅，
本次不擅自修改真实场景的 height blend strength，也不删除其仍在消费的源数组。

## 可重放测量

入口：

```powershell
py -3 tool/validation/terrain/validate_composited_bake_gpu.py
```

要求 Windows Vulkan 设备、MinGW `g++` 和 `glslangValidator`。
可用 `--compiler`、`--validator` 显式指定工具，`--output` 指定报告目录。
默认 JSON 报告在 gitignored 的 `artifacts/terrain/composited-gpu/report.json`；
本记录保留结论，脚本和 GPU harness 源码进入版本控制。

2026-11-19 实测 NVIDIA GeForce RTX 3080：

- 真实生产 compute dispatch/readback 八组：非恒等覆盖页映射、repeat、
  local→global→slice、源高度 mip3、1×1 尾部、缺映射诊断、显式源 LOD、面积过滤。
  每组 18,496 texel，错误数均为 0；缺映射组得到预期 status=4，
  其他组 status=0。
- 距离对照真实 GPU dispatch/readback：18,496 texel，测试距离
  `0,7.999,8,8.001,26.5,44.999,45,45.001,100 m`，strength 为 0/3，
  扫描不对称双层权重、两种源法线和粗糙度。共享 GLSL 求值结果与独立 C++
  旧 smoothstep/score/clamp/归一化公式比较：albedo 最大差 1 byte，
  normal 最大差 0 byte，ORM 最大差 0 byte，距离分流错误 0，
  超过 1-byte 阈值的 texel 为 0。输出 alpha 独立记录直接/VT 路由以检查 45 m 边界。
- 根据真实 `M_terrain.json` 声明参数和 sampler，装配真实
  `M_terrain.vertex.glsl`、`M_terrain.surface.glsl` 与引擎
  `base.vert/tesc/tese/frag` 模板；四阶段 SPIR-V 编译与跨阶段链接均通过。

距离测量覆盖实际 GPU 的共享高度公式、分流函数与材质属性累积，
不是完整场景截图差分；其中页侧使用同一冻结公式，不覆盖整个 Cook/Store/Runtime
发布链。真实场景跨越 45 m 的像素连续性、几何 LOD 法线差异与性能仍由集成验收负责。
