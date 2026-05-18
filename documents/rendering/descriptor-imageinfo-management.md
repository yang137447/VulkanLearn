# 贴图 DescriptorImageInfo 管理方案总结

## 背景

当前仓库在材质贴图和全局贴图上采用了两种不同的管理方式：

- `MaterialInstance` 会在初始化阶段预先生成并缓存自己的 `vk::DescriptorImageInfo`
- `SceneLoader` 持有的全局贴图在更新 Descriptor Set 时临时拼装 `vk::DescriptorImageInfo`

后者为了保证 `vk::WriteDescriptorSet` 引用的内存生命周期有效，引入了局部 `std::vector<vk::DescriptorImageInfo>` 作为补丁式缓存。这个方案能工作，但代码风格不统一，也让资源描述和绑定逻辑分散在调用点。

本文总结 3 种常见思路，并给出当前仓库的推荐方向。

## 方案 1: 资源自带描述信息

### 思路

让 `Texture` 在创建完成后，直接持有自己的默认 `vk::DescriptorImageInfo`。

典型内容包括：

- `imageLayout`
- `imageView`
- `sampler`

调用方在绑定时直接使用：

```cpp
write.setImageInfo(texture->GetDescriptorInfo());
```

### 优点

- 生命周期天然正确，`DescriptorImageInfo` 跟着 `Texture` 走
- 不需要在更新 Descriptor Set 时再构造临时对象
- 全局贴图和材质贴图可以统一成同一种读取方式
- 调用点更干净，不需要补丁式的局部容器保活

### 缺点

- 默认假设一张 `Texture` 对应一种常用绑定描述
- 如果后续需要同一张图配多种 `ImageView` 或多种 `Sampler`，需要额外扩展

### 适用场景

- 当前仓库这种 `Texture` 已经内聚了 `imageView` 和 `sampler` 的结构
- 大多数贴图只需要一个默认采样方式
- 希望快速统一全局资源和材质资源的绑定路径

## 方案 2: 视图与采样器独立管理

### 思路

`Texture` 只代表底层图像资源，真正用于着色器绑定的是单独的视图对象或资源视图对象。`Sampler` 也不挂在 `Texture` 上，而是来自采样器缓存池或状态对象。

绑定时在更高一层组装：

- `Texture`
- `ImageView`
- `Sampler`
- `DescriptorImageInfo`

### 优点

- 更灵活
- 同一张图可以支持多种读取视图
- 采样器可以复用，不和具体贴图强耦合
- 更接近大型引擎的底层 RHI 设计

### 缺点

- 资源管理复杂度明显更高
- 调用点需要更明确的绑定上下文
- 仍然需要处理 `DescriptorImageInfo` 的生命周期问题

### 适用场景

- 需要同一张贴图支持不同 mip 范围、array slice、cube face 或采样状态
- 已经有比较明确的 RHI / RenderGraph 资源层

## 方案 3: Bindless / 全局描述符表

### 思路

不再在每个材质或每个 Pass 上频繁更新单独的贴图绑定，而是维护一个全局大表，把大量贴图统一注册进去。Shader 通过索引访问贴图。

典型形式：

- CPU 侧维护全局 descriptor array
- 材质只保存贴图索引
- Shader 用索引从全局表中采样

### 优点

- 大幅减少单次 draw / pass 的贴图绑定开销
- 更适合大规模资源和材质系统
- 资源访问模型更统一

### 缺点

- 架构改造成本最高
- 对材质系统、反射、shader 约定都有连锁影响
- 不适合作为当前仓库的小步迭代修改

### 适用场景

- 大规模现代渲染架构
- 已经明确要走 bindless 或 descriptor indexing 路线

## 当前仓库推荐

当前仓库最适合采用方案 1。

原因：

- `Texture` 已经持有 `image`、`imageView`、`sampler`
- `MaterialInstance` 本身就已经在做“预先缓存贴图描述信息”这类工作
- 当前问题主要出在全局贴图没有统一的描述持有位置，而不是架构本身不支持
- 方案 1 改动范围最小，却能把 `sceneObject.cpp` 和 `renderGraph.cpp` 里的临时补丁逻辑去掉

## 当前落地建议

### 第一步

给 `Texture` 增加默认 `vk::DescriptorImageInfo` 缓存，并提供只读访问接口。

### 第二步

在 `Texture` 构造完成时初始化这份缓存。

### 第三步

把以下调用点切换为直接读取 `Texture` 自带的描述信息：

- `SceneObject::UpdateDescriptorSet()`
- `Renderpass::UpdateDescriptorSets()`

### 第四步

删除局部 `std::vector<vk::DescriptorImageInfo>` 这类为保活临时对象而引入的补丁代码。

## 后续演进建议

如果未来出现以下需求，再考虑从方案 1 继续升级：

- 同一张纹理需要多个视图
- 同一张纹理需要多种采样状态
- 大量贴图更新的 CPU 开销开始明显
- 准备引入 bindless 或 descriptor indexing

在此之前，方案 1 是当前仓库里最直接、最稳定、最符合现有资源管理习惯的解法。
