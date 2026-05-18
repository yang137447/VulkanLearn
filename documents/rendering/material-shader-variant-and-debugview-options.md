# 材质 Shader 变体与 Debug View 方案整理

## 目标

本文档整理当前围绕同一 shader 的多材质实例宏开关、shader 变体管理、以及 Debug View 支持的几套可选方案，作为后续确认实际需求时的讨论基础。

重点讨论的问题是：

- 同一个 `shader` 是否允许不同 `MI_*.json` 走不同特性组合
- 某些特性关闭后，是否还必须继续声明和绑定对应资源
- Debug View 是更偏“全局调试模式”，还是“材质级 shader 变体”
- 当前工程基于 shader reflection 的材质校验链，如何与 variant 机制协作

本文档先做方案归纳，不直接给出最终实现结论。

## 当前现状

结合当前工程代码，现有材质链路具有几个重要特征：

- `Material` 基本以 `shaderName` 作为核心身份
- `SceneLoader` 会按 shader 反射结果校验材质参数和贴图
- 运行时 descriptor layout、参数顺序和贴图需求，当前都强依赖 SPIR-V 反射
- `MaterialInstance` 负责保存实例值，但不定义 shader 接口

这意味着当前系统天然更接近：

- 一个 `shaderName`
- 对应一套固定 shader 接口
- 对应一套固定 pipeline / descriptor layout 预期

因此，当前对“同一 shader，不同材质实例开启不同宏”的支持会比较脆弱。脆弱点主要不在于宏本身，而在于：

- 变体 identity 没有进入缓存键
- 反射结果默认只对应单一编译产物
- 资源校验逻辑默认假设同名 shader 的接口稳定

## 讨论维度

在继续定方案前，需要先把三类需求拆开看：

### 1. 材质特性开关

例如：

- `USE_NORMAL_MAP`
- `USE_EMISSIVE`
- `USE_CLEAR_COAT`
- `USE_ALPHA_CLIP`

这类需求通常会影响：

- shader 内部计算路径
- 是否需要某些参数
- 是否需要某些贴图
- 有时甚至会影响 pipeline state 或 pass 行为

### 2. 调试输出切换

例如：

- 输出 `baseColor`
- 输出 `normal`
- 输出 `metallic`
- 输出 `roughness`
- 输出 AO、UV、world position 等中间结果

这类需求更像“调试视图模式切换”，往往具有：

- 希望运行时即时切换
- 希望全局统一观察
- 不希望为每次切换都重新生成 shader 产物

### 3. Shader 接口稳定性

这是当前工程里最关键的现实约束。

如果某套方案会改变：

- descriptor set layout
- binding 是否存在
- uniform block 成员是否存在

那么它不仅仅是在改“shader 内部逻辑”，而是在改运行时接口。只要接口变了，`Material` 缓存、反射、descriptor layout、材质实例校验就都要一起考虑。

## 方案一：预编译宏变体

### 思路

让材质实例或材质描述显式声明宏集合，例如：

- `USE_NORMAL_MAP`
- `USE_EMISSIVE`
- `DEBUG_VIEW_NORMAL`

引擎把“shader 名 + 宏集合”视为一个独立 shader variant，并生成独立的 SPIR-V 与 pipeline。

这类方案本质上仍然使用 `#if / #ifdef / #define` 体系来裁剪代码。

### 适合解决的问题

- 某个特性关闭后，希望相关贴图和参数彻底不存在
- 不同材质实例确实需要不同 shader 接口
- 某些特性会显著改变着色路径，希望编译期完全裁剪
- 某些逻辑会影响阴影、前向/延迟、alpha clip 等分支结构

### 优点

- 自由度最高
- 可以真正裁掉未使用资源和代码路径
- 可以让“关闭特性后不再需要对应 binding”成为可能
- 与传统 keyword / permutation 系统最接近，表达能力最强

### 缺点

- 变体数量容易膨胀
- 需要把 variant identity 纳入缓存键
- 每种宏组合都需要独立反射、独立 pipeline
- 如果运行时按需编译，会引入卡顿或构建复杂度

### 对当前工程的含义

如果采用这条路，至少需要接受一个事实：

- `Material` 的身份不能再只是 `shaderName`

它更像应该升级为：

- `shaderName + variantKey`

其中 `variantKey` 至少要稳定表达：

- 宏列表
- 可能的阶段差异
- 可能影响 pipeline 的开关

同时，以下环节都要和 variant 对齐：

- SPIR-V 产物命名
- shader reflection 缓存
- pipeline 缓存
- `SceneLoader` 中的材质实例校验

### 风险

如果不控制宏来源，后面很容易出现：

- `MI_` 任意组合宏
- 调试宏、功能宏、平台宏混在一起
- 同一基础 shader 的变体不可预测

最终结果通常是：

- 构建缓存混乱
- 反射结果不稳定
- 材质实例配置难以维护

所以即使选这条路，也通常需要先把宏分层。

## 方案二：Specialization Constant

### 思路

把一部分“开关”从预处理宏改成 Vulkan 的 specialization constant。

这类常量在 GLSL / SPIR-V 层是稳定接口，在创建 pipeline 时由不同 material 或 pipeline variant 提供不同值。

### 适合解决的问题

- 同一份 SPIR-V 需要派生少量行为差异
- 希望减少“重新编译多份 SPIR-V”的压力
- 某些布尔或枚举开关只影响计算路径，不改变资源接口

### 优点

- SPIR-V 编译产物可以保持较少份数
- 运行时仍有机会让驱动做常量传播和死代码裁剪
- 比完全 runtime 分支更接近“静态变体”

### 缺点

- 不适合改变 shader 对外资源接口
- 不能指望它像宏一样把 descriptor binding 真正删掉
- 仍然需要把不同常量组合视为不同 pipeline identity
- 方案理解成本高于普通 uniform 分支

### 对当前工程的关键限制

这条路最需要明确的一点是：

- specialization constant 更适合改“逻辑”
- 不适合改“接口”

如果一个特性关闭后你希望：

- 不再要求法线贴图
- 不再要求 emissive 贴图
- 不再要求某个 UBO 成员存在

那么 specialization constant 往往不够彻底。因为对当前工程来说，真正决定资源校验的是反射结果，而反射面对的是那份“接口仍然存在”的 SPIR-V。

所以它更适合以下类型的开关：

- 某段计算是否启用
- 某个采样结果是否参与最终混合
- 某种 BRDF 路径是否启用，但相关资源本身始终存在

## 方案三：全局 Uniform / Push Constant 动态分支

### 思路

把某些开关做成运行时数据，例如：

- `debugViewMode`
- `visualizeMipLevel`
- `showShadowCascade`

shader 保持单一版本，运行时通过 uniform 或 push constant 在 shader 内部做分支或 `switch`。

### 适合解决的问题

- Debug View
- 实时诊断开关
- 希望立刻切换、不重新建 pipeline 的观察模式
- 全局一致的调试输出

### 优点

- 不增加 shader 变体数量
- 切换成本低
- 运行时体验最好
- 对当前材质反射与 descriptor 体系侵入最小

### 缺点

- 不能移除未使用资源接口
- shader 内仍然保留所有调试路径
- 如果把太多材质特性也塞进 runtime branch，shader 会逐渐失控

### 对当前工程的适配性

如果需求是：

- 观察 `baseColor`
- 观察法线
- 观察粗糙度
- 观察 metallic

那么这类“Debug View”本质上更像统一调试模式，而不是材质 authoring 能力。

在这个前提下，这条路和当前工程其实最自然，因为它：

- 不要求 `Material` 身份变化
- 不要求新 SPIR-V 产物
- 不要求反射接口变化
- 不要求 `MI_` 跟着切换资源声明

也就是说，它最适合作为“调试输出层”能力，而不是“材质功能层”能力。

## 方案四：混合策略

### 思路

把问题拆成两层，而不是试图用一套机制解决全部需求。

推荐拆分方式通常是：

- 结构性材质特性 -> 静态 variant
- 观察性调试模式 -> 动态分支

具体来说：

- 会改变资源接口、pass 行为、阴影路径、alpha clip 路径的能力，放进静态变体
- 只是为了观察中间结果的 Debug View，放进全局动态分支

### 优点

- 把最贵的静态变体数量控制在真正必要的范围
- 保留 Debug View 的实时切换体验
- 与当前工程的 shader reflection 机制更容易兼容

### 缺点

- 系统边界必须定义得很清楚
- 必须先约束什么属于“功能宏”，什么属于“调试模式”
- 文档和数据规范要跟上，否则后面很容易混回一锅

## 四套方案对比

| 维度 | 预编译宏变体 | Specialization Constant | 动态分支 | 混合策略 |
| --- | --- | --- | --- | --- |
| 是否能改变资源接口 | 强 | 弱 | 弱 | 按拆分结果决定 |
| 是否需要多份 SPIR-V | 通常需要 | 通常不需要 | 不需要 | 部分需要 |
| 是否需要多份 Pipeline | 需要 | 需要 | 通常不需要 | 部分需要 |
| 是否适合 Debug View 即时切换 | 不适合 | 一般 | 很适合 | 很适合 |
| 是否适合同 shader 多 MI 差异化贴图需求 | 很适合 | 一般 | 不适合 | 很适合 |
| 与当前反射校验体系兼容性 | 需要改造 | 中等 | 高 | 中高 |
| 主要风险 | 变体爆炸 | 接口认知混乱 | shader 逻辑膨胀 | 规则定义不清 |

## 当前工程下的现实判断

如果把问题放回当前工程上下文，可以先得到几个偏现实的判断：

### 1. 不是所有“宏”都应该进入 `MI_`

`MI_` 更适合承载实例值，而不是无限制承载 shader 结构开关。

原因是当前系统里，`MI_` 后面连着的是：

- shader reflection 校验
- 贴图需求检查
- 参数布局检查
- material / pipeline 复用

只要某个开关会改变 shader 接口，它就已经不是单纯“实例参数”了，而是在参与决定一个新的 `Material` 身份。

### 2. Debug View 和材质特性最好分开讨论

如果把 Debug View 也做成宏变体，会出现几个问题：

- 只是切换观察输出，却要重新创建 variant
- 只是调试模式，却污染材质 identity
- 调试宏和功能宏混在一起，缓存键会迅速失控

因此 Debug View 更适合看成：

- 全局渲染调试状态

而不是：

- 材质实例自身配置

### 3. 当前反射驱动校验决定了“接口变化”不能轻描淡写

当前工程的优势是：

- 运行时以最终 shader 反射结果为准

但它也意味着：

- 只要接口变了，就必须把 variant 当成真实的一等对象来管理

否则就会出现表面上“只是多几个宏”，实质上：

- 资源校验错位
- 贴图要求错位
- descriptor layout 错位
- pipeline 缓存命中错误

## 推荐的讨论顺序

在真正实现前，建议先依次确认以下问题：

1. 哪些能力会改变资源接口
2. 哪些能力只是调试观察模式
3. 哪些能力需要做到“同一 shader、不同 MI 使用不同贴图集合”
4. 哪些能力会影响 shadow pass、blend、depth、alpha clip 等渲染结构
5. `MI_` 是否真的应该直接声明宏，还是应该引用一个更高层的 variant 描述

如果这些问题不先分清，后面讨论“用哪种技术”会很容易混在一起。

## 建议先确认的需求清单

后续对实际需求时，建议优先确认下面这些点：

- 你希望 `MI_` 控制的是“实例值”，还是“shader 结构变体”
- 哪些宏关闭后，需要真正不再声明对应纹理和参数
- Debug View 是全局模式，还是允许某个材质单独进入 debug shader
- 是否允许同一个基础 shader 同时衍生出很多资源接口不同的变体
- 是否接受首次遇到新变体时的编译/加载成本
- 是否希望把“功能宏”和“调试宏”分成两套完全不同的系统

## 当前阶段的一句话结论

就当前工程的材质反射链路来看，更像应该优先接受以下判断：

- 会改变 shader 接口的能力，不能只当成普通 `MI_` 参数
- Debug View 不适合直接混入材质宏系统
- 最现实的方向通常不是单选题，而是“静态 variant + 动态 debug view”的混合策略

但最终是否这样落地，仍然取决于我们接下来确认的实际需求边界。

## 问答收敛后的真实需求表

在完成一轮针对实际使用场景的问答后，当前已经明确的需求边界如下。

这一节不再讨论“哪种技术更优”，而是先固定：

- 哪些能力属于基础材质类型
- 哪些能力属于结构性 feature
- 哪些数据属于材质实例
- Debug View 第一版到底要覆盖到什么深度

### 1. 基础材质类型

当前先按两大类基础材质类型思考：

- `Opaque`
- `Transparent`

其中 `Transparent` 不再被当作普通小开关，而是明确视为独立的渲染路径语义。

原因是透明与不透明通常会连带影响：

- blend state
- depth write 行为
- 渲染顺序
- 调试输出口径
- 阴影参与规则

### 2. Transparent 第一版范围

`Transparent` 第一版明确包含两种模式：

- `AlphaBlend`
- `Additive`

当前需求约束如下：

- 两种透明模式第一版都要支持
- 透明度来源统一先复用 `baseColor.a`
- 透明物体也需要进入 Debug View 体系
- Debug View 里既要能看混合前结果，也要考虑最终混合后结果

透明与阴影的关系当前确定为：

- `AlphaBlend`
  - 第一版参与阴影
- `Additive`
  - 第一版不参与阴影

这意味着透明体系后续大概率不仅需要“透明/不透明”区分，还需要“透明模式”区分。

### 3. Feature 集与材质层职责

当前已经确认，结构性特性允许由材质层选择，但不建议直接在 `MI_` 中堆原始宏。

更符合当前需求的分层是：

- 材质层可以选择 feature
- `MI_` 通过引用独立的 feature 集来声明结构性特性
- `MI_` 自身主要保存实例参数值与贴图值

一句话概括：

- feature 对材质作者可见
- variant 身份由引擎稳定管理
- `MI_` 不是直接的宏堆叠容器

### 4. 当前已确认的结构性 Feature

#### `normal map`

- 材质层可选
- 关闭后移除接口
- 不影响 shadow pass

这说明 `normal map` 的结构性主要体现在：

- 是否需要法线贴图资源接口
- 是否启用对应着色路径

它不参与定义阴影几何轮廓。

#### `emissive`

- 材质层可选
- 关闭后移除接口

当前对 `emissive` 的 Debug View 需求是：

- 第一版至少作为输入项可观察

后续如果需要，也可以再扩展“最终发光贡献”的调试口径。

#### `alpha clip`

`alpha clip` 当前已经有非常明确的边界：

- 材质层可选
- 复用 `baseColor.a`
- `threshold` 属于材质层可调参数
- 主 pass 与 shadow pass 必须一致裁剪

这意味着：

- `alpha clip` 不是普通运行时布尔值
- 它会参与定义物体的最终可见轮廓
- 它必须同步影响阴影轮廓

因此它应被视为 `Opaque` 家族中的结构性 feature，而不是普通参数开关。

#### `double sided`

`double sided` 当前也已经明确：

- 材质层可选
- 主 pass 与 shadow pass 都必须参与
- 目标是保证投影与模型一致

这类需求在底层通常会影响：

- `cull mode`
- pipeline state
- shadow 相关行为

因此它也应进入结构性 feature 层，而不是仅作为 shader 内普通布尔值存在。

### 5. 材质实例 `MI_` 的职责

当前需求下，`MI_` 更适合承载以下内容：

- 参数值
- 贴图值
- `alphaClipThreshold`
- 对某个 feature 集的引用

当前不建议让 `MI_` 直接承载：

- 原始 shader 宏列表
- 调试输出模式
- 不受约束的结构性接口开关

原因是这会重新把“实例值”和“材质身份”混到一起。

### 6. Debug View 的定位

Debug View 当前已经明确为：

- 全局调试模式
- 仅在 Debug 版通过整体编译开关启用
- 第一版采用全局枚举模式
- 第一版先不做“仅选中物体”的过滤能力
- 保留一个正常最终结果模式，便于和调试输出对照

也就是说，Debug View 属于：

- 渲染器级调试能力

而不是：

- 材质实例自身配置

### 7. Debug View 的组织方式

第一版推荐的组织方式已经比较明确：

- 顶层按结果分组
- 组内按着色流程顺序排序

当前约定的主要分组方向是：

- 最终结果
- 输入项
- 直接光
- 间接光
- BRDF 中间项

### 8. Debug View 第一版清单

#### 最终结果

- `Final`

#### 输入项

- `BaseColor`
- `Alpha`
- `Normal`
- `Roughness`
- `Metallic`
- `AO`
- `Emissive`

说明：

- 当前输入项先采用“基础输入组”
- 暂未把 `UV`、`world position`、`depth` 等基础诊断项强制纳入第一版最小清单

#### 直接光

第一版至少覆盖：

- `Direct Total`
- `Direct Diffuse`
- `Direct Specular`

并继续细分到 BRDF 中间项：

- `D`
- `G`
- `F`
- `NoL`

这说明直接光调试不是只看一个总结果，而要能继续拆到着色中间量。

#### 间接光

第一版至少覆盖：

- `Indirect Total`
- `Indirect Diffuse`
- `Indirect Specular`

并继续细分到来源项：

- `Irradiance`
- `Prefilter`
- `BRDF LUT`
- `AO Contribution`

这说明间接光调试也不是只看最终合成结果，而要能区分环境来源。

### 9. 当前可以直接作为后续设计输入的结论

截至当前讨论，可以先把以下内容视为下一阶段设计的输入条件：

- `Transparent` 是独立基础材质类型
- `Transparent` 第一版包含 `AlphaBlend` 与 `Additive`
- `AlphaBlend` 第一版参与阴影，`Additive` 第一版不参与阴影
- `normal map`、`emissive`、`alpha clip`、`double sided` 属于材质层可选的结构性 feature
- `alpha clip` 使用 `baseColor.a + threshold`，且主 pass 与 shadow pass 必须一致裁剪
- `double sided` 主 pass 与 shadow pass 都必须参与
- `MI_` 直接声明 `renderStates`、`artMacros`、`parameters` 与 `textures`
- Debug View 是 Debug 版整体启用的全局枚举调试系统
- Debug View 第一版覆盖输入项、直接光、间接光、BRDF 中间项与最终结果

### 10. 当前仍保留的少量待定项

目前仍可留到下一轮设计时再定的内容主要有：

- `double sided` 与 `Transparent` 组合时是否需要额外限制
- `AlphaBlend` 参与阴影时，具体采用何种阴影近似口径
- `Emissive` 是否在后续加入“最终贡献”观察模式
- `UV`、`depth`、`world position` 等基础诊断项是否并入第一版 Debug View

这些问题已经不再影响当前的总方向判断，可以留到后续结构设计阶段继续细化。

## 建议数据结构

在进一步讨论后，当前更推荐的 `MI_` 数据分层不是“feature 集引用 + 实例值”，而是直接在 `MI_` 中拆成四类字段：

- `renderStates`
- `artMacros`
- `parameters`
- `textures`

这套划分的目标是：

- 用固定字段承载渲染结构语义
- 用开放的美术宏承载 shader 功能组合
- 用参数与贴图承载实例值
- 同时避免把所有能力都退化成随意拼接的字符串宏

### 1. 顶层结构示例

推荐的 `MI_` 结构示例如下：

```json
{
  "shader": "pbr",
  "renderStates": {
    "renderMode": "OpaqueClip",
    "cullMode": "None"
  },
  "artMacros": [
    "USE_NORMAL_MAP",
    "USE_EMISSIVE"
  ],
  "parameters": {
    "alphaClipThreshold": 0.45,
    "emissiveStrength": 1.0
  },
  "textures": {
    "baseColor": "leaf_albedo.png",
    "normal": "leaf_normal.png",
    "emissive": "leaf_emissive.png"
  }
}
```

### 2. 四类字段的职责

#### `renderStates`

`renderStates` 用来描述引擎必须理解的渲染结构状态。

它们的特点是：

- 语义稳定
- 会影响渲染路径
- 会影响 pipeline state
- 会影响 shadow 行为
- 会进入 variant identity

当前第一版建议最小集合为：

- `renderMode`
- `cullMode`

推荐取值如下：

- `renderMode`
  - `Opaque`
  - `OpaqueClip`
  - `TransparentAlphaBlend`
  - `TransparentAdditive`
- `cullMode`
  - `Back`
  - `None`

这套命名对应当前已确认的需求关系：

- `TransparentAlphaBlend`
  - 表达透明路径与标准透明混合
- `TransparentAdditive`
  - 表达透明路径与加法混合
- `alpha clip`
  - 由 `renderMode = OpaqueClip` 表达
- `double sided`
  - 由 `cullMode = None` 表达

这样做的好处是：

- 结构性语义不再依赖松散的原始宏字符串
- `Opaque` 和 `OpaqueClip` 被明确区分，便于和 `Early-Z`、shadow 裁剪要求对应
- 不再需要让材质作者自己组合 `materialType + blendMode + alphaMode`

#### `artMacros`

`artMacros` 用来承载美术侧希望灵活开关的 shader 功能宏。

它们的特点是：

- 主要影响 shader 内部功能组合
- 需要保留配置灵活性
- 仍然参与 variant key
- 但不直接表达基础渲染路径语义

这类宏当前不再建议由引擎预先做白名单收口，而是直接开放给用户自行配置。

典型示例如下：

- `USE_NORMAL_MAP`
- `USE_EMISSIVE`
- `USE_CLEAR_COAT`
- `USE_RIM_LIGHT`
- `USE_GLINT`

这里的关键约束是：

- `artMacros` 可以由材质直接写
- 引擎不预先解释其业务语义
- 但仍建议在进入缓存键前做去重、排序和空字符串过滤

#### `parameters`

`parameters` 保存材质实例的数值型输入。

例如：

- `alphaClipThreshold`
- `emissiveStrength`

它们的特点是：

- 表达实例值
- 不直接决定渲染路径语义
- 是否必须存在，由 `renderStates + artMacros` 共同决定

#### `textures`

`textures` 保存材质实例实际引用的贴图。

例如：

- `baseColor`
- `normal`
- `emissive`

是否要求某张贴图必须存在，不再单独由材质作者主观决定，而应由：

- `renderStates`
- `artMacros`
- 最终 shader 反射结果

共同收敛得出。

### 3. 为什么不再单独引入 feature 集

当前讨论后，更推荐把结构性 feature 直接写在 `MI_` 里，而不是额外维护一层 feature 集资产。

原因主要有：

- 如果 feature 集只是包一层开关，容易变成多一层壳但没有明显收益
- 当前阶段更需要灵活探索，而不是先把资产层固定得太重
- 直接写在 `MI_` 中，更符合“材质就是完整声明”的使用直觉

但这里的“直接写”并不是：

- 直接堆任意宏字符串

而是：

- 结构语义写入 `renderStates`
- 功能宏写入 `artMacros`

### 4. 非法组合规则

第一版建议在材质加载阶段显式校验非法组合，而不是把问题留到运行时渲染后再暴露。

推荐先从以下规则开始：

- `renderMode = Opaque`
  - 不启用 alpha clip
- `renderMode = OpaqueClip`
  - 必须存在 `alphaClipThreshold`
  - 必须存在 `baseColor`
- `renderMode = TransparentAlphaBlend`
  - 第一版允许参与阴影
- `renderMode = TransparentAdditive`
  - 第一版不参与阴影
- 存在 `USE_NORMAL_MAP`
  - 必须存在 `normal`
- 存在 `USE_EMISSIVE`
  - 必须存在 `emissive` 相关输入
- `artMacros`
  - 进入缓存键前建议做去重、排序和空字符串过滤

这些规则的目标不是一次性穷举所有情况，而是优先把当前已经确定的结构性约束固化下来。

### 5. Variant Key 的建议构成

当前更推荐把 variant identity 拆成两部分理解：

结构部分：

- `shader`
- `renderStates.renderMode`
- `renderStates.cullMode`

功能部分：

- 排序后的 `artMacros`

合并后可以形成稳定的 variant key，例如：

```text
pbr|OpaqueClip|None|USE_EMISSIVE,USE_NORMAL_MAP
```

这样做的好处是：

- 结构性差异一眼可见
- 美术功能组合也保留进缓存身份
- 更容易做调试和错误排查

### 6. `renderMode` 到运行时行为的映射

第一版更推荐把 `renderMode` 看成“渲染路径身份”，而不是单纯的材质小开关。

当前建议的映射如下：

| `renderMode` | Shader 变体 | Pipeline 状态 | Shadow 行为 | 备注 |
| --- | --- | --- | --- | --- |
| `Opaque` | 进入 `shaderVariantKey`，shader 路径最简 | `blend=off` `depthWrite=on` `depthTest=on` | 参与普通阴影 | 最标准路径，`Early-Z` 最友好 |
| `OpaqueClip` | 明确需要独立 shader 路径 | `blend=off` `depthWrite=on` `depthTest=on` | 主 pass 与 shadow pass 都做同样 clip | 使用 `baseColor.a + alphaClipThreshold`，必须与普通 `Opaque` 区分 |
| `TransparentAlphaBlend` | 进入 `shaderVariantKey`，shader 是否独立可按实现决定 | `blend=alpha` `depthWrite=off` `depthTest=on` | 第一版参与阴影 | 透明队列，需要排序 |
| `TransparentAdditive` | 进入 `shaderVariantKey`，shader 是否独立可按实现决定 | `blend=additive` `depthWrite=off` `depthTest=on` | 第一版不参与阴影 | 透明队列，需要排序 |

这张表表达的核心原则是：

- 所有 `renderMode` 都进入最终材质身份判断
- 但不要求所有 `renderMode` 都生成完全不同的 shader 文件
- 第一版先把路径身份收清楚，再按真实需要决定哪些模式共享实现

#### 为什么 `OpaqueClip` 必须单独区分

`OpaqueClip` 虽然仍属于 opaque 家族，但它与普通 `Opaque` 的差异已经足以形成独立路径：

- 会引入基于 `baseColor.a + alphaClipThreshold` 的裁剪逻辑
- 会影响 `Early-Z` 的预期和片元执行行为
- 会要求主 pass 与 shadow pass 使用同一套裁剪规则

因此它不能简单视为“普通 Opaque 再加一个参数”。

#### 为什么 `TransparentAlphaBlend` 与 `TransparentAdditive` 也要区分

这两类透明模式虽然很多时候 shader 代码主体接近，但它们的运行时语义已经不同：

- blend 状态不同
- 阴影参与规则不同
- 调试观察口径不同

因此第一版更适合直接把它们视为两个离散的 `renderMode`。

#### `cullMode` 的角色

当前更推荐让 `cullMode` 继续只表达 pipeline 层的光栅化状态：

- `Back`
- `None`

它对应当前讨论中的：

- 普通单面
- 双面

并且主 pass 与 shadow pass 都应同步使用相同的 `cullMode`，以满足“投影与模型一致”的要求。

#### 建议的 Key 拆分

当前更推荐把材质身份继续拆成两层：

`shaderVariantKey`

- `shader`
- `renderStates.renderMode`
- 规范化后的 `artMacros`

`pipelineVariantKey`

- `renderPass`
- `shaderVariantKey`
- `renderStates.cullMode`
- `sampleCount`
- depth / blend 等 pipeline 状态

这样做的好处是：

- `renderMode` 负责渲染路径身份
- `artMacros` 负责 shader 功能组合
- `cullMode` 与其他 Vulkan 状态继续归 pipeline 侧管理

#### 变体哈希与映射表

当前更推荐使用：

- `shaderVariantHash`
- 对应的变体映射表

来管理最终的 shader 编译产物，而不是直接把可读变体串完整拼进 `.spv` 文件名。

推荐原因：

- 文件名更短，避免宏组合增长后路径失控
- 更适合 Windows 环境下的路径长度约束
- 后续 `artMacros` 增多时，不需要反复调整产物命名规则
- 调试时仍可通过映射表反查实际变体语义

这里的关键边界是：

- 哈希只对应 `shaderVariantKey`
- 不混入 `pipelineVariantKey` 的 Vulkan 状态

也就是说，哈希输入只应包含：

- `shader`
- `renderStates.renderMode`
- 规范化后的 `artMacros`

不应包含例如：

- `renderPass`
- `sampleCount`
- `cullMode`
- depth / blend 等 pipeline 状态

因为这些内容决定的是 pipeline identity，而不是 shader 编译产物 identity。

#### 规范化输入

在计算 `shaderVariantHash` 之前，建议先把 `shaderVariantKey` 规范化为稳定字符串。

推荐格式例如：

```text
shader=pbr|renderMode=OpaqueClip|artMacros=USE_EMISSIVE,USE_NORMAL_MAP
```

其中 `artMacros` 需先做：

- 空字符串过滤
- 去重
- 排序

这样可以确保同一个变体不会因为输入顺序不同而得到不同哈希。

#### 产物命名建议

当前更推荐按 `shader` 分目录，再用哈希命名具体变体，例如：

```text
shader/spv/pbr/A13F29C4.vert.spv
shader/spv/pbr/A13F29C4.frag.spv
```

这种做法相比“全量可读字符串文件名”更稳：

- 可读性仍保留在目录层
- 具体变体由哈希保证简短稳定
- 适合后续持续扩展

#### 映射表建议

建议同时维护一份“哈希 -> 变体描述”的映射表，用于调试、排错和构建可视化。

推荐示例如下：

```json
{
  "A13F29C4": {
    "normalizedKey": "shader=pbr|renderMode=OpaqueClip|artMacros=USE_EMISSIVE,USE_NORMAL_MAP",
    "shader": "pbr",
    "renderMode": "OpaqueClip",
    "artMacros": [
      "USE_EMISSIVE",
      "USE_NORMAL_MAP"
    ]
  },
  "C82D9E11": {
    "normalizedKey": "shader=pbr|renderMode=TransparentAdditive|artMacros=USE_EMISSIVE",
    "shader": "pbr",
    "renderMode": "TransparentAdditive",
    "artMacros": [
      "USE_EMISSIVE"
    ]
  }
}
```

这张表的主要用途是：

- 反查某个 `.spv` 对应的真实变体
- 排查 shader 反射结果为何与预期不一致
- 在运行时或离线构建日志中输出人类可读的变体信息

#### 当前阶段的建议

当前阶段更推荐：

- `shaderVariantHash` 只对应 shader 编译产物
- `pipelineVariantKey` 继续建立在 `shaderVariantHash` 之上
- `.spv` 文件用哈希命名
- 调试和排错依赖映射表提供可读语义

这样可以把“文件命名稳定性”和“调试可读性”同时保留下来。

#### 第一版的直接建议

第一版可以直接把以下 4 条路径视为固定集合：

- `Opaque`
- `OpaqueClip`
- `TransparentAlphaBlend`
- `TransparentAdditive`

其中：

- `Opaque` 与 `OpaqueClip` 必须明确区分
- `TransparentAlphaBlend` 与 `TransparentAdditive` 必须明确区分
- `cullMode` 继续单独作为 pipeline 状态

这能把路径语义、shader 功能组合和 Vulkan 状态三者的边界先稳定下来。

### 7. 当前阶段的一句话建议

在当前项目阶段，更平衡的一版方案是：

- 用 `renderStates` 封装稳定的渲染结构语义
- 用 `artMacros` 保留美术功能组合的灵活性
- 用 `parameters` 与 `textures` 承载实例值
- 由引擎根据这四类信息共同计算最终 variant identity

这比“全都做成 enum”更灵活，也比“全都退化成自由宏字符串”更可控。

## 第一阶段实施方案

这一阶段的目标不是一次性改完整个材质系统，而是先把“身份层”从当前的 `shaderName` 单键模型中拆出来。

一句话概括：

- 先打通 `Shader Variant != Pipeline Variant`

### 阶段目标

第一阶段需要正式落地以下概念：

- `RenderMode`
- `ShaderVariantKey`
- `ShaderVariantHash`
- `PipelineVariantKey`

同时让当前代码链路具备以下能力：

- `MI` 可以最小读取 `renderStates + artMacros`
- shader 编译产物可以按变体 hash 命名
- pipeline 缓存不再只靠 `shaderName`
- `Material` 不再只是某个基础 shaderName 的壳

### 阶段范围

第一阶段只做“结构落地”和“编译链打底”，不追求一次性做满全部能力。

本阶段要做：

- 定义变体相关数据结构
- 打通 shader variant 编译和 hash 命名
- 升级 pipeline 缓存键
- 让 `SceneLoader` 最小接入新字段
- 用 1 到 2 个材质做试点验证

本阶段不做：

- 不做全量 `MI` 资产迁移
- 不做 `artMacros` 语义白名单或高级校验
- 不做完整 Debug View 接入
- 不做透明阴影的高级策略细化
- 不做复杂工具链 UI

### 实施主线

第一阶段建议始终围绕三条主线推进：

1. 把 shader 身份从 `shaderName` 升级到 `ShaderVariantKey`
2. 把编译产物从“按 shader 文件名输出”升级到“按 variant hash 输出”
3. 把 pipeline 复用从“按 shaderName + state”升级到“按 shaderVariant + state”

### 关键结构

#### `RenderMode`

第一阶段固定为 4 条路径：

- `Opaque`
- `OpaqueClip`
- `TransparentAlphaBlend`
- `TransparentAdditive`

它们负责表达渲染路径语义，而不是单纯材质小开关。

#### `ShaderVariantKey`

第一阶段建议至少包含：

- `shaderName`
- `renderMode`
- 规范化后的 `artMacros`

它只承载会影响 shader 编译产物身份的内容。

#### `ShaderVariantHash`

由 `ShaderVariantKey` 的规范化字符串生成，例如：

```text
shader=pbr|renderMode=OpaqueClip|artMacros=USE_EMISSIVE,USE_NORMAL_MAP
```

这个 hash 只用于 shader 编译产物，不混入 pipeline 状态。

#### `PipelineVariantKey`

第一阶段建议至少包含：

- `renderPass`
- `shaderVariantHash` 或 `ShaderVariantKey`
- `cullMode`
- `sampleCount`
- depth / blend 等 pipeline 状态
- `bIsShadowPass`

它只承载 Vulkan pipeline 复用身份。

### 文件落点

第一阶段建议尽量少打散现有结构，直接围绕下面这些文件推进：

- [shaderCompiler.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/shaderCompiler.h)
- [shaderCompiler.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/shaderCompiler.cpp)
- [pipelineFactory.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/pipeline/pipelineFactory.h)
- [pipelineFactory.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/pipeline/pipelineFactory.cpp)
- [material.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/material.h)
- [material.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/material.cpp)
- [sceneLoader.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/sceneLoader.cpp)

### 具体改动清单

#### 1. 定义基础类型

目标：

- 先让代码里正式出现 `RenderMode` 和 `ShaderVariantKey`

建议位置：

- [pipelineFactory.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/pipeline/pipelineFactory.h)

建议新增：

- `enum class RenderMode`
- `struct ShaderVariantKey`
- `struct ShaderVariantKeyHash`

阶段结果：

- 代码层不再只有 `shaderName` 这一种 shader 身份表达

#### 2. 升级 pipeline 缓存键

目标：

- 让 pipeline 缓存不再只按 `shaderName` 区分

涉及文件：

- [pipelineFactory.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/pipeline/pipelineFactory.h)
- [pipelineFactory.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/pipeline/pipelineFactory.cpp)

建议改动：

- 把现有 `GraphicsPipelineKey` 中的 `shaderName`
- 升级为 `ShaderVariantKey`

同步修改：

- `operator==`
- hash 逻辑
- `CreateGraphicsPipeline(...)` 的入参和构造流程

阶段结果：

- pipeline 缓存从“按 shader 名 + 状态”
- 升级为“按 shader 变体 + 状态”

#### 3. 升级 Material 身份

目标：

- 让 `Material` 绑定的是 shader 变体，而不是裸 `shaderName`

涉及文件：

- [material.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/material.h)
- [material.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/material.cpp)

建议改动：

- `Material` 构造函数接收 `ShaderVariantKey`
- 内部保存 `ShaderVariantKey`
- 保留便捷接口返回基础 `shaderName`，避免短期影响过大

阶段结果：

- `Material` 变成“某个 shader 变体的材质”

#### 4. 扩展 ShaderCompiler

目标：

- 让编译链真正支持“带变体信息编译”

涉及文件：

- [shaderCompiler.h](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/shaderCompiler.h)
- [shaderCompiler.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/shaderCompiler.cpp)

建议新增：

- `ShaderCompileRequest`
- `ShaderVariantCompileResult`
- `NormalizeArtMacros()`
- `BuildNormalizedKeyString()`
- `BuildRenderModeMacros()`
- `BuildShaderVariantHash()`

第一阶段至少做到：

- 支持带宏编译
- 支持 hash 命名
- 支持 manifest 写入

阶段结果：

- 编译链具备真正的 shader variant 产物能力

#### 5. 规范 `renderMode` 到编译宏的映射

目标：

- 不让材质作者手写路径宏

建议固定映射：

- `Opaque -> RENDER_MODE_OPAQUE`
- `OpaqueClip -> RENDER_MODE_OPAQUE_CLIP`
- `TransparentAlphaBlend -> RENDER_MODE_TRANSPARENT_ALPHA_BLEND`
- `TransparentAdditive -> RENDER_MODE_TRANSPARENT_ADDITIVE`

阶段结果：

- `renderMode` 正式进入 shader 编译语义

#### 6. 打通 hash 产物与 manifest

目标：

- 让 shader 产物不再只依赖基础 shaderName 文件名

建议输出路径：

```text
shader/spv/<shaderName>/<hash>.vert.spv
shader/spv/<shaderName>/<hash>.frag.spv
```

建议同时维护：

- `shader/spv/variants.json`

manifest 至少记录：

- `hash`
- `normalizedKey`
- `shader`
- `renderMode`
- `artMacros`

阶段结果：

- shader 产物管理从“基础 shader 文件输出”
- 升级为“按 shader variant hash 输出”

#### 7. SceneLoader 最小接入

目标：

- 让新 schema 能开始进入运行时

涉及文件：

- [sceneLoader.cpp](file:///d:/YYBWorkSpace/GitHub/VulkanLearn/source/sceneLoader.cpp)

第一阶段只做最小接入：

- 允许读取 `renderStates.renderMode`
- 允许读取 `renderStates.cullMode`
- 允许读取 `artMacros`
- 缺失字段时走默认值

推荐默认值：

- `renderMode = Opaque`
- `cullMode = Back`
- `artMacros = []`

阶段结果：

- 老资产先不崩
- 新资产可以开始试点

#### 8. 建一个试点材质链

目标：

- 不全量迁移，只验证新链路是否成立

推荐试点：

- 一个 `OpaqueClip`
- 一个 `TransparentAlphaBlend`

要验证的点：

- `renderMode` 变化是否命中不同 `shaderVariantKey`
- `cullMode` 变化是否只影响 pipeline
- 同一 `shaderVariantHash` 是否能派生多个不同 pipeline
- 旧资产在未迁移时是否仍能走默认值链路

### 推荐执行顺序

建议按以下顺序推进：

1. 改 `pipelineFactory.h/.cpp`
2. 改 `material.h/.cpp`
3. 改 `shaderCompiler.h/.cpp`
4. 改 `sceneLoader.cpp`
5. 做试点材质验证

这个顺序的好处是：

- 先把身份和缓存层打稳
- 再接编译产物
- 最后让 JSON 入口进入新模型

### 第一阶段完成标准

这一阶段完成后，至少应满足：

- 代码里正式存在 `RenderMode`
- 代码里正式存在 `ShaderVariantKey`
- `PipelineFactory` 已不再只靠 `shaderName`
- `ShaderCompiler` 已能按变体生成 hash 命名产物
- manifest 已能记录 hash 对应的变体描述
- `SceneLoader` 已能最小读取 `renderStates + artMacros`
- 至少 1 个试点材质能跑通新链路

### 第一阶段最重要的验收问题

推进时最值得反复确认的是：

- `renderMode` 改变时，是否真的命中不同 `shaderVariantKey`
- `cullMode` 改变时，是否不触发 shader 重编译
- 同一个 `shaderVariantHash` 是否能派生多个不同 pipeline
- `OpaqueClip` 是否真的与普通 `Opaque` 区分开
- 反射校验链是否仍以后续 SPIR-V 为真相源

### 当前阶段的一句话收口

第一阶段的本质不是“上完整材质系统”，而是：

- 先把 `shaderName` 单键时代拆掉
- 让 shader 身份、编译产物、pipeline 身份三层关系在代码里正式成立
