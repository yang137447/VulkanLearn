# Environment Cubemap 与 IBL 统一执行计划

## 状态

- 类型：当前迁移计划
- 范围：HDRI、程序化天空、environment cubemap、SH9、specular prefilter
- 实施策略：一次完成职责拆分和主链迁移，不接 RenderGraph，不做异步 compute，不做 dirty update
- 验收目标：HDRI 与程序化天空只有 cubemap 来源不同，cubemap 之后完全共用同一条 IBL 链路

## 核心技术路线

```text
HDRI
  -> equirectToCubemap
  -> environmentCube

ProceduralSky
  -> skyToCubemap
  -> environmentCube

environmentCube
  -> cubemapSHGenerate
  -> environmentSH

environmentCube
  -> prefilterEnvMap
  -> prefilteredEnvironmentCube

lighting
  -> environmentSH
  -> prefilteredEnvironmentCube
  -> brdfLut
  -> environmentIntensity
```

以下规则是本次实现的硬约束：

1. `EnvironmentType` 只决定 `environmentCube` 的来源。
2. SH 和 prefilter 不允许出现 HDRI/ProceduralSky 两套实现。
3. lighting shader 不判断 environment type。
4. `RendererResourceCache` 负责注册、查询和生命周期，不负责理解资源如何创建。
5. 正常调用路径使用 `Acquire` 或 `Require`，调用方不写重复的空指针分支。
6. 生成模块不绑定 `globalTextures`；环境结果统一注册到 `worldTextures`。
7. 本次继续在 graphics queue 的同一 command buffer 中顺序执行 compute 和 graphics。

## 最终职责

| 模块 | 职责 | 不负责 |
| --- | --- | --- |
| `RendererResourceCache` | 保存 `shared_ptr`、按 key 查询、world 切换和延迟释放 | 创建 cubemap、录制 compute、选择环境类型 |
| `RendererEnvironmentLoader` | HDRI 资源加载、equirect 转 cubemap、注册 `environmentCube` | SH、prefilter、lighting binding |
| `ProceduralSkyCubeGenerator` | 创建程序化天空 cubemap、录制 `skyToCubemap`、注册 `environmentCube` | SH、prefilter、环境类型决策 |
| `EnvironmentIblBaker` | 对任意 `environmentCube` 计算 SH 和 prefilter | cubemap 来源、场景 JSON 解析 |
| `RenderSystem` | 选择 cubemap 来源、安排执行顺序 | 创建 HDR 资产贴图、持有环境贴图生命周期 |
| `RendererDescriptorWriter` | 从 resource cache 获取已注册纹理并写 descriptor | 创建纹理、决定 active environment |

## 资源键与生命周期

```text
globalTextures
  brdfLut

worldTextures
  environmentCube
  prefilteredEnvironmentCube

textures
  材质资产纹理缓存
```

`Texture` 持有 Vulkan image、memory、sample view 和 sampler。`RendererResourceCache` 中的 `shared_ptr<Texture>` 是 renderer 资源查询和 world-local 生命周期入口。生产模块可以持有同一份 `shared_ptr`，但必须在对应的 environment resource shutdown/rebuild 阶段释放自己的引用。

程序化 cubemap 和 prefiltered cubemap 还包含 storage view。storage view 仍由对应 generator/baker 管理，并且必须在底层 image 释放前销毁。

## 最终接口

### RendererResourceCache

文件：

- `source/render/resource/rendererResourceCache.h`
- `source/render/resource/rendererResourceCache.cpp`

新增以下接口，替换目前暴露 map 内部元素地址的 `GetWorldTexture`：

```cpp
std::shared_ptr<Texture> FindWorldTexture(
    std::string_view bindingName) const;

std::shared_ptr<Texture> RequireWorldTexture(
    std::string_view bindingName) const;
```

契约：

```text
FindWorldTexture
  -> 已注册：返回 shared_ptr
  -> 未注册：返回 nullptr

RequireWorldTexture
  -> 已注册：返回有效 shared_ptr
  -> 未注册：抛出 runtime_error，表示资源准备顺序错误
```

`RendererResourceCache` 不增加通用 `CreateTexture()` 或 `AcquireWorldTexture()`。cache 不掌握 HDR 路径、pipeline、backend、command buffer 和生成参数，不能负责创建。

### RendererEnvironmentLoader

HDRI 请求接口返回时必须保证 cubemap 已经注册：

```cpp
std::shared_ptr<Texture> AcquireHdriEnvironmentCube(
    const nlohmann::json& environmentConfig) const;
```

执行逻辑：

```text
FindWorldTexture("environmentCube")
  -> 命中：直接返回
  -> 未命中：读取 hdrPath 和 cubeSize
             执行 EnvironmentCubemapGenerator::Generate
             BindWorldTexture("environmentCube", cube)
             返回 cube
```

这里不再调用 `EnvironmentPrefilterGenerator::Generate`。

### ProceduralSkyCubeGenerator

新文件：

- `source/render/environment/proceduralSkyCubeGenerator.h`
- `source/render/environment/proceduralSkyCubeGenerator.cpp`

接口：

```cpp
class ProceduralSkyCubeGenerator
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);

    void Shutdown(RendererBackendVulkan& rendererBackend);

    std::shared_ptr<Texture> AcquireEnvironmentCube();

    void Record(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
};
```

`AcquireEnvironmentCube()` 的契约：

```text
内部 cubemap 已创建
  -> 返回已有 shared_ptr

内部 cubemap 未创建
  -> 创建 image / memory / storage view / cube sample view / sampler
  -> 构造 Texture
  -> BindWorldTexture("environmentCube", texture)
  -> 返回有效 shared_ptr
```

`Record()` 只执行：

```text
environmentCube -> General
skyToCubemap dispatch
environmentCube -> ShaderReadOnlyOptimal
```

### EnvironmentIblBaker

文件：

- `source/render/environment/environmentIblBaker.h`
- `source/render/environment/environmentIblBaker.cpp`

接口：

```cpp
class EnvironmentIblBaker
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);

    void Shutdown(RendererBackendVulkan& rendererBackend);

    std::shared_ptr<Texture> AcquirePrefilteredEnvironmentCube();

    void Record(
        vk::CommandBuffer commandBuffer,
        const std::shared_ptr<Texture>& environmentCube,
        uint32_t swapchainImageIndex);
};
```

`AcquirePrefilteredEnvironmentCube()` 缺失时创建并注册：

```cpp
resourceCache.BindWorldTexture(
    "prefilteredEnvironmentCube",
    prefilteredEnvironmentCube);
```

`Record()` 统一执行：

```text
environmentCube
  -> cubemapSHGenerate
  -> compute write environmentSH

environmentCube
  -> prefilterEnvMap（逐 mip dispatch）
  -> prefilteredEnvironmentCube

compute write
  -> fragment shader uniform/sample read barrier
```

## 一次性执行步骤

### 1. 完成 RendererResourceCache 查询接口

- [ ] 删除当前未完成的 `GetWorldTexture()` 声明和调用。
- [ ] 实现 `FindWorldTexture()`，按值返回 `shared_ptr<Texture>`。
- [ ] 实现 `RequireWorldTexture()`，未注册时抛出包含 resource key 的错误。
- [ ] 保留 `BindWorldTexture()` 作为唯一 world texture 注册入口。
- [ ] 不修改 `GetGlobalTexture()` 的 descriptor fallback 行为。

验收：不再有代码取得 `worldTextures` map 内部 `shared_ptr` 的地址。

### 2. 把程序化 cubemap 来源拆出

- [ ] 从 `ProceduralSkyIblGenerator` 移走 `skyToCubemapPipeline`。
- [ ] 移走 `SkyCubeResources`、sky cubemap 创建/销毁、对应 descriptor set。
- [ ] 新建 `ProceduralSkyCubeGenerator`。
- [ ] 将 sky cubemap 包装为 `shared_ptr<Texture>`。
- [ ] `AcquireEnvironmentCube()` 创建后注册为 `worldTextures["environmentCube"]`。
- [ ] `Record()` 完成写入和转为 shader read layout。

验收：`ProceduralSkyCubeGenerator` 中不存在 SH 和 prefilter 代码。

### 3. 完成 EnvironmentIblBaker

- [ ] 将 `skySHGeneratePipeline` 移入 `EnvironmentIblBaker`。
- [ ] 将 `prefilterEnvMapPipeline` 移入 `EnvironmentIblBaker`。
- [ ] 移入 `PrefilteredCubeResources`、prefilter storage views 和 params buffers。
- [ ] 移入 SH/prefilter descriptor pool、descriptor sets 和更新逻辑。
- [ ] `AcquirePrefilteredEnvironmentCube()` 创建并注册 world texture。
- [ ] `Record()` 接受外部传入的 `environmentCube`。
- [ ] descriptor 中的 sampled cubemap 必须来自传入的统一 cubemap。
- [ ] 保留 compute-to-compute、compute-to-fragment 和 UBO 可见性 barrier。

验收：`EnvironmentIblBaker` 中不存在 `EnvironmentType` 判断。

### 4. 修正 shader 命名

- [ ] 将 `shader/glsl/generator/skySHGenerate.comp` 改名为 `cubemapSHGenerate.comp`。
- [ ] pipeline 创建名改为 `generator/cubemapSHGenerate`。
- [ ] shader 内容保持对任意 samplerCube 进行 SH9 投影，不引用 procedural sky 参数。
- [ ] 不手动编辑 `shader/spv/`，由项目 shader 编译流程重新生成。

验收：代码和 shader 中不存在“SH 只能来自 procedural sky”的命名暗示。

### 5. 收窄 RendererEnvironmentLoader

- [ ] `ProceduralSky` 分支不创建任何 HDRI 资源。
- [ ] `Hdri` 分支通过 `AcquireHdriEnvironmentCube()` 创建并注册 `environmentCube`。
- [ ] 删除 loader 内 `EnvironmentPrefilterGenerator::Generate()` 调用。
- [ ] 删除 loader 对 `prefilteredEnvironmentCube` 的注册。
- [ ] 暂时保留旧 `EnvironmentPrefilterGenerator` 文件，确认主路径无引用后再单独清理。

验收：loader 输出只有来源 cubemap，不输出 IBL 派生资源。

### 6. 改造 RenderSystem 成为调度者

成员替换：

```cpp
VL::ProceduralSkyCubeGenerator proceduralSkyCubeGenerator;
VL::EnvironmentIblBaker environmentIblBaker;
```

新增函数：

```cpp
std::shared_ptr<Texture> AcquireActiveEnvironmentCube();
void PrepareEnvironmentResources();
void RecordEnvironmentIbl(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex);
```

`AcquireActiveEnvironmentCube()` 只在这里判断类型：

```cpp
std::shared_ptr<Texture> RenderSystem::AcquireActiveEnvironmentCube()
{
    switch (currentRenderScene.environment.type)
    {
    case VL::EnvironmentType::ProceduralSky:
        return proceduralSkyCubeGenerator.AcquireEnvironmentCube();

    case VL::EnvironmentType::Hdri:
        return VL::RendererResourceCache::GetInstance()
            .RequireWorldTexture("environmentCube");
    }

    throw std::runtime_error("Unsupported environment type");
}
```

`PrepareEnvironmentResources()` 必须在 descriptor refresh 前执行：

```text
AcquireActiveEnvironmentCube
AcquirePrefilteredEnvironmentCube
RefreshRuntimeDescriptors
```

`RecordEnvironmentIbl()`：

```text
AcquireActiveEnvironmentCube

if ProceduralSky
  -> proceduralSkyCubeGenerator.Record

environmentIblBaker.Record(environmentCube)
```

- [ ] 初始化/关闭两个新模块。
- [ ] 在 `InitializeCurrentRenderSceneResources()` 中先 prepare，再 refresh descriptors。
- [ ] 在 `RecordAndSubmitCurrentRenderScene()` 中先上传 UBO，再录制环境链路，再录制 graphics passes。
- [ ] 删除 `BindActiveEnvironmentTextures()`。
- [ ] 删除 `proceduralSkyIblGenerator` 成员和所有调用。
- [ ] 删除旧生成器文件，确保没有重复 pipeline 和资源 ownership。

验收：`RenderSystem` 只有 cube 来源选择存在 type 分支。

### 7. 统一 descriptor 获取路径

lighting 继续声明：

```glsl
layout(set = 0, binding = 2) uniform samplerCube prefilteredEnvironmentCube;
```

`RendererDescriptorWriter` 当前通过 `GetGlobalTexture()` 查询，而该函数会先查 global、再 fallback 到 world。最终状态：

```text
globalTextures["prefilteredEnvironmentCube"]
  -> 不存在

worldTextures["prefilteredEnvironmentCube"]
  -> EnvironmentIblBaker 注册的唯一结果
```

- [ ] 删除程序化生成器中的 `BindGlobalTexture("prefilteredEnvironmentCube", ...)`。
- [ ] 删除 RenderSystem 中同名 global alias。
- [ ] 保持 descriptor plan 和 shader binding 名字不变。

验收：同名资源只有一个 world texture 注册来源，不存在 HDRI/程序化抢 binding。

### 8. 保持 GPU-owned SH 更新规则

- [ ] CPU 的 global UBO 更新继续跳过 `environmentSH` 字节范围。
- [ ] HDRI 也由 `cubemapSHGenerate` 写 `environmentSH`。
- [ ] 删除“程序化路径 GPU 写、HDRI 路径 CPU 写”的旧注释或代码。
- [ ] shadow UBO 更新同样不得覆盖 `environmentSH`。

验收：整个主路径不存在 CPU SH 生成或上传分支。

### 9. 生命周期与 world 切换

- [ ] `environmentCube` 和 `prefilteredEnvironmentCube` 注册为 world texture。
- [ ] world 重新加载时，旧 cache 引用进入 `ResourceRetireQueue`。
- [ ] producer 在重建资源前释放上一 world 的 `shared_ptr` 引用。
- [ ] 先销毁 storage views，再释放拥有底层 image 的 `Texture`。
- [ ] 若 `cubeSize` 改变，在 renderer 的 world resource reinitialize 阶段重建，不在每帧临时重建。
- [ ] swapchain 重建时重建依赖 global UBO 数量的 descriptor sets，但不重复加载 HDR 资产。

验收：切换 HDRI/程序化场景后，不引用上一 world 的 image view。

### 10. 清理过渡代码

- [ ] 删除 `ProceduralSkyIblGenerator`。
- [ ] 删除 `BindActiveEnvironmentTextures()`。
- [ ] 删除返回 `const std::shared_ptr<Texture>*` 的 world texture API。
- [ ] 删除旧的 HDRI prefilter 主路径。
- [ ] 清理旧注释、未使用 include 和缩进。
- [ ] `rg` 检查不存在旧类名和旧 shader 名。

## 最终帧内顺序

```text
UpdateGlobalUniformBufferExceptGpuOwnedRanges

if environment.type == ProceduralSky
  skyToCubemap
  barrier: compute write -> compute sample

if environment.type == Hdri
  environmentCube 已由 world load 阶段准备

cubemapSHGenerate
prefilterEnvMap (all mips)
barrier: compute write -> fragment uniform/sample read

shadow / geometry / lighting / post process
```

当前版本可以每帧执行 baker，先保证统一链路正确。dirty update、跨帧分摊、双缓冲和 async compute 后续独立设计，不混入这次迁移。

## 验证顺序

### 静态检查

```powershell
rg -n "ProceduralSkyIblGenerator|skySHGenerate|BindActiveEnvironmentTextures" source shader/glsl
rg -n "EnvironmentPrefilterGenerator::Generate" source
git diff --check
```

预期：前三个旧路径查询无结果；`git diff --check` 无新增空白错误。

### 编译

```powershell
cmake --build build -j
```

### 运行验证

1. 使用 `SC_speedtree.json` 验证程序化天空。
2. 使用 `SC_sifi_head.json` 验证 HDRI。
3. 检查 diffuse IBL：临时降低 direct light，确认 SH 仍影响背光面。
4. 检查 specular IBL：观察不同 roughness 材质的反射 mip 变化。
5. 连续切换 HDRI 与程序化场景，确认无黑图、旧环境残留和 validation error。
6. 使用 RenderDoc 或 Vulkan validation 确认 compute-to-fragment 可见性和 image layout 正确。

## 完成标准

- [ ] HDRI 与程序化天空仅在 cubemap 生成阶段分流。
- [ ] 两种来源共用 `EnvironmentIblBaker`。
- [ ] lighting shader 不知道 environment type。
- [ ] `Acquire` 返回时资源已创建并注册。
- [ ] `Require` 返回有效资源，否则明确报错。
- [ ] cache 不承担 pipeline 或资源生成逻辑。
- [ ] `prefilteredEnvironmentCube` 只有一个 world texture 生产者。
- [ ] CPU 不覆盖 GPU-owned `environmentSH`。
- [ ] MinGW 构建通过。
- [ ] 两种场景运行通过且 Vulkan validation 无新增错误。
