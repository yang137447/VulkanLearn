# SpeedTree 与植被算法知识库迁移方案

> 状态：已于 2026-07-24 在知识库仓库中完成落地与验证。
> 研究基线：SpeedTree Modeler / Runtime SDK 10.2 语义。Oak 资产文件报告的格式版本可能是 10.0，不能把资产版本、SDK 版本和 UE SpeedTree 9 混为一谈。

## 目标

把树木研究中有长期复用价值的算法，与 SpeedTree 10.2 的格式和实现事实分开。知识库固定使用四个顶层归属：

~~~text
rendering-math
program/algorithms
rendering/vegetation
rendering/speedtree-final-quality
~~~

不再新增第五个顶层分类。SpeedTree 专题可以引用前三组的通用页面，但通用页面不能依赖某个 SpeedTree 字段才能成立。

## 落地结果

以下路径均相对于 `D:\YYBWorkSpace\GitHub\yyb-knowledge-book`：

- `src/content/docs/rendering-math/` 已补齐方向量化、离散打包与数值量化、曲线采样、程序噪声与相位页面。
- `src/content/docs/program/algorithms/` 保留基础小公式；既有 LUT 页面迁入 `rendering-math`，旧 URL 保留兼容入口。运行时专题与实验台已在后续审计中删除。
- `src/content/docs/rendering/vegetation/` 已保留与 Oak 还原相关的风、叶片表示与渲染一致性页面；程序化建树与后续质量扩展已在审计中移除。
- `src/content/docs/rendering/performance/` 已新增游戏常用 LOD 过渡方案；原 vegetation 下旧 URL 保留重定向。
- 16 个算法保留为独立知识页（Billboard 独立专题与 Main/Shadow 一致性专题已在后续审计中移除）；每页包含核心公式、公式解析和只呈现该算法的交互示例，不向读者暴露跨算法切换器。
- 两个 Canvas 实验台仅作为内部实现复用，内部保留 8 个数学和 11 个植被/LOD 模式；其中 16 个独立页面通过固定 `mode` 只打开对应算法，路由为 `/interactive/math-algorithm-lab/` 和 `/interactive/vegetation-algorithm-lab/`。等距一维 LUT 沿用专属的 `/interactive/piecewise-linear-lut/` 实验页。
- `rendering/speedtree-final-quality/` 的专题页面只链接相关数学或植被页；版本、格式、byte offset 和 SDK 状态仍保留在 SpeedTree 专题中。
- 已通过静态构建；保留页面继续按审计清单逐页验收。

## 四组边界

### 1. rendering-math：通用数学与编码

这里记录与树木无关的基础算法；SpeedTree 只能作为案例。

| 算法 | 核心内容 | SpeedTree 案例 |
| --- | --- | --- |
| 球面方向量化 | Fibonacci sphere、golden angle、最大点积匹配 | 256 个方向索引、UNORM8 |
| 离散数据打包 | mixed-radix 编码/解码、范围分配、量化误差 | 9 x 9 x 3 offset 打进一个 byte |
| UNORM / SNORM 归一化整数编码 | 固定区间、端点规则、等距步长、round-trip 误差 | 顶点 wind、权重、TBN 分量 |
| binary16 半精度浮点 | sign/exponent/fraction、ULP、非均匀误差 | half position、UV、局部参数 |
| 等距一维 LUT | 均匀采样、区间索引、分段线性插值 | Runtime SDK 10.2 的 20 点响应曲线 |
| 非等距曲线采样 | 节点搜索、边界策略、插值 | 作为曲线模型对照 |
| 程序噪声 | hash/value noise、平滑插值、通道统计 | 风噪声 fallback |
| 相位去相关 | offset、seed、实例位置 | branch noise offset、instance independence |

### 2. program/algorithms：通用运行时与工程算法

本次迁移不保留独立运行时算法页。相关公式、状态约束和测量方法必须在实际拥有对象、数据和验证目标的领域页面中说明。

### 3. rendering/vegetation：植被领域通用算法

这里记录本次 Oak / Runtime SDK 还原需要的植被算法与渲染约束。

#### 运行时风与结构变形

| 算法 | 说明 |
| --- | --- |
| 分层风变形 | 整体、粗枝、细枝、叶片使用不同频段和空间频率 |
| 保长弯曲 | influence direction/weight 推导 anchor，变形后归一化 |
| 枝条受风的横向分量 | 从环境风中取出绕分叉点产生弯曲力矩的横向分量 |
| 多尺度噪声 | 冠层、枝条、叶片 ripple 的叠加 |

#### 叶片、LOD 与远景表示

| 算法 | 说明 |
| --- | --- |
| Alpha coverage mip | 维持固定 cutoff 下的叶片覆盖率 |
| Alpha clip / A2C / dither | 处理薄片轮廓和亚像素叶片 |
| 风动 Velocity | current/previous 两次完整变形 |

> LOD 过渡方案已迁至 `rendering/performance/lod-transition-schemes.mdx`：硬切换/迟滞、逐顶点 Morph、Dither Cross-fade、Billboard/Impostor 与组合管线不再依赖植被成立；Oak 的 `lodPosition` 仅作为 Morph 的资产案例保留。

> Main/Shadow Coverage 一致性独立专题已删除；跨 Pass 共享变形、UV、mip 与 cutoff 的约束保留在 SpeedTree 材质与光照专题。

### 4. rendering/speedtree-final-quality：SpeedTree 专用事实

这里保留版本、文件布局、精确参数和官方实现顺序，不改写成行业通用标准：

- Runtime SDK 10.2 的 19 条曲线分组与每条 20 个样本。
- Shared、Branch 1、Branch 2、Ripple 的具体曲线名称和消费关系。
- Standard.lua 的精确 stride、byte offset 和 UNORM8x4 通道布局。
- Branch 1/2/Ripple/blend 的当前版本语义。
- 256 点 Fibonacci 方向表的确切常数和 pack 搜索策略。
- 9 x 9 x 3 noise offset 的编码范围、tree extent 缩放和解码规则。
- Ripple -> Branch 2 -> Branch 1 -> Shared 的执行顺序。
- branch anchor、stretch limit、effective wind、Shared height weight 公式。
- Gust 的精确随机触发判定、时间参数和状态布局。
- SpeedTreeWindStateGPU 的 13 个 vec4 布局。
- Oak 文件版本、Modeler 源码证据、VL 坐标转换和 UE SpeedTree 9 对照。

SpeedTree 专题应链接相关数学和植被页：20-point profile -> LUT；Fibonacci byte -> 球面方向量化；noise offset -> 程序噪声；Alpha -> 植被表示算法。

## 不要混淆的边界

| 通用概念 | SpeedTree 专用实例 |
| --- | --- |
| 一维 LUT 采样 | 19 条、每条 20 点的 Runtime SDK profile |
| 球面方向量化 | Standard.lua 的 256 个 Fibonacci byte 索引 |
| 离散打包 | 9 x 9 x 3 的 noise offset byte |
| UNORM / SNORM 与 binary16 | Standard 顶点记录的具体字段格式和 byte offset |
| Value Noise 与多频信号 | 冠层、Branch 1/2、Ripple 的频段和消费顺序 |
| 稳定相位去相关 | 植被实例、枝条和叶片的风相位打散 |
| 分层顶点变形 | Ripple、Branch 2、Branch 1、Shared 四层 |
| 植被叶片光照 | SpeedTree 的具体 material/shading 路径 |
| LOD / impostor | SpeedTree 文件中的 block、frame、atlas metadata |

归属由算法的输入、输出和不变量决定，不由某个教学案例决定。通用页可以链接一个明确标注的领域案例，但核心公式和验证必须脱离植被仍成立；一旦页面需要树种 profile、枝条层级、叶片 coverage、风层顺序或资产 byte offset 才能成立，就应下沉到 `rendering/vegetation` 或 `rendering/speedtree-final-quality`。

## 已有内容的处理要求

知识库原有 `program/algorithms/piecewise-linear-lut.mdx`。规范内容迁入 `rendering-math/piecewise-linear-lut.mdx`，旧 URL 仅保留兼容入口：Runtime SDK 10.2 是 20 点；UE 旧路径可能为 10 点；完整 SpeedTree 曲线消费关系留在 `speedtree-final-quality/wind-deformation.mdx`。现有 SpeedTree 专题不整体搬入通用目录。

## 新对话的执行顺序

1. 读取知识库 AGENTS.md、目录 order.json 和已有相关页面。
2. 盘点 LUT、噪声、LOD、Alpha coverage、时间域的重复内容。
3. 在既有目录新增或修订通用页面，避免复制 SpeedTree 文档。
4. 更新对应 order.json，保持已有 URL 不变。
5. 在 SpeedTree 页面补充通用算法链接。
6. 运行知识库 npm run build 验证 MDX、链接和目录配置。

## 完成标准

- 公式脱离 SpeedTree 后仍成立，放前三组。
- 依赖版本、packer、byte offset 或 SDK 状态，留第四组。
- 普通树木、灌木、草都能使用的表示算法，放 rendering/vegetation。
- 已有等价页面，修订并链接，不重复创建。
