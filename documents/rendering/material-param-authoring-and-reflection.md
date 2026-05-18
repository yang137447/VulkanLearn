# 材质参数生成与反射方案

## 目标

本文档定义一套精简的材质参数工作流，同时满足两类需求：

- 让 shader 编写阶段有稳定的参数声明，不因为未手写 `set = 1` 资源而报错
- 让运行时继续以 shader 反射结果为准，避免 descriptor 绑定错误

这套方案不引入新的 shader 语言，也不把 `param json` 作为运行时真相源。

## 核心定位

三类数据的职责边界如下：

- `param json`
  - 仅服务于 shader 编写体验
  - 描述参数名、贴图名、默认值和生成顺序
  - 生成给 GLSL 使用的参数声明文件
- `MaterialInstance`
  - 仅保存实例值
  - 不定义 shader 接口
- shader reflection
  - 作为运行时真实使用资源的依据
  - 决定 descriptor layout、descriptor write 和校验行为

核心原则：

- 编写期便利性来自 `param json`
- 运行时安全性来自反射

## Set 约定

当前建议保持现有 descriptor set 职责不变：

- `set 0`: 全局资源
- `set 1`: 材质资源
- `set 2`: 物体资源
- `set 3`: pass 局部资源

其中 `set 1` 约定为材质专用槽位：

- `binding 0`: 材质参数 UBO
- `binding 1..N`: 材质贴图，按 `param json` 中声明顺序生成

这个顺序只用于生成声明，运行时仍以反射出的实际资源为准。

## 生成文件位置

生成文件使用“同目录 `generated/` 子目录”方案。

推荐目录示例：

- `shader/glsl/unlit.vert`
- `shader/glsl/unlit.frag`
- `shader/glsl/generated/unlit.param`
- `shader/glsl/pass/sky.vert`
- `shader/glsl/pass/sky.frag`
- `shader/glsl/pass/generated/sky.param`

推荐原因：

- include 路径稳定，适配当前按“引用者所在目录”解析 include 的行为
- 手写 shader 与生成文件分离，目录更清晰
- 每个 shader 的生成物就近存放，便于定位和增量更新

不建议把这类文件放到：

- `build/`
- `shader/spv/`
- 全局统一的 `shader/glsl/generated/...` 根目录

## 工作流

推荐工作流如下：

1. 编写 `shader/meta/<shaderName>.param.json`
2. 工具生成与 shader 同目录对应的 `generated/<shaderName>.param`
3. `vert` 或 `frag` 按需 `#include` 该生成文件
4. 现有编译链继续执行 `glsl -> spv`
5. 运行时通过 SPIR-V 反射获取真实的 `set / binding / type / stage`
6. 材质实例仅按参数名和贴图名提供值
7. 反射结果与实例值做匹配和绑定

## 生成文件格式

推荐每个 shader 生成一份参数声明文件，例如：

`shader/glsl/generated/unlit.param`

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
#include "generated/unlit.param"
```

注意：

- `vs` 和 `ps` 是分别预处理、分别编译的
- 哪个阶段用到材质资源，哪个阶段就应 `include`
- 如果两个阶段都声明同一资源，则声明必须保持一致

## Param Json 的作用范围

`param json` 只负责以下内容：

- 参数名
- 参数类型
- 贴图名
- 贴图类型
- 默认值
- 生成顺序

`param json` 不负责以下内容：

- 决定运行时最终哪些 binding 必须创建
- 替代反射生成 descriptor set layout
- 作为运行时资源使用真相源

原因是未被 shader 实际使用的资源，可能在编译优化后被移除。运行时如果仍强行按 `param json` 绑定，反而更容易出错。

## 反射的职责

反射仍然是这套方案的核心安全机制：

- 确认 shader 实际使用了哪些材质资源
- 确认真实的 `set / binding / descriptor type / stage`
- 避免 `param json` 与最终 SPIR-V 不一致时发生错误绑定

推荐规则如下：

- `param json` 负责生成声明
- 反射负责运行时绑定
- 两者之间允许存在“声明了但未使用”的情况

## 功能模块

对于闪点、边缘光、溶解、流光这类“美术功能”，推荐把它们视为材质功能模块，而不是零散参数。

模块只影响 authoring 层，运行时依然只认最终 shader 的反射结果。

当前推荐的模块模型是“库式模块”：

- 模块负责提供参数定义、贴图定义和函数库
- 基础 shader 负责显式拼装并调用模块函数
- 模块系统不自动决定运行时计算顺序

### Public 和 Private

模块可见性参考 CMake 的 `public/private` 语义：

- `public`
  - 模块向材质公开的参数、贴图和函数入口
  - 会进入最终的材质参数表
  - 会参与 `*.param` 生成
  - 允许 `MaterialInstance` 提供实例值
- `private`
  - 模块内部使用的常量、辅助函数和中间实现细节
  - 不进入材质实例接口
  - 不要求美术在材质实例中填写
  - 可以存在于模块自己的 GLSL 代码中

推荐约束如下：

- 只有 `public` 资源可以出现在材质参数 JSON 中
- `private` 资源不应成为外部材质约定的一部分
- 运行时绑定只关心最终 shader 反射出的真实资源，不关心它来自哪个模块

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

当前工程已经具备以下基础：

- `MaterialInstance` 负责实例参数与贴图保存
- shader 编译链已经支持 `#include`
- pipeline 创建阶段已经读取 debug SPIR-V 并做反射

当前仍存在的缺口主要是：

- `set 1` 的材质声明仍然需要在 GLSL 中手写
- 材质接口定义没有独立资产层
- 编写期缺少自动生成的参数声明文件

因此，这份方案的第一目标不是替换现有系统，而是在现有链路前面增加一个轻量的 authoring 层。

## 第一版非目标

第一版不需要解决：

- JSON 到完整 GLSL 的全量转译
- Shader Graph
- keyword / variant 系统
- 自动生成整套 pipeline state
- 复杂材质编辑器

第一版只需要做到：

- 用 `param json` 生成一份可 `include` 的参数声明
- 让 shader 编写时不报未定义错误
- 让运行时继续完全依赖反射结果绑定

## 建议实施顺序

### 阶段 1

- 定义 `param json` 格式
- 支持参数 UBO 和 `sampler2D` 两类资源

### 阶段 2

- 实现生成器
- 输出同目录 `generated/*.param`

### 阶段 3

- 在材质 shader 中接入 `#include`
- 保持现有 `shaderc` 编译链不变

### 阶段 4

- 在加载材质实例时，继续使用反射结果做绑定与校验
- 只把 `param json` 用作编写辅助，不进入运行时绑定真相链

## 一句话结论

这套方案的本质是：

- 用 `param json` 提供编写期材质声明
- 用同目录 `generated/*.param` 保证 shader 开发体验
- 用反射保证运行时绑定正确

它是对当前系统的增量增强，而不是一次新的 shader 语言替换。
