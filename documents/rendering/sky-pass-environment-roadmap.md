# Sky Pass 与环境资源改造路线

## 目标

本文档定义基于独立 `Sky Pass` 的天空系统实现方向，并记录从当前 HDRI 环境天空演进到程序化天空、动态 IBL 与分帧更新的学习路线。

当前工程已经具备独立 `Sky Pass` 与 HDRI 环境 IBL 基础。当前阶段的直接目标是：

- 基于已有视线方向重建，把天空背景从运行时 cubemap 采样演进为第一版程序化天空
- 增加可学习、可调试的天空参数，如天顶色、地平线色、太阳方向、太阳盘和 glow
- 先让程序化天空影响屏幕背景，再逐步同步到 SH 漫反射 IBL、环境 cubemap 与 prefilter 链路
- 为后续 time-of-day、动态环境资源与分帧更新准备清晰的状态边界

这份文档不把天空理解为一个临时“天空盒功能”，而是把它定义为后续所有天空相关技术共享的渲染基础设施。

## 为什么走这条路线

当前工程已经具备：

- 启动阶段生成 BRDF LUT
- 场景加载阶段把经纬 HDR 转成 cubemap
- 场景级持有运行时 `environmentCube`
- 基于 `environmentCube` 生成 `prefilteredEnvironmentCube`
- 基于 HDRI 生成 9 项 `environmentSH`
- `Sky Pass` 在 `geometry` 之后通过 fullscreen triangle 绘制背景
- 全局 UBO 已包含 `invView / invProjection / invViewProjection / cameraPosition`
- render graph 已支持每个输出 attachment 的 `loadOp/storeOp`
- 基于 fullscreen triangle 的全屏 pass 基础能力

当前真正缺失的已经不是“如何画天空”，而是：

- 程序化天空参数的场景数据模型
- 天空太阳方向与 directional light 的明确同步方式
- 程序化天空到 `SH`、cubemap、prefilter 的派生更新链路
- 参数变化时的 dirty 标记、低频更新与分帧更新策略

由于后续明确要做大气和体积云，天空应该被视为“基于屏幕像素视线方向的渲染过程”，而不是“绑定某个球体或立方体模型的功能”。

## 设计原则

- 使用独立 `Sky Pass`，不走天空模型路径
- 使用 fullscreen triangle，不引入球或立方体模型依赖
- 场景共享的天空与环境资源放入 `GlobalSet`
- 前序 pass 输出的局部输入资源继续放入 `PassSet`
- 把环境 cubemap 视为场景级运行时资源
- 阶段 1 的天空派生资源走 GPU-first：CPU 只负责参数、dirty 标记与调度，不负责采样天空、积分 SH 或生成 IBL
- 继续使用 render graph 已有的 attachment `loadOp/storeOp` 控制
- 程序化天空第一版继续沿用“`geometry` 之后执行 `Sky Pass`，基于深度只补背景”的方案

## 直接 GPU 接入的数据流

从第一版 procedural sky 开始，天空参数和派生环境数据就直接走 GPU 资源，不先绕一条 CPU 计算链路。

推荐最小数据流：

```text
scene/config/defaults
    -> CPU SkyParameters staging/update
    -> SkyParametersGPU buffer in GlobalSet
    -> sky.frag reads SkyParametersGPU for screen background
    -> proceduralSkyToSH.comp reads SkyParametersGPU
    -> EnvironmentSHGPU buffer
    -> pbr.frag reads EnvironmentSHGPU for diffuse IBL
    -> proceduralSkyToCubemap.comp reads SkyParametersGPU
    -> environmentCube / prefilteredEnvironmentCube
    -> pbr.frag reads specular IBL
```

这里的 CPU 只做控制面：

- 解析 JSON 或默认参数
- 根据 directional light 计算太阳方向
- 上传 `SkyParametersGPU`
- 设置 `skyParametersDirty / environmentShDirty / environmentCubeDirty / prefilterDirty`
- 调度 compute pass 或 GPU workload

这里的 GPU 做数据面：

- 根据 `SkyParametersGPU` 评估 procedural sky
- 生成屏幕背景
- 归约 SH9
- 生成 procedural cubemap
- 生成 prefiltered specular IBL
- 在 PBR shader 中直接消费 GPU 侧环境资源

明确不要做的事：

- 不用 GPU 算完 SH 后 readback 到 CPU 再填 `UBOGlobal.environmentSH`
- 不在 CPU 侧复制一份 procedural sky evaluator 用来积分 SH
- 不把 procedural sky 的长期参数直接塞进已经很拥挤的 `UBOGlobal`

## 最终目标形态

当前渲染流程已经支持如下结构，后续 procedural sky 应继续沿用：

1. `shadow` pass 渲染阴影资源
2. `geometry` pass 渲染场景颜色与深度
3. `sky` pass 在 `geometry` 之后执行，只对未被几何覆盖的背景像素绘制天空
4. 后处理链消费最终 HDR 场景颜色

当前 `Sky Pass` 已使用运行时 cubemap 采样。后续程序化天空不应推翻 pass 结构，而应优先替换或扩展 sky shader 与天空参数来源，使同一个 pass 逐步承接：

- 解析式大气
- 大气加太阳盘
- 体积云
- 大气、云层与场景的统一合成

## 教程完成后的预期效果

完成这条教程路线后，目标不是立即做出 Unreal Engine Sky Atmosphere 那种生产级大气系统，而是得到一个可调、可解释、能接入 PBR/IBL、后续能扩展到大气和云的程序化天空基础设施。

最终应达到的效果：

- 天空背景不再依赖 HDRI cubemap，而是由 `sky.frag` 根据世界空间视线方向实时生成
- 天空具备可调的天顶色、地平线色、下半球颜色、太阳盘、太阳 glow 与太阳强度
- 太阳方向和主 `directionalLight` 绑定，天空太阳、直接光方向、阴影方向能对得上
- 修改天空参数后，非金属材质的环境漫反射会跟着天空颜色变化，也就是 procedural sky -> SH diffuse IBL 跑通
- 金属和低 roughness 材质的反射能看到程序化天空趋势，也就是 procedural sky -> cubemap -> prefilter -> specular IBL 跑通
- 后续做 time-of-day 时，屏幕天空可以每帧变化，但 SH、cubemap、prefilter 这些 GPU 环境资源可以低频或分帧更新，不会每帧全量重建

阶段效果对齐如下：

- 阶段 1 完成后：得到只影响屏幕背景的 procedural sky，PBR 环境光仍来自 HDRI
- 阶段 2-3 完成后：天空从 shader 小实验变成数据驱动系统，太阳方向与 directional light 对齐
- 阶段 4 完成后：compute shader 根据 procedural sky 生成 SH 漫反射 IBL，物体暗部和非金属表面的环境光会跟着天空变化
- 阶段 5 完成后：procedural sky 影响镜面 IBL，金属或低 roughness 材质能反射出天空颜色和太阳方向的大趋势
- 阶段 6 完成后：天空参数可以连续变化，IBL 资源通过 dirty flag 和分帧更新逐步收敛

一句话目标：教程完成后，应拥有一个从屏幕背景到 diffuse IBL、specular IBL、动态更新都能打通的 GPU-first procedural sky 原型。第一版不追求物理极致，但必须保证链路完整、方向正确、后续能自然升级到大气、云和 time-of-day。

## 长期大阶段：直到完整天气系统

这条路线可以拆成 8 个大阶段。当前教程主要覆盖大阶段 1，也会为大阶段 2 的动态更新打基础。

### 大阶段 1：Sky Foundation

目标：

- 完成 procedural sky 基础闭环
- 让背景、太阳方向、SH diffuse IBL、cubemap/prefilter specular IBL 都能来自同一套 sky 参数
- 建立 GPU-first 的环境派生路径

约束：

- CPU 只负责 `SkyParameters`、light 方向同步、dirty 标记、更新预算和 dispatch 调度
- CPU 不负责采样 procedural sky、积分 SH、生成 cubemap 或 prefilter
- 现有 CPU `EnvironmentSHGenerator` 只作为 HDRI 基准和 SH 数学参考
- procedural sky -> SH / cubemap / prefilter 都应逐步落到 compute shader 或 GPU pass

完成后效果：

- 晴天、黄昏、冷暖天空都能调
- PBR diffuse IBL 与 specular IBL 能和天空趋势一致
- 后续 time-of-day 可以复用这套参数和 GPU 派生链路

### 大阶段 2：Dynamic Sky 与 Time-of-Day

目标：

- 让天空随时间连续变化
- 建立太阳/月亮方向、时间曲线、曝光补偿和动态 IBL 更新策略

完成后效果：

- 一天从清晨到夜晚能连续变化
- 屏幕天空每帧变化，SH/cubemap/prefilter 低频或分帧收敛

### 大阶段 3：Physical Atmosphere

目标：

- 从美术渐变天空升级到基于大气散射的天空
- 学习 Rayleigh、Mie、transmittance LUT、sky-view LUT 和 aerial perspective

完成后效果：

- 太阳低角度时天空和地平线颜色更自然
- 远处物体具备基础空气透视

### 大阶段 4：Fog 与 Aerial Perspective

目标：

- 让天气开始影响场景空间，而不只是背景
- 接入 height fog、distance fog、directional inscattering 和 scene depth 合成

完成后效果：

- 清晨雾、远景发灰、逆光雾感能被表现出来

### 大阶段 5：Cloud System

目标：

- 先做 2D procedural cloud layer，再升级 volumetric cloud
- 接入云量、云速、风向、coverage、density、height、云阴影和云对太阳遮挡

完成后效果：

- 晴天、多云、阴天能明显区分
- 云会移动，并影响天空、光照和 IBL 氛围

### 大阶段 6：Weather State System

目标：

- 从手调参数升级为天气状态系统
- 定义 `WeatherPreset`、`WeatherState`、状态过渡曲线、风速、湿度、云量、降水强度和能见度

完成后效果：

- 可以从晴天渐变到多云、阴天、雾天、雨天或暴风雨
- 天空、大气、云、雾、光照由统一天气状态驱动

### 大阶段 7：Precipitation 与地表反馈

目标：

- 接入雨、雪、风向、湿润材质、roughness 变化、puddle/wetness mask、涟漪和可选镜头雨滴

完成后效果：

- 下雨不只是屏幕雨线，地面和材质也会变湿，反射和粗糙度会随天气变化

### 大阶段 8：完整天气渲染系统

目标：

- 统一 weather config、runtime weather controller、time-of-day controller、debug view、性能档位和热更新调参入口

完成后效果：

- 可以配置晴天 -> 多云 -> 阴天 -> 小雨 -> 暴雨 -> 雨后放晴的天气循环
- 天空、云、雾、光照、IBL、降水和材质湿润能一起合理变化

## 当前状态总结

### 已具备

- 启动时 BRDF LUT 生成
- 场景 JSON 中的 `environment` 节点解析
- 经纬 HDR 到 cubemap 的 compute 转换
- `environmentCube` 作为场景级运行时资源由 `SceneLoader` 持有
- `prefilteredEnvironmentCube` 生成与全局绑定路径
- `environmentSH[9]` 生成与 PBR diffuse IBL 消费路径
- 独立 `Sky Pass`
- 全局 cubemap descriptor 绑定路径
- 全局 shader 输入中的 `invView` 与 `invProjection`
- render graph 对每个输出 attachment 的 `loadOp/storeOp` 语义支持
- render pass 中的 fullscreen triangle 绘制能力

### 下一步缺口

- 程序化天空参数结构与配置来源
- `sky.frag` 从 cubemap 采样切换到第一版 procedural sky 计算
- 程序化太阳方向与 directional light 的绑定策略
- 程序化天空到 `environmentSH` 的低频同步
- 程序化天空到 cubemap / prefilter 的后续动态更新策略
- 分帧更新的状态结构、更新预算和同步规则

### 当前 cubemap 生成链的现状

当前 `EnvironmentCubemapGenerator` 已经做对了以下关键事项：

- 创建了 `arrayLayers = 6` 且带 `eCubeCompatible` 标记的 image
- 使用 compute shader 按 6 个 face 写入环境图
- 生成结果已经具备逐面导出为 `px/nx/py/ny/pz/nz.exr` 的调试基础
- 生成完成后返回带 `Cube` view 与 sampler 的 `Texture`
- 非调试路径会把 cubemap 转到 `ShaderReadOnlyOptimal`
- 逐面 EXR 导出只在 debug 构建中启用

因此，当前问题不是“没有生成 cubemap”，也不是“把运行时资源做成了六张独立 2D 图”，更不是“缺少天空 pass”。

当前真正的缺口在于：这条链路仍以 HDRI 文件为环境源。下一阶段应把程序化天空也变成可派生环境数据的来源。

因此，后续这条链路的核心目标是：

- 保留现有 HDRI -> cubemap -> prefilter 链路作为基准
- 先实现屏幕空间 procedural sky，只改变背景
- 再实现 procedural sky -> SH，让低频漫反射环境光跟随天空参数
- 最后实现 procedural sky -> cubemap -> prefilter，并引入分帧更新

## 高层架构

### 1. 运行时环境资源

当前的环境生成器已经承担运行时资源生产职责，不再只是调试导出工具。

这里的行为约束应继续保持如下：

- 运行时 cubemap 生成始终执行
- EXR 逐面导出只在 debug 模式下执行
- 生成完成后的 cubemap 资源不能在生成器内部销毁
- 生成器返回的资源由 `SceneLoader` 持有和管理生命周期

在当前项目里，继续推荐复用现有 `Texture` 作为 GPU 纹理承载，而不是额外新增一个 `RuntimeCubemap` 类型。

原因：

- `Texture` 已经持有 GPU 纹理的核心字段：`image / memory / imageView / sampler / mipLevels / format`
- 当前问题主要是 `Texture` 的构造路径偏向“从文件加载 2D 贴图”，而不是它不适合当 GPU 纹理壳子
- 对当前阶段来说，扩展现有 `Texture` 比引入新概念更符合仓库的渐进式实现风格

如果后续资源字段继续膨胀，可以再引入场景环境资源结构：

```cpp
struct EnvironmentRuntimeResources
{
    std::shared_ptr<Texture> environmentCube;
    bool valid = false;
};
```

这里的 `environmentCube` 语义上仍然是“一个真正的 cubemap 纹理对象”，不是六张独立 `Texture`。

也就是说：

- 底层仍应是一个 `eCubeCompatible` 的 6-layer image
- 采样时使用 `vk::ImageViewType::eCube`
- 如有 compute 继续写入需求，可额外保留 `2DArray` 类型的 storage view

不建议把运行时 cubemap 做成六张独立 2D 图，原因如下：

- `Sky Pass` 需要直接使用 `samplerCube`
- 如果拆成六张 2D 图，就需要在 shader 中手写 face 选择与方向到 UV 的映射
- 后续 `prefilterCubeMap`、mip、LOD 与 IBL 链路都会变得更别扭

当前阶段不要为了结构好看而立即重构 `SceneLoader` 字段。更推荐在 procedural sky、动态 cubemap、分帧更新开始出现真实状态需求后，再引入 `EnvironmentRuntimeResources` 或 `SkyRuntimeState`。

职责划分：

- `EnvironmentCubemapGenerator` 负责生成运行时 cubemap
- `EnvironmentCubemapGenerator` 只在 debug 模式下执行逐面 EXR 导出
- `SceneLoader` 负责持有场景级环境资源，并管理其生命周期
- 运行时资源生成与调试导出必须解耦，不能因为导出调试图而改变最终运行时资源的生存期

### 2. 独立 Sky Pass

天空应通过独立的 fullscreen pass 渲染。

输入：

- 全局相机数据
- 运行时环境 cubemap 或程序化天空参数

输出：

- HDR 场景颜色缓冲中的天空背景

`Sky Pass` 不依赖几何模型，也不要求球体或立方体网格。

### 3. 用于视线重建的全局相机输入

当前全局 UBO 已经具备天空方向重建所需的核心矩阵。后续应保持它作为天空、大气和体积效果的标准输入。

当前推荐结构方向如下：

```cpp
struct alignas(16) UBOGlobal
{
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
    Eigen::Matrix4f invView;
    Eigen::Matrix4f invProjection;
    Eigen::Matrix4f viewProjection;
    Eigen::Matrix4f invViewProjection;
    Eigen::Matrix4f lightViewProj;
    alignas(16) Eigen::Vector3f cameraPosition;
    alignas(16) int debugViewMode = 0;
    alignas(16) float environmentIntensity = 1.0f;
    std::array<Eigen::Vector4f, 9> environmentSH{};
};
```

这样可以统一实现一套 `GetViewRayWS()`，供以下系统复用：

- cubemap 天空
- 大气
- 体积云
- aerial perspective
- 后续其他依赖屏幕空间视线的效果

### 4. Descriptor Set 职责边界

当前 set 划分建议保持不变：

- `set 0`: 场景级全局共享资源
- `set 1`: 材质资源
- `set 2`: 物体资源
- `set 3`: pass 局部资源

环境 cubemap 以及未来天空相关资源应放在 `GlobalSet`，不应放在 `PassSet`。

原因：

- 它们属于场景共享资源
- 它们不是前序 render graph pass 的输出
- 它们应能被多个 pass 复用，而不仅限于某个后处理式 pass

### 5. Render Graph 的 attachment 语义

独立 `Sky Pass` 需要明确的 attachment 行为控制。当前 render graph 已经具备 per-output `loadOp/storeOp` 配置，后续应继续使用这条路径，而不是为 sky 写特殊逻辑。

render graph 应支持并保持对每个 pass 输出配置：

- `loadOp`
- `storeOp`

原因：

- `geometry` 需要先写颜色和深度
- `sky` 需要在 `geometry` 之后加载颜色与深度
- `sky` 不能再清空 `geometry` 已经写出的结果

本路线的推荐配置语义：

- `geometry.offscreenColor`: `clear/store`
- `geometry.sceneDepth`: `clear/store`
- `sky.offscreenColor`: `load/store`
- `sky.sceneDepth`: `load/store`

这项能力已经是当前 `Sky Pass` 能独立存在的关键前提，后续 procedural sky 不应绕过它。

## Sky Pass 的执行顺序方案

### 主推荐方案：`geometry` 后执行

当前与后续第一版 procedural sky 都推荐采用：

1. 先执行 `geometry`
2. 再执行 `sky`
3. `sky` 通过深度测试，只在背景像素上绘制天空

选择这条路线的原因：

- 对后续大气、体积云更友好
- 天空只处理真正可见的背景像素，未来更省
- 更容易与基于场景深度的云层、大气合成衔接
- 逻辑上更像“背景补全”而不是“先打底色”

### 深度策略

在当前标准深度约定下：

- 近处接近 `0`
- 远处接近 `1`
- 深度清值一般是 `1.0`

因此 `Sky Pass` 推荐配置为：

- 开启深度测试
- 关闭深度写入
- 片元深度固定到远平面
- 深度比较优先使用 `LessOrEqual`

这样：

- 被几何体覆盖的像素通常深度小于 `1.0`，天空不会覆盖它们
- 仍保持清屏深度的背景像素会通过测试，天空可以正常绘制

### 备选方案：`sky` 先于 `geometry`

如果未来某个阶段只需要非常便宜的 cubemap 背景，也可以使用：

1. `sky`
2. `geometry`

此时配置语义应为：

- `sky.offscreenColor`: `clear/store`
- `geometry.offscreenColor`: `load/store`
- `geometry.sceneDepth`: `clear/store`

该方案实现简单，但对后续依赖深度的天空合成扩展性稍弱，因此不作为第一推荐。

## Shader 方向

### Sky Vertex Shader

- 只绘制 fullscreen triangle
- 输出 `uv`

### Sky Fragment Shader

当前基准版已经根据世界空间视线方向采样运行时环境 cubemap。

概念流程：

```glsl
vec2 ndc = uv * 2.0 - 1.0;
vec4 clipPos = vec4(ndc, 1.0, 1.0);
vec4 viewPos = uboVP.invProjection * clipPos;
vec3 viewDir = normalize(viewPos.xyz / viewPos.w);
vec3 worldDir = normalize((uboVP.invView * vec4(viewDir, 0.0)).xyz);
vec3 skyColor = texture(environmentCube, worldDir).rgb;
```

下一版 procedural sky 应复用同一套 `GetViewRayWS()`，只把 `texture(environmentCube, worldDir)` 替换为程序化天空颜色计算。第一版程序化模型优先包含：

- 天顶到地平线渐变
- 太阳盘
- 简单太阳 glow
- 可调强度
- 与 directional light 同步的太阳方向

如果采用 `geometry` 后执行方案，则还需要让 sky 的输出深度落在远平面，并结合深度测试只绘制背景。

天空 shader 的命名应体现 pass 职责，而不是当前实现细节。推荐使用 `sky`，不使用 `skybox`。

## Render Graph 规划

主推荐 pass 顺序：

1. `shadow`
2. `geometry`
3. `sky`
4. `postProcess`
5. `bloomPrefilter`
6. `bloomDownsample`
7. `bloomUpsample`
8. `toneMapping`

`Sky Pass` 已经是 `renderGraphConfig.json` 中的正式 pass。后续 procedural sky 应继续沿用这个 pass，而不是注入 `geometry` 的临时特殊逻辑。

## 实施阶段

### 阶段 0：当前基础复盘

目标：

- 建立 HDRI sky / IBL 的基准画面与基准理解
- 确认 `sky` pass 当前能从 `environmentCube` 绘制 HDRI 背景
- 确认 `geometry -> sky` 的 `load/store` 行为正确
- 确认 PBR 已经能消费 `environmentSH` 与 `prefilteredEnvironmentCube`
- 确认外部资源目录中存在 `materials/pass/MI_sky.json`

需要学习的资料：

- `source/main.cpp`：启动顺序
- `config/renderGraphConfig.json`：pass 顺序、`loadOp/storeOp`
- `shader/glsl/pass/sky.vert` 与 `shader/glsl/pass/sky.frag`：fullscreen sky 绘制
- `source/sceneLoader.cpp`：environment、pass material、IBL 资源加载
- `source/renderGraph.cpp`：attachment 和 framebuffer 创建
- [Vulkan Render Pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)：只读 attachment 与 load/store
- [Vulkan Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)：只读 image layout transition 基础

大致修改步骤：

- 不做功能改动，只做阅读、运行和记录
- 构建并运行当前场景，记录一张基准画面
- 检查 debug 输出或 RenderDoc/Nsight 标记中是否能看到 `sky` pass
- 确认 `environmentCube / environmentSH / prefilteredEnvironmentCube / brdfLut` 的创建和绑定路径
- 记录当前 PBR IBL 仍来自 HDRI，作为后续 procedural sky 第一版的对照基准

验收结果：

- 程序能从当前 `initScene` 启动，不因为 `MI_sky.json` 或 environment 资源缺失而失败
- 背景天空来自 `environmentCube`，相机旋转时方向稳定
- 几何体不被天空覆盖
- PBR 材质能看到 diffuse IBL 与 specular IBL 的贡献
- 你能说清楚 `shadow -> geometry -> sky -> postProcess -> bloom -> toneMapping` 的基本流向

### 阶段 1：第一版 Procedural Sky 背景

目标：

- 在 `sky.frag` 中保留 `GetViewRayWS()`
- 暂时不改 IBL，只把 sky 背景从 `texture(environmentCube, worldDir)` 改为程序化颜色
- 实现天顶/地平线渐变、太阳盘、太阳 glow
- 保持输出仍进入 HDR、post-process、bloom 与 tone mapping 链路

需要学习的资料：

- `shader/glsl/pass/sky.vert`：fullscreen triangle 顶点生成
- `shader/glsl/pass/sky.frag`：`GetViewRayWS()` 与 cubemap sky 采样
- `shader/glsl/common/commonUbo.glsl`：`invProjection / invView`
- `documents/rendering/tone-mapping-tutorial.html`：HDR、曝光、tone mapping、bloom 职责
- [Preetham, Shirley, Smits - A Practical Analytic Model for Daylight](https://courses.cs.duke.edu/cps124/spring08/assign/07_papers/p91-preetham.pdf)：只读天空颜色与太阳高度的直觉
- [Hošek-Wilkie Sky Dome Appearance Project](https://cgg.mff.cuni.cz/projects/SkylightModelling/)：只看模型目标，不实现完整模型

大致修改步骤：

- 在 `shader/glsl/pass/sky.frag` 中新增 `EvaluateProceduralSky(worldDir)`
- 先使用 shader 内部常量控制 `zenithColor / horizonColor / sunDirection / sunIntensity`
- 用 `worldDir.y` 做天顶到地平线的渐变
- 用 `dot(worldDir, sunDirection)` 做太阳盘和 glow
- 保持 render graph、descriptor set、scene loader 暂不变化
- 明确记录这一阶段的短期不一致：背景是 procedural sky，但 PBR IBL 仍来自 HDRI

验收结果：

- 天空颜色随相机旋转保持世界空间稳定，不粘屏幕
- 地平线、天顶、太阳盘都清晰可见
- 几何体不被天空覆盖
- bloom/tone mapping 后太阳不过度失控
- 修改 shader 常量后能明显改变天空颜色或太阳强度
- PBR IBL 仍来自 HDRI，并在验证记录中明确标出

### 阶段 2：SkyParameters 与配置入口

目标：

- 给程序化天空提供最小参数集合
- 建立直接供 shader 和 compute 读取的 `SkyParametersGPU` buffer
- 优先使用 `vec4` 风格字段，降低 std140 / std430 对齐学习成本
- 参数先保持简单，避免一开始实现完整物理大气模型
- 让旧场景不配置 sky 参数也能使用稳定默认值

需要学习的资料：

- `source/baseStructs.h`：参考现有 GPU 数据结构风格，但不要把 sky 参数继续塞进 `UBOGlobal`
- `source/renderSystem.cpp`：参考 per-frame buffer 更新和 descriptor 使用路径
- `shader/glsl/common/commonUbo.glsl`：GLSL 侧 UBO 对齐与字段顺序
- `source/sceneLoader.cpp`：scene/config JSON 读取方式
- Vulkan / GLSL `std140 / std430` 对齐规则，重点理解 `vec3 + float` 与 `vec4` 的差异
- descriptor set layout、descriptor update、frames-in-flight uniform/storage buffer 管理

建议参数：

```cpp
struct SkyParametersGPU
{
    Eigen::Vector4f zenithColor;
    Eigen::Vector4f horizonColor;
    Eigen::Vector4f groundColor;
    Eigen::Vector4f sunDirectionIntensity;
    Eigen::Vector4f sunParams;
};
```

推荐字段语义：

- `zenithColor`: 天顶颜色
- `horizonColor`: 地平线颜色
- `groundColor`: 视线朝下时的下半球颜色或雾化底色
- `sunDirectionIntensity.xyz`: 世界空间太阳方向
- `sunDirectionIntensity.w`: 太阳强度
- `sunParams.x`: 太阳盘角半径或锐度
- `sunParams.y`: glow 强度
- `sunParams.zw`: 预留

大致修改步骤：

- 新增 `SkyParametersGPU`，并为它创建独立 GPU buffer
- 在 `GlobalSet` 中新增 sky 参数 buffer binding，让 `sky.frag` 和后续 compute shader 都能读取
- 新增 `shader/glsl/common/skyParameters.glsl` 或等价 include，集中声明 GPU 侧 sky 参数布局
- 在 CPU 侧只负责默认值、JSON 覆盖、directional light 同步和 buffer upload
- 把 `sky.frag` 中的硬编码 sky 常量替换为 `SkyParametersGPU` 读取
- 明确 sky 参数放在 scene `environment` 还是 config 中；第一版优先选择最少改动路径

验收结果：

- 不改 shader，只改 JSON 或默认参数就能改变天空颜色、太阳强度、太阳盘大小
- 没有把 procedural sky 参数追加进 `UBOGlobal`
- RenderDoc 中能看到 `SkyParametersGPU` 作为独立 GlobalSet buffer 被绑定
- `sky.frag` 直接读取 GPU buffer，而不是读取 CPU 计算出的临时常量
- 缺省参数场景能正常运行
- 参数命名能直接解释含义，不依赖临时注释猜测
- RenderDoc 或 shader 反射中能看到 sky 参数绑定符合预期

### 阶段 3：太阳方向绑定 Directional Light

目标：

- 让程序化天空太阳盘方向与主 directional light 保持一致
- 明确“太阳方向”到底表示光照传播方向还是指向太阳的方向
- 保证阴影方向、直接光方向、天空太阳盘方向三者可解释
- 没有 directional light 时仍保留稳定默认太阳方向

需要学习的资料：

- `source/sceneLoader.cpp`：directional light 加载
- `source/sceneObject.*`：light transform / rotation 的表达
- `source/lightManager.cpp`：`LightGPU.directionPad` 的写入语义
- `shader/glsl/common/lighting.glsl`：directional light 在 shader 中如何被使用
- `source/renderSystem.cpp`：shadow pass 如何使用 directional light 构建 light view-projection

大致修改步骤：

- 先选择场景中的第一个 directional light 作为 sun light
- 用明确命名函数转换 light direction 与 sky sun direction
- 不在 shader 中猜测方向语义
- CPU 侧只计算方向语义，并把 sun direction 上传到 `SkyParametersGPU`
- 为后续多 directional light 或显式 sun light id 留出扩展空间，但第一版不实现复杂选择器

验收结果：

- 调整 directional light 旋转后，太阳盘位置同步变化
- 直接光方向、阴影投射方向、太阳盘方向之间关系清楚且可说明
- shader 里不出现临时取反猜测，方向转换集中在命名清楚的 CPU 控制逻辑中，结果上传到 GPU buffer
- 没有 directional light 时，程序化天空使用稳定默认太阳方向
- 修改太阳方向不会破坏现有 shadow pass

### 阶段 4：Procedural Sky -> SH 漫反射环境光

目标：

- 让程序化天空不只是背景，也能影响 PBR 的低频环境光
- 使用 compute shader 生成 9 项 radiance SH，再沿用现有 `EvaluateIrradianceSH()` 消费路径
- 只在天空参数变化时更新 SH，不做每帧无条件重算
- 第一版不急着生成 procedural cubemap

需要学习的资料：

- [Peter-Pike Sloan - Stupid Spherical Harmonics Tricks](https://www.ppsloan.org/publications/StupidSH36.pdf)：重点读 SH 基底、投影、重建直觉
- [Filament: Physically Based Rendering](https://google.github.io/filament/Filament.md.html)：重点读 diffuse IBL、irradiance、exposure
- `source/pipeline/environmentSHGenerator.cpp`：只作为 SH 数学和基底顺序参考，不作为 procedural sky 的目标实现路径
- `shader/glsl/common/lighting.glsl`：`EvaluateIrradianceSH()` 与 band weights
- `shader/glsl/pbr.frag`：diffuse IBL 如何进入 PBR
- compute reduction 基础：workgroup 局部累加、跨 workgroup 二次归约、storage buffer barrier

大致修改步骤：

- 新增 `proceduralSkyToSH.comp`，在 GPU 侧按球面采样 procedural sky 并投影到 SH
- CPU 只上传 `SkyParameters`，设置 SH dirty 标记和调度 compute dispatch
- 第一版可以用两个 pass：每个 workgroup 输出一组 partial SH，再用第二个 compute pass 做最终归约
- SH 结果写入 `EnvironmentSHGPU` storage/uniform buffer，PBR shader 直接读取
- procedural sky 路径不经过 `UBOGlobal.environmentSH`；现有 CPU SH 只作为 HDRI/默认 fallback
- 在 `lighting.glsl` 中把 SH 来源抽象为 GPU buffer 读取，保持 radiance SH + band weights 的数学语义不变
- 先用较低采样数验证趋势，再提高采样数或做分帧归约

验收结果：

- 修改天顶/地平线颜色后，非金属材质的漫反射环境光随之变化
- 金属表面的 diffuse IBL 仍按 metallic 被压低
- `environmentIntensity` 仍能统一影响 sky 与 IBL 强度
- 关闭或不启用 procedural SH 时，仍可回退到 HDRI SH 或默认 SH
- SH 更新由 dirty 标记驱动，不发生在每帧无条件路径中
- procedural SH 生成过程中没有 GPU -> CPU readback stall
- compute 写入和 PBR 读取之间有明确 barrier 或帧延迟策略，validation layer 无 layout/access/lifetime 错误

### 阶段 5：Procedural Sky -> Cubemap -> Prefilter

目标：

- 让程序化天空成为 specular IBL 的环境源
- 复用现有 `prefilteredEnvironmentCube + brdfLut` 消费路径
- 为动态 time-of-day 与分帧更新打基础
- 先做“参数变化时全量更新”，再做分帧

需要学习的资料：

- `source/pipeline/environmentCubemapGenerator.cpp`：HDRI -> cubemap 的 compute 路径
- `source/pipeline/environmentPrefilterGenerator.cpp`：prefilter cubemap 生成
- `shader/glsl/generator/equirectToCubemap.comp`：cubemap face 写入方式
- `shader/glsl/generator/prefilterEnvMap.comp`：roughness mip 预过滤
- `shader/glsl/generator/brfdLut.comp`：BRDF LUT 生成
- [Brian Karis - Real Shading in Unreal Engine 4](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf)：重点读 split-sum IBL
- [Disney - Physically-Based Shading at Disney](https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)：重点读 roughness/metallic 参数背景

大致修改步骤：

- 新增或复用 compute 生成 procedural cubemap 的路径
- cubemap 仍使用真正的 cube-compatible 6-layer image
- 写入时使用 `2DArray` storage view，采样时使用 `Cube` sample view
- 生成完成后复用现有 `EnvironmentPrefilterGenerator`
- 保留 debug face dump 或等价方向检查手段
- `Sky Pass` 屏幕背景仍然可以每帧直接算，不必等待 cubemap 更新完成

验收结果：

- 金属或低 roughness 材质的反射能看到程序化天空趋势
- cubemap 朝向与屏幕 sky 背景一致
- prefilter mip 随 roughness 变化合理
- 参数变化后 cubemap 和 prefilter 可以重建并被 PBR 使用
- 屏幕 sky 背景不需要等待 IBL 重建完成才能显示

### 阶段 6：分帧更新与动态环境状态

目标：

- 把昂贵环境更新拆成低频任务
- 支持 cubemap face、prefilter mip、SH 的独立 dirty 与更新进度
- 避免 time-of-day 一开启就每帧全量重建所有 IBL 资源
- 最终让背景、SH、specular IBL 收敛到同一套 sky 参数

需要学习的资料：

- [Vulkan Tutorial: Frames in Flight](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/03_Drawing/03_Frames_in_flight.html)：多帧资源所有权和 fence 等待点
- [Vulkan Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)：image barrier、queue visibility、shader read/write
- `source/renderSystem.cpp`：当前 command buffer 录制、提交、present 流程
- `source/pipeline/environmentCubemapGenerator.cpp` 与 `source/pipeline/environmentPrefilterGenerator.cpp`：后续需要拆分的 GPU workload

建议状态结构：

```cpp
struct SkyRuntimeState
{
    bool skyParametersDirty = true;
    bool environmentShDirty = true;
    bool environmentCubeDirty = true;
    bool prefilterDirty = true;
    uint32_t cubemapFaceUpdateIndex = 0;
    uint32_t prefilterMipUpdateIndex = 0;
};
```

更新频率建议：

- 屏幕空间 sky 背景：每帧直接计算
- `environmentSH`: 参数变化时或低频更新
- procedural cubemap: 参数变化后按 face 或全量更新
- prefilter: cubemap 更新后按 mip/face 分帧更新

大致修改步骤：

- 给 sky 参数、SH、cubemap、prefilter 分别设置 dirty 和 update cursor
- 每帧只消费有限更新预算，例如一个 cubemap face 或一个 prefilter mip
- 明确资源被渲染读取时是否允许同时更新，必要时使用双缓冲或延迟替换
- 增加调试输出或 UI 状态，能看到当前更新到哪个 face/mip
- 先让全量更新稳定，再逐步切成分帧更新

验收结果：

- time-of-day 或 sky 参数连续变化时，帧时间不会因为每帧全量 IBL 更新而大幅抖动
- 更新进度可观察，能说明当前更新到哪个 face/mip
- 旧环境资源在新资源未完成前仍可稳定用于渲染
- 没有 validation layer 的 layout / access / lifetime 错误
- 分帧更新完成后，背景、SH、specular IBL 最终收敛到同一套 sky 参数

## 学习资料路线

这组资料按学习阶段阅读。已经在当前代码中完成的阶段，也可以按同样顺序做源码复盘；不要一次性读完所有大气论文。

### 阶段 A：现有 Sky Pass 与 Render Graph 复盘

目标：

- 理解 `geometry -> sky -> postProcess` 的执行顺序
- 理解 `sky` 为什么要 `load` 颜色和深度，而不是重新 `clear`
- 理解 pass 输出资源在 attachment 与 sampled image 之间切换时需要哪些 layout 与同步

建议阅读：

- [Vulkan Render Pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
  - 只读 attachment、`loadOp/storeOp`、`initialLayout/finalLayout` 相关内容
- [Vulkan Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
  - 只读 pipeline stage、access mask、image layout transition 的基本关系

对应代码：

- `config/renderGraphConfig.json`
- `source/renderGraph.cpp`
- `source/renderSystem.cpp`

### 阶段 B：屏幕空间天空与视线重建

目标：

- 理解 fullscreen triangle 为什么可以替代天空盒 mesh
- 理解如何从屏幕 `uv` 通过 `invProjection / invView` 重建世界空间视线方向
- 理解相机旋转时天空方向应该如何变化

建议阅读：

- 此阶段以源码复盘为主，重点对照 `sky.vert`、`sky.frag` 和 `UBOGlobal`
- 外部资料可以暂缓，避免在第一轮被 Vulkan 帧同步内容分散注意力

对应代码：

- `shader/glsl/pass/sky.vert`
- `shader/glsl/pass/sky.frag`
- `shader/glsl/common/commonUbo.glsl`
- `source/baseStructs.h`
- `source/renderSystem.cpp`

### 阶段 C：第一版 Procedural Sky

目标：

- 先写出可控、可调、画面反馈明确的程序化天空
- 实现天顶/地平线渐变、太阳盘、太阳 glow
- 暂时只影响天空背景，不要求同步 IBL

建议阅读：

- [Preetham, Shirley, Smits - A Practical Analytic Model for Daylight](https://courses.cs.duke.edu/cps124/spring08/assign/07_papers/p91-preetham.pdf)
  - 第一轮只读问题定义、turbidity、太阳高度与天空颜色的关系
- [Hošek-Wilkie Sky Dome Appearance Project](https://cgg.mff.cuni.cz/projects/SkylightModelling/)
  - 先读页面上的模型目标和对 Preetham 的改进说明，不要第一版直接实现完整模型

对应代码：

- `shader/glsl/pass/sky.frag`
- 实施阶段 2 会新增 `SkyParameters` 与对应 UBO、Push Constant 或全局参数路径

### 阶段 D：Procedural Sky 参与漫反射环境光

目标：

- 让程序化天空不只是背景，也能影响 PBR 的低频环境光
- 优先做 GPU procedural sky -> `SH9`，而不是一开始生成完整 cubemap/prefilter 链
- 理解 radiance SH 与 diffuse irradiance 的区别
- 理解 compute reduction、storage buffer barrier，以及为什么不能用 GPU -> CPU readback 作为常规路径

建议阅读：

- [Peter-Pike Sloan - Stupid Spherical Harmonics Tricks](https://www.ppsloan.org/publications/StupidSH36.pdf)
  - 重点读 SH 基底、投影、重建、低频环境光直觉
- [Filament: Physically Based Rendering](https://google.github.io/filament/Filament.md.html)
  - 重点读 image based lighting、diffuse irradiance、exposure/tone mapping 相关章节

对应代码：

- `source/pipeline/environmentSHGenerator.cpp`：参考现有 SH 基底和投影公式，不作为 procedural sky 目标路径
- `shader/glsl/common/lighting.glsl`
- `shader/glsl/pbr.frag`
- `source/renderSystem.cpp`
- 后续新增 `shader/glsl/generator/proceduralSkyToSH.comp`

### 阶段 E：镜面 IBL 与 Split-Sum

目标：

- 理解 `prefilteredEnvironmentCube + brdfLut` 为什么能近似镜面 IBL
- 理解 roughness mip、BRDF LUT、F0 在运行时组合的位置
- 为后续 procedural sky -> cubemap -> prefilter 做准备

建议阅读：

- [Brian Karis - Real Shading in Unreal Engine 4](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf)
  - 重点读 image-based lighting 与 split-sum approximation
- [Disney - Physically-Based Shading at Disney](https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)
  - 重点读材质参数设计与 roughness/metallic 工作流背景

对应代码：

- `source/pipeline/environmentPrefilterGenerator.cpp`
- `shader/glsl/generator/prefilterEnvMap.comp`
- `shader/glsl/generator/brfdLut.comp`
- `shader/glsl/common/lighting.glsl`

### 阶段 F：分帧更新与动态环境

目标：

- 区分每帧更新、低频更新、参数变化时重建
- 后续把 cubemap face、prefilter mip、SH 更新拆成可调度的小任务
- 保证多帧资源和 GPU 同步不会相互踩踏

建议阅读：

- [Vulkan Tutorial: Frames in Flight](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/03_Drawing/03_Frames_in_flight.html)
  - 复读多帧资源所有权与 fence 等待点
- [Vulkan Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
  - 回到 image barrier、queue visibility、shader read/write 关系

对应后续结构：

- `SkyRuntimeState`
- cubemap face update cursor
- prefilter mip update cursor
- dirty flag 与 update budget

### 阶段 G：生产级大气与长期方向

目标：

- 在第一版 procedural sky 稳定后，理解现代实时大气如何组织 LUT、参数和 aerial perspective
- 为后续解析式大气、体积云、time-of-day 做技术储备

建议阅读：

- [Sébastien Hillaire - A Scalable and Production Ready Sky and Atmosphere Rendering Technique](https://diglib.eg.org/items/8a3e5350-18b3-46bd-9274-3add5af88c75)
  - 做完第一版 procedural sky 后再读，重点看低维 LUT、动态大气参数与 aerial perspective
- [sebh/UnrealEngineSkyAtmosphere](https://github.com/sebh/UnrealEngineSkyAtmosphere)
  - 作为 Hillaire 技术的工程参考，先看 README 和 shader 文件组织
- [Bruneton - Precomputed Atmospheric Scattering: A New Implementation](https://ebruneton.github.io/precomputed_atmospheric_scattering/)
  - 适合后续做预计算大气 LUT 时阅读
- [Unreal Engine Sky Atmosphere Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)
  - 主要看成熟引擎暴露哪些艺术参数，不作为第一版实现目标

## 验证清单

### Cubemap 朝向

- 地平线保持水平
- 上下没有翻转
- 左右没有镜像
- 相机 yaw 和 pitch 与环境方向映射正确

### Render Graph 行为

- `geometry` 先正确写入 HDR 颜色和深度
- `sky` 正确加载 `geometry` 输出，而不是清空结果
- `sky` 只在背景像素通过深度测试

### 视觉检查

- 天空在所有几何体背后可见
- bloom 与 tone mapping 链路不会吞掉天空背景
- 当场景没有覆盖整个屏幕时，不会出现黑底

### Procedural Sky 检查

- 天顶、地平线、下半球颜色随相机旋转保持世界空间稳定
- 太阳盘位置与 directional light 方向一致
- 太阳强度进入 HDR 链路后不会让 tone mapping 完全失控
- 第一版 procedural 背景启用时，明确记录 PBR IBL 是否仍来自 HDRI

## 后续扩展

这套设计应在未来继续沿用，用于承接：

- `cube -> SH`
- `cube -> prefilterCubeMap`
- 解析式大气
- 体积云
- 大气与云层合成
- 对场景几何的 aerial perspective

本文路线的核心价值，是让这些系统建立在统一的天空基础设施之上，而不是等后续功能到来时再做一次新的底层改造。

## 第一版 Procedural Sky 的非目标

第一版程序化天空只替换屏幕背景颜色，不需要解决：

- 解析式大气
- 云层渲染
- procedural sky 到 SH 漫反射 IBL 的同步
- procedural sky 到 prefilter 镜面 IBL 的同步
- 昼夜切换与 time-of-day 曲线
- 天气系统
- 分帧环境更新

第一版只需要把“可参数化的程序化天空背景”跑起来，并复用现有 `Sky Pass`、全局 UBO、HDR 后处理链路。后续这些功能应在同一套天空基础设施上继续接入，而不是再次重做 pass 结构。
