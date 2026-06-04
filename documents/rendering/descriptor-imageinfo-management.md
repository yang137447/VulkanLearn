# 贴图 DescriptorImageInfo 管理方案总结

## 背景

当前仓库在材质贴图、pass 输入贴图和全局渲染贴图上统一采用“资源持有默认描述，descriptor 写入阶段只引用稳定描述”的方式：

- `Texture` 在创建完成后持有默认 `vk::DescriptorImageInfo`
- `MaterialInstance::GetTextureDescriptorInfo()` 直接返回对应 `Texture` 的默认描述
- pass / object descriptor 写入通过 `RendererDescriptorContext`、`RendererDescriptorWriter` 和 `RendererObjectResourceManager` 读取稳定描述并提交给 backend
- `RendererBackendVulkan` 把现有 `vk::WriteDescriptorSet` 映射成 Vulkan device boundary 使用的 `RHIDescriptorWrite`

`vk::WriteDescriptorSet` 引用的 image info 不再依赖调用点临时拼装出来的局部对象。若 descriptor writer 需要批量提交，它只在 backend/device boundary 调用范围内复制成提交所需的短生命周期数组，不把补丁式缓存散落到 pass 或 object 代码里。

本文总结 3 种常见思路，并记录当前仓库已经采用的合同。

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
- 更接近大型引擎的底层资源视图 / backend 设计

### 缺点

- 资源管理复杂度明显更高
- 调用点需要更明确的绑定上下文
- 仍然需要处理 `DescriptorImageInfo` 的生命周期问题

### 适用场景

- 需要同一张贴图支持不同 mip 范围、array slice、cube face 或采样状态
- 已经有比较明确的 Vulkan backend / RenderGraph 资源层

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

## 当前仓库合同

当前仓库采用方案 1。

原因：

- `Texture` 已经持有 `image`、`imageView`、`sampler`
- `MaterialInstance` 本身就已经在做“预先缓存贴图描述信息”这类工作
- pass 输入和 object descriptor 写入已经通过 renderer descriptor context / writer 显式传递
- 资源描述和 descriptor 写入逻辑集中在 `Texture`、`MaterialInstance`、`RendererDescriptorWriter`、`RendererObjectResourceManager` 和 Vulkan backend/device boundary

## 当前落地状态

### Texture

`Texture` 保存默认 `vk::DescriptorImageInfo`，并通过 `GetDescriptorInfo()` 提供只读访问。

### Material

`MaterialInstance::GetTextureDescriptorInfo()` 从绑定的 `Texture` 读取描述，不在材质实例里重复创建 image info。

### Pass / Object Descriptor

`RenderGraph` 和 object descriptor 更新不回读 mutable World，也不依赖已删除的 scene wrapper。当前写入入口是：

- `RendererDescriptorWriter`
- `RendererObjectResourceManager`
- `RendererBackendVulkan::UpdateDescriptorSets()`
- `RHIDeviceVulkan::UpdateDescriptorSets()`

### Backend / Device Boundary

`RHIDescriptorWrite` 保持 Vulkan-native descriptor type、buffer info 和 image info。`RHIDescriptorWrite`、`RHIBufferHandle`、`RHIImageHandle` 等名称只表示当前 Vulkan resource lifecycle handle 语境，不引入 API-neutral descriptor 类型。

## 后续演进建议

如果未来出现以下需求，再考虑从方案 1 继续升级：

- 同一张纹理需要多个视图
- 同一张纹理需要多种采样状态
- 大量贴图更新的 CPU 开销开始明显
- 准备引入 bindless 或 descriptor indexing

在此之前，方案 1 是当前仓库里最直接、最稳定、最符合现有资源管理习惯的解法。
