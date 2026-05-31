# 材质模块系统草案

## 目标

本文档讨论材质 `module` 的设计边界，重点解决以下问题：

- 什么能力适合做成模块
- 模块之间的顺序依赖如何表达
- 如何借鉴 C++ 的组织方式，让系统可控而不是“任意拼接代码”

这份文档是对 [material-param-authoring-and-reflection.md](../../rendering/material-param-authoring-and-reflection.md) 的补充。前者解决参数声明和反射职责，这里专门讨论材质功能模块。

## 核心定位

第一版推荐把 `module` 定义为：

- 参数与贴图的复用单元
- 函数与局部算法的复用单元
- 等待基础 shader 拼装和调用的代码单元

第一版不把 `module` 定义为：

- 自动调度执行的生命周期节点
- 任意改写 shader 主流程的黑盒拼装器
- 自动修改 pipeline state 的机制
- 自动扩展 render graph 或 pass 结构的机制

一句话概括：

- `module` 更像 C++ 中“带接口的库”
- 基础 shader 更像真正的调用方
- 装配器负责准备声明和 include，不负责替你决定计算顺序

## 借鉴 C++ 的思路

模块系统可以沿用几条 C++ 风格原则：

- 先声明依赖，再使用依赖
- 公共接口和私有实现分离
- 依赖关系显式书写，不依赖隐式顺序
- 调用顺序由调用方控制，而不是由被调库自己决定

推荐映射关系如下：

- `public`
  - 类似头文件中暴露给外部的接口
  - 包括材质参数、贴图、公开函数入口
- `private`
  - 类似源文件中的内部实现
  - 包括辅助函数、内部常量、中间计算
- `dependsOn`
  - 类似 `#include` 或链接依赖
  - 明确当前模块在编译和装配层依赖哪些其他模块
- `base shader`
  - 类似最终的可执行调用方
  - 决定模块何时被调用、以什么顺序被调用

## 模块边界

推荐把模块严格限制在材质 authoring 层。

第一版允许模块做的事：

- 暴露公开参数
- 暴露公开贴图
- 提供公开函数入口
- 提供私有实现
- 提供可被装配的 include 单元

第一版不允许模块做的事：

- 改动 `set 0`
- 改动 `set 3`
- 自动修改 blend、depth、cull 等 pipeline state
- 自动创建新的 pass
- 自动生成大量变体

这些能力更适合作为：

- 基础 shader 类型
- variant
- render graph 配置
- pipeline 配置

## Public / Private / Interface

为了更贴近 C++ 思想，模块建议分为三个可见层级：

- `public`
  - 对材质作者可见
  - 进入最终 `generated/*.param`
  - 允许材质实例提供值
- `private`
  - 对模块内部可见
  - 不进入材质实例接口
  - 只作为实现细节存在
- `interface`
  - 只声明接口约定，不直接引入实例参数
  - 适合后续扩展，目前第一版可以先不实现

第一版最小集合只需要：

- `public`
- `private`

但设计文档里保留 `interface` 概念，有利于后续扩展复杂模块依赖。

## 顺序依赖

顺序依赖是模块系统最需要收敛的地方。

### 基本原则

推荐顺序规则如下：

- 顺序不能靠文件系统枚举顺序
- 顺序不能靠名字字典序
- 顺序不能靠材质作者“默认猜测”
- 编译依赖和执行顺序要分开看
- 执行顺序应由基础 shader 或装配代码显式表达

### 两类顺序

推荐把顺序拆成两类：

1. 编译与装配顺序
2. 运行时执行顺序

编译与装配顺序用于解决：

- 哪些模块需要先 include
- 哪些模块依赖其他模块的公开接口
- 参数声明如何合并

运行时执行顺序用于解决：

- 先调用哪个模块函数
- 后调用哪个模块函数
- 某个模块函数的输出是否作为下一个模块函数的输入

这部分不应由模块系统自动猜，而应由基础 shader 或装配后的 shader 显式写出来。

## 显式依赖

推荐每个模块都可以声明依赖：

```json
{
  "name": "glintHighlight",
  "dependsOn": ["glintCommon"]
}
```

约束如下：

- `dependsOn` 表达的是编译与装配依赖，不直接表达运行时调用先后
- 不允许形成环
- 依赖关系冲突时直接报错

这点很像 C++：

- 用到了谁，就显式写出依赖
- 不允许靠“刚好先 include 了”来偷过

推荐规则：

- `dependsOn` 决定 include 和装配顺序
- 实际执行顺序由 shader 中的函数调用顺序决定
- 不要把 `dependsOn` 当成“自动执行次序”

## 装配模型

第一版更推荐“库式装配”而不是“hook 调度”。

模块提供：

- 参数定义
- 贴图定义
- 公开函数
- 私有实现

基础 shader 负责：

- 选择要使用哪些模块
- 决定 include 哪些模块
- 决定调用哪些模块函数
- 决定模块函数的执行顺序

装配器负责：

- 读取模块描述
- 校验模块依赖
- 合并公开参数和贴图
- 生成同目录 `generated/*.param`
- 生成稳定的模块 include 列表

装配器不负责：

- 自动决定模块函数的执行顺序
- 自动往主函数任意位置插入逻辑
- 自动修改 shader 主流程

## Include 规则

为了尽量接近 C++ 的使用体验，同时避免强位置关系，推荐支持两种 include 语义：

- `#include "..."` 
  - 用于相对路径 include
  - 适合同目录局部文件和 `generated/*.param`
- `#include <...>`
  - 用于模块逻辑 include
  - 适合通过模块名查找公开接口

推荐示例：

```glsl
#include "generated/pbr.param"
#include <glint/public>
#include <rimLight/public>
```

这里的语义是：

- `generated/pbr.param` 从当前 shader 所在目录相对查找
- `<glint/public>` 表示“从模块系统中查找 `glint` 的公开入口”
- `<rimLight/public>` 表示“从模块系统中查找 `rimLight` 的公开入口”

### 模块路径映射

推荐模块目录结构如下：

- `shader/glsl/modules/glint/glint.module.json`
- `shader/glsl/modules/glint/glint_public.glsl`
- `shader/glsl/modules/glint/glint_private.glsl`
- `shader/glsl/modules/rimLight/rimLight.module.json`
- `shader/glsl/modules/rimLight/rimLight_public.glsl`
- `shader/glsl/modules/rimLight/rimLight_private.glsl`

includer 或装配器需要维护一层逻辑映射：

- `<glint/public>` -> `shader/glsl/modules/glint/glint_public.glsl`
- `<rimLight/public>` -> `shader/glsl/modules/rimLight/rimLight_public.glsl`

这样 shader 依赖的是模块名，而不是模块的物理位置。

### Public 和 Private 的 include 约束

推荐约束如下：

- 外部 shader 允许 include `<moduleName/public>`
- 外部 shader 不允许直接 include `<moduleName/private>`
- 模块内部可以通过相对路径或内部规则引用自己的 `private`

这与 C++ 的常见使用习惯一致：

- 外部依赖公共接口
- 私有实现留在模块内部

## 命名与冲突

模块组合后最常见的问题是名字冲突。

推荐规则：

- 公开参数必须带模块前缀
- 公开贴图必须带模块前缀
- 公开函数入口必须带模块前缀

例如：

- `glintIntensity`
- `glintColor`
- `glintMask`
- `Glint_ApplyPostLighting()`

不建议使用：

- `color`
- `mask`
- `apply`

这样做类似 C++ 中使用命名空间，避免模块组合后接口互相覆盖。

## 推荐的数据结构

第一版可以先从如下结构开始：

```json
{
  "name": "glint",
  "dependsOn": [],
  "public": {
    "parameters": [
      { "name": "glintIntensity", "type": "float", "default": 1.0 },
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

基础材质装配配置则显式声明模块列表：

```json
{
  "baseShader": "pbr",
  "modules": [
    "glint",
    "rimLight"
  ]
}
```

生成器负责：

- 读取基础 shader
- 读取模块描述
- 校验模块依赖
- 对依赖关系做拓扑排序
- 合并 `public` 参数与贴图
- 生成同目录 `generated/*.param`
- 生成模块 include 顺序

### 调用方式示意

装配完成后，真正的调用顺序仍然由 shader 主体表达。例如：

```glsl
#include "generated/pbr.param"
#include <glint/public>
#include <rimLight/public>

void main()
{
    vec3 directLighting = EvaluateDirectLighting();
    vec3 indirectLighting = EvaluateIndirectLighting();
    vec3 color = directLighting + indirectLighting;
    color += Glint_Evaluate();
    color += RimLight_Evaluate();
    outColor = vec4(color, 1.0);
}
```

这里的重点是：

- `Glint_Evaluate()` 和 `RimLight_Evaluate()` 的调用顺序由 shader 明确写出
- 模块系统只负责让这些函数和参数可被使用
- 不由模块系统自动替你决定谁先谁后

## 哪些功能适合做模块

适合做模块的功能通常具有这些特征：

- 只追加参数和贴图
- 只在局部计算中生效
- 不改 pipeline state
- 不改 pass 结构

典型例子：

- 闪点
- 边缘光
- 流光
- 溶解遮罩

## 哪些功能不适合做模块

以下功能更适合作为基础 shader 类型或 variant：

- 透明与不透明切换
- alpha clip 对阴影路径的影响
- 双面渲染
- 改变深度写入策略
- 改变混合方式

这些功能不是简单的“局部材质增强”，而是会改变渲染结构。

## 第一版建议

第一版最值得坚持的约束是：

- 模块依赖必须显式声明
- `dependsOn` 只表达装配依赖
- 执行顺序由基础 shader 显式调用决定
- 运行时不理解模块，只理解最终反射结果

如果第一版把这些边界守住，后面再扩展 `interface`、更强装配规则和更复杂模块关系都会容易得多。

## 讨论重点

后续继续讨论时，建议优先确认以下问题：

- 模块 include 列表是全自动生成，还是允许主 shader 手写控制
- `dependsOn` 是否只允许模块级依赖，还是需要函数级依赖
- 基础 shader 是否需要单独的装配文件
- `public` 和 `private` 的边界是否还需要 `interface` 补层
- 模块是否只允许追加 `set 1` 资源
- 模块的调用模板是否需要统一约定

## 一句话结论

材质模块系统更适合沿用 C++ 的思路：

- 公共接口和私有实现分离
- 依赖关系显式书写
- 依赖顺序和执行顺序分开处理
- 基础 shader 显式拼装并调用模块函数

这样可以把模块做成“可组合的库式单元”，而不是“不可控的自动调度器”。
