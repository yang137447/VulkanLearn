# 材质参数生成与反射方案

## 目标

本文档定义一套精简的材质参数工作流，同时满足两类需求：

- 让 shader 编写阶段有稳定的参数声明，不因为未手写 `set = 1` 资源而报错
- 让 M_ schema 为跨 Pass 的 Set 1 layout 提供稳定合同
- 让 shader 反射验证每个 Pass 的实际使用子集

这套方案不引入新的 shader 语言。M_ 参数与贴图 schema 是 Set 1 layout 真相源，
SPIR-V reflection 是单个 Pass 实际资源使用情况的真相源。

## 核心定位

四类数据的职责边界如下：

- `M_*.json`
  - 描述参数名、类型、贴图和默认值
  - 生成 GLSL include 与完整 `MaterialDescriptorSchema`
  - 决定 Set 1 binding、UBO std140 offset 和 layout stage visibility
- `MaterialInstance`
  - 仅保存实例值
  - 不定义 shader 接口
  - `renderStateOverrides`、`macros`、`parameters`、`textures` 都只保存显式 override
  - 等于 M_ 默认值或为空的 override 对象必须省略
- shader reflection
  - 描述一个 Base/ShadowDepth Pass 实际使用的资源子集
  - 必须通过 `MaterialDescriptorSchema` 子集校验
- `Material`
  - 持有完整 Set 1 Schema
  - 汇总 Base 与 ShadowDepth 的 active binding，驱动 descriptor write

核心原则：

- 编写期声明与运行时 layout 来自同一个 M_ schema
- reflection 不再生成 Set 1 完整 layout，只负责实际使用与一致性校验
- M_ 保存 static macro 默认值，MI 保持稀疏；resolver 在构建 variant key 前合成完整有效宏

## 输入 Schema 职责

M_ 参数和贴图只声明完整、稳定的材质 schema，不要求材质作者重复维护输入与宏或
Render Mode 的条件关系。MI 可以稀疏覆盖任意已声明参数和贴图；宏负责选择 shader
variant，SPIR-V reflection 负责描述该 Pass/variant 实际使用的资源子集。

如果后续调参面板需要按当前 variant 隐藏未使用输入，应从已编译 variant 的 reflection
或引擎生成的元数据推导，不能把同一条件再次交给 M_ 作者手写维护。

## MaterialInstance 运行时身份

同一个 MI asset 在一次 world resource generation 内只对应一个运行时
`MaterialInstance`。Cache key 使用规范化后的 MI asset path，不包含：

- pass name
- render-pass identity
- pipeline state
- pipeline cache key

多个兼容 pass 引用同一个 MI asset 时，共享参数、纹理、材质 UBO 和 runtime instance。
Material/pipeline 使用独立的 render-pass compatibility key；当前单一 `baseMaterial` 模型要求
同一 MI 的所有 pass 引用具备兼容 pipeline contract。不兼容复用在加载阶段报错，不能通过
复制 MaterialInstance 隐式解决。完整的不兼容 multi-pass variant 由后续 Material Multi-Pass
设计处理。

## Set 约定

当前建议保持现有 descriptor set 职责不变：

- `set 0`: 全局资源
- `set 1`: 材质资源
- `set 2`: 物体资源
- `set 3`: pass 局部资源

其中 `set 1` 约定为材质专用槽位：

- `binding 0`: 材质参数 UBO
- `binding 1..N`: 材质贴图，按 M_ schema 稳定顺序生成

完整 Set 1 layout、UBO 大小和 member offset 都由 `MaterialDescriptorSchema` 生成。
第一版所有 Set 1 binding 的 stage visibility 固定为 Vertex + Fragment。

## Material ShadowCaster 约定

当前轻量 ShadowCaster 不引入第二份 MI 或参数表。带 `shaderEvaluation` 的 M_ 材质
会从相同的顶点和 Surface Evaluation 自动组合 ShadowDepth；Surface shader `xxx`
仍可通过以下完整文件对提供最高优先级 override：

```text
shader/glsl/xxx.shadow.vert
shader/glsl/xxx.shadow.frag
```

文件对从现有 `shaderName` 自动推导，不需要在 `M_*.json` 或 `MI_*.json` 中增加
`shadowMode`/`shadowShader` 字段。两个文件都不存在表示没有专用实现；只存在一个 stage
会输出资产警告并继续自动路由。完整配对的编译失败或反射合同不兼容仍会终止加载。

专用 Shadow variant 继承 Base variant 的 `renderMode`、ShadingModel 和材质宏，因此
Alpha Clip、Wind/WPO 等静态分支与当前 MI 保持一致。运行时 `Material` 持有 Base pipeline
和可选 Shadow pipeline，`MaterialInstance` 仍只持有一份参数与纹理值。

专用 Shadow pipeline 直接绑定对象已经创建的 Base Set 0..2。为保证 descriptor set
兼容，Set 0/2 继承 Base engine reflection 合同，Set 1 始终使用完整 M_ schema；Shadow
SPIR-V 的 Set 1 binding 必须是 schema 子集，并逐项校验：

- set/binding 与 descriptor type
- binding 名称
- shader stage visibility 必须是 schema visibility 的子集
- UBO block size、member size、member offset 与 member name

第一版禁止专用 Shadow shader 使用 Set 3 或更高 set。Set 1 schema 已包含 Vertex +
Fragment visibility，因此 WPO/Wind 不再依赖 Base reflection 恰好保留 Vertex visibility。

没有 override 时，Opacity Mask、WPO 或 Two-Sided 材质自动生成 ShadowDepth；普通
`Opaque` 使用公共 Opaque ShadowCaster；透明材质不进入普通 Shadow Map。

## 生成文件位置

生成文件使用“同目录 `generate/` 子目录”方案。

推荐目录示例：

- `shader/glsl/M_unlit.vertex.glsl`
- `shader/glsl/M_unlit.surface.glsl`
- `shader/glsl/generate/M_unlitParamter.glsl`
- `shader/glsl/pass/sky.vert`
- `shader/glsl/pass/sky.frag`
- `shader/glsl/pass/generate/M_skyParamter.glsl`

推荐原因：

- include 路径稳定，适配当前按“引用者所在目录”解析 include 的行为
- 手写 shader 与生成文件分离，目录更清晰
- 每个 shader 的生成物就近存放，便于定位和增量更新

不建议把这类文件放到：

- `build/`
- `shader/spv/`
- 全局统一的 `shader/glsl/generate/...` 根目录

## 工作流

推荐工作流如下：

1. 编写 `M_*.json` 的 parameters、textures 与静态 features
2. 工具生成同目录 `generate/M_*Paramter.glsl`
3. Schema Builder 从同一个 M_ 生成完整 Set 1 layout
4. Standalone shader 或 Material Composer include 生成文件
5. 现有编译链继续执行 `glsl -> spv`
6. SPIR-V reflection 校验为 schema 子集
7. MI 提供实例值；descriptor write 使用已选择 Pass 的 reflection 并集

## 生成文件格式

推荐每个 shader 生成一份参数声明文件，例如：

`shader/glsl/generate/M_unlitParamter.glsl`

示例：

```glsl
layout(set = 1, binding = 0) uniform UBOMaterial
{
    vec4 tintColor;
    float roughness;
} uboMaterial;

layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
```

shader 中按需引用：

```glsl
#include "generate/M_unlitParamter.glsl"
```

注意：

- `vs` 和 `ps` 是分别预处理、分别编译的
- 哪个阶段用到材质资源，哪个阶段就应 `include`
- 如果两个阶段都声明同一资源，则声明必须保持一致

## M_ Schema 的作用范围

M_ schema 负责以下内容：

- 参数名
- 参数类型
- 贴图名
- 贴图类型
- 默认值
- 稳定生成顺序
- Set 1 descriptor layout
- UBO std140 member offset 与 block size

M_ schema 不负责以下内容：

- 猜测某个 Pass 实际执行了哪些材质逻辑
- 强迫 MI 提供未被任何选中 Pass 使用的可选贴图
- 替代 reflection 检查最终 SPIR-V

声明但未使用的资源可能在编译优化后被移除，因此 layout 使用完整 schema，实际
descriptor write 使用 Base 与 ShadowDepth reflection 并集。

## 反射的职责

反射仍然是这套方案的核心安全机制，但不再拥有完整 Set 1 layout：

- 确认 shader 实际使用了哪些材质资源
- 确认真实的 `set / binding / descriptor type / stage`
- 验证最终 SPIR-V 的每个 Set 1 binding 都属于 M_ schema

推荐规则如下：

- M_ schema 负责生成声明和完整 layout
- reflection 负责子集校验和 active descriptor write
- 两者之间允许存在“声明了但未使用”的情况

## 功能模块

对于闪点、边缘光、溶解、流光这类“美术功能”，推荐把它们视为材质功能模块，而不是零散参数。

模块只影响 authoring 层；合并后的 M_ schema 决定 Set 1 layout，最终 shader
reflection 决定各 Pass 的 active 使用子集。

当前推荐的模块模型是“库式模块”：

- 模块负责提供参数定义、贴图定义和函数库
- 基础 shader 负责显式拼装并调用模块函数
- 模块系统不自动决定运行时计算顺序

### Public 和 Private

模块可见性参考 CMake 的 `public/private` 语义：

- `public`
  - 模块向材质公开的参数、贴图和函数入口
  - 会进入最终的材质参数表
  - 会参与 `M_*Paramter.glsl` 生成
  - 允许 `MaterialInstance` 提供实例值
- `private`
  - 模块内部使用的常量、辅助函数和中间实现细节
  - 不进入材质实例接口
  - 不要求美术在材质实例中填写
  - 可以存在于模块自己的 GLSL 代码中

推荐约束如下：

- 只有 `public` 资源可以出现在材质参数 JSON 中
- `private` 资源不应成为外部材质约定的一部分
- 完整 layout 只关心模块合并后的 schema；active write 只关心最终 reflection，
  两者都不需要知道资源来自哪个模块

### 模块边界

推荐把模块分成两类：

- 输入模块
  - 追加参数和贴图
  - 例如 `glintIntensity`、`glintMask`
- 实现模块
  - 提供辅助函数和局部算法
  - 例如噪声函数、闪点计算函数、边缘光计算函数

第一版优先支持输入模块，因为它最适合与当前 `set 1` 的材质资源模型结合。

### 模块示例

以 `glint` 为例，推荐结构如下：

```json
{
  "name": "glint",
  "public": {
    "parameters": [
      { "name": "glintIntensity", "type": "float", "default": 1.0 },
      { "name": "glintSize", "type": "float", "default": 8.0 },
      { "name": "glintColor", "type": "vec3", "default": [1.0, 1.0, 1.0] }
    ],
    "textures": [
      { "name": "glintMask", "type": "sampler2D" }
    ],
    "includes": [
      "shader/glsl/modules/glint_public.glsl"
    ]
  },
  "private": {
    "includes": [
      "shader/glsl/modules/glint_private.glsl"
    ]
  }
}
```

建议行为如下：

- `public.parameters` 进入材质 UBO 声明
- `public.textures` 进入 `set 1` 的贴图声明
- `public.includes` 作为可复用的公开模块入口
- `private.includes` 只作为实现细节，不对材质实例暴露

### 组合规则

当基础材质与多个模块组合时，生成器负责：

- 合并基础材质的公开参数
- 合并模块的 `public` 参数和贴图
- 生成统一的 `set 1` 参数声明
- 生成稳定的模块 include 顺序

建议保持以下规则：

- `set 1, binding 0` 固定为材质参数 UBO
- `set 1, binding 1..N` 按合并后的公开贴图顺序生成
- 模块参数名和贴图名必须带模块前缀，避免冲突
- 模块函数的实际调用顺序由基础 shader 显式决定

例如：

- `glintIntensity`
- `glintColor`
- `glintMask`

不建议使用过于通用的名称，例如：

- `intensity`
- `color`
- `mask`

### 不建议模块化的内容

以下内容不建议在第一版中作为普通材质模块处理：

- 会修改 pipeline state 的功能
- 会改变 pass 结构的功能
- 会显著增加 shader variant 数量的功能

这些能力更适合作为：

- 基础 shader 类型
- 独立 variant
- render graph 或 pipeline 配置项

而不是简单的材质输入模块。

## 与当前工程的关系

当前工程已经实现：

- `MaterialInstance` 负责实例参数与贴图保存
- `MaterialParameterIncludeGenerator` 生成 M_ 参数声明
- `MaterialDescriptorSchema` 生成完整 Set 1 layout 与 UBO offset
- Base/ShadowDepth debug SPIR-V reflection 做 schema 子集校验
- Descriptor pool 按完整 layout 分配，write 按 active reflection 并集执行
- Material Composer 的详细合同见 `material-mesh-pass-composition.md`

## 第一版非目标

第一版不需要解决：

- JSON 到完整 GLSL 的全量转译
- Shader Graph
- keyword / variant 系统
- 自动生成整套 pipeline state
- 复杂材质编辑器

第一版参数类型只支持 `float`、`vec2`、`vec3`、`vec4`，贴图只支持
`sampler2D`。更多 descriptor 类型、Bindless Material Table 与动态数组不在
当前合同内。

## 一句话结论

这套方案的本质是：

- 用 M_ schema 同时生成编写期声明和运行时 Set 1 layout
- 用 reflection 验证各 Pass 子集并决定 active descriptor write
- 让 Base 与 ShadowDepth 共用同一份 MI 参数、贴图和 descriptor set

它是对当前系统的增量增强，而不是一次新的 shader 语言替换。
