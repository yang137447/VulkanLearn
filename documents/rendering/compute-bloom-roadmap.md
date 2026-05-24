# Compute Bloom 改造方案

## 目标

本文档定义 `VulkanLearn` 的 bloom 从当前 fullscreen graphics pass 方案升级为 `compute shader + 单张多 Mip 金字塔` 方案的改造路线。

当前阶段的直接目标是：

- 用 compute shader 接管 `prefilter / downsample / upsample`
- 使用一张支持多级 mip 的 `bloomPyramid` 纹理承载整条 bloom 链
- 保持 `toneMapping` 为最终输出到 swapchain 的 graphics pass
- 为后续更多 compute 后处理提供最小可复用的 render graph 基础

这份文档的重点不是“再做一版 bloom 特效”，而是把 bloom 升级为当前项目里第一条正式的 compute 后处理链。

## 为什么走这条路线

当前工程已经具备：

- `.comp` shader 编译能力
- `ComputePipeline` 创建、绑定与 `dispatch` 基础
- 用 compute 生成环境 cubemap 和预过滤环境图的先例

当前真正缺失的并不是“GPU 上能不能跑 compute”，而是：

- render graph 仍主要围绕 graphics render pass 建模
- 后处理执行路径仍假设所有 pass 都以 `beginRenderPass -> draw fullscreen triangle` 方式运行
- 渲染资源还没有把 `storage image + per-mip image view` 当作正式能力

如果继续扩展当前 bloom fullscreen pass 方案，会遇到这些问题：

- downsample 和 upsample 层级一多，graphics pass 数量快速膨胀
- attachment、framebuffer 和 render pass 切换成本变高
- 后续想做更完整的金字塔滤波、Mip 复用和更复杂的 blur 时，表达会越来越别扭

因此，当前最优路线不是继续堆 fragment bloom，而是：

- 保留 `toneMapping` 为 graphics pass
- 只把 bloom 改成 compute 链
- 让 render graph 获得“最小但正式”的 compute 后处理能力

## 设计原则

- bloom 内部统一使用 `compute shader`
- bloom 资源统一使用单张多 Mip `bloomPyramid`
- `toneMapping` 继续作为 graphics pass 负责最终输出
- 第一版不引入额外的全尺寸 ping-pong 纹理
- 第一版优先支持稳定的 `threshold / knee / strength / firefliesClamp`
- 只为 bloom 改造 render graph 所需最小能力，不顺手重写整个后处理系统

## 最终目标形态

改造完成后，渲染流程应支持如下结构：

1. `shadow`
2. `geometry`
3. `sky`
4. `postProcess`
5. `bloomPrefilterCompute`
6. `bloomDownsampleCompute`
7. `bloomUpsampleCompute`
8. `toneMapping`

其中：

- `postProcess` 输出 `sceneColor`
- `bloomPrefilterCompute` 从 `sceneColor` 写 `bloomPyramid mip0`
- `bloomDownsampleCompute` 逐级把 `mip i` 写到 `mip i + 1`
- `bloomUpsampleCompute` 逐级把低分辨率 bloom 累加回更高分辨率 mip
- `toneMapping` 读取 `sceneColor` 和 `bloomPyramid mip0`

## 当前状态总结

### 已具备

- `renderGraphConfig.json` 已有 bloom pass 顺序和 tone mapping 合回路径
- `toneMapping` 已经有 `scene + bloom * bloomStrength` 的职责拆分
- 工程已有 `ComputePipeline`
- 工程已有 compute shader 反射与编译路径

### 当前 bloom 的限制

- bloom 仍基于 graphics pass
- 当前只有少量固定分辨率中间纹理
- `threshold` 和 `knee` 仍是 shader 内硬编码
- 资源模型没有显式承载 `storage image`
- 资源模型没有显式承载 `per-mip image view`
- pass 间 barrier 主要面向 color attachment -> fragment sampled

### 这次改造真正要补的缺口

- compute pass 类型
- storage image 资源声明
- bloom 金字塔资源布局
- compute 与 graphics 之间的同步
- bloom 参数正式外置化

## 推荐方案

### 1. 资源形态

新增一张名为 `bloomPyramid` 的 HDR 纹理，建议约束如下：

- 格式：`R16G16B16A16_SFLOAT`
- 用途：`sampled + storage`
- mip 数：按分辨率自动计算，建议至少保留到最小边接近 `1`
- 视图：
  - 一个全图 `imageView` 供最终 `toneMapping` 采样 `mip0`
  - 每个 mip 单独一个 `imageView`，供 compute 逐级读写

第一版不推荐额外创建两张全尺寸 ping-pong 图。

原因：

- bloom 金字塔天然适合单张多 mip 组织
- 资源更少
- descriptor 管理更简单
- 后续需要 separable blur 时再评估是否增加 ping-pong

### 2. Shader 组织

第一版建议使用 3 个 compute shader：

- `pass/bloomPrefilter.comp`
- `pass/bloomDownsample.comp`
- `pass/bloomUpsample.comp`

职责拆分如下：

- `bloomPrefilter.comp`
  - 输入：`sceneColor`
  - 输出：`bloomPyramid mip0`
  - 功能：亮部提取、soft knee、fireflies 抑制
- `bloomDownsample.comp`
  - 输入：`bloomPyramid mip i`
  - 输出：`bloomPyramid mip i + 1`
  - 功能：逐级降采样并做基础滤波
- `bloomUpsample.comp`
  - 输入：`bloomPyramid mip i + 1`
  - 输出：`bloomPyramid mip i`
  - 功能：逐级回卷并累加 bloom 能量

### 3. Fireflies 抑制

Fireflies 抑制应放在 `prefilter` 阶段，而不是放在最终合成时补救。

第一版推荐做法：

- 先计算亮度
- 对极高亮度做上限 clamp
- 再进行 `threshold + soft knee`

这样可以尽早阻止极亮单像素污染整条金字塔。

### 4. Tone Mapping 职责保持稳定

`toneMapping` 不应该知道 bloom 内部是 graphics pass、ping-pong 还是多级 mip。

它只需要稳定地消费：

- `sceneColor`
- `bloomPyramid mip0`

也就是说，bloom 内部结构可以演进，但 tone mapping 的输入语义应尽量不变。

## Render Graph 改造点

### 1. Pass 类型

当前 `Renderpass` 结构默认持有：

- `vk::RenderPass`
- `framebuffers`
- `clearValues`

这套字段天然偏向 graphics pass。

建议新增显式 pass 类型：

- `Graphics`
- `Compute`

对 compute pass：

- 不创建 `vk::RenderPass`
- 不创建 `framebuffer`
- 不走 `beginRenderPass`
- 改为 `bind compute pipeline + bind descriptor sets + dispatch`

### 2. 资源 usage

当前 render graph 资源 usage 已支持：

- `colorAttachment`
- `sampled`
- `present`
- `transferSrc`
- `transferDst`

需要新增：

- `storage`

如果后续希望让资源描述更清楚，也可以在后续迭代中增加：

- `mipLevels`
- `generateMipChain`

但第一版也可以由代码按资源尺寸自动推导 bloom mip 数。

### 3. Compute Pass 描述

建议在 `renderGraphConfig.json` 中为 compute bloom pass 引入显式标记，例如：

```json
{
  "name": "bloomPrefilterCompute",
  "passType": "compute",
  "shaderName": "pass/bloomPrefilter",
  "input": [
    { "resource": "sceneColor" }
  ],
  "output": [
    { "resource": "bloomPyramid" }
  ]
}
```

第一版不必把 JSON 做得过度复杂，但至少需要能表达：

- 这是一个 compute pass
- 它使用哪个 compute shader
- 它读哪些资源
- 它写哪些资源

## 渲染资源改造点

### 1. `RenderResource`

`RenderResource` 需要从“单 image + 单 imageView”升级为“支持 mip 视图的图像资源壳子”。

建议补充的能力：

- `mipLevels`
- `std::vector<vk::ImageView> mipImageViews`
- 是否支持 storage usage

如果不想在 `RenderResource` 中堆太多状态，也可以保留原字段，再额外补：

- `fullImageView`
- `mipImageViews`

关键点不是命名，而是 bloom pyramid 需要稳定拿到每一级 mip 对应的 view。

### 2. Descriptor Image Info

当前 post-process 路径更偏向 sampled image。

compute bloom 需要同时支持：

- sampled input
- storage output

因此，descriptor 更新路径需要覆盖：

- `vk::DescriptorType::eCombinedImageSampler`
- `vk::DescriptorType::eStorageImage`

## RenderSystem 改造点

### 1. 执行分支

当前后处理执行路径假设所有 pass 都是 graphics pass。

建议改为：

- `Graphics` pass 继续走原有 `beginRenderPass -> bind pipeline -> draw`
- `Compute` pass 走 `bind compute pipeline -> bind descriptor sets -> dispatch`

### 2. Barrier

compute bloom 真正容易出错的地方不是 shader，而是同步。

当前 pass 间 barrier 更偏向：

- color attachment 写入
- fragment shader 采样读取

compute bloom 需要补齐这些转换：

- `eColorAttachmentOptimal -> eShaderReadOnlyOptimal`
- `eShaderReadOnlyOptimal -> eGeneral`
- `eGeneral -> eGeneral`
- `eGeneral -> eShaderReadOnlyOptimal`

同时需要覆盖的 stage 和 access 语义包括：

- `eColorAttachmentOutput`
- `eFragmentShader`
- `eComputeShader`
- `eColorAttachmentWrite`
- `eShaderRead`
- `eShaderWrite`

如果不把这部分显式抽象出来，后面每增加一个 compute pass 都会重复踩坑。

### 3. Dispatch 尺寸

第一版建议每个 compute shader 使用固定 workgroup，例如：

- `8 x 8`

dispatch 维度按当前 mip 尺寸计算：

- `groupX = (width + 7) / 8`
- `groupY = (height + 7) / 8`

对 `downsample / upsample`：

- 不需要引入额外 draw call
- 在单个 pass 内循环多个 mip，或拆成多个 compute pass，二选一即可

对当前项目第一版，更推荐：

- 保持 `prefilter / downsample / upsample` 三个逻辑 pass
- 在 `downsample` pass 内循环写完整个下采样链
- 在 `upsample` pass 内循环完成全部回卷

这样 render graph 结构清楚，同时不会把每一级 mip 都做成一个 JSON pass。

## 推荐参数

第一版建议参数范围：

- `threshold`: `1.0 - 1.2`
- `knee`: `0.3 - 0.5`
- `bloomStrength`: `0.05 - 0.10`
- `firefliesClamp`: `10.0 - 20.0`

第一版建议优先把这些参数放到 bloom 专用参数里，而不是继续硬编码在 shader 内。

## 分阶段实施建议

### 阶段 1：资源与配置改造

- 给 render graph 资源增加 `storage` usage
- 为 `bloomPyramid` 创建多 mip 图像
- 为每级 mip 创建独立 `imageView`
- 在配置中替换原有 `bloomDownSample1 / 2` 和 `bloomUpSample1`

### 阶段 2：pass 类型与执行路径改造

- 为 `Renderpass` 增加 `passType`
- 为 compute pass 增加 pipeline 获取与 descriptor 更新路径
- 在 `RenderSystem` 中加入 compute pass 执行分支
- 抽象 graphics / compute 间共享的资源 barrier 逻辑

### 阶段 3：compute bloom shader 接入

- 新增 `bloomPrefilter.comp`
- 新增 `bloomDownsample.comp`
- 新增 `bloomUpsample.comp`
- 移除原有 bloom fragment shader 依赖

### 阶段 4：tone mapping 接口收口

- 让 `toneMapping` 统一读取 `bloomPyramid mip0`
- 保持 `scene + bloom * bloomStrength` 合回路径不变

### 阶段 5：调试与验收

- 增加关闭 bloom 的开关
- 增加仅显示 bloom 结果的 debug 路径
- 验证天空、高光、emissive、HDRI 背景不会被 bloom 吞掉

## 文件改造建议

### 必改

- `config/renderGraphConfig.json`
- `source/renderGraph.h`
- `source/renderGraph.cpp`
- `source/renderSystem.cpp`
- `source/pipeline/computePipeline.h`
- `source/pipeline/computePipeline.cpp`
- `shader/glsl/pass/bloomPrefilter.comp`
- `shader/glsl/pass/bloomDownsample.comp`
- `shader/glsl/pass/bloomUpsample.comp`
- `shader/glsl/pass/toneMapping.frag`

### 大概率会改

- `resources/materials/pass/MI_toneMapping.json`
- bloom 相关材质或参数配置文件
- descriptor 更新相关工具函数

### 不建议第一版就改

- `toneMapping` 输出路径
- 全量后处理框架重写
- 自动曝光
- lens dirt
- anamorphic bloom

## 风险点

- compute 与 graphics 之间的 image layout 切换容易漏
- per-mip descriptor 和 image view 管理容易写乱
- 如果把每一级 mip 都暴露成 render graph pass，配置会迅速膨胀
- 如果在第一版同时引入 ping-pong，会显著增加资源和同步复杂度

## 验收标准

- bloom 内部全部由 compute shader 完成
- `toneMapping` 仍正常输出到 swapchain
- bloom 强度、阈值和 knee 可调
- 极亮单像素不会造成明显 fireflies
- 天空背景、高亮 emissive 和镜面高光在开启 bloom 后观感稳定
- 代码结构上已具备继续加入更多 compute 后处理的基础

## 最终建议

对当前 `VulkanLearn`，最优改造方案不是“继续补 fragment bloom”，而是：

- 用 `compute shader` 正式实现 bloom
- 使用 `单张多 Mip bloomPyramid`
- 保持 `toneMapping` 为最终 graphics pass
- 只做 bloom 所需的最小 render graph compute 扩展

这条路线的收益最大，同时能把本项目的后处理能力从“只会 fullscreen draw”升级为“支持正式 compute 后处理”。
