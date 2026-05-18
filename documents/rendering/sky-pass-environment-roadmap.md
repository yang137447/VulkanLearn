# Sky Pass 与环境资源改造路线

## 目标

本文档定义基于独立 `Sky Pass` 的天空系统实现方向。

当前阶段的直接目标是：

- 基于视线方向重建，从运行时 cubemap 绘制天空背景
- 把环境 cubemap 从“仅调试导出结果”升级为“场景级运行时资源”
- 为后续大气、体积云与 IBL 链路准备统一的 render graph 与全局资源基础

这份文档不把天空理解为一个临时“天空盒功能”，而是把它定义为后续所有天空相关技术共享的渲染基础设施。

## 为什么走这条路线

当前工程已经具备：

- 启动阶段生成 BRDF LUT
- 场景加载阶段把经纬 HDR 转成 cubemap
- 基于 fullscreen triangle 的全屏 pass 基础能力

当前真正缺失的并不是“如何画一个立方体模型”，而是：

- 环境资源的运行时持有与生命周期
- 独立的天空渲染阶段
- 可复用的视线方向重建输入

由于后续明确要做大气和体积云，天空应该被视为“基于屏幕像素视线方向的渲染过程”，而不是“绑定某个球体或立方体模型的功能”。

## 设计原则

- 使用独立 `Sky Pass`，不走天空模型路径
- 使用 fullscreen triangle，不引入球或立方体模型依赖
- 场景共享的天空与环境资源放入 `GlobalSet`
- 前序 pass 输出的局部输入资源继续放入 `PassSet`
- 把环境 cubemap 视为场景级运行时资源
- 给 render graph 增加 attachment `loadOp/storeOp` 控制
- 第一版主推荐使用“`geometry` 之后执行 `Sky Pass`，基于深度只补背景”的方案

## 最终目标形态

改造完成后，渲染流程应支持如下结构：

1. `shadow` pass 渲染阴影资源
2. `geometry` pass 渲染场景颜色与深度
3. `sky` pass 在 `geometry` 之后执行，只对未被几何覆盖的背景像素绘制天空
4. 后处理链消费最终 HDR 场景颜色

`Sky Pass` 第一版使用运行时 cubemap 采样，但 pass 结构本身应保持稳定，以便后续直接替换为：

- 解析式大气
- 大气加太阳盘
- 体积云
- 大气、云层与场景的统一合成

## 当前状态总结

### 已具备

- 启动时 BRDF LUT 生成
- 场景 JSON 中的 `environment` 节点解析
- 经纬 HDR 到 cubemap 的 compute 转换
- render pass 中的 fullscreen triangle 绘制能力

### 尚缺失

- 环境 cubemap 的运行时所有权
- 独立 `Sky Pass`
- 全局 cubemap descriptor 绑定路径
- 全局 shader 输入中的 `invView` 与 `invProjection`
- render graph 对每个输出 attachment 的 `loadOp/storeOp` 语义支持

### 当前 cubemap 生成链的现状

当前 `EnvironmentCubemapGenerator` 已经做对了以下关键事项：

- 创建了 `arrayLayers = 6` 且带 `eCubeCompatible` 标记的 image
- 使用 compute shader 按 6 个 face 写入环境图
- 生成结果已经具备逐面导出为 `px/nx/py/ny/pz/nz.exr` 的调试基础

因此，当前问题不是“没有生成 cubemap”，也不是“把运行时资源做成了六张独立 2D 图”。

当前真正的缺口在于它仍停留在“调试产物”阶段，而不是“运行时资源”阶段：

- 当前导出路径默认参与主流程，调试输出与运行时资源生成没有彻底解耦
- compute 写完后如果进入导出流程，会把 image 切到 `TransferSrcOptimal`，还没有在最终结果上稳定回到 `ShaderReadOnlyOptimal`
- 当前只创建了用于 compute 写入的 `2DArray` view，还缺少供 `samplerCube` 采样的 `Cube` view
- 导出 EXR 之后直接销毁了 cubemap 相关 GPU 资源，没有把它持久化交给场景系统

因此，这条链路的核心改造目标是：

- 保留现有 cubemap 生成方式
- 把 EXR 导出改为仅在 debug 模式下启用
- 补齐 `cubeView`
- 补齐运行时最终 layout
- 生成完成后不销毁 cubemap 资源
- 把资源所有权明确交给 `SceneLoader`

## 高层架构

### 1. 运行时环境资源

当前的环境生成器不应继续只作为调试导出工具，而应升级为运行时资源生产器。

这里的行为约束应明确如下：

- 运行时 cubemap 生成始终执行
- EXR 逐面导出只在 debug 模式下执行
- 生成完成后的 cubemap 资源不能在生成器内部销毁
- 生成器返回的资源由 `SceneLoader` 持有和管理生命周期

在当前项目里，优先推荐复用现有 `Texture` 作为 GPU 纹理承载，而不是额外新增一个 `RuntimeCubemap` 类型。

原因：

- `Texture` 已经持有 GPU 纹理的核心字段：`image / memory / imageView / sampler / mipLevels / format`
- 当前问题主要是 `Texture` 的构造路径偏向“从文件加载 2D 贴图”，而不是它不适合当 GPU 纹理壳子
- 对当前阶段来说，扩展现有 `Texture` 比引入新概念更符合仓库的渐进式实现风格

推荐的场景环境资源结构：

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

职责划分：

- `EnvironmentCubemapGenerator` 负责生成运行时 cubemap
- `EnvironmentCubemapGenerator` 只在 debug 模式下执行逐面 EXR 导出
- `SceneLoader` 负责持有场景级环境资源，并管理其生命周期
- 运行时资源生成与调试导出必须解耦，不能因为导出调试图而改变最终运行时资源的生存期

### 2. 独立 Sky Pass

天空应通过独立的 fullscreen pass 渲染。

输入：

- 全局相机数据
- 运行时环境 cubemap

输出：

- HDR 场景颜色缓冲中的天空背景

`Sky Pass` 不依赖几何模型，也不要求球体或立方体网格。

### 3. 用于视线重建的全局相机输入

当前全局 UBO 不足以支撑长期天空方向重建需求，需要升级为适合天空、大气和体积效果的标准输入。

推荐结构：

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
    float pad0 = 0.0f;
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

独立 `Sky Pass` 需要更明确的 attachment 行为控制。

render graph 应支持对每个 pass 输出配置：

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

如果没有这项能力，独立 `Sky Pass` 就只能退化成 `geometry` 内的特殊逻辑，无法成为真正的一等公民。

## Sky Pass 的执行顺序方案

### 主推荐方案：`geometry` 后执行

第一版主推荐采用：

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

第一版应根据世界空间视线方向采样运行时环境 cubemap。

概念流程：

```glsl
vec2 ndc = uv * 2.0 - 1.0;
vec4 clipPos = vec4(ndc, 1.0, 1.0);
vec4 viewPos = uboVP.invProjection * clipPos;
vec3 viewDir = normalize(viewPos.xyz / viewPos.w);
vec3 worldDir = normalize((uboVP.invView * vec4(viewDir, 0.0)).xyz);
vec3 skyColor = texture(environmentCube, worldDir).rgb;
```

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

`Sky Pass` 应该是 `renderGraphConfig.json` 中的正式 pass，而不是注入 `geometry` 的临时特殊逻辑。

## 实施阶段

### 阶段 1：环境 cubemap 运行时持有

目标：

- 把 `EnvironmentCubemapGenerator` 改造成运行时资源生产器
- 返回持久化 cubemap 资源
- 保留可选 EXR 调试导出

预期影响区域：

- `source/pipeline/environmentCubemapGenerator.h`
- `source/pipeline/environmentCubemapGenerator.cpp`
- `source/sceneLoader.h`
- `source/sceneLoader.cpp`

### 阶段 2：全局 UBO 升级

目标：

- 增加逆矩阵与 view-projection 数据
- 标准化视线方向重建输入

预期影响区域：

- `source/baseStructs.h`
- `source/renderSystem.cpp`
- `shader/glsl/common/commonUbo.glsl`

### 阶段 3：Render Graph 的 load/store 升级

目标：

- 支持 `geometry` 与 `sky` 正确共享颜色和深度资源
- 为 pass 输出引入显式 `loadOp/storeOp`

预期影响区域：

- `config/renderGraphConfig.json`
- `source/renderGraph.h`
- `source/renderGraph.cpp`

### 阶段 4：Sky Pass 材质与 shader

目标：

- 增加独立天空材质实例
- 增加 fullscreen sky 顶点与片元 shader

预期影响区域：

- `resources/materials/pass/MI_sky.json`
- `shader/glsl/pass/sky.vert`
- `shader/glsl/pass/sky.frag`

### 阶段 5：RenderSystem 中接入独立 Sky Pass

目标：

- 增加独立 `sky` pass 执行分支
- 绑定天空 pipeline 与全局 descriptor
- 绘制 fullscreen triangle

预期影响区域：

- `source/renderSystem.cpp`

### 阶段 6：验证

目标：

- 验证 cubemap 朝向
- 验证 `geometry -> sky` 的深度与 load/store 行为
- 验证天空经过 HDR、后处理和 tone mapping 后的结果

### 第一版完成后的下一优先级：SH 漫反射 IBL

当 `Sky Pass` 第一版稳定后，下一步主推荐不是立刻做 `prefilterCubeMap`，而是先做 `cube -> SH` 与漫反射 IBL 支持。

原因：

- `SH` 直接复用已经稳定的运行时环境 cubemap
- 它只需要解决低频漫反射环境光，不依赖 roughness 维度和 mip 预过滤链
- 相比镜面 IBL，`SH` 更适合作为环境资源从“能显示天空”走向“能参与场景光照”的第一步
- 它可以先验证场景级环境资源如何进一步进入 `geometry` 着色阶段

推荐目标形态：

- 场景加载阶段生成环境 cubemap
- 基于该 cubemap 预计算 9 项 RGB `SH` 系数
- `SceneLoader` 持有 `environmentCube + environmentSH`
- `geometry` pass 在全局 descriptor 中读取 `SH` 系数
- PBR shader 先接入漫反射 IBL，镜面 IBL 继续留给后续 `prefilterCubeMap`

### 阶段 7：`cube -> SH` 预计算

目标：

- 从运行时环境 cubemap 生成 9 项 `SH` 系数
- 把 `SH` 结果作为场景级运行时资源持有
- 让 `SH` 生成链与天空渲染链共用同一个环境 cubemap 源

推荐约束：

- `environmentCube` 继续作为环境源资源的唯一真源
- `SH` 结果应是从 `environmentCube` 派生出的附属运行时资源
- 第一版优先关注 `L0 + L1 + L2` 的 9 项 RGB 系数，不扩展额外变体
- 第一版不要求把 `SH` 放进 render graph pass；场景加载后一次性生成即可

推荐资源结构：

```cpp
struct EnvironmentSH9
{
    std::array<Eigen::Vector3f, 9> coefficients;
    bool valid = false;
};
```

或者把它并入场景环境资源：

```cpp
struct EnvironmentRuntimeResources
{
    std::shared_ptr<Texture> environmentCube;
    std::array<Eigen::Vector3f, 9> environmentSH;
    bool hasEnvironmentCube = false;
    bool hasEnvironmentSH = false;
};
```

职责划分：

- `EnvironmentCubemapGenerator` 继续负责 `HDR -> cubemap`
- 新增 `EnvironmentSHGenerator` 或等价模块，负责 `cube -> SH`
- `SceneLoader` 负责持有 `SH` 系数，并提供只读访问接口

预期影响区域：

- `source/sceneLoader.h`
- `source/sceneLoader.cpp`
- `source/pipeline/` 下新增 `environmentSHGenerator.*` 或同等职责文件
- 如需统一数据结构，可同时调整 `source/baseStructs.h`

### 阶段 8：在 `geometry` 中接入 SH 漫反射 IBL

目标：

- 让场景材质能消费 `SH` 漫反射环境光
- 建立环境资源从 `SceneLoader -> GlobalSet -> geometry shader` 的正式链路

推荐接入方式：

- 把 `SH` 系数放入全局共享 shader 输入，而不是材质私有参数
- shader 侧提供统一的 `EvaluateIrradianceSH(normalWS)` 或同等函数
- PBR diffuse ambient 项优先改为由 `SH irradiance * albedo` 驱动
- 在没有 `SH` 资源时保留安全回退路径，避免场景直接变黑

这一阶段暂不要求：

- 镜面 IBL
- `prefilterCubeMap`
- 多环境探针混合
- 动态时变环境更新

预期影响区域：

- `shader/glsl/common/` 下新增或扩展 `SH` 评估函数
- `shader/glsl/pass/` 下的 `geometry` 相关 shader
- `source/renderGraph.cpp`
- `source/renderSystem.cpp`
- 如全局 descriptor 布局需要扩展，也可能涉及材质与反射相关加载路径

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

## 后续扩展

这套设计应在未来继续沿用，用于承接：

- `cube -> SH`
- `cube -> prefilterCubeMap`
- 解析式大气
- 体积云
- 大气与云层合成
- 对场景几何的 aerial perspective

本文路线的核心价值，是让这些系统建立在统一的天空基础设施之上，而不是等后续功能到来时再做一次新的底层改造。

## 第一版 Sky Pass 的非目标

第一版不需要解决：

- 解析式大气
- 云层渲染
- SH 漫反射 IBL
- prefilter 镜面 IBL
- 昼夜切换
- 天气系统

第一版只需要把正确的渲染结构搭起来，使后续这些功能能够直接接入，而不需要再次进行基础设施重构。
