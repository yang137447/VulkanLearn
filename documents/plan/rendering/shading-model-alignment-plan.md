# Shading Model 实现与验证计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | Shading Model 的**按论文实现 + 验证**计划（未来工作） |
| 状态 | 共享设施已就绪（`M-07` / `M-08` + 四个工具）；**旧实现已整体删除**（2026-09-12），逐模型按论文重做尚未开始（见 §3 进度表） |
| 修订 | 2026-09-12 第 5 版：**前提变更**——旧的逐模型实现（ClearCoat / Cloth / Eye / Hair / Subsurface ×3 / ThinTranslucent / TwoSidedFoliage）与其案例资产整体删除，本计划从"验证已有实现"改为"**按论文实现，再验证**"；接口与 GBuffer 继续参考 UE（见 §0.6） |
| 修订 | 2026-09-12 第 4 版：`M-08` 完成——探针图升级为数学规范坐标系（轴框 / 主次刻度 / 刻度数值 / 轴名 / 图头说明与图例），
曲线描边改为**等宽屏幕宽度**，探针场景 scale 统一为"精确填满画面"；四条曲线的逐列误差全部 ≤0.49 px |
| 修订 | 2026-09-12 第 3 版：`M-07` 完成——V 方向与 `fov` 约定实测钉死、`M_brdfPlot` 竖直镜像修正、共享测量工具入库；结论落在 §1.3 / §1.5.3，证据落在 `shading-model-case-records.md` |
| 修订 | 2026-09-10 第 2 版：改为「逐 shading model 推进 + 共享底层验证 case」，每个案例标注论文 / 书本来源与链接 |
| 策略 | **不允许自定算法**：算法本体一律取自论文 / 书本 / 官方规范；实现里凡论文中找不到的公式进 §1.4 溯源审计表并置 ⚠️ 待整改（见 §0.3） |
| 取代 | 本文件第 1 版（层级式 Level 1/2/3 组织），原文件内容已整体重写 |
| 知识库 | `D:\YYBWorkSpace\GitHub\yyb-knowledge-book`；书库索引页 `src\content\docs\tools-resources\books\index.mdx` |
| 渲染器原则 | 只加载和渲染静态验证资产，不在运行时执行布料或头发模拟 |

本文是未来工作计划。`documents/rendering/` 下与已删模型对应的旧合同已归档（见 §0.6），
现在那里只剩与 shading model 无关的渲染合同（shader 缓存 / 热重载 / 纹理与材质作者流程等）。

---

## 0. 本计划的组织方式与推进规则

### 0.6 重建前提：旧实现已删除，接口与 GBuffer 仍对齐 UE（2026-09-12）

用户决定：**旧的逐模型实现不再保留**，每个 shading model 都按论文从零重做；**接口与 GBuffer 继续参考 UE**。
因此本计划的定位变了一次：

| 之前（第 2–4 版） | 现在（第 5 版起） |
| --- | --- |
| 审计**已有实现**的公式出处，再验证它 | **按论文写出实现**，再用同一套 case 验证它 |
| §1.4 审计表是"整改清单" | §1.4 审计表是**历史记录 + 重做时的检查清单**（被审计的代码已经不在仓库里） |
| §2.x ③ 算法来源表指向现存代码 | 同表的"实现锚点"列是**历史位置**，重做后必须重新填写 |
| §2.x ① 状态 ◐ / ⚠️ / ⛔ 描述旧实现 | 全部重置为「☐ 待实现」 |

**删除边界（明确写下来，避免以后混淆）**：

- **删掉的是实现**：母材质 `M_*`、engine 求值器 `*Lighting.glsl`、模型分发与 customData 编码分支、
  数据供给链路（LUT 生成器、资源装配、SSS pass、逐模型校验与绘制分支）、以及只服务它们的案例资产；
- **保留的是接口面**（按用户要求继续对齐 UE）：`engine/materialInputs.glsl` 里各模型的 MaterialInputs 结构体、
  `engine/materialSurface.glsl` 的字段、`engine/gbufferCodec.glsl` 的 GBuffer 布局与槽位、
  `common/shadingModel.glsl` 的 ShadingModelID 表、Debug View 的数据结构与 setter、各 renderMode / 输出宏。
  它们现在**没有实现去消费**，这是刻意的：重做某个模型时，先按 UE 语义确认它的接口与槽位，再写求值器；
- **保留的是测量设施**：`M_brdfPlot` 及其依赖（`common/clothBrdf.glsl`、`common/hairPathScattering.glsl`、
  `common/microfacetDistribution.glsl`）、四个探针场景、`tool/validation/*`、`SC_sphere_array` 球阵基线；
- 旧实现文档（逐模型合同 + 旧开发计划 + NeoX 角色对齐线）归档在
  `documents/plan/rendering/archive/`，**不再是合同**；那里同时记录了本次删除的完整清单与理由。

**重做一个模型时的入口顺序**（与 §0.5 配合，前者是"接口"，后者是"验证"）：

```text
1. 接口冻结  按 UE 语义确认该模型的 MaterialInputs 字段、ShadingModelID、GBuffer 槽位与 renderMode
2. 论文冻结  从书库取论文，定位每条公式与图号（§2.x ② 已经写好，可直接用）
3. 写实现    求值器 + 分发 case + customData 编码 + 调试数据；不得引入论文之外的公式（§0.3）
4. 验实现    按 §0.5 走基础 case → 论文 case → 差异归因 → 归档
```

> ⚠️ 第 3 步写完前，该模型的 ShadingModelID 会落到 `default:`（= DefaultLit）——这正是 §1.6 的 `A-07`
> 记录的风险。所以**补 case 与补材质校验必须与实现同批完成**，不能只写求值器。

### 0.1 一个模型走完再走下一个

对齐工作**按 Shading Model 串行推进**，不同时铺开。每个模型是一个独立的工作单元，内部固定五段：

```text
§2.x <Model>
  ① 对齐状态        这个模型现在到哪一步
  ② 论文来源表      本模型引用了哪些论文 / 书本 / 官方文档，各自负责哪条公式
  ③ 算法来源表      实现里每一条公式的中文溯源（论文公式号 + 代码位置）
  ④ Case 列表       分「基础 case」与「论文 case」两张表，每行标注来源
  ⑤ 专属约定        只属于这个模型的边界与坑
```

一个模型只有在**它的全部 case 结束时**才可标记为「已对齐」。判定规则：

| 状态 | 含义 |
| --- | --- |
| ☐ 待办 | 还没有开始 |
| ◐ 进行中 | 已有 case 通过，仍有未结 case |
| ⚠️ **待整改** | 实现里存在**论文里没有的公式**，必须先删除或换成论文 / 已发布来源的形式 |
| ☑ 已对齐 | 全部 case 结束、每个 case 有归档记录、且**实现里不含任何非论文公式** |
| ⛔ 受限 | 有 case 因**引擎参数面缺失**无法做真复现；已明确记录缺口，不宣称对齐 |

**⚠️ 优先于一切其它工作**：只在 ⚠️ 存在时，任何"这个模型已经对了"的结论都无效——因为被判定的
对象本身就不等于论文。**⛔ 是允许的终态，☑ 是需要证据的终态。** 缺参数就写缺参数，
不允许用「观感接近」或自创公式顶替。

### 0.2 共享底层验证 Case

三个层次的基础设施是**跨模型共享**的，任何模型都不许自己再造一套：

| 共享层 | 内容 | 服务于 | 章节 |
| --- | --- | --- | --- |
| 共享 A｜求值与测量设施 | 真实实现曲线探针 `M_brdfPlot`、脚本化截图、像素级测量脚本 | 所有需要曲线的 case | §1.5 |
| 共享 B｜架构一致性设施 | Shading Model ID 分发、Debug View、GBuffer 编码、Forward/Deferred 路径 | 所有模型 | §1.6 |
| 共享 C｜观察条件与场景骨架 | 球阵基线、灯光/相机/曝光固定约定、辅助材质、资源组织 | 所有模型 | §1.7 |

共享层只做一次，新增模型时优先**复用**；只有当共享层确实表达不了新模型时才扩展它，并且扩展必须
对已有模型的 case 无影响。

### 0.3 唯一允许的 Case 来源：论文与书本

**本项目不允许自定算法。** 每个模型的算法本体必须能在论文 / 书本 / 官方规范里找到对应公式，
case 就是"抄对了没有"的验证。三类来源的效力如下：

| 类别 | 例子 | 效力 |
| --- | --- | --- |
| **论文 / 书本 / 官方规范** | Marschner 2003 §4 的 R/TT/TRT；Jensen 2001 的 dipole；Jakob layerlab 的 adding equations；PBRT 的微表面与电介质章节；UE / Filament / OpenPBR 官方文档 | ✅ **算法本体的唯一合法来源**，必须绑定到具体节号 / 图号 / 公式号 |
| **参考实现（含其配套文档）** | UE 5.8 Legacy `HairShading` / `TwoSidedBxDF` / ClearCoat 闭包；Filament 的 cloth model 与其 Materials 文档；three.js / glTF 的参考实现 | ✅ 可以定义**参数映射约定**（如 anisotropy 合法域、coat 权重语义）；引用时必须写明版本或节号 |
| **本项目自带** | —— | ❌ **不存在的类别**。算法本体、中间分布、visibility、可见性核都不得自创 |

### 0.3.1 遇到"论文没写"时的处理阶梯

论文没覆盖某个量时，按顺序往下走，**不许跳到最后一格**：

```text
① 换成论文写了的东西      —— 首选。论文没给各向异性就退回论文给的各向同性形式。
② 引用其它已发布来源      —— Filament / UE / OpenPBR / 参考实现的文档或源码，写明版本与位置。
③ 登记为「工程约定」      —— 只允许是**不参与数值**的约定（通道布局、编码、单位换算）。
④ 声明该项做不了          —— 记为 ⚠️ 或 ⛔，不实现"看起来差不多"的自制版本。
```

③ 的判定标准：**去掉它，着色结果是否变化？** 变化 → 它是算法，不许自制；不变化 → 它只是数据搬运，
可以登记。举例：GBuffer 的 5+5 bit 打包是编码约定（可登记）；各向异性 visibility 的掠射衰减项
会改变结果，是算法（不许自制）。

**禁止"自行补全中间步骤"**：论文给了分布 A 与可见性 B，但没给 A+B 的联合形式，此时不得自创一个
"把 B 变形后套到 A 上"的中间式。要么找到给了这个联合形式的外部来源，要么如实声明该组合做不了。

### 0.3.2 工程约定登记表

只登记**不参与数值**的约定。当前登记项：

| 登记项 | 位置 | 属于哪种约定 | 验证方式 |
| --- | --- | --- | --- |
| Cloth GBufferF.w 的 5+5 bit 各向异性打包 | `engine/gbufferCodec.glsl:107` | 编码约定（有损但只是搬运） | 断点往返误差（§1.6.2 已给 1/31 的量化上限） |
| ClearCoat 底层法线的相对八面体编码 | `engine/gbufferCodec.glsl:175` | 编码约定 | case `C-07` |
| Eye profile pair 整数打包与版本位 | `engine/gbufferCodec.glsl:192` | 编码约定 | case `E-05` |
| 材质参数的合法域 / 默认值 | 各 `M_*.json` | 生产参数化的取值范围 | 参数面审计 case |

**判定标准**：登记表里出现任何"公式""分布""visibility""衰减""近似"字样，就说明它其实是算法，
必须移出登记表，改为在论文里找来源或声明做不了。

### 0.4 一个 Case 的固定形状

每个 case 在归档记录里固定七个字段，缺一个就不算结束：

| 字段 | 内容 |
| --- | --- |
| **ID** | 形如 `D-04`（模型首字母 + 两位序号），全文档唯一，跨轮次不复用 |
| **来源** | 论文 / 书 / 官方文档 + **具体图号、公式号或节号** |
| **验证目标** | 一句话说清这个 case 在证明什么 |
| **形式** | `曲线` / `单变量扫描` / `Debug 视图` / `离线对照` / `架构核对` |
| **实现锚点** | 被验证的代码位置：`文件:行` 或函数名 |
| **判定** | 通过 / 不通过 / 受限 + 具体数值（曲线类给逐列像素误差，扫描类给数值或图像差异） |
| **差异归因** | 实现不同 / 参数面缺失 / 资产差异 / **非论文公式**；**归因不能写"无"** |

### 0.5 推进一个模型的标准流程

```text
1. 来源冻结   从书库取论文，定位每条公式与图号 → 填好 §2.x 的②③两张表
2. 基础 case  先做「实现是否等于公式」的 case：曲线、端点、能量、LUT 自洽
3. 论文 case  再做「模型在真实几何上是否成立」的 case：论文图复现、边界行为
4. 差异归因   每个未完全一致的 case 写归因，禁止用"观感接近"结案
5. 归档       把结论与证据路径写进 shading-model-case-records.md（§0.4 的七字段），
              同时更新 §2.x ① 与 §3 进度表
```

> `artifacts/` 被 `.gitignore` 忽略，`<resourcePath>` 是另一个仓库：**case 记录文档才是结论的持久载体**，
> 截图与日志路径只作为当次运行的现场证据引用。

---

# 1. 共享层

## 1.1 两个必须分别回答的问题

1. **实现是否等于公式？** 引擎当前的 BRDF / 分布 / 几何项 / 传输核，与论文描述的模型差多少？
2. **模型在真实几何上是否仍成立？** 换到论文里那种复杂资产时，观感与边界是否符合预期？

这两个问题的证据形式不同：前者靠**曲线与数值**，后者靠**论文图对照与归因**。任何模型都必须先回答第 1
个再回答第 2 个——公式层没对上，资产上的差异无法归因。

## 1.2 数学与物理基础（所有模型共同的验证底座）

下面的量是所有模型的共用底座，它们的验证属于**共享 case**，不重复计入单个模型。

| 共享 ID | 来源 | 验证目标 | 形式 |
| --- | --- | --- | --- |
| `M-01` | [数学与物理基础](../../../../yyb-knowledge-book/src/content/docs/rendering/materials/shading-models/mathematical-foundations/index.mdx)｜Rendering Equation 与 BSDF 定义 | 确认引擎的 `f_r * NdotL` 组合与渲染方程的被积函数一致，没有漏项或重复乘 `PI` | 架构核对 |
| `M-02` | [PBRT 4ed｜Reflection Models → Roughness Using Microfacet Theory](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)｜NDF 投影面积归一化 | NDF 的归一化测度正确：`∫ D(h)(n·h) dω_h = 1`，验证 `DistributionGGX` 的 `a` 参数约定 | 数值积分（离线脚本） |
| `M-03` | [Heitz 2014 JCGT｜Understanding the Masking-Shadowing Function](https://jcgt.org/published/0003/02/03/)｜§3–§5 | 遮蔽项满足 `0 ≤ G1 ≤ 1`、`G1(c→1)=1`、互易性 `D·G/(4|n·l||n·v|)` 对称 | 曲线 + 端点 |
| `M-04` | [PBRT 4ed｜Reflection Models](https://pbr-book.org/4ed/Reflection_Models)｜White furnace test | 各 lobe 的半球积分 ≤ 1（单次散射下必须 ≤ 1，不允许凭空造能量） | 数值积分（离线脚本） |
| `M-05` | [Hoffman 2013｜Background: Physics and Math of Shading](https://blog.selfshadow.com/publications/s2013-shading-course/hoffman/s2013_pbs_physics_math_slides.pdf)｜全文 | 引擎的 public parameterization 与物理量对应关系正确（albedo 上限、F0 语义、roughness 感知映射） | 架构核对 |
| `M-06` | UE 官方 [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine) + UE 5.8 Legacy 源码定义 | Shading Model ID 表与 UE 5.8 Legacy 槽位一一对应，未实现模型保留原始槽位 | 架构核对 |
| `M-07` | 本仓库探针工件（**必须先做**） | **texCoord V 方向与相机 fov 约定要在同一次进程、同一次 `tonemap 0` 会话里重新探明**。现有工件互相矛盾（见 §1.3），在被重新钉死之前，任何模式的"预期行位置 vs 实测行位置"都还没有确定的竖直朝向，1 像素误差目标不可达 | 探针（单次会话四连拍） | ✅ **2026-09-12 完成**（结论见 §1.3，记录见 `shading-model-case-records.md#m-07`） |
| `M-08` | 本仓库探针工件 + 数学制图规范 | 探针图必须自带**坐标轴识别与刻度数值**：矩形轴框、朝内主 / 次刻度、由刻度值格式化出的数值标签、轴名（y 轴名竖排）、参数注记。这样"图上读到的值"与"曲线定义域"同源，判定不依赖外部脚本或口头解释；同时把绘图区矩形变成材质参数，测量脚本用同一组值换算并**从图上核对轴框位置** | 架构核对 + 像素对照 | ✅ **2026-09-12 完成**（§1.5.2 / §1.7.2.1，记录见 `shading-model-case-records.md#m-08`） |

> `M-02` / `M-04` 是**离线**数值积分，不进 shader：用同一份公式在脚本里独立积分，再与 shader 的
> 逐像素输出对照。这条链路是能量守恒类判定唯一可接受的证据形式。

## 1.3 隐式约定：已在 `M-07` 中重新探明（2026-09-12）

本节两条约定原先互相矛盾（历史过程见本节末）。`M-07` 已在**同一次进程、同一次 `tonemap 0` +
bloom 0 会话**里用 mode 3 UV 探针重新实测（单会话四连拍，切场景走 `loadworld`），
**下表是当前唯一有效版本**；完整七字段判定与证据路径见
`shading-model-case-records.md#m-07`。

| 约定 | 实测结论 | 证据 |
| --- | --- | --- |
| mesh `texCoord.y` 方向 | **沿屏幕向上增大**（`uv.y = 1` 在画面顶部）。mode 3 探针：画面顶部绿通道 0.768、底部 0.232，斜率 `-7.4636e-4 /px` | `m07_mode3_uv.bmp` + `measure_plot_curve.py uv-map` |
| 相机 `fov` | **是水平视场角**：`halfWidth = distance·tan(fov/2)`，`halfHeight = halfWidth / aspect`。由实测映射反推 `halfWidth 2.4871` / `halfHeight 1.3972`，与模型值 `2.48528` / `1.39797` 一致；半宽/半高 = 1.780 ≈ aspect 16:9 | 同上 |
| 输出链路 | `tonemap 0` + bloom 0 + exposure 1 时 `pixel8 = 255·sRGB_encode(clamp(shaderLinear))`，**读数必须先 sRGB 解码**；面板铺满整帧（mode 3 蓝通道=0 覆盖 99.904%），四角像素与 uv 值逐位吻合 | 同上 |

由此得到的 `uv ↔ 像素` 映射（探针面板，相机 z=6、`fov=45`、16:9；探针场景的 scale 按
§1.7.2.1 取 `(halfWidth, halfHeight)`，所以面板精确填满画面）：

```text
halfWidth = 6·tan(22.5°) = 2.485281        halfHeight = halfWidth/(16/9) = 1.397971
uv.x = (x + 0.5) / W            uv.y = 1 - (y + 0.5) / H        （x,y 为屏幕像素，左上原点）
一般式：uv.x = 0.5 + ndc_x·halfWidth /(2·scaleX)    uv.y = 0.5 - ndc_y·halfHeight/(2·scaleY)
        ndc = 2·(pixel+0.5)/size - 1（Vulkan NDC 的 y 向下为正，所以 y 项取负）
实测核对：uv.x = 0.00078129·x + 0.00028、uv.y = -0.00138899·y + 0.999325，
        即斜率正好是 1/1280 与 1/720；四角 uv = (0.0003, 0.9993) / (0.9996, 0.9993) / …
```

曲线画在内缩的**绘图区**矩形里（留白写刻度值与轴名），那套版式与本节两条约定无关：
绘图区由材质的像素参数给出，测量脚本用同一组值把像素换算成轴坐标（§1.5.2 / §1.7.2.1）。

**`M-07` 带出的实现修正**：`M_brdfPlot.surface.glsl` 原先用 `plotY = 1 - uv.y`，前提是
"uv.y 沿屏幕向下增大"——前提错了，结果 mode 0 / 1 / 2 的探针图**整体竖直镜像**（值 1 被画在屏幕底部）。
判据是同一批 mode 2 数据在镜像假设下误差 ~700 px、在直立假设下 ≤0.5 px，相差三个数量级。
已改为曲线值直接作为轴坐标；`M-08` 又把描边改成**等宽屏幕宽度**后重测：Schlick 最大 `0.465 px`、
Smith `0.462 px`、mode 0 的 GGX / Charlie `0.486 px` / `0.467 px`（更早两版分别是 0.484 / 0.418
与 0.410 / 0.374）。**曲线类 case 的 1 像素判定门限自此可达。**

### 历史记录（为什么一度被判为"互相矛盾"，以及真实原因）

以下是 `M-07` 之前的早期结论与当时的实测依据，**保留仅作为"别再这样推断"的记录**：

```text
早期结论①（与本次实测一致）：uv_probe.bmp（mode 3）沿列采样，
  G(=uv.y) = 223(y=5 顶部) → 216 → 206 → 189 → 160(y=714 底部) ⇒ uv.y 在画面顶部更大

早期结论②（无法复核）：brdf_plot_g1_only.bmp（mode 2）红/Schlick 曲线 y ≈ 11 @ x=256 → y ≈ 280 @ x=1215
  ⇒ 当时推断"较大的曲线值落在较小的屏幕 y"，再与 shader 的 plotY = 1 - uv.y 联立，
    要求 uv.y 在顶部更小 —— 与①矛盾
```

真实原因有两条，都不是"约定本身矛盾"：

1. **那张 g1 截图与现在的代码状态不是同一版**：它摄于 12:20:47，而 `plotY` 的翻转与面板
   framefill 的 scale 调整都发生在它之后。用当前映射去核对它，直立与镜像两种假设都对不上
   （实测分别差 ~24 px 与 ~700 px），所以它既不能证明 V 方向、也不能证伪——**唯一正确的处理是判它无效**；
2. **推断里少了"输出编码"这一环**：默认色调映射不是线性编码，8-bit 读数无法直接换算成 shader 值，
   而当时的结论②是从"曲线在屏幕上的走向"间接推朝向的。

因此 `M-07` 的做法固定为：**同一次会话里 `tonemap 0` + `bloom strength 0` 之后才采图，且优先相信
mode 3 这种"材质直接输出 uv"的直接证据，不再从曲线走向反推坐标轴。**

## 1.4 公式溯源审计表（按"论文里有没有"逐条核对）

> ⚠️ **本节是历史记录（2026-09-12 起）**：被审计的那些实现已经整体删除（见 §0.6），
> 所以下面的"代码位置"列指向的文件**已经不在仓库里**。它现在的用途有两个：
> ① 记录上一版实现哪里偏离了论文（重做时不要重复这些错）；
> ② 作为重做时的**检查清单**——每条都要在新实现里重新定位到论文式号，审计结论必须重写，不能照抄。

本项目**不允许自定算法**（§0.3）。下表把当前实现里的公式逐条与论文原文核对。

### 1.4.1 核对方法（可复现）

书库的 PDF 已由 `pypdf` 抽成纯文本，可直接检索式号与公式：

```powershell
# 需要 pypdf（本机已有）：py -3 -m pip install pypdf
py -3 -c @"
import glob, os, pypdf
src = r'D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\tools-resources\books\resource\pdfs'
dst = r'tmp\pdftext'
os.makedirs(dst, exist_ok=True)   # 不加这句会在目录不存在时直接 FileNotFoundError
for p in sorted(glob.glob(os.path.join(src, '*.pdf'))):
    name = os.path.splitext(os.path.basename(p))[0]
    text = ''.join((pg.extract_text() or '') for pg in pypdf.PdfReader(p).pages)
    open(os.path.join(dst, name + '.txt'), 'w', encoding='utf-8').write(text)
    print('%-58s %7d chars' % (name, len(text)))
"@
```

> **两条必须注意的坑**：① 抽取文本里希腊字母常丢失（`k = /2` 实际是 `k = α/2`），
> 判定公式**必须结合上下文**，不能只看单行；② 2 页的短文（如 Estévez & Kulla）内容远少于
> 标题给人的预期，**不要假设论文覆盖了它没写的东西**。

### 1.4.1b 决策树（每个模型的"可选做法 × 我们做不做"）

审计结论只是"某条公式有没有出处"，还需要一张图回答**更上位的问题**：这条着色路线上到底有哪些
决策维度、每个维度下世界上存在哪些主流做法、我们分别做了哪几支。

九棵树（DefaultLit / ClearCoat / Cloth-Sheen / Hair / Subsurface / TwoSidedFoliage / Eye /
ThinTranslucent / Unlit）用知识库的**脑图功能**呈现（`rehypeMindmap` + `mindmapLayout`）：

```text
yyb-knowledge-book/src/content/docs/rendering/materials/shading-models/
  <page>.mindmap                        ← 缩进大纲，与页面同目录（rehypeMindmap 按页面目录解析）
  resource/images/
    shading-model-trees.data.mjs        ← 数据（唯一真源）
    build-mindmaps.generate.mjs         ← 生成器：data.mjs -> 九份 .mindmap
```

- **为什么用脑图而不是流程图**：脑图由构建期布局引擎生成**内联 SVG**（真实 DOM），
  因此文字可选中、可被 Pagefind 检索、随页面亮暗主题换色；流程图只能作为图片引用，三者都做不到；
- **状态标记**：`✔` 已实现 / `▢` 计划做 / `✘` 不做·未采纳 / `⚠` 待整改 / `◐` 部分·受限。
  标记直接写在节点标题里（脑图节点是纯文本），语义与本文 §1.4.2 审计表、§2 各模型 case 列表一一对应；
- **重新生成**：`node build-mindmaps.generate.mjs`（在 `resource/images/` 下执行，幂等）；
- **每个模型页已内嵌对应脑图**（知识库的 `## 决策树：可选做法与我们的取舍` 一节），
  读者不需要离开书页就能看到"这条路上还有什么没做"；
- **引用可深链到论文页码**：节点备注带页码（如 `Karis 式2·p3`），链接形如
  `/tools-resources/books/?pdf=karis-2013-real-shading-ue4:3#karis-ue4`；
  `npm run build` 的收尾脚本 `scripts/fix-mindmap-pdf-links.mjs` 在构建后把它换成
  `/_astro/<name>.<hash>.pdf#page=3`。用 `?query` 而不是把标记塞进 `#hash` 的原因：
  塞进 hash 会得到非法锚点，dev 下连书库卡片都定位不到；query 形态下 hash 始终是真实锚点。
  **页码是从 PDF 抽取文本里逐条定位式号得到的**，不是估的。

> 维护约定：**状态变了先改 `shading-model-trees.data.mjs`，再重跑生成器**。直接手改 `.mindmap`
> 会让图和审计结论脱节，而"图和结论同步"正是这些树存在的理由。

### 1.4.2 审计结果

状态：✅ 论文形式（式号已核） ｜ ❌ 论文里没有 ｜ 🔎 待核对（尚未逐项确认）

**DefaultLit**

| # | 公式 | 代码位置 | 论文核对结果 | 状态 |
| --- | --- | --- | --- | --- |
| D-a | `k = (Roughness+1)²/8` 的 Schlick G1 | `common/microfacetDistribution.glsl:31` | [Karis 2013](#karis-ue4) §Specular G 式(4)，原文：*"we chose to use the Schlick model, but with k = α/2 … We also chose to use Disney's modification … remapping roughness using (Roughness+1)/2 before squaring. **It's important to note that this adjustment is only used for analytic light sources**; if applied to image-based lighting, the results at glancing angles will be much too dark."* | ✅ |
| D-b | 该 G1 被 `GeometrySmith` 用于间接光 / IBL 路径 | `common/lighting.glsl:431` `GeometrySmith()`，调用点 `:568` `:614` | 论文**明确禁止**这套 k 用于 IBL | ⚠️ **用法与论文相悖**（见 `D-20`） |
| D-c | `D_GGX`，`α = roughness²` | `common/microfacetDistribution.glsl:14` | [Karis 2013](#karis-ue4) 式(3) + 正文 *"Disney's reparameterization of α = Roughness²"* | ✅ |
| D-d | Lambert `c_diff/π` | `common/lighting.glsl:255` | [Karis 2013](#karis-ue4) 式(1)：`f = c_diff/π`，原文说明为什么不用 Burley diffuse | ✅ |
| D-e | SH 辐照度 band 权重 `(π, 2π/3 ×3, π/4 ×5)` | `common/lighting.glsl:227` | Ramamoorthi & Hanrahan 2001 的 9 项辐照度重构系数（Karis 引用 [12] 的同一套） | ✅ |
| D-f | LUT 里的 `k = α²/2` | `generator/brfdLut.comp:66` `GeometrySchlickGGXIBL()` | **Karis 2013 全文没有 `α²/2`**；论文只给 `k=α/2`（direct 拟合）与 `k=(r+1)²/8`（analytic 调整），**IBL 域的 G 未指定** | ❌ 论文无此式（见 `D-21`） |
| D-g | `SmithG1Ggx` 精确参考曲线 | `common/microfacetDistribution.glsl:49` | 与 [Neubelt & Pettineo 2013](#neubelt-order-1886) 式(5) 数学恒等：两边同乘 `c+√(α²+(1-α²)c²)` 即得 `2c/(c+√(…))`。等价于 [Heitz 2014](#heitz-masking-shadowing) 式(86) `Λ = (−1+√(1+1/a²))/2`，`a = 1/(α_o tanθ_o)`，再取 `G1 = 1/(1+Λ)` | ✅ **之前标"来源待查"是标注错误，已改正** |

**Cloth / Sheen**

| # | 公式 | 代码位置 | 论文核对结果 | 状态 |
| --- | --- | --- | --- | --- |
| S-a | `ClothCharlieDistribution` = `(2 + 1/α) sin^(1/α)θ_h / (2π)` | `common/clothBrdf.glsl:32` | [Estévez & Kulla 2017](#estevez-sheen) 式(2)：`D(m) = (2 + 1/r) sin^(1/r)θ / 2π`，**逐项一致** | ✅ |
| S-b | `ClothSheenRoughnessToAlpha` = `r²` | `common/clothBrdf.glsl:15` | 论文的分布参数 `r` 直接是 `sin` 的指数（`(0,1]`），**没有 `r → α` 的映射**；代码的 `α = r²` 是 [Karis 2013](#karis-ue4) 的 Disney 约定被**平移**到 Charlie 上 | 🔎 待定（见 `S-24`） |
| S-c | `ClothNeubeltVisibility` = `1/(4(N·L+N·V−N·L·N·V))` | `common/clothBrdf.glsl:44` | [Neubelt & Pettineo 2013](#neubelt-order-1886) 式(23) 的分母 **逐字一致**。但原文语境是：*"we dropped the geometry term"* —— 它是**最终 BRDF 里替换传统 microfacet 分母的那一项**，不是独立的 visibility 函数。函数取名 `…Visibility` 属本项目命名 | ✅ 公式有出处（命名待改） |
| S-d | 各向异性 visibility 的掠射衰减 `4(1-α)(1-warpNoL)(1-warpNoV)` | `common/clothBrdf.glsl:135` | Estévez & Kulla 全文**无各向异性内容**（论文仅 2 页）；Neubelt 只给 GGX 的各向异性代入。**补查后**：有出处的各向异性布料模型走的是另一条路线，见 §1.4.3 | ❌ **论文无此式** |
| S-e | `aspect = 2^anisotropy` | `common/clothBrdf.glsl:61` | 两篇论文都没有 anisotropy→轴比的映射。**有出处的替代**：Neubelt 式(12)(13) `α_t=α`、`α_b=lerp(0,α,1−anisotropy)`（但是给 GGX 的）；microcylinder 路线则完全不同（§1.4.3） | ❌ **论文无此式**（存在有出处的替代） |
| S-f | 椭圆切平面 Charlie `D_iso·(α_T α_B sin²θ_h)/denom` | `common/clothBrdf.glsl:97` | Estévez & Kulla 不提供各向异性 Charlie；把 Neubelt 的椭圆代入套到 Charlie 上**没有任何论文给出这个联合形式** | ❌ 论文无此式 |
| S-g | cross 两瓣混合 `mix(primary, 0.5(primary+cross), cross)` | `common/clothBrdf.glsl:184` | 两篇论文都没有 cross / 双轴纤维瓣；microcylinder 路线用 thread tangent 表达各向异性，也没有 cross 参数 | ❌ 论文无此式 |
| S-h | `T_b = 1 − c_s·E_s(NoV, α_s)` 分层 | `engine/clothLighting.glsl` | Estévez & Kulla §5 用 Kelemen–Szirmay-Kalos albedo scaling，**缩放因子是 `min(α_i, α_o)`**（入射与出射方向各一个 albedo 取较小者），不是 `1 − E_s(NoV)` | ❌ 与论文式不同（见 `S-27`） |

**Hair**

| # | 公式 | 代码位置 | 论文核对结果 | 状态 |
| --- | --- | --- | --- | --- |
| H-a | 路径吸收 `T = exp(−2σ_a(1+cos2γ_t))` 每段 | `materialFunction/mf_hairAbsorption.glsl`、`common/hairPathScattering.glsl` | [Marschner 2003](#marschner-2003) §4.3：内部路径长 = `2 + 2cos(2γ_t)` 倍半径，每段贡献 `T(σ_a,h) = exp(−2σ_a(1+cos2γ_t))` | 🔎 待逐项核对代码是否用了同一路径长 |
| H-b | TT/TRT 的路径长比 `0.5·√(1−(h/n′)²)/cosθ_d`、`0.8/cosθ_d` | `common/hairPathScattering.glsl:134` `:164` | 这是 UE Legacy 的经验形式；Marschner 给的是 `T(σ_a,h)^p`（`p` 为内部段数） | 🔎 待定（见 `H-20`） |
| H-c | 纵向高斯 + `Shift=0.035`、`n′ = 1.19/cosθ_d + 0.36cosθ_d` | `common/hairPathScattering.glsl:53` `:122` | 属 UE Legacy 参数化，不是 Marschner 的 `M(θ_i,θ_r)` 形式 | 🔎 待定（见 `H-20`） |

**Subsurface / PreintegratedSkin / SubsurfaceProfile**

| # | 公式 | 代码位置 | 论文核对结果 | 状态 |
| --- | --- | --- | --- | --- |
| U-a | 扩散 profile / dipole | `engine/subsurfaceProfileLighting.glsl` | [Jensen 2001](#jensen-bssrdf) §3 式(7)–(9) 需逐项对照 | 🔎 待核对 |
| U-b | 预积分皮肤传输函数 | `generator/subsurfaceLookupTables.comp` | [Neubelt & Pettineo 2013](#neubelt-order-1886) 式(16)(17) 给了精确形式：`D(θ,r) = ∫cos(θ+x)R(2r sin(x/2))dx / ∫R(2r sin(x/2))dx`，按 `cos(θ)` 索引；并说明了 SH 环境下的接法 | ✅ **已定位论文式**，待与 LUT 生成器逐项对照 |

**公式齐全但尚未逐条核对的模型**：TwoSidedFoliage、Eye、ThinTranslucent —— 它们各自的基准已写明
（UE Legacy 闭包 / UE 官方文档），但没有做上面这种"抽文本对式号"的核对，作为 ⚠️ 待完成的审计项。

### 1.4.3 文献补查：各向异性 sheen 到底有没有出处？

§1.4.2 的结论是"Estévez & Kulla 与 Neubelt 都没有各向异性 sheen 的联合形式"。为此做了独立文献补查，
**结论：有出处，但不是 sheen / Charlie 路线的扩展，而是另一条完整的布料模型路线。**

#### 查到的可直接使用的来源

| 文献 | 与各向异性的关系 | 可获取性 |
| --- | --- | --- |
| **Sadeghi, Bisker, De Deken, Jensen 2013**, *A practical microcylinder appearance model for cloth rendering*, ACM TOG 32(2) | **完整给出各向异性布料散射模型**。纤维方向通过 **thread tangent `t`** 进入：thread 被建模为"由 fiber 子圆柱组成的大圆柱"，散射函数在 `t` 的法平面坐标架里定义；表面项与体积项都带**绑定切线方向的 Gaussian 宽度** `γ_s` / `γ_v` | ✅ 开放获取：[eScholarship PDF](https://escholarship.org/content/qt6v11p5b0/qt6v11p5b0.pdf)（GREEN OA） |
| **Irawan & Marschner 2012**, *Specular Reflection from Woven Cloth*, ACM TOG 31(1) | 编织布程序化散射模型，含**纱线几何与织法**、经/纬两套 thread 方向 | 摘要页公开（[Cornell](https://www.cs.cornell.edu/~srm/publications/TOG12-cloth.html)），正文需 ACM |
| **Zhu & Montazeri 2024**, *Recent Advances in Realistic Cloth Rendering*, SIGGRAPH Asia 2024 Course | 布料渲染最新综述，用于确认"该选哪条路线" | 需 ACM：[DOI](https://dl.acm.org/doi/abs/10.1145/3680532.3689587) |
| Jakob, Arbree, Moon, Bala, Marschner 2010, *A radiative transfer framework for rendering materials with anisotropic structure* | 纤维朝向的各向异性**相位函数 / 体积散射**（microflake 系）；Sadeghi 明确引为同源思路 | 作者页公开：[Cornell PDF](https://www.cs.cornell.edu/~kb/publications/SIG10Aniso.pdf) |

#### microcylinder 模型的关键式（已核对原文式号）

```text
f_r,s(t,ωi,ωr) = F_r(η, w_i) · cos(φd/2) · g(γs, θh)                    (2)   表面反射
f_r,v(t,ωi,ωr) = F·(1−kd)·g(γv, θh) + kd/(cosθi + cosθr)·A              (3)   体积散射
f_s(t,ωi,ωr)   = ( f_r,s + f_r,v ) / cos²θd                             (4)   完整 BSDF
其中  F = F_t(η, w_i)·F_t(η′, w′_r)（两次透射 Fresnel）
      θh = (θi+θr)/2,  φd = φi − φr
      g = 单位面积 Gaussian；γ_s 控制表面粗糙，γ_v 控制前向散射锥宽
```

**论文原话级的依据**（直接回答"各向异性从哪来"）：

> *"our model is a large thread cylinder composed of tiny fiber subcylinders … the orientation of the
> fiber subcylinders differs from that of the thread cylinder. **We model this deviation as a normal
> distribution centered on the thread tangent.** Therefore, a flat thread will have a much smaller
> variance than a twisted thread."*
>
> *"We experimented with various phase functions as well, but found them inadequate due to their
> decoupled behavior from the direction of the thread. Our approach is similar in spirit to
> [Jakob et al. 2010], which defines **phase functions oriented to the direction of fibers** to
> achieve highly anisotropic volume scattering."*

也就是说：**各向异性在这条路线里不是"给 D 换一个椭圆轴比"，而是"把散射瓣的坐标架与宽度绑定到纤维 /
纱线切线方向"。** 这正好解释了为什么把 Neubelt 的椭圆代入套到 Charlie 上找不到出处——那条路线上
根本不用椭圆轴比表达各向异性。

#### 能量与分层：microcylinder 用物理项，不用 albedo LUT

原文没有 directional-albedo LUT，也没有 `1 − E_s` 这类能量补偿；能量由**两次透射 Fresnel `F_t`**、
`kd` 与 `A` 显式给出。这意味着若改用该模型，`M_cloth` 现有的
`clothDirectionalAlbedoLut` / `clothAnisotropicDirectionalAlbedoLut` 与 `T_b = 1 − c_s·E_s(NoV)`
（`S-27`）都要重新设计——**必须作为独立整改项列出，不能顺手替换**。

#### 两条路线的外观差异（决定了它们能不能"都要"）

**是两个不同外观，不是同一个模型的两个参数设置。**

| | 路线 A：sheen / Charlie 系 | 路线 B：microcylinder 系 |
| --- | --- | --- |
| 出处 | [Estévez & Kulla 2017](#estevez-sheen) 式(2) + [Neubelt & Pettineo 2013](#neubelt-order-1886) 式(23) | [Sadeghi et al. 2013](#sadeghi-microcylinder) 式(2)(3)(4) |
| 各向异性 | **没有**。Charlie 只吃 `N·H`，切线不进入公式 → 整块布料外观均匀，转向不变 | **有**。thread tangent `t` 定义反射锥，`γ_s` / `γ_v` 沿切线方向展宽 → 高光呈条纹状并随织纹方向转向 |
| 控制量 | 只有 sheen roughness | **`γ_s` 与 `γ_v` 两个宽度参数** |
| 典型观感 | 正对偏暗、掠射出现柔软亮边的"毛绒 / 天鹅绒" | 绸缎、斜纹布那种**有方向性的高光条纹** |
| 论文自述的差别 | 论文只声明处理 *"back-scattering properties of cloth-like materials"* | 论文按 thread 类型分别验证（Fig 8，与实测 BRDF 对照）：**twisted thread → 宽 surface Gaussian + 更宽的 volume Gaussian；flat thread → 极窄 surface Gaussian + 小的有色 albedo** |

也就是说：**A 能做出"毛绒感"，但做不出"织纹方向"；B 两者都能做**（flat thread 时 `γ_s` 取极窄，
surface 项自然接近镜面，volume 项仍沿切线展宽）。B 在外观表达上严格更强，但
**两者不是逐点数值相等的同一模型**（A 的 Charlie 指数形式 ≠ B 的 Gaussian 宽度形式），
所以不能把 A 当成 B 的一个参数档位。

**三条可选路线（都必须只用论文式，因此都符合 §0.3）**：

| 方案 | 内容 | 代价 | 适用 |
| --- | --- | --- | --- |
| **A**（`S-23`） | 只留论文各向同性 Charlie + 式(23) 分母，取消各向异性输入 | 最小 | 只想要"正确的毛绒布料"，不要织纹方向 |
| **B**（`S-29`） | 只留 microcylinder 模型 | 中（能量项与两张 LUT 重做） | 要织纹方向，且只维护一套公式 |
| **C**（`S-31`） | **两条都保留**：A 作为"论文各向同性 sheen"对照臂，B 作为生产臂；两臂在**不同材质**下各跑 case，不混进同一个 BRDF | 最大（两套公式 + 两套能量账本） | 学习型项目：把"哪种近似丢了什么"直接变成可对照的证据 |

**方案 C 的教学价值**：它把"分布族的选择如何改变外观"变成可测量的对照——同一场景、同一光照，
A 与 B 并排，差异可归因到"切线是否进入公式"这一条。这正是
[Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 6 左（GGX vs Charlie 分布对比）想要但没有做到的那组对照。

**方案 C 的三条硬约束**（否则会退化成"两个都能用所以随便选"）：

1. **必须是两种材质实例 / 两个 shader variant，不是一个 `if` 开关**。两套公式不得出现在同一份
   编译产物里，否则 `M_cloth` 会同时存在两个能量账本，"这个材质究竟用了哪条"无法从产物判断；
2. **必须指定生产臂与对照臂**：B 为生产臂（外观更完整、各向异性有独立依据），A 为对照臂，
   A 不得被用作交付外观；归档记录的每张图必须标注用的是哪条；
3. **两臂的能量各自闭合**，不允许"用 A 的能量项去补偿 B 的 lobe"——那等于把两条路线又拼成一条新路线。

### 1.4.4 状态判定

| 状态 | 模型 | 含义 |
| --- | --- | --- |
| ❌ **论文里没有** | **Cloth / Sheen**（S-d / S-e / S-f / S-g / S-h） | 实现在论文之外；必须按 §0.3.1 阶梯清算。**这是当前唯一被确认的自创公式区** |
| ⚠️ **用法与论文相悖** | DefaultLit（D-b / D-f） | 公式本身有出处，但被用在了论文明确禁止的路径上 |
| 🔎 **待核对** | Hair、Subsurface、Foliage、Eye、ThinTranslucent | 基准已知，但尚未逐条抽文本对式号 |

**不算审计项的两类**（它们不改变着色结果）：

- GBuffer / 参数的编码与合法域约定：登记在 §0.3.2；
- 辅助材质与探针（`M_brdfPlot`、`M_measureGrid`、`M_unlit`）：不参与产品着色路径。

### 1.4.5 维护规则

1. 每条都必须有明确的清算方式，不允许写"先留着"；
2. 新增实现时，如果某条公式在论文里找不到出处，**先记账、不要先写代码**；
3. 每个模型转为 ☑ 的前置条件是该模型相关的审计项全部为 ✅ 或已按阶梯处理（含"声明做不了"）；
4. **审计结论必须写式号**。只写"和论文一致"不算——式号是可复核的最小单位。

## 1.5 共享 A｜求值与测量设施

### 1.5.1 设计原则

- **求值走真实实现**：曲线不照抄公式重画，而是直接调用 shader 库里同一份数学实现
  （`common/microfacetDistribution.glsl`、`common/clothBrdf.glsl`、`common/hairPathScattering.glsl`；
  `M_pbr` 的 DefaultLit 也走其中之一）。复现的是这些函数的行为，而不是文档里的公式抄写。
- **数据驱动**：场景、材质实例、参数扫描都是 JSON，不改 C++ 或 render graph。
- **可脚本化截屏**：同一组启动参数产生同一张截图。
- **一个 case 只扫一个变量**：其余参数固定，才能与论文图逐项对照。

### 1.5.2 论文曲线探针 `M_brdfPlot`

`shader/glsl/M_brdfPlot.surface.glsl` 是 Unlit 材质，画的是**带完整坐标系的图**：
矩形轴框 + 朝内刻度（主 / 次）+ 刻度数值 + 轴名（y 轴名竖排）+ **曲线图例** + 参数注记。
曲线求值直接调用引擎真实函数，坐标轴与图例只是版式，不参与任何着色公式。

| mode | x 轴 | y 轴 | 图例（曲线身份） | 备注 |
| --- | --- | --- | --- | --- |
| 0 | θh，0..90°，主刻度 15°、次刻度 7.5° | GGX / Charlie 各按峰值归一化后的 **log10 轴**（1e-3..1，主刻度为四个十倍程，次刻度为 2× / 5×） | `GGX` / `Charlie` | Neubelt & Pettineo 2013 Fig 6 左 |
| 1 | sinθl，-1..1，主刻度 0.5、次刻度 0.25 | R / TT / TRT 各自的 log10 轴（同上） | `R` / `TT` / `TRT` | Marschner 2003 Fig 5、pbrt Fig 9 |
| 2 | cosθ，左端 1、右端 0，主刻度 0.25、次刻度 0.125 | G1 线性轴 0..1，主刻度 0.25、次刻度 0.125 | `Schlick (engine)` / `Smith (exact)` | Karis 2013 Fig 2；红线 = 引擎实际用的 `GeometrySchlickGGX`，绿线 = 精确 Smith |
| 3 | —— | —— | —— | UV 诊断：`baseColor = (uv.x, uv.y, 0)`，**不画任何标注**（叠加会污染 uv 读数） |

**图头（说明行 + 图例）在绘图区上方、外面**，自上而下是：

```text
说明行：这张图在画什么 + 在当前参数下能得出的结论（数字由 shader 现算）
图例第 1 条 ━━ 名称
图例第 2 / 3 条（一条一行）
──────────────────────────  轴框上边
```

参数注记（`r=0.60` / `r=0.25 tv=0.60`）右对齐在说明行上。每个 mode 的说明与图例：

| mode | 说明行（结论数字现算） | 图例 |
| --- | --- | --- |
| 0 | `GGX vs Charlie: normalized peak ratio <GGX 峰值/Charlie 峰值>` | `GGX` / `Charlie` |
| 1 | `Hair R, TT, TRT: normalized log10 Mp` | `R` / `TT` / `TRT` |
| 2 | `Schlick vs Smith: max G1 deviation <定义域最大偏差>` | `Schlick (engine)` / `Smith (exact)` |

两条设计约束：

1. **图头必须在绘图区外面**：① 绘图区内任何位置都会被曲线穿过，压上去要么挡曲线、要么被
   曲线染色；② 图例色线"与曲线同色同粗细"（这正是它的价值），留在绘图区内会被逐列测量
   误认成曲线——实测 mode 0 的平均误差会从 0.153 px 涨到 4.990 px。放外面两条都不成立，
   既不需要保留区参数，也不需要底板与边框；
2. **结论里的数字必须现算**，不能写死在文案里：mode 0 写的是两条分布的峰值比（正好解释
   "为什么纵轴要用 log10"），mode 2 写的是引擎近似与精确参考在定义域上的最大偏差
   （`PlotMaxG1Deviation()`）。图上写的数要与曲线同源，否则改一个 roughness 就会留下过期结论。
   `M-08` 里这两条结论都用独立的 python 复算核对过（`13.1` / `0.226`），并用"解出点阵字体、
   反向识别渲染结果"验证过图上文字与预期字符串逐字一致（case 记录里有方法）。

`u_plotRectPixels.w`（上留白）必须容纳「1 行说明 + 最多 3 行图例」；下留白只需容纳
x 刻度值与 x 轴名。图例仍画在**曲线之前**：曲线始终压在最上层，
"曲线最亮处就是曲线颜色"这条测量前提不受影响。

**刻度数值由刻度值本身格式化出来**（`PlotTextPushFixed`），不是手写字符串——"图上写的数"
与"刻度的位置"永远同源，不会出现标签与刻度错位。纵轴名的系数（`D` / `Mp`）随 mode 切换。

材质参数（都在 `M_brdfPlot.json` 里，MI 不需要重复默认值）：

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `u_plotRectPixels` | 绘图区到面板四边的留白（左、下、右、上，**像素**）：轴框就是内缩后的这个矩形。上留白要容纳说明行 + 最多 3 行图例，下留白要容纳 x 刻度值与 x 轴名 | `(92, 52, 18, 85)` |
| `u_plotTextPixels` | 版式度量（像素）：字模单元边长、主刻度长度、线宽、标签间隙 | `(2, 8, 1.2, 5)` |

图头（说明行 + 图例）不需要额外参数：它在绘图区外面（上方），测量脚本只在绘图区内找曲线，
天然不会碰到它。

**为什么尺寸用像素、而且不必逐场景调**：uv 对屏幕是仿射映射，`fwidth(uv)` 精确等于
"每像素的 uv 步长"，所以 `uv / fwidth(uv)` 就是以面板左下角为原点、y 向上的像素坐标；
只要面板**精确填满画面**（§1.7.2.1 的探针约定），这个坐标就与屏幕像素一一对应。
换 scale、换机位、换面板尺寸都不需要改参数。

**绘制顺序**：网格 → 刻度 → 刻度数值 → 轴名 → **轴框** → 参数注记 → 曲线。
轴框必须在网格之后（网格主刻度与边框共线，否则边框会被盖成网格色，测量脚本的边框核对会失配），
曲线必须在最后（"曲线最亮处就是曲线颜色"是逐列测量的前提）。

**曲线是等宽屏幕宽度，粗细按"网格线的倍数"给**：描边把"轴坐标下的纵向距离"除以它在屏幕上的
梯度长度得到**像素距离**（SDF 标准做法，梯度由 `dFdx/dFdy` 免费给出），于是

```text
网格线半宽 = PLOT_GRID_STROKE_SCALE(0.6) · u_plotTextPixels.z
曲线半宽   = PLOT_CURVE_WIDTH_IN_GRID_LINES(2.0) · 网格线半宽 = 1.44 px（默认参数下）
```

**不要改回"与轴坐标常量比较"**：那种写法是恒定**纵向**厚度，垂直宽度会在陡峭段按
`1/sqrt(1+slope²)` 收缩——实测 mode 2 的 Schlick 平缓段 16.5 px、陡段 8.8 px，看起来就是
"同一条曲线粗细不一致"。测量脚本的 `--curve-half-width-px` 缺省时**从材质 JSON 反算同一公式**
（也会从 JSON 读 `u_plotRectPixels`），命令行只在需要临时覆盖时才用——两边各写一个数一定会漂移。

**新增标注必须保持无彩色**：坐标轴 / 刻度 / 文字 / 网格的通道差都 ≤ 0.08，曲线是强彩色；
测量脚本用彩度门限（≥0.15）把标注排除在曲线判定之外。这条约束是工具与材质之间的接口。

求值来源：

- `DistributionGGX`、`GeometrySchlickGGX`、`SmithG1Ggx` —— `common/microfacetDistribution.glsl`
- `ClothCharlieDistribution`、`ClothSheenRoughnessToAlpha` —— `common/clothBrdf.glsl`
  （注意：探针**没有**调用 `ClothSheenUnitResponse`；那个函数原本只被已删除的 Cloth LUT 生成器与
  `engine/clothLighting.glsl` 使用，重建 Cloth 时再决定是否接回）
- `EvaluateHairUeR / TT / TRT`、`BuildHairUeScatteringContext` —— `common/hairPathScattering.glsl`

mode 0 / 1 的纵轴按 log10 压缩 3 个数量级（GGX 低粗糙度峰值可达 `1/(π a²)`，与 Charlie 相差十几倍，
论文本身也是对数轴）。mode 2 两条曲线都在 `[0,1]`，用线性纵轴。

几何用 `Common/Source/Models/plot_quad.obj`（顶点为 ±1，即两单位宽，+Z 法线，`vt.y = 1` 在 +Y 顶点）。
因此面板的 UV 是 `uv = 0.5 + worldXY/(2·scale)`——`M-07` 实测核对过（§1.3）。

### 1.5.3 共享测量工具（✅ 2026-09-12 已交付）

原先的缺口是：`tool/` 下没有任何 BMP 解析 / 曲线测量代码，已有曲线结论来自仓库外的临时脚本、
无法复现。**`M-07` 一并把这两个工具做出来了**：

| 工具 | 位置 | 职责 |
| --- | --- | --- |
| `measure_plot_curve.py` | `tool/validation/` | `uv-map`：实测 mode 3 的 `uv ↔ 像素` 映射、逐点核对相机模型、检查"面板精确铺满"约定；`curve`：按列取曲线**亮核平台**中心，与解析期望行位置比对，输出平均 / 最大误差、"直立 / 镜像"两种假设的对照，并做**轴框位置交叉核对**；`panel`：§1.7.2.1 的四角 / 铺满检查 |
| `run_paper_case.ps1` | `tool/validation/` | 启动 `main.exe`（排队 `tonemap 0` / `bloom strength 0`）、按 `-Shots`/`-Scenes` 配对采图、等产物、跑四角检查、把日志与截图归档到 `artifacts/<name>/<timestamp>/`，任一步失败即非零退出 |
| `bmp_reader.py` | `tool/validation/` | 24bpp BMP 读取（负高度 top-down、行 4 字节对齐、BGR 通道序）统一成屏幕坐标，并提供 sRGB 编解码 |
| `console_inject.ps1` | `tool/validation/` | 往运行中进程的控制台注入命令行；**一个进程内切场景多拍**靠它（原因见 §1.5.4） |

结构与 `tool/ue-lite-final-validation.ps1` 一致（串行、日志落 `artifacts/<name>/<timestamp>/`、非零退出即中止）。

**`curve` 的列剔除规则**（全部**模型驱动**、必须显式报出剔除数量，否则等于挑数据）：

- **交叉列（整列丢弃）**：用**解析期望行**判断两条曲线的间距小于 `--crossing-margin`（默认 20 px）
  就整列丢弃——用期望值而不是实测值，才能在"其中一条没被检到"时也正确判出重叠；
- **贴边 / 平台过薄（按曲线各自丢弃）**：曲线贴到绘图区上下边界（带子被裁一半）或亮核厚度
  明显低于该列应有的厚度（被运行时 UI 叠加、残余交叉光晕啃掉）时，只丢这一条曲线的这一列，
  不牵连另一条——mode 0 的两条曲线长期各贴一边，整列丢弃会丢掉八成数据；
- "应有厚度"由等宽描边按该列斜率折算：`2·halfWidth·sqrt(1+slope²)`（§1.5.2 的等宽描边）。

实测：这几条规则把 mode 0 的 GGX 假误差从 7 列 1.7~5.1 px 压到 0 列，最大误差 0.550 px。
**图例不在剔除清单里**：它在绘图区外面（§1.5.2），扫描范围根本到不了它，
所以不需要为它引入保留区参数——这也是"图例必须放外面"的第二个理由。

工具必须满足（已满足）：

- 输入是**场景名 + 截图名 + 期望曲线函数**，输出是可直接贴进归档记录的误差表；
- 自己从 quad scale + `fov` + aspect 反推 `uv ↔ 像素` 映射（§1.7.2.1 与 §1.3），
  **不假设整帧就是 `uv∈[0,1]`**；
- 不允许要求人工目视读数，也不允许把识图模型的描述当输入。

**测量前必须先把色彩链路变成可逆的**：排队的 console 命令里加 `tonemap 0`（Linear clamp）并清零 Bloom，
再截图。默认色调映射不是线性编码，不这么做的话"像素值 ↔ 曲线值"无法解析反推，只能做相对比较。
这条要求在 `M-07` 里被再次证实：走默认 ACES 的旧截图连"按描边色找曲线"都会退化
（见 case 记录里的自测表）。

**两个必须避开的坑（实测）**：

1. **左上角有 UI 叠加**：`--no-dev-ui` 只关 ImGui，运行时 UI（RmlUi）仍会画一条约 186×14 的控件
   （实测 bbox `x[31..216] y[32..45]`，mode 0 图上延伸到 x≈232）。它近白、会被"按描边色投影"的判定
   误当成曲线。工具因此加了两道闸：UI 盒子跳过 + **彩度门限**（通道差 ≥0.15；底板 0.04、网格 0.07、
   UI 0.04，而所有曲线描边色 ≥0.50）。做像素测量时不要把测量区放在左上角；
2. **亮核平台中心才是曲线位置**：`PlotCurveLine` 在 `|value - plotY| <= width` 处恒为 1，形成固定厚度的
   平台；若改用 argmax 或整段重心，靠近画面上下边缘时被截断的平台会把位置拉偏
   （实测偏 6 px 量级）。工具因此要求平台完整落在画面内，否则丢弃该列；
   两条曲线**交叉**的列颜色互相混合，也整列丢弃并单独计数。

可用于回归的旧截图（`<resourcePath>/Generated/Screenshots/`，1280×720 24-bit BMP、行序 top-down、
像素数据偏移 54）：`brdf_plot_ggx_charlie.bmp`、`plot_cloth_solo.bmp`、`brdf_plot_g1_only.bmp`、
`brdf_plot_g1_framefill.bmp`、`brdf_plot_g1_after_vfix.bmp`、`uv_probe.bmp`。
其中 **`brdf_plot_three_cases.bmp` 与 `brdf_plot_two_cases.bmp` 是失效样本**（面板没盖满 + 重叠），
只能用来验证工具能不能识别出"不该测的图"。

⚠️ **这些旧图都不是当前 shader 状态的样本**：`plot_cloth_solo.bmp` 摄于 `plotY = 1 - uv.y` 时期
（工具实测它在**镜像**假设下误差 0.23 px，与当时代码一致），`brdf_plot_g1_only.bmp` 更早，
连场景 scale 都还没是现在的值。它们只适合验证"工具能否识别朝向与失效样本"，
**竖直朝向与曲线判定的规范样本是 `m07_*` 这一批**（见 case 记录）。

### 1.5.4 运行与截图

启动参数在 `source/engine/launchOptions.cpp`。**完整可取用的参数只有这些**（其余 token 是硬错误，
进程会打印 help 并退出）：

| 参数 | 用途 |
| --- | --- |
| `--help` | 打印 usage |
| `--dev-ui` / `--no-dev-ui` | 强制打开 / 关闭 ImGui 开发工具 |
| `--initial-scene <scene-path>` | 覆盖 `config/config.json -> initScene` |
| `--console-command <line>` | **可重复**，按命令行顺序排队提交 |
| `--console-command-delay <frames>` | 排队命令提交前等待的帧数，**默认 60** |
| `--shader-force-rebuild` | 强制重编并重发布全部启动期 shader 产物 |
| `--worker-thread-count <1\|2>` | 覆盖 worker 模式，其它值被拒绝 |
| `--exit-after-tests` | 成功退出 0 / 失败退出 2；**必须搭配下面三条之一** |
| `--shader-reload-test` / `--shader-compute-reload-test` / `--world-graph-transaction-test` | 三条受支持的 runtime test，互斥且必须串行 |

> ⚠️ **不存在的参数**：`--framesmoke`、`--hair-validation-test` 在 `source/` 里没有任何实现。
> 旧文档（`neox-b-f-3725-*` 系列）里出现的这两个 flag、以及 `scenes/SC_b_f_3725_p0.json`、
> `skin_p4_*.bmp` 等产物，**今天都不存在**——不要把它们抄进新的 case 脚本。

```powershell
.\build\bin\main.exe `
  --initial-scene "Maps/SC_paper_case_brdf_plot_g1/SC_paper_case_brdf_plot_g1.json" `
  --console-command "tonemap 0" `
  --console-command "bloom strength 0" `
  --console-command "screenshot my_case.bmp" `
  --console-command-delay 120 --no-dev-ui
```

**可脚本化的 console 命令**（都走 `--console-command`，与手输同一条解析路径）：

```text
debugview <mode>                        tonemap <0..3>      0=Linear clamp 1=Reinhard 2=Hable 3=ACES
bloom <strength|threshold|knee|clamp> <value>
loadworld <scene>                       environment <v>
windgust on|off|once                    windstrength <0..1>
camera get|position <x y z>|lookat <x y z>|pose <px py pz tx ty tz>
shaderreload changed|all                shadercache stats
screenshot [file.bmp]
```

- **没有材质参数的 console 命令**。参数扫描只能预先写成多个 `MI_*.json` 再由场景引用，
  这正是现有 `SC_paper_case_*` 场景的做法；
- `--console-command-delay` 默认 60。**必须给足延迟**：第 1 帧时环境贴图、CLUT 与 swapchain 都未稳定，
  立即 `screenshot` 只会拿到无效画面；
- 截图写入 `<resourcePath>/Generated/Screenshots/`（绝对路径原样使用），扩展名强制 `.bmp`；
- 截图是该帧一次性 GPU 回读 + 一次 `WaitIdle`，因此不依赖窗口是否在前台。

**脚本化截屏的坑（`M-07` 实测补充）**：

1. **不要把窗口最小化**。`Start-Process -WindowStyle Minimized` 会让 client area 尺寸失效，
   swapchain out-of-date，渲染线程以 `vk::Queue::presentKHR: ErrorOutOfDateKHR` 退出。
   （`-WindowStyle Hidden` 在本仓库用过但未验证，采用前先确认尺寸仍有效。）
2. **`Start-Process -ArgumentList` 不自动加引号**。传数组时 `--console-command "debugview 67"`
   会被拆成两个 token，触发参数错误并打印 help。
3. **composed 材质 shader 在场景加载期编译**，不出现在启动那批 `Shader build: entries=...`
   汇总里；看那行判断不出材质 shader 有没有重编。改完 shader 想确认生效，用 `--shader-force-rebuild`。
4. **一个启动批次只能拍到一张图**。`ConsoleSubsystem::QueueLaunchCommands` 在 delay 帧后把整批命令
   **一次性**交给 DebugConsole（`ExecuteQueuedCommands` 一次 drain 全部），而 `RenderSystem` 的
   `pendingScreenshotPath` 只保留**一个**待处理路径——后一条 `screenshot` 覆盖前一条。
   所以"同一进程内 `loadworld` 切场景、每切一次拍一张"**无法**用 `--console-command` 表达：
   必须用 `tool/validation/console_inject.ps1` 往控制台**分时注入**命令行
   （DebugConsole 的交互输入走 `_kbhit/_getch`，读的是控制台输入缓冲区，不理会 stdin 重定向）。
   `tool/validation/run_paper_case.ps1` 已经把这件事封装好；
5. **`--no-dev-ui` 不等于画面干净**。运行时 UI（RmlUi）仍会在左上角绘制控件（实测
   `x[31..216] y[32..45]`），像素测量必须避开该区域（见 §1.5.3）；
6. **注入类工具必须另起进程**：`AttachConsole` / `FreeConsole` 会改动调用进程的标准句柄，
   之后从同一进程再启动子进程会报 "getting the console mode" 失败——`console_inject.ps1`
   因此把注入动作放在一次性 pwsh 子进程里。

### 1.5.5 现有可复用探针场景

场景与材质实例是**多对一**复用的：`Common/Meshes/SM_plot_quad*.json` 决定面板用哪个 MI，
场景只负责把面板摆到画面里的位置与大小。

**四个探针场景的机位与 scale 现已统一**：相机 `[0,0,6]`、`fov=45`、
mesh scale `[2.485281, 1.397971, 1.0]`（= 该机位下的 `halfWidth` / `halfHeight`，
即面板精确填满画面，见 §1.7.2.1）。新增探针场景必须照这条来，否则坐标轴版式会整体偏移。

| 场景 | 面板 mesh | MI（实际模式） | 用途 |
| --- | --- | --- | --- |
| `SC_paper_case_brdf_plot_g1` | `SM_plot_quad_g1` | `MI_plot_smith_schlick`（mode 2） | **mode 2 像素测量用这个** |
| `SC_paper_case_plot_cloth_solo` | `SM_plot_quad` | `MI_plot_ggx_charlie`（mode 0） | **mode 0 像素测量用这个** |
| `SC_paper_case_plot_hair_solo` | `SM_plot_quad_hair` | `MI_plot_hair_paths`（mode 1） | **mode 1 像素测量用这个**（数值对照仍缺期望值模型，见 `H-20`） |
| `SC_paper_case_brdf_plot_uv` | `SM_plot_quad_uv` | `MI_plot_uv_probe`（mode 3） | UV 探针，确定 texCoord 与屏幕方向的对应；**不画坐标轴** |
| `SC_paper_case_brdf_plot` | `SM_plot_quad` + `SM_plot_quad_hair` + `SM_plot_quad_g1` | 三个 MI（mode 0 / 1 / 2），各面板 scale `[1.3,1,1]`、x=∓2.4 / 0 | 三面板并排浏览，**溢出且互相重叠，不用于测量**；scale 也不是新约定，**不要**拿它做测量 |

> 三面板时代的 `SC_paper_case_brdf_plot` 是按早期**错误的 fov 假设**摆放的，面板会溢出画面并在屏幕上
> 互相重叠。做像素测量请一律用 solo 场景。
>
> 另外注意 `MI_plot_uv_probe` 归属在 `SC_paper_case_brdf_plot_g1/Materials/` 下，而
> `SC_paper_case_brdf_plot_uv/` 目录里只有场景 JSON、没有 `Materials/`——新增面板时不要把
> MI 放到同名场景目录下猜路径。

## 1.6 共享 B｜架构一致性设施

架构侧贴近 UE，这部分是**共享 case**，每个模型落地时都要回到这里核对一次。

| 共享 ID | 来源 | 验证目标 | 形式 |
| --- | --- | --- | --- |
| `A-01` | UE 官方 [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)｜各模型输入表 | 每个模型的 Material Inputs 与 UE 同名输入的语义一致（不是数值一致，是**语义**一致） | 架构核对 |
| `A-02` | UE 5.8 Legacy GBuffer 布局 + 本仓库 `documents/plan/rendering/deferred-gbuffer-ue-aligned-plan.html` | ShadingModelID、SelectiveOutputMask、customData 的位分配与解码自洽；每个模型只消费自己声明的位 | 架构核对 |
| `A-03` | 本仓库 `documents/rendering/shader-structure-and-material-function.md` | Forward / Deferred 两条路径下同一模型的着色结果在有定义处一致；差异必须能归因到路径本身 | 场景对照 |
| `A-04` | UE 官方文档 + `documents/rendering/*.md` 各合同 | 每个模型的 Debug View 能单独输出该模型的关键中间量（`D` / `visibility` / path 分量 / profile），用于把误差定位到公式而不是资产 | Debug 截图 |
| `A-05` | 本仓库阴影合同（`documents/plan/rendering/*shadow*`） | Shadow Caster 路由正确：每个模型在 shadow pass 用与主 pass 一致的 alpha / 双面 / 位移语义 | 架构核对 |
| `A-06` | 本仓库 GBuffer 编码（`engine/gbufferCodec.glsl`） | **GBuffer 往返精度上限**：每个模型的 customData / 法线 / 各向异性编码精度足够支撑该模型 case 的判定门限（见 §1.5.2） | 数值 | 
| `A-07` | UE 5.8 Legacy 槽位 + `source/material/materialAssetUtils.h` | **未实现的 ID 不得被静默降级**：ID 10 / 12 目前无 dispatch case，会落到 `default:` 变成 DefaultLit | 架构核对 |

### 1.6.1 已实测的架构事实（验证前必须先知道）

2026-09-12 的旧实现清理改写了这一节：原先"某个已删模型在某条路径缺 case / 被后处理改写"的记录
随实现一起失效，下表按**清理后**的状态重写。删除清单与留存的接口面见
`documents/plan/rendering/archive/README.md`。

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 两条路径都只有 Unlit + DefaultLit 有 evaluator | `engine/forwardLighting.glsl`、`engine/deferredLighting.glsl` | 其余 ShadingModelID 一律落到 `default:` = DefaultLit，无 assert、无警告、无校验。**重建一个模型 = evaluator + 两条路径的 case + customData 编码 + 材质校验，必须同一次改完**（`A-07`） |
| 只有 `deferredLighting` / `forwardOpaque` / `sky` / `forwardTransparent` 写 `sceneColor` | `config/renderGraphConfig.json` | 旧的 3 路光照拆分（diffuse / nonDiffuse / transmission）+ sssSource + 两次 SSS 模糊 + sssComposition 随 SSS 实现删除；`deferredLighting` 回到单输出 `sceneColor`（`loadOp=clear`）。将来重建 SSS / ThinTranslucent 时由该模型自带合成 pass，**不要**再让所有模型为它多写一路输出 |
| `VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT` 现在是编译期 `#error` | `engine/passTemplate/base.frag.glsl` | 打开该 macro 会明确报错，而不是静默产出错误画面；重建 ThinTranslucent 要连 renderMode / 混合状态一起设计 |
| `GBUFFER_HAS_PRECOMPUTED_SHADOW_MASK`（0x20）never written / never read | `common/shadingModel.glsl:31` | 声明存在但语义为空，不要把它当成可用证据 |
| `ShadingModelMask()` 无调用者 | `common/shadingModel.glsl:38` | 死代码，不要引用为"已实现分发"。对照：`ShadeForwardSurface()` 现在**有**调用者（`materialForwardOutput.glsl` 经它出片元色），不再是死代码 |
| 已删模型的 Debug 管线留下"空壳" | `engine/materialDebugView.glsl` 的 `SetMaterialDebug*Data()`、`engine/materialForwardOutput.glsl` 的 74-79 保护 | 模式编号（UE 对齐）与 setter 仍在，但**没有任何生产者写这些字段**，用它们做判定只会读到 0。重建模型时把 setter 接回即可 |
| Debug View 模式 92 未定义 | `engine/materialDebugView.glsl` 与 `UiSubsystem::GetDebugViewName` 都没有 | UI 显示 "Unknown"，属已知空号 |
| `ENABLE_DEBUG_VIEW` 只在非 `NDEBUG` 构建生效 | `source/shaderCompiler.cpp:1634-1646` | **Release 构建里所有 Debug View 失效**（本地 build 是 Debug，当前可用）；做 Debug 类 case 时必须确认构建类型 |
| GBuffer 编解码仍保留全部模型的槽位语义 | `engine/gbufferCodec.glsl` | 这是**有意留存的接口面**：Eye / Hair / Cloth / Skin / Foliage 的编码与解码分支都在，但当前没有 evaluator 消费它们。case 测量不要把这些分支读成"模型已实现" |
| 四个模型 LUT 生成 compute 已删除 | `shader/glsl/generator/`、`source/pipeline/`、`source/render/*/` | set 3 的 binding 10..13（Hair / Eye / Cloth LUT）连同 graph 的 pass input、pass 材质 JSON 的 texture 槽一起下线。重建时这三处必须同步加回，且 **binding 顺序必须与 graph 的 input 顺序一致** |
| `pass/generate/*Paramter.glsl`、`shader/spv/shader-build-cache.json` 可能残留已删 pass 的条目 | `shader/glsl/pass/generate/`、`shader/spv/` | 生成物在启动期重写、manifest 由编译器自己维护；**不要手工编辑 manifest**（AGENTS.md），判断是否生效以启动日志为准 |

### 1.6.2 GBuffer 往返精度上限（决定 case 的判定门限）

| 量 | 存储 | 精度上限 | 对 case 的影响 |
| --- | --- | --- | --- |
| 法线 | `gbufferA` = `A2B10G10R10_UNORM`，`dir*0.5+0.5` | 每通道 10 bit，解码后 ≥ ~1e-3 绝对误差 + 非零角度误差 | 法线相关 case 不能要求 1e-4 级一致 |
| packed byte（ShadingModelID + mask） | fp16 存 `k/255` | fp16 步长 4.88e-4，解码容差 ±0.5/255 | 不要期望与 `k/255` 位精确相等 |
| Cloth 各向异性 / cross | `gbufferF.w` 5+5 bit 打包 | 每轴 1/31 ≈ 0.0323 | `S-07` / `S-14` 的判定门限只能按 0.03 量级给 |
| ClearCoat 底层法线偏移 | `gbufferD` 编码，偏置 `128/255`，范围 ±0.5 | 顶层与底层法线偏离越大，角分辨率越差 | `C-07` 必须分倾角区间给误差 |
| Eye profile pair | `EncodeEyeProfilePair` 返回整数 0..1023 | fp16 下无损 | Eye profile 相关 case 不受量化限制 |
| Cloth 反射率（deferred decode 强制 `metallic = 0`） | `gbufferCodec.glsl:529` | —— | Cloth 的 deferred 结果与材质 metallic 无关，这是设计而非偏差 |
| Subsurface(2) 的 opacity 被覆盖为 1.0 | `gbufferCodec.glsl:433-435` | —— | ID 2 的透明度不往返，不要用它验证 alpha |

## 1.7 共享 C｜观察条件与场景骨架

### 1.7.1 球阵基线 `SC_sphere_array`

11 × 11 = 121 个球的参数阵，作为**所有模型的参数标尺**：

| 轴 | 参数 | 范围 | 建立方式 |
| --- | --- | --- | --- |
| `y00..y10` | roughness | 0.05 → 1.0，步长 0.095 | `u_pbrFactors.x = 0.05 + 0.095·y` |
| `x00..x10` | metallic | 0.0 → 1.0，步长 0.1 | `u_pbrFactors.y = 0.1·x` |

球心 `world = (-4.5 + 0.9x, -4.5 + 0.9y)`，间距 0.9；相机 `[0,0,16]`、`fov=80`；环境 `sunset.exr`。

> ⚠️ **轴容易记反**：文件名里的 `x` 是 metallic，`y` 是 roughness。做新扫描时按上面这条公式核对，
> 不要凭 `x=0.05` 的外观猜（`x10_y10` 是"全金属 + 最粗糙"这一角）。

固定 `u_tintColor = (0.5, 0.5, 0.5, 1.0)`、`u_pbrFactors.z(AO) = 1.0`、`u_pbrFactors.w(reserved) = 0.0`。
每次做新的参数扫描时，**端点必须落在与球阵相同的约定上**，这样两个场景的结论可以互相引用。

### 1.7.2 灯光 / 相机 / 曝光的固定约定

- 主光方向与光源尺寸在可比较案例之间尽量固定；
- 背景使用低干扰的中性颜色，避免背景色被误认为材质散射颜色；
- **曲线探针必须关掉环境背景或确认面板铺满画面**——本项目已因 `sunset.exr` 的暖色背景导致一次测量失效。
  已实测的判定办法：读面板**四角像素**，若不是 shader 底板色 `vec3(0.13,0.14,0.17)` 经色调映射后的值，
  就说明背景漏进来了（见 §1.7.2.1）；
- 默认曝光、色调映射与 Bloom 保持一致，另存艺术化 Beauty 视图时必须记录差异；
- 透明、Alpha Clip、Eye 与 ThinTranslucent 必须记录其 Forward / Deferred 限制。

#### 1.7.2.1 像素测量的前置检查：面板是否铺满，以及探针面板的成像约定

**探针约定（2026-09-12 起强制）**：`plot_quad.obj` 的顶点是 ±1，所以探针场景的
**scale 必须取 `(halfWidth, halfHeight)`**——即该机位下视锥在面板平面上的半宽 / 半高：

```text
相机 z=6、fov=45（水平）、16:9 时：
  halfWidth  = 6 * tan(22.5°) = 2.485281        halfHeight = halfWidth / (16/9) = 1.397971
  四个探针场景的 mesh scale 都是 [2.485281, 1.397971, 1.0]
  ⇒ uv 0..1 正好覆盖整幅画面：uv.x = (x+0.5)/W，uv.y = 1 - (y+0.5)/H
```

这条约定的两个作用：① 画面像素与 uv 是一一对应的仿射关系，测量脚本不再需要从
scale/fov/aspect 反推可见范围（虽然它仍会核对）；② `M_brdfPlot` 的像素版式
（绘图区留白、字号、刻度长度）就是用 `fwidth(uv)` 换算的，只有在"面板铺满"时
那些像素值才等于屏幕像素。`measure_plot_curve.py uv-map` 会直接检查
`scale == (halfWidth, halfHeight)`，不满足就判不通过。

**绘图区**：曲线只画在面板内缩的矩形里（留白给说明、图例、刻度数值与轴名），矩形由
`u_plotRectPixels`＝`(左, 下, 右, 上)` 像素给出，默认 `(92, 52, 18, 85)`，
对应 1280×720 下的轴框 `x ∈ [92, 1262]`、`y ∈ [85, 668]`（上留白 85 px 里依次是
说明行与最多 3 行图例，下留白 52 px 里是 x 刻度值与 x 轴名）。测量脚本用 `--plot-rect` 传同一组值，
并会**从图上检出轴色边框位置做交叉核对**（实测偏差 ≤1 px），
所以"版式改了但脚本没跟上"会直接报错而不是给出错误的误差表。

**判定规则**：

| 现象 | 结论 |
| --- | --- |
| 四角 = 底板色（`vec3(0.13,0.14,0.17)` 经 sRGB 编码，实测 8-bit `(101,105,115)`） | 面板铺满，可用于像素测量 |
| 四角出现高亮或彩色（HDRI 泄漏） | 面板没盖满，**该截图不能用于测量** |
| 四角正常但左上角出现近白小色块 | 运行时 UI 叠加，不是背景泄漏；测量避开 `x[31..216] y[32..45]` 即可（§1.5.3） |

已实测：`brdf_plot_three_cases.bmp` 与 `brdf_plot_two_cases.bmp` 的四角背景是变化的（两种不同亮度），
属"面板没盖满 + 互相重叠"的失效样本，**不得用于测量**。
`tool/validation/measure_plot_curve.py panel` 已把上表实现成自动判定，返回非零即视为该图不可用。

### 1.7.3 观察证据的分级

1. `Beauty` 是默认机位；
2. `Backlight` 只在验证皮肤、叶片、头发或薄透射时启用；
3. `Debug` 只在出现材质绑定、法线、切线、Alpha 或 Shading Model ID 疑问时启用；
4. `KeyOnly`、Forward / Deferred 对照和性能基线等视觉结果稳定后再补做。

不要为每个观察条件复制一套模型资源；同一资产用不同机位 / 灯光配置表达。

### 1.7.4 辅助材质

`M_unlit`、`M_vertexColor`、`M_measureGrid`、`M_shadow` 作为**辅助或调试材质**：用于底座、展示台、
背景、发光源、曲线探针本体与测量网格。它们必须在记录中明确标注，**不得与主案例结论混在一起**，
也不为其建立独立的论文 case。

### 1.7.5 可复用场景索引（现状盘点）

2026-09-12 清理后，`<resourcePath>/Maps/` 下是 **15 个场景、268 个对象**（mesh 219 / camera 15 /
environment 14 / directionalLight 14 / pointLight 3 / spotLight 2 / sunLight 1）。
**没有任何 `terrain` 对象，也没有 `Terrains/` 目录。** 环境只有两种：`hdri`
（`Common/Environments/sunset.exr`，cube 512）与 `proceduralSky`（cube 128）。
删掉的 9 个案例场景（`SC_hair_showcase*`、`SC_simple_character`、`SC_marble_bust_01`、
`SC_foliage_potted_plant_02`、`SC_paper_case_{clearcoat_sweep,cloth_sheen,subsurface_models,thin_translucent}`）
不在此表，重建对应模型时要重新设计 case 场景——**不要试图找回旧场景**，清单见 `archive/README.md`。

其中直接服务于本计划的是下面这些——**新增 case 前先确认能不能复用它们**，不要为同一件事再造一个场景。

| 场景 | 结构（实测参数） | 覆盖的模型 | 服务于哪些 case |
| --- | --- | --- | --- |
| `SC_sphere_array` | 11×11 = 121 球 + `SM_axis`；相机 `[0,0,16]` `fov=80`；dir 光 int1 + point 光 int250；121 个 `M_pbr` 参数阵 | DefaultLit | `D-08` `D-10` |
| `SC_paper_case_brdf_plot` / `_g1` / `_uv` / `_plot_cloth_solo` / `_plot_hair_solo` | 曲线探针面板（面板材质由 `SM_plot_quad*.json` 直接给出，不经 MI） | 全部曲线类 | §1.5.5 |
| `SC_car_showcase` | 6 mesh（车身 8 槽 + 4 轮 + 地面），**12 个 MI 现在全部指向 `M_pbr`**；相机 `[7.4,4.884,8.584]` `fov=24` | DefaultLit（原 ClearCoat + ThinTranslucent） | 重建 ClearCoat 后由 `C-12` 重新 author 车漆：**当前车漆没有清漆层**，灯罩也不是 ThinTranslucent |
| `SC_speedtree` | Oak（6 槽，`.stsdk`）+ 50×50 地面；树用 `M_speedtree`（DefaultLit），非树皮槽用 `OpaqueClip` + `u_alphaClipThreshold=0.1`；7 个 MI = `M_speedtree`×6 + `M_pbr`×1 | DefaultLit + WPO | 不在本计划范围（风场另有专项计划） |
| `SC_bistro_exterior_modular` | 街景 kit，68 个摆放 / 472 个材质槽，**111 个 MI 全是 `M_pbr`** | DefaultLit | 只作 Large-scene 压力场景 |
| `SC_scene03` | `SM_demo_scene` → `MI_measureGrid`（`M_measureGrid`） | 辅助 | 网格标尺 |
| `SC_scene02` / `SC_sculpture` / `SC_uds_mountain_range` | 普通学习场景（`M_pbr` / 空 MI 的默认材质） | DefaultLit | 仅作加载与观感检查 |
| `SC_sifi_head` | 头模（2 个 MI 均为 `M_pbr`） | DefaultLit | 见下方已知问题 |

**已知问题场景 / 资产**：

- `SC_sifi_head`：两个头模放在**完全相同的 transform `[1,0,1]`** 上互相重叠，且是 `M_pbr` +
  `u_emissiveStrength=20`，不适合做任何对照 case；
- `SC_scene02` / `SC_scene03` / `SC_sculpture` / `SC_sifi_head` 的 `name` 字段都还是过期的 `"Scene 01"`；
- `SC_scene01` 用了场景类型 `sunLight`——**全仓库唯一一处，且不在 `AGENTS.md` 记录的类型列表里**；
  启动期 UI 场景选择器会跳过它（日志有 `skipped an unsupported scene asset`），实际不可加载；
- `Common/Meshes/SM_axis.json` 的内部 `name` 是 `"Vilking Room"`（仅命名不一致，不影响加载）。


### 1.7.6 可复用的资产与 profile

做论文 case 时优先复用这些已入库的资产，避免重复导入：

| 类别 | 路径 | 内容 |
| --- | --- | --- |
| 基础几何 | `Common/Source/Models/` | `sphere.obj`（球阵 / 扫描的主力）、`plot_quad.obj`（曲线面板）、`material_preview_ball.obj`、`axis.obj`、`viking_room.obj` |
| Pass 材质实例 | `Common/Materials/Pass/` | 6 个：bloom ×3、deferredLighting、sky、toneMapping |
| 环境与光照 | `Common/Environments/` | 与 `environment_templates.jsonc` 配套的 HDR / SH 输入 |

2026-09-12 清理删掉的 profile 与 LUT（`Common/Profiles/{Eye,Hair,SkinLuts,Subsurface}/`、
`Generated/Runtime/{eyeCausticLut,hairAzimuthalLut}.json`）**没有备份**：重建对应模型时，profile/LUT 要
按论文重新设计并重新入库，不要指望从旧文件里抄参数（旧参数语义见 `archive/` 下的旧合同，但必须逐条与论文核实）。


### 1.7.7 资源组织
```text
<resourcePath>/
  Maps/SC_<case>/SC_<case>.json
  Maps/SC_<case>/Meshes/SM_*.json          # 一个 mesh 一个材质槽，指向一个 MI
  Maps/SC_<case>/Materials/MI_*.json
  Maps/SC_<case>/Terrains/TR_*.json
```

资产入库必须完成：

- 网格对象与材质槽拆分符合主材质 / 辅助材质边界；
- UV、Normal、Tangent 与 `tangent.w` 语义稳定；
- 贴图颜色空间与通道含义写入纹理描述或材质合同；
- `SM_*.json`、`MI_*.json`、纹理 JSON 与场景引用全部使用资源根相对路径；
- 外部授权与署名文件随运行时资源一起保存；
- 不把 DCC 临时缓存提交为运行时依赖。

## 1.8 实测确立的材质与着色器契约

1. **材质实例不能重复材质默认值**：`Material.LoadFailed: Material instance parameter redundantly
   repeats M_ default "<name>"`。扫描数组要刻意避开默认值。
2. **Material Surface 阶段不能 include 依赖全局 UBO 的头**。`common/lighting.glsl` 引用
   `uboVP` / `csmParameters` → `error: 'uboVP' : undeclared identifier`。
3. **Material Surface 阶段不能 include 依赖 sampler 的头**。当时的例子是已删除的
   `engine/hairScattering.glsl`：它的 `SampleHairAzimuthalLut()` 引用 `hairAzimuthalLut` → 编译失败。
   重建模型时同样的坑仍然成立（surface 阶段不能碰 sampler）。
4. **部分材质需要 `parameters` 之外的自有资产字段**。当时的例子是 `M_preintegratedSkin` 的实例必须带
   顶层 `"skinLut": "..."`；这类"实例级资产字段"在做新模型时会重新出现，实现时要同时给出校验与文档。
5. **mesh `texCoord.y` 沿屏幕向"上"增大**（`M-07` 实测改正：原记录写的是"向下增大"，结论相反）。
   曲线值 1 出现在上方**不需要**翻转；`M_brdfPlot` 里曾经的 `plotY = 1 - uv.y` 正是基于旧结论写的，
   把探针图整体镜像了，已在 `M-07` 中删除（见 §1.3）。
6. **相机的 `fov` 是水平视场角**。反推可见范围时 `halfWidth = distance * tan(fov/2)`，
   `halfHeight = halfWidth / aspect`；`M-07` 用 mode 3 探针实测反推核对过（§1.3），弄反会让验证差出上百像素。
7. **swapchain 输出是 sRGB 编码的**：`tonemap 0` + bloom 0 + exposure 1 时
   `pixel8 = 255·sRGB_encode(clamp(shaderLinear))`。像素测量必须先解码再与 shader 值比较；
   不先把色调映射切到 `tonemap 0`（并清零 Bloom）时，色彩链路不可逆，只能做相对比较。

拆分公共头时遵守：**只搬不改**，被拆走的行逐字节复制，原文件改为 include，保证数学定义在仓库里
仍然只有一份。

## 1.9 两条不可绕过的验证纪律

### 1.9.1 曲线与分布类必须做像素级数值对照

1. 用 shader 里的**真实实现**求值并绘制（不是照论文重抄公式）；
2. 用 `--console-command "screenshot ..."` 采图（切场景时改用 `console_inject.ps1` 分时注入，见 §1.5.4）；
3. 直接解析 BMP 像素，测出曲线在每一列的行位置（亮核平台中心，见 §1.5.3）；
4. 用同一套公式独立算出解析期望行位置；
5. 逐列比对，**误差在 1 像素量级才算通过**。

> `M-07` 已完成该门限的可行性验证：mode 2 / mode 0 的实测最大误差分别是 0.484 px / 0.486 px
> （直立假设），而镜像假设下是 ~700 px。工具与判据见 `tool/validation/measure_plot_curve.py`。

### 1.9.2 识图模型不能作为验证证据

本项目已出现识图模型对同一张图给出**自相矛盾**读数的情况（把三面板图读成"一张合并的图"）。
它只能提出线索，不能判定通过。曾经基于识图定性描述得出的"已验证"结论，在发现坐标轴假设错误后
已全部降级。

### 1.9.3 每个案例必须留下可追溯记录

场景名、材质实例、关键参数、截图文件名、对照结论，以及**差异归因**（实现不同 / 参数面缺失 / 资产差异 / 自定近似）。
记录写在 `shading-model-case-records.md`：`artifacts/` 被 `.gitignore` 忽略，**只有那份记录是持久载体**。

## 1.10 测试治理

- 模块与纯逻辑测试必须使用 GoogleTest，并通过根 `vulkanlearn_add_gtest(...)` 注册；
- 唯一受支持的运行时测试命令是 `--shader-reload-test`、`--shader-compute-reload-test` 与
  `--world-graph-transaction-test`，且必须串行执行；
- 本计划本身不新增运行时测试命令，也不把场景截图当作数值正确性的替代品；
- 曲线 / 能量类数值验证走**仓库外脚本或离线工具**（见 §1.5.3），不进运行时测试命令。

---

# 2. 逐 Shading Model 对齐清单

每个模型固定五段：① 对齐状态、② 论文来源、③ 算法来源、④ Case 列表、⑤ 专属约定。

状态：☐ 待办 ｜ ◐ 进行中 ｜ ☑ 已对齐 ｜ ⛔ 受限（缺引擎参数，做不了真复现）

> **读法（2026-09-12 起）**：除 DefaultLit 外的模型，其旧实现已删除，① 一律读作"待按论文实现"，
> ③ 表里的"实现锚点/位置"是**历史位置**（文件已不在仓库），重建后必须重新填写。
> ② 论文来源与 ④ Case 列表不受影响——它们是按论文写的验证方案，对新实现同样适用。
> 但 ④ 里凡是把**已删产物**本身当成被测对象或证据来源的条目（例如以 `pass/sssComposition.frag`、
> `generator/subsurfaceLookupTables.comp`、`engine/subsurfaceProfileFilter.glsl`、
> `generator/eyeCausticLut.comp` 为对象的 case），在新实现落地前**无法执行**：它们描述的是新实现
> 必须满足的性质，重建时要把锚点改成新文件、并顺手确认判定门限仍然成立。
> DefaultLit 的 `M_pbr` 仍在仓库里，但它的几何项用法本身有已知问题（`D-20` / `D-21`），
> 重做 DefaultLit 时要一并处理。

| 模型 | 旧实现状态 | 现在 |
| --- | --- | --- |
| ClearCoat / Cloth / Eye / Hair / Subsurface ×3 / ThinTranslucent / TwoSidedFoliage | 已删除（2026-09-12） | ☐ 待按论文实现 |
| DefaultLit | 仍在（`M_pbr`） | ◐ 有实现，但有 `D-20` / `D-21` 待处理 |

## 2.1 DefaultLit

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中 |
| 实现锚点 | `M_pbr`；`common/lighting.glsl` `CalculateDirectLightingLobes()` / `CalculateSpecularIbl()`；`common/microfacetDistribution.glsl` |
| 已完成 | 球阵基线；**mode 2 曲线逐列像素对照通过**（2026-09-12，Schlick 161 列最大 0.484 px、Smith 23 列最大 0.418 px，画的是引擎自己的 IBL 变体，不是论文的 `k=α/2`） |
| 未完成 | `D-03`（`k=α/2` direct 变体的新 mode）、split-sum 误差边界、几何项端点 |
| 阻塞 | 无：`M-07` 与 §1.5.3 的测量脚本都已就绪（§1.3 / §1.5.3） |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Karis 2013, *Real Shading in Unreal Engine 4* | GGX 选择、`k=α/2` 直接光几何项、`k=(r+1)²/8` IBL 变体、split-sum 近似 | [书库 `#karis-ue4`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#karis-ue4) · [PDF](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) |
| Burley 2012, *Physically Based Shading at Disney* | Principled 参数化、directional albedo、G 函数对比 | [书库 `#burley-disney`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#burley-disney) · [PDF](https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf) |
| Heitz 2014 JCGT, *Understanding the Masking-Shadowing Function* | Smith 可见性、微表面归一化 | [书库 `#heitz-masking-shadowing`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#heitz-masking-shadowing) · [JCGT](https://jcgt.org/published/0003/02/03/) |
| Hoffman 2013, *Background: Physics and Math of Shading* | 公共物理与数学、参数化边界 | [书库 `#hoffman-physics-math`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#hoffman-physics-math) |
| Neubelt & Pettineo 2013, *Crafting a Next-Gen Material Pipeline* | grazing 高光、specular AA、几何项常见错误 | [书库 `#neubelt-order-1886`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#neubelt-order-1886) |
| PBRT 4ed | Conductor / Dielectric BSDF、微表面理论、白炉测试 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) · [pbr-book.org](https://pbr-book.org/4ed/contents) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| `DistributionGGX` | `common/microfacetDistribution.glsl:14` | [Karis 2013](#karis-ue4) §3 式(2)；[PBRT 4ed｜Microfacet Distributions](https://pbr-book.org/4ed/Reflection_Models/Microfacet_Distributions) | 入参是 perceptual roughness，函数内做 `a = roughness²` |
| `GeometrySchlickGGX` | `common/microfacetDistribution.glsl:31` | [Karis 2013](#karis-ue4) §3 式(5) 的 **IBL 变体** `k=(r+1)²/8` | 与论文 direct 变体 `k=α/2` 不是同一条曲线，已在 mode 2 中显式标注 |
| `SmithG1Ggx`（参考曲线，不参与着色） | `common/microfacetDistribution.glsl:49` | Smith 高度场闭合解，见 [Heitz 2014](#heitz-masking-shadowing) §4 | 只为量化近似误差而存在 |
| split-sum IBL | `common/lighting.glsl:278` `CalculateSpecularIblWithF0()` | [Karis 2013](#karis-ue4) §5 式(9) | LUT 由 `generator/brfdLut.comp` 用 Hammersley/GGX 重要性采样生成 |
| LUT 内几何项 | `generator/brfdLut.comp:66` `GeometrySchlickGGXIBL()` | [Karis 2013](#karis-ue4) §5 式(7) `k = α²/2` | **与 `common/lighting.glsl` 中 direct 路径的 `k=(r+1)²/8` 不是同一表达式**；需在 `D-04` 中量化两者差异 |
| Diffuse（Lambert） | `common/lighting.glsl` | [Hoffman 2013](#hoffman-physics-math)｜Lambert 与 `1/π` 约定 | —— |
| Principled 参数化 | `M_pbr` `u_pbrFactors` | [Burley 2012](#burley-disney) §3 | 只采用 Principled 的参数语义，未实装 Burley diffuse |

### ④ Case 列表

**基础 case**（实现 vs 公式）

`M_pbr` 参数面：`u_tintColor`（albedo/opacity）、`u_pbrFactors = (roughness, metallic, ambientOcclusion, reserved)`、
`u_emissiveStrength`。`inputs.specular` **恒为默认 `0.5`**（`engine/materialInputs.glsl:145`），没有作者入口——
凡是需要独立 F0 的论文 case 都要先登记这个缺口。

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `D-01` | [Heitz 2014](#heitz-masking-shadowing) §3–§5 · NDF / G1 归一化 | `∫ D(h)(n·h)dω_h = 1`；`G1(c→1)=1`；`0 ≤ G1 ≤ 1` | 数值积分（离线） | ☐ |
| `D-02` | [Karis 2013](#karis-ue4) Fig 2 · Schlick vs 精确 Smith G1 | 两条曲线逐列量化；引擎用的是 `k=(r+1)²/8` | 曲线（mode 2） | ✅ **2026-09-12 通过**（`m07_mode2_g1`：Schlick 161 列最大 0.484 px、Smith 23 列最大 0.418 px；镜像假设 ~700 px，见 case 记录） |
| `D-03` | [Karis 2013](#karis-ue4) Fig 2 + §3 | **补一个画 `k=α/2` direct 变体的 mode**，与 `D-02` 并排，确认引擎选的到底是哪条 | 曲线（新增 mode） | ☐ 不再被 `D-11` 阻塞（range 已核对为 `[0,3]`），只等新 mode |
| `D-04` | [Karis 2013](#karis-ue4) §5 式(7) vs §3 式(5) | 量化 `brfdLut.comp` 的 `k=α²/2` 与 `lighting.glsl` 的 `k=(r+1)²/8` 在 `[0,1]²` 上的最大偏差 | 数值积分（离线） | ☐ |
| `D-05` | [Burley 2012](#burley-disney) Fig 12 · directional albedo | 各 G 函数（Smith / GGX / Schlick 变体）的方向反照率曲线对比 | 数值积分（离线） | ☐ |
| `D-06` | [PBRT 4ed｜Microfacet Distributions](https://pbr-book.org/4ed/Reflection_Models/Microfacet_Distributions) | Beckmann 与 GGX 在相同 α 下的分布形状；确认 GGX 长尾的正确朝向 | 曲线（新增 mode） | ☐ |
| `D-07` | [PBRT 4ed｜Reflection Models](https://pbr-book.org/4ed/Reflection_Models)｜White furnace | conductor 与 dielectric 单次散射半球积分 ≤ 1 | 数值积分（离线） | ☐ |
| `D-08` | [Hoffman 2013](#hoffman-physics-math)｜参数化边界 | metallic = 1 时可见 diffuse 为 0；albedo 上限不被突破 | 球阵 | ☐ |
| `D-09` | [Karis 2013](#karis-ue4) §4–§5 | split-sum 近似 vs 逐样本参考的环境预积分误差 | 球阵（离线参考对照） | ☐ |
| `D-10` | 球阵基线 `SC_sphere_array` | roughness × metallic 端点与单调性回归，作为其它模型标尺 | 球阵 | ◐ 已建，**缺数值判定** |
| `D-11` | 工具链缺陷（`M_brdfPlot.json`） | `u_plotMode` 的 schema `range` 是 `[0,1]`，而 shader 实现 4 个模式（`M_brdfPlot.surface.glsl` 的 `<0.5 / <1.5 / <2.5 / else`）。**运行时不 clamp、不报错**（直接比较原始值），但编辑器与任何 schema 校验都不认 mode 2 / 3。已把 range 扩到 `[0,3]` 并补全描述 | 架构核对 | ◐ **range 已核对为 `[0,3]`（2026-09-12）；编辑器 / 校验侧仍未核对** |
| `D-19` | 一致性验收（本仓库） | 如果 §4.1 引入"场景 / MI 引用校验"，它会打在既有文件 `MI_plot_smith_schlick` / `MI_plot_uv_probe` 上（它们在 range 修正前用的是越界的 mode 2 / 3）。校验规则必须按**改后的** range 判断 | 架构核对 | ☐ |
| `D-12` | 参数面审计（本仓库） | 记录 `inputs.specular` 无作者入口（恒 0.5）这一缺口，并列出它影响的论文量（独立 F0 / specular tint） | 架构核对 | ☐ |
| `D-20` | [Karis 2013](#karis-ue4) §Specular G 正文 | **用法与论文相悖**：论文明确写 *"this adjustment is only used for analytic light sources; if applied to image-based lighting, the results at glancing angles will be much too dark."*，而 `GeometrySmith()`（`common/lighting.glsl:431`）用的是 `k=(r+1)²/8`，被 `:568`（direct）与 `:614`（ClearCoat）共用。逐项确认调用方是否都属于"analytic light source"；属于 IBL 的必须改用论文允许的形式 | 架构核对 + 对照 | ☐ **审计发现** |
| `D-21` | 🔎 [Karis 2013](#karis-ue4) 全文 | `generator/brfdLut.comp:66` 的 `k = α²/2` **在论文里不存在**：论文只给 `k=α/2`（direct 拟合）与 `k=(r+1)²/8`（analytic 调整），**IBL 域的 G 没有指定**。决定：换用论文的 `k=α/2`、换用 Neubelt 式(5) 的精确 Smith G1，还是按 §0.3 登记为"论文未指定" | 公式溯源 | ☐ **审计发现** |

**论文 case**（模型在真实几何上是否仍成立）

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `D-15` | [Karis 2013](#karis-ue4) Fig 4 / Fig 5 · split-sum 参考 vs 近似 | 环境预积分近似的误差在真实几何上的可见边界 | 球阵即可 | ☐ |
| `D-16` | [Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 1 · conditional G 归零 artifacts | 几何项实现错误造成的掠射黑边 | 球阵即可 | ☐ |
| `D-17` | [Karis 2013](#karis-ue4) Fig 3 · 金属 / 电介质粗糙度对照 | 生产参数化的观感端点 | 球阵即可 | ☐ |
| `D-18` | `SC_paper_case_cloth_sheen` 下半行 | 该场景第 2 行是 **DefaultLit(GGX) 与 Cloth(Charlie) 逐球同参数对照**（roughness 0.2 / 0.5 / 0.8）：既是 DefaultLit 的分布对照 case，也是 `S-12` 的共享基线。两个模型共享同一批截图，**不要各拍一套** | 球阵（已有） | ◐ 资产已有，**缺数值判定与共享记录** |

### ⑤ 专属约定

- `SC_sphere_array` 是本模型的稳定基线，**同时作为其它模型的参数标尺**；
- metallic / roughness 的端点语义（conductor 的可见 diffuse 为 0）不得为了好看而放宽；
- `D-03` 是本模型的关键 case：**在把「和论文一致」写进结论之前，必须先确认引擎选的是哪个变体**。

## 2.2 ClearCoat

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ⛔ 受限（影响面可验证，膜厚 / 吸收做不了） |
| 实现锚点 | `M_carPaint`；`documents/plan/rendering/archive/car-paint-shading-model.md`；UE 5.8 Legacy Clear Coat |
| 已完成 | impact-face 粗糙度 / 权重扫描（`SC_paper_case_clearcoat_sweep`）；`SC_car_showcase` 基线 |
| 未完成 | 分层能量核对、法线耦合边界、Fresnel 方向性 |
| 阻塞 | `M_carPaint` 参数面只有 `u_clearCoat`（权重）与 `u_clearCoatRoughness`，**没有膜厚与吸收项** |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Jakob 2015, *layerlab: A Framework for Control Over Layered Material Design* | 四个边界算子与 adding equations | [书库 `#jakob-layerlab`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#jakob-layerlab) |
| Jakob 2014, *A Comprehensive Framework for Rendering Layered Materials* | 分层传输、层间多次反射 | [书库 `#jakob-layered-tr`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#jakob-layered-tr) |
| Weidlich & Wilkie 2007, *Arbitrarily Layered Micro-Facet Surfaces* | 任意层微表面、膜厚与吸收 | [书库 `#weidlich-wilkie`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#weidlich-wilkie) |
| Burley 2012 | GTR1 clearcoat lobe 与 coat 权重语义 | [书库 `#burley-disney`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#burley-disney) |
| Filament | base attenuation、独立 coat normal 的实时实现 | [书库 `#filament`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#filament) |
| OpenPBR | coat slab / coverage / base IOR 的规范语义 | [书库 `#openpbr`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#openpbr) |
| PBRT 4ed｜Scattering from Layered Materials | 分层 BSDF 的离线推导 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) |
| Walter 2007, *Microfacet Models for Refraction through Rough Surfaces* | 粗糙界面透射与层间界面 | [书库 `#walter-microfacet-refraction`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#walter-microfacet-refraction) |
| UE [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)｜Clear Coat | Clear Coat 输入语义（UE 本身没有膜厚参数） | [书库 `#ue-shading-models`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-shading-models) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| coat lobe `D_GGX` + `Vis_SmithJointApprox` | `documents/plan/rendering/archive/car-paint-shading-model.md`；`common/lighting.glsl:387` | UE 5.8 Legacy Clear Coat 闭包（实时参考实现）；分布本体见 [Karis 2013](#karis-ue4) | 顶层固定 IOR 1.5 / F0 0.04 |
| 固定 Eta 折射点积近似 | 同上 | UE 5.8 Legacy | 属于参考实现，需同时标注其物理出处 |
| coat 顶层 Fresnel | `common/lighting.glsl:387` 区域 | [Hoffman 2013](#hoffman-physics-math)｜Fresnel 与 F0；[PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF) | 方向性必须满足 `F(0°)=F0`、`F(90°)=1` |
| 底层法线八面体偏移编码 | `shadingModel.glsl:19`；`gbufferCodec.glsl:175` | 自定编码（本项目）；语义参考 UE Legacy 的 clear coat bottom normal | 编码必须无损到可分辨的精度 |
| base 能量衰减 | `M_carPaint` surface 路径 | [Jakob 2015｜adding equations](#jakob-layerlab)；[Filament｜base attenuation](#filament) | 当前只做 UE Legacy 特化，未启用 GGX energy conservation |
| 膜厚 / 吸收 | **不存在** | [Weidlich & Wilkie 2007](#weidlich-wilkie) §3–§4 需要膜厚与吸收系数 | ⛔ 缺口，登记在 §2.2⑤ |

### ④ Case 列表

**基础 case**

`M_carPaint` 参数面：`u_tintColor`、`u_pbrFactors`、`u_clearCoat`（权重 → `CustomData.x`）、
`u_clearCoatRoughness`（→ `CustomData.y`）、`u_clearCoatBottomNormalTiling` /
`u_clearCoatBottomNormalStrength`（**仅在 `USE_CLEAR_COAT_BOTTOM_NORMAL_MAP=1` 时进入 shader，默认宏为 0**）。
coat IOR 固定 1.5 / F0 0.04，不可作者化。

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `C-01` | [Hoffman 2013](#hoffman-physics-math) + [PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF) | coat 界面 Fresnel 曲线：`F(0°)=0.04`、`F(90°)=1`、单调、无符号翻转 | 曲线（新增 mode） | ☐ |
| `C-02` | [Jakob 2015｜adding equations](#jakob-layerlab) | coat 权重 `w∈[0,1]` 时底层漫反射能量 = `1-w`，与解析值逐点比对 | 数值积分（离线） | ☐ |
| `C-03` | [Jakob 2015](#jakob-layerlab) §3｜层间多次反射 | 当前实现忽略多层反射，量化由此损失的能量上界 | 数值积分（离线） | ☐ |
| `C-04` | `SC_paper_case_clearcoat_sweep` | coat 粗糙度扫描：峰值展宽单调、能量单调递减 | 单变量扫描 | ◐ 已建，**是影响面不是膜厚，缺数值判定** |
| `C-05` | `SC_paper_case_clearcoat_sweep` | coat 权重扫描：`w=0` 严格退化为顶层无涂层；`w=1` 底层反射被完全压制 | 单变量扫描 | ☐ |
| `C-06` | [PBRT 4ed｜Reflection Models](https://pbr-book.org/4ed/Reflection_Models)｜White furnace | 分层模型整体半球积分 ≤ 1 | 数值积分（离线） | ☐ |
| `C-07` | `gbufferCodec.glsl:175` + `shadingModel.glsl:19` | 底层法线八面体偏移编解码往返误差，**按倾角区间分档给数**（见 §1.5.2） | 架构核对 + 数值 | ☐ |
| `C-08` | [Filament｜base attenuation](#filament) | 底层法线独立于顶层时，底层光照不泄漏到 coat 高光之外 | 单变量扫描 | ☐ |
| `C-13` | 参数面审计（本仓库） | 记录 coat IOR 固定 1.5 / F0 0.04 无作者入口，以及 `Tiling` / `Strength` 在默认宏下不生效这两条边界 | 架构核对 | ☐ |

**论文 case**

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `C-09` | [Jakob 2015](#jakob-layerlab) Fig 4 · 金龙 + 清漆 | 底材与涂层粗糙度组合下的分层传输、coat 独立高光 | 缺 | ☐ |
| `C-10` | [Weidlich & Wilkie 2007](#weidlich-wilkie) Fig 5 · 白球 + 黄清漆**厚度** 0.0→15.0 | 膜厚驱动的吸收与颜色加深 | 球阵即可 | ⛔ 无膜厚 / 吸收项 |
| `C-11` | [Weidlich & Wilkie 2007](#weidlich-wilkie) Fig 3 · 清漆有 / 无吸收 | 层吸收对颜色的影响 | 球阵即可 | ⛔ 同上 |
| `C-12` | `SC_car_showcase` | 车漆综合观感基线 | 已有 | ◐ 基线存在，**未绑定论文图** |

### ⑤ 专属约定

- `M_carPaint` 当前参数面只有 `u_clearCoat` 与 `u_clearCoatRoughness`，**没有膜厚 / 吸收项**；
- 因此本模型在论文 case 上只能做"影响面"层面的近似复现，**不得宣称复现了 Weidlich Fig 5 / Fig 3**；
- 是否增加膜厚 / 吸收项属于功能性新增，需要先决定语义：UE Clear Coat 本身没有该参数，
  真做必须参考 [Weidlich & Wilkie 2007](#weidlich-wilkie) 的层吸收模型并建立独立版本；
- 顶层直接光保留 UE Legacy 的固定 Eta 折射点积近似，`C-01` 是唯一能判断该近似代价的 case。

## 2.3 Cloth / Sheen

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ⚠️ **待整改**（v2 各向异性经审计确认含 **5 项论文里没有的公式**，见 §1.4.2「Cloth / Sheen」）；v1 各向同性路径的两项公式已核对有出处 |
| 实现锚点 | `M_cloth`；`common/clothBrdf.glsl`；`engine/clothLighting.glsl`；`generator/clothLookupTables.comp` |
| 已完成 | `SC_paper_case_cloth_sheen` 球阵；Charlie D / visibility debug 截图；方向反照率 LUT 烘焙；**§1.4.2 的公式溯源审计（已抽出论文式号）**；**mode 0 曲线逐列像素对照通过**（2026-09-12，GGX 最大 0.486 px、Charlie 最大 0.447 px，见 `S-01`） |
| 未完成 | directional albedo 数值对照（`S-03`）、v1 端点退化（`S-05` `S-06`）；`S-26` / `S-27` 两项映射与分层因子的决策 |
| 阻塞 | **整改路线未定**：`S-23`（方案 A，各向同性）/ `S-29`（方案 B，microcylinder）/ `S-31`（方案 C，两套并存）；B 与 C 都牵动 `S-27` 与两张 directional-albedo LUT。论文 case 的双色天鹅绒圆柱与服装需要新资产 |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Estevez & Kulla 2017, *Production Friendly Microfacet Sheen BRDF* | Charlie 分布、sheen visibility、生产级布料外观 | [书库 `#estevez-sheen`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#estevez-sheen) · [PDF](https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf) |
| Neubelt & Pettineo 2013 | GGX vs Charlie 分布对比、grazing / fuzz 观感、式(23) 的布料分母 | [书库 `#neubelt-order-1886`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#neubelt-order-1886) |
| **Sadeghi, Bisker, De Deken, Jensen 2013**, *A practical microcylinder appearance model for cloth rendering*, ACM TOG 32(2) | **各向异性布料散射的完整模型**：以 thread tangent 为坐标架，式(2)(3)(4) | ✅ 开放获取：[eScholarship PDF](https://escholarship.org/content/qt6v11p5b0/qt6v11p5b0.pdf)（书库暂无此条，见 `S-30`） |
| Irawan & Marschner 2012, *Specular Reflection from Woven Cloth*, ACM TOG 31(1) | 编织布程序化散射模型、经/纬 thread 方向 | [Cornell 摘要页](https://www.cs.cornell.edu/~srm/publications/TOG12-cloth.html)（正文需 ACM） |
| Burley 2012 | sheen 的生产语义与 Principled 参数化 | [书库 `#burley-disney`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#burley-disney) |
| Real-Time Rendering 4th, Ch 9 §9.10 | 布料 / 绒毛的实时近似与材质混合 | [书库 `#real-time-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#real-time-rendering) |
| Filament｜Cloth model | 实时 cloth 参数与实现细节 | [书库 `#filament`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#filament) |
| GPU Gems 2 Ch 11, *Approximate Bidirectional Texture Functions* | 牛仔布 / 羊毛的预计算 BTF 近似 | [书库 `#gpu-gems-2`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#gpu-gems-2) |
| PBRT 4ed｜Roughness Using Microfacet Theory | NDF 投影面积归一化与采样测度 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| `ClothSheenRoughnessToAlpha` | `common/clothBrdf.glsl:15` | 🔎 论文的分布参数 `r` 直接是 `sin` 指数，**无 `r→α` 映射** | `α = r²` 是 Disney 约定被平移到 Charlie 上；按 `S-26` 决定保留并溯源还是取消 |
| `ClothCharlieDistribution` | `common/clothBrdf.glsl:20` | ✅ [Estévez & Kulla 2017](#estevez-sheen) **式(2)** | 与 `D(m) = (2+1/r)sin^(1/r)θ/2π` 逐项一致 |
| `ClothNeubeltVisibility` | `common/clothBrdf.glsl:37` | ✅ [Neubelt & Pettineo 2013](#neubelt-order-1886) **式(23)** 分母 | 原文语境是"drop 掉 geometry 项后替换传统 microfacet 分母的那一项"；函数命名 `…Visibility` 待改（`S-24`） |
| `ClothAnisotropyToAspect` | `common/clothBrdf.glsl:61` | ❌ **论文无此式** | 两篇论文都没有 anisotropy→轴比映射；Neubelt 式(12)(13) 给的是有出处的替代式（`S-23`） |
| `ClothAnisotropicCharlieDistribution` | `common/clothBrdf.glsl:68` | ❌ **论文无此式** | Estévez & Kulla 无各向异性 Charlie；椭圆代入套到 Charlie 上是本项目推的（`S-23`） |
| `ClothAnisotropicVisibility` | `common/clothBrdf.glsl:114` | ❌ **论文无此式** | 掠射连续衰减项两篇论文都没有（`S-23`） |
| `ClothAnisotropicSheenUnitResponse` | `common/clothBrdf.glsl:165` | ❌ **论文无此式** | cross 双轴纤维瓣两篇论文都没有（`S-23`） |
| `T_b = 1 − c_s·E_s(NoV, α_s)` 分层 | `engine/clothLighting.glsl` | ❌ 与论文式不同 | Estévez & Kulla §5 用 `min(α_i, α_o)`（Kelemen–Szirmay-Kalos albedo scaling），不是 `1 − E_s(NoV)`（`S-27`） |
| 方向反照率 LUT 积分 | `generator/clothLookupTables.comp:41` | [Estevez & Kulla 2017](#estevez-sheen)｜directional albedo 定义；采样见 [Veach 1997](#veach-thesis) 的 QMC | 均匀半球 + 128 Hammersley；`LUT.y = alpha`，不是 perceptual roughness |
| 各向异性 LUT 33 层手工插值 | `engine/clothLighting.glsl:98` | 采样实现（编码约定，不改数值定义） | 避免驱动对 `sampler2DArray` 层坐标的离散取整；LUT 的**定义**仍取自论文 |
| base / sheen 能量互补 | `engine/clothLighting.glsl` | [Estevez & Kulla 2017](#estevez-sheen)｜`1 - directionalAlbedo` | 必须与 LUT 值一致 |

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `S-01` | [Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 6 左 · GGX vs Charlie 分布 | 两条分布的形状与峰值位置逐列比对（对数纵轴） | 曲线（mode 0） | ✅ **2026-09-12 通过**（`m07_mode0_cloth`，roughness 0.35：GGX 129 列最大 0.486 px、Charlie 171 列最大 0.447 px，另丢弃 3 列交叉列；此前"测量脚本有 bug"的结论已在 `M-07` 中定位为**探针图竖直镜像**并修正） |
| `S-02` | [Estevez & Kulla 2017](#estevez-sheen) Fig 2 · Charlie 分布随 `θh` 与 roughness | `alpha` 增大时分布展宽、峰值高度下降的形状族 | 曲线（mode 0 多 roughness） | ☐ |
| `S-03` | [Estevez & Kulla 2017](#estevez-sheen)｜directional albedo 定义 | 球阵 LUT 与离线 Monte Carlo 积分逐点比对（`N·V × alpha` 全网格） | 数值积分（离线） | ☐ |
| `S-04` | [Estevez & Kulla 2017](#estevez-sheen)｜能量互补 | `baseScale = 1 - directionalAlbedo` 在整张 LUT 上落在 `[0,1]` 且单调 | 数值核对 | ☐ |
| `S-05` | `common/clothBrdf.glsl:20` · 端点 | Charlie 分布的端点：`θh=0` 时最小值、`θh→90°` 时峰值、`alpha→0` 不发散到 NaN | 曲线 + 端点 | ☐ |
| `S-06` | [Neubelt & Pettineo 2013](#neubelt-order-1886) sheen visibility | `N·L` 或 `N·V` → 0 时 `V → 0`（不是无穷）；`N·L=N·V=1` 时 `V = 1/4` | 曲线 + 端点 | ☐ |
| `S-07` | ✅ 已核：[Neubelt & Pettineo 2013](#neubelt-order-1886) **式(12)(13)** | 确认 `α_t = α`、`α_b = lerp(0, α, 1−anisotropy)` 是论文给的映射；它的配套 **式(14) `D_aniso` 是给 GGX 的椭圆形式**，论文没有把代入套到 Charlie 上 | 公式溯源 | ✅ 审计完成 |
| `S-08` | ✅ 已核：[Estévez & Kulla 2017](#estevez-sheen) 全文 | 论文只有 **2 页**（Overview / Microfacet Distribution / Shadowing Term / Terminator Softening / Layering），**没有各向异性、没有 visibility 的 per-direction 形式** | 公式溯源 | ✅ 审计完成 |
| `S-09` | ✅ 已核：两篇论文全文 | 没有 cross / 双轴纤维瓣的任何形式 | 公式溯源 | ✅ 审计完成 |
| `S-10` | [PBRT 4ed｜White furnace](https://pbr-book.org/4ed/Reflection_Models) | cloth lobe 半球积分 ≤ 1；sheen 与 base 之和不超 1 | 数值积分（离线） | ☐ |
| `S-11` | [Estevez & Kulla 2017](#estevez-sheen) Fig 1 · sheen 叠在红色 diffuse | sheen roughness 递增时 grazing 亮边展宽、红色 diffuse 不被吃掉 | 单变量扫描（`SC_paper_case_cloth_sheen` 上行） | ◐ 已建，**缺数值判定** |
| `S-12` | [Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 6 右 · GGX 球 vs 天鹅绒球 | 同一粗糙度下 GGX 与 Charlie 球的观感差异可解释到分布差异 | 单变量扫描（`SC_paper_case_cloth_sheen` 下半行，与 `D-18` 共享截图） | ◐ 已建，**缺归因记录** |
| `S-13` | `engine/materialDebugView.glsl`｜`clothCharlieD` / `clothNeubeltVisibility` | Debug 视图输出与离线解析值一致（不是只看形状对不对） | Debug 对照 | ◐ 已有截图，**无数值对照** |
| `S-14` | [Estévez & Kulla 2017](#estevez-sheen)｜各向异性参数行为 | anisotropy 扫描：在 `S-22` 确定映射之后，确认轴向语义稳定、正负号含义来自所选外部来源；`anisotropyCross` 单独扫描不改变 `sheenColor` 能量语义 | 单变量扫描 | ⚠️ 待 `S-22` |
| `S-18` | 工具链缺陷（`M_cloth.json`） | 宏表声明了 `USE_PBR_MAP`，但**没有 `pbrParamMap` 贴图槽**（`M_clothParamter.glsl` 只生成 albedo/emission/normal 三个 sampler）：MI 打开该宏无槽可绑；先记录，再决定是补槽还是删宏 | 架构核对 | ☐ **阻塞项** |
| `S-19` | 参数面审计（本仓库） | 记录两条缺口：**无 anisotropy mask 贴图**（源资产用 `ParamMap.B`，见 `documents/plan/rendering/archive/cloth-shading-model.md`）、**`sheenIblVersion = 0`**（IBL 仍是标记为 `clothIblFallback` 的 irradiance fallback，没有 Charlie 专用预滤波） | 架构核对 | ☐ |
| `S-20` | `gbufferCodec.glsl:529` | Deferred 路径 decode 强制 `metallic = 0`：确认 Cloth 的 deferred 结果本就与材质 metallic 无关，不得当成偏差 | 架构核对 | ☐ |
| `S-21` | [Estevez & Kulla 2017](#estevez-sheen)｜环境 sheen | 在 `S-19` 的 fallback 前提下，量化环境光照下 sheen 与 base 的能量分配与论文期望的差距；`sheenIblVersion=0` 时**不得宣称复现了 Estevez 的环境 sheen 图** | 数值对照 | ☐ |
| `S-22` | ✅ 已核：[Estévez & Kulla 2017](#estevez-sheen) **式(2)** | `ClothCharlieDistribution` 与 `D(m) = (2+1/r) sin^(1/r)θ / 2π` **逐项一致**，审计通过 | 公式溯源 | ✅ 审计完成 |
| `S-26` | 🔎 [Karis 2013](#karis-ue4) + [Estévez & Kulla 2017](#estevez-sheen) | `α = r²` 这个映射：Karis 的 `α = Roughness²` 是给 GGX 的；Estévez & Kulla 的 `r` 直接是指数。**决定**：沿用 `r²`（须写明它是 Disney 约定平移）还是直接用 `r`（照论文）。两者会给出不同的 sheen 展宽 | 公式溯源 + 数值对照 | ☐ |
| `S-27` | 🔎 [Estévez & Kulla 2017](#estevez-sheen) §5 | 分层因子：论文用 `min(α_i, α_o)`（入射/出射 albedo 取小），代码用 `1 − c_s·E_s(NoV, α_s)`。量化两者差异并决定是否改为论文式 | 公式溯源 + 数值对照 | ☐ |
| `S-30` | 知识库维护（书库索引页） | 把 **Sadeghi et al. 2013**（开放获取）加进书库条目并分配锚点 `#sadeghi-microcylinder`（含离线 PDF 与署名），让 §2.3 的引用与其它模型统一走锚点格式。**这是书库仓库的改动**，需在其 `books/index.mdx` 与 `resource/pdfs/` 下完成 | 知识库维护 | ☐ |
| `S-31` | [Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 6 左 + [Sadeghi et al. 2013](#sadeghi-microcylinder)｜**方案 C** | **两条都保留时的对照实验**：A 臂（各向同性 Charlie）与 B 臂（microcylinder）并排同场景同光照。(1) 确认 A 臂在旋转织纹 / 改变 tangent 时**外观完全不变**，B 臂**随切线方向转向**——这是"切线是否进入公式"的直接证据；(2) 两臂各自的能量账本分别闭合，**不允许交叉补偿**；(3) A 臂与 B 臂必须是两个 variant，产物里不得同时出现两套公式。三条约束见 §1.4.3 | 对照实验 | ☐ 方案 C |
| `S-23` | [Neubelt & Pettineo 2013](#neubelt-order-1886)｜**方案 A** | **只留论文各向同性 sheen**：`anisotropy ≠ 0` 路径整体退回论文形式 —— 去掉 `S-10` / `S-11` / `S-12` 三项论文里没有的公式，改用论文的各向同性 Charlie（式 2）+ 式(23) 分母；`anisotropy` / `anisotropyCross` 输入随之取消，或明确标注为"论文无对应量"。**代价：失去织纹方向外观** | 整改 | ☐ 方案 A |
| `S-29` | [Sadeghi et al. 2013](#sadeghi-microcylinder)｜**方案 B** | **只留 microcylinder**：各向异性由 **thread tangent** 表达，不再需要椭圆轴比 / warp visibility / cross 三个自创量。代价：`T_b = 1 − c_s·E_s` 与两张 directional-albedo LUT（`S-27`）要改为论文的 Fresnel/`kd`/`A` 能量项，属**独立整改** | 模型替换 | ☐ 方案 B |
| `S-24` | ✅ 已核：[Neubelt & Pettineo 2013](#neubelt-order-1886) **式(23)** + 正文 | `ClothNeubeltVisibility` 的分母与式(23) **逐字一致**，但原文语境是 *"we dropped the geometry term"* 后"替换传统 microfacet 分母"的那一项。决定函数名与注释是否改为 `ClothNeubeltDenominator` 之类，避免被误读成独立的 visibility 模型 | 命名与注释整改 | ☐ |
| `S-25` | [Neubelt & Pettineo 2013](#neubelt-order-1886)｜各向同性行为 | `ClothSheenUnitResponse` 在 `anisotropy = 0` 时的行为：确认它等于 `D_Charlie(θ_h) · 1/(4(N·L+N·V−N·L·N·V))`，且这就是论文式(2)+式(23) 分母的组合（不含任何额外项） | 数值核对 | ☐ |

**论文 case**

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `S-15` | [Neubelt & Pettineo 2013](#neubelt-order-1886) Fig 5 · 双色天鹅绒包裹圆柱 | Charlie 分布的掠射亮边与双色 sheen | 缺圆柱资产 | ☐ |
| `S-16` | [Estevez & Kulla 2017](#estevez-sheen) Fig 3 / Fig 4 · Ashikhmin-Shirley 对比、terminator softening | 分布选型与阴影过渡 | 球阵 + 圆柱 | ☐ |
| `S-17` | 展示型人模 + 独立服装 | 织物褶皱、切线方向与接触关系 | 需 MD/CLO 导出 | ☐ |

### ⑤ 专属约定

- **一个时间点只扫一个变量**：sheen roughness、sheen 权重、各向异性分别做，不要混在一个场景里；
- 论文 case 使用**展示型人模 + 独立服装**，不使用悬空布片或布料球；服装需静态 Pose 与贴身褶皱；
  MD/CLO 只用于离线模拟与导出，运行时不执行布料模拟；
- **本模型当前是 ⚠️ 待整改状态**：§1.4.2 审计确认 v2 各向异性路径含 5 项论文里没有的公式
  （`S-10` `S-11` `S-12` = 椭圆 Charlie / warp visibility / cross 混合，`S-e` = `aspect=2^anisotropy`，
  `S-27` = 分层因子）。整改完成前，Cloth 的任何结论都不能写成"与论文一致"；
- **整改有三条都符合 §0.3 的路线**（外观差异与约束见 §1.4.3）：
  - **方案 A（`S-23`）**：只留论文各向同性 Charlie（式 2）+ Neubelt 式(23) 分母。改动最小，
    但**失去织纹方向外观**；
  - **方案 B（`S-29`）**：只留 **Sadeghi et al. 2013 microcylinder 模型**。各向异性由 thread tangent
    表达，天然有出处；必须同时重做能量项（`S-27`）与两张 directional-albedo LUT；
  - **方案 C（`S-31`）**：**两套都保留**——B 为生产臂，A 为对照臂，作为"切线是否进入公式"的
    可测量对照。必须满足三条硬约束：两个 variant（不是一个 `if` 开关）、指定生产臂与对照臂、
    两套能量账本各自闭合且不交叉补偿；
- **A 与 B 是两种不同外观，不是同一模型的两个参数档**：A 只吃 `N·H`（转向不变），
  B 由切线定义反射锥（高光条纹随织纹转向）。因此"都要"只能是**两个独立闭包**，不能做成一个开关；
- v1 各向同性路径的两项公式**已核对有出处**（`S-22` 式(2)、`S-24` 式(23)），它们是本模型可信的部分；
  整改时不要动它们；

## 2.4 Hair

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中（实现齐备，数值验证缺失） |
| 实现锚点 | `M_hair` / `M_hairProbe`；`common/hairPathScattering.glsl`；`engine/hairScattering.glsl`；`generator/hairAzimuthalLut.comp` |
| 已完成 | `SC_hair_showcase_6125` 场景；R / TT / TRT 与 scatter / coverage 的 debug 截图；**mode 1 曲线已采图**（`m07_mode1_hair.bmp`，单会话四连拍之一） |
| 未完成 | mode 1 曲线的**数值对照**（测量工具的期望值模型需要整条 UE Legacy 路径的 python 复刻，而这正是 `H-20` 待定的基准问题）、方位角 LUT 自洽、Kajiya-Kay scatter 定位 |
| 阻塞 | Hair tangent 需要离线生成并固定；`M_hairProbe` 的参照系需要先核对 |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Marschner et al. 2003, *Light Scattering from Human Hair Fibers* | R / TT / TRT 光路、纵向 / 方位角分离、测量现象 | [书库 `#marschner-2003`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#marschner-2003) · [作者页](https://www.cs.cornell.edu/~srm/publications/SG03-hair.pdf) |
| d'Eon et al. 2011, *An Energy-Conserving Hair Reflectance Model* | 纵向函数的能量守恒形式 | [书库 `#deon-2011`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#deon-2011) · [EGSR](https://onlinelibrary.wiley.com/doi/10.1111/j.1467-8659.2011.01976.x) |
| Chiang et al. 2016, *A Practical and Controllable Hair and Fur Model* | 生产级参数化、方位粗糙度、物种差异 | [书库 `#chiang-2016`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#chiang-2016) · [DOI](https://doi.org/10.1145/2897824.2925880) |
| pbrt｜*Implementation of a Hair Scattering Model* | `M_p` 纵向函数实现与符号约定 | [书库 `#pbrt-hair`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#pbrt-hair) · [pbrt 4ed｜Hair](https://pbr-book.org/4ed/Reflection_Models/Hair) |
| UE 官方 [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)｜Hair | UE Hair 输入语义（Scatter / Backlit / Tangent） | [书库 `#ue-shading-models`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-shading-models) |
| GPU Gems 2 Ch 23, *Hair Animation and Rendering in the Nalu Demo* | Kajiya-Kay 类实时发丝与多重散射 | [书库 `#gpu-gems-2`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#gpu-gems-2) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| `BuildHairUeScatteringContext` | `common/hairPathScattering.glsl:28` | [Marschner 2003](#marschner-2003) §4｜`θh=(θi+θo)/2`、`θd=(θo-θi)/2`、`φ` 定义 | 方向均从交点指向外部 |
| `EvaluateHairUeGaussian` | `common/hairPathScattering.glsl:53` | [Marschner 2003](#marschner-2003) §4｜纵向高斯 `M_p` | 只钳制归一化分母，不钳制指数，低粗糙度保留原始高光形状 |
| `EvaluateHairUeFresnel` | `common/hairPathScattering.glsl:62` | [Marschner 2003](#marschner-2003)｜Fresnel 与 `h` 项；Schlick 近似形式见 [Hoffman 2013](#hoffman-physics-math) | 固定 IOR 1.55 |
| `EvaluateHairUeR` | `common/hairPathScattering.glsl:71` | [Marschner 2003](#marschner-2003)｜R 路径；cuticle tilt `Shift=0.035` 与 UE Legacy 常量 | `alpha=-2*Shift`，方位角项 `0.25*cos(φ/2)` |
| `EvaluateHairUeTT` | `common/hairPathScattering.glsl:108` | [Marschner 2003](#marschner-2003)｜TT 路径；`n' = 1.19/cosθd + 0.36 cosθd`、方位角 `exp(-3.65 cosφ - 3.98)` | 路径吸收 `|baseColor|^(0.5·sqrt(1-(h/n')²)/cosθd)` |
| `EvaluateHairUeTRT` | `common/hairPathScattering.glsl:146` | [Marschner 2003](#marschner-2003)｜TRT 路径；方位角 `exp(17 cosφ - 16.78)` | 界面权重 `(1-F)²·F` |
| Kajiya-Kay 多重散射（`hairScatter`） | `engine/hairScattering.glsl`；`M_hair` `u_pbrFactors.y` | [GPU Gems 2 Ch 23](#gpu-gems-2)｜Kajiya-Kay 类发丝着色 | 复用 `MaterialInputs.metallic` 槽位但语义不同，不是金属开关 |
| 方位角 LUT | `generator/hairAzimuthalLut.comp`；`engine/hairScattering.glsl` `SampleHairAzimuthalLut()` | 自定预积分；方位角散射结构见 [Marschner 2003](#marschner-2003) §5 | 生产路径当前**不**由它驱动能量（`documents/plan/rendering/archive/hair-shading-model.md`），必须验证其自洽 |
| 吸收 `sigma_a` | `materialFunction/mf_hairAbsorption.glsl` | [Marschner 2003](#marschner-2003)｜`exp(-σa·pathLength)` | 单位 `1/m` |

> ⚠️ **历史遗留的表述错误**：曲线探针 mode 1 的注释与旧文档把它写成
> "复现 pbrt Fig 9 / Marschner Fig 5 的 pbrt 实现"。实际上 `M_brdfPlot` mode 1 调用的是
> `EvaluateHairUeR / TT / TRT`，即 **UE Legacy 参数化 + Marschner 光路结构**，不是 pbrt 的
> `M_p` 实现。`H-01` 必须先把基准写清楚，再判定。

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `H-01` | [Marschner 2003](#marschner-2003) Fig 5 · 散射随 `θh`（R / TT / TRT 峰位） | 三条路径的纵向高斯峰被各自 Shift 错开；峰位与论文一致 | 曲线（mode 1，`SC_paper_case_plot_hair_solo`） | ☐ **从未测量**；已有 `plot_hair_solo.bmp` 只在左半幅能测到 R 路径，**需先重拍并确认三色齐备** |
| `H-02` | [Marschner 2003](#marschner-2003) §4 纵向函数 vs [pbrt `M_p`](#pbrt-hair) | 用同一 `(θi, θo, β)` 分别求值引擎实现与 pbrt 参考式，量化差异并归因到参数化 | 离线数值对照 | ☐ |
| `H-03` | [d'Eon 2011](#deon-2011) Fig 4 · 能量守恒纵向函数形状 | 引擎纵向函数在白炉条件下的半球积分行为；量化为达到守恒还缺什么 | 数值积分（离线） | ☐ |
| `H-04` | [Marschner 2003](#marschner-2003)｜方位角散射 | 方位角 LUT 与引擎解析方位角项逐点比对，误差 ≤ LUT 量化精度 | 数值对照 | ☐ |
| `H-05` | `common/hairPathScattering.glsl`｜端点 | `sinθl = ±1`、`cosθd → 0`、`cosφ = ±1` 等退化点上有限且连续，无 NaN | 端点扫描 | ☐ |
| `H-06` | [Marschner 2003](#marschner-2003)｜cuticle tilt | `Shift = 0.035` 在 R / TT / TRT 上的作用方向与论文一致（R 向一侧、TT 向另一侧、TRT 更远） | 曲线 | ☐ |
| `H-07` | [GPU Gems 2 Ch 23](#gpu-gems-2)｜Kajiya-Kay 多重散射 | `hairScatter ∈ [0,2]` 的端点与单调性；`=0` 时退化到单次散射 | 曲线 + 端点 | ☐ |
| `H-08` | [d'Eon 2011](#deon-2011)｜纵向能量 | `<cosθi>` 加权后的纵向函数积分有界，随 `β` 单调 | 数值积分（离线） | ☐ |
| `H-09` | UE Hair tangent 约定 + `documents/plan/rendering/archive/hair-shading-model.md` | rootward 方向与 `tangent.w` handedness 正确；镜像 UV 不造成吸色翻转 | Debug 视图 | ◐ 已有 debug 截图，**缺数值判定** |
| `H-10` | `engine/materialDebugView.glsl`｜`hair_r` / `hair_tt` / `hair_trt` | 三条路径的 Debug 输出之和 = 主着色结果（无漏项、无重复计入） | Debug 对照 | ◐ 已有截图，**无加和核对** |
| `H-11` | [Marschner 2003](#marschner-2003)｜路径身份 | R 不乘吸收、TT / TRT 乘 `exp(-σa·L)`；同一 `σa` 下 TRT 比 TT 更暗 | 单变量扫描 | ☐ |
| `H-16` | 参数面审计（本仓库） | `M_hair`（OpaqueClip → deferred）与 `M_hairProbe`（TransparentAlphaBlend → forward）**不是同一数值路径**；且 `M_hairProbe` 的 `hairScatter` 上界被压到 `[0,1]`，无法表达 6125 实测的 `Scatter ≈ 1.2074`。做论文 case 必须用 `M_hair` | 架构核对 | ☐ **阻塞项** |
| `H-17` | 参数面审计（本仓库） | `u_hairFringeRange` 在 Surface 阶段**无任何引用**，唯一消费点是 `base.frag.glsl` 的 `USE_HAIR_FRINGE_RANGE` 分支（`M_hair.json` 默认为 0）：确认 coverage 不被重复裁剪，且该参数当前对像素无影响 | 架构核对 + Debug | ☐ |
| `H-18` | `engine/hairLighting.glsl:121-133` | Debug 模式 28 / 32 是**唯一**采样方位角 LUT 的模式；其余模式下 `pathLength` / `lutCoordinates` 读 0 属预期，不得当作 bug | Debug 对照 | ☐ |
| `H-19` | 设施审查（本仓库） | `M_hairProbe` 是 `M_hair.surface.glsl` 的一行包装，但**全资源库没有任何场景 / MI 引用它**，唯一引用是孤儿 fixture `tool/hair-tests/fixtures_MI_hair_probe.json`（也无测试加载）：要么为它建 scene + MI + mesh，要么把扫描类 case 改用 `M_hair` | 架构核对 | ☐ |
| `H-20` | 🔎 [Marschner 2003](#marschner-2003) §4.3（**式号需补**） | **基准选择 + 逐项溯源**：论文给的路径吸收是 `T(σ_a,h) = exp(−2σ_a(1+cos2γ_t))`，内部段长 = `2 + 2cos(2γ_t)` 倍半径，总权重 `A(p,h) = (1−F)²F^(p−1)T^p`；代码用的是 UE Legacy 的 `0.5·√(1−(h/n′)²)/cosθ_d`（TT）与 `0.8/cosθ_d`（TRT）。逐项列出差异，并**明确声明基准是哪一个**（论文式 / UE Legacy），不得混称 | 公式溯源 | ☐ **审计发现** |

**论文 case**

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `H-12` | [Marschner 2003](#marschner-2003) Fig 12 · Kajiya-Kay / 本模型 / 真实照片三联 | R / TT / TRT 路径身份与次高光 | `SC_hair_showcase_6125` 已有 | ◐ 资产已有，**缺机位与灯光设计** |
| `H-13` | [Marschner 2003](#marschner-2003) Fig 13 · 62K 纤维 + 复杂光照 | 复杂光照下的稳定性 | 同上 | ☐ |
| `H-14` | [d'Eon 2011](#deon-2011) Fig 12 · 各种自然发色 + dual scattering | 吸收色与 glints | 缺多发型资产 | ☐ |
| `H-15` | [Chiang 2016](#chiang-2016) Fig 2 / Fig 7 · 物种差异化、毛球方位粗糙度 | 方位粗糙度的表现 | 缺 | ☐ |

### ⑤ 专属约定

- 源数据无可靠 mesh tangent，必须在**离线转换阶段**生成并固定 rootward 方向与 `tangent.w` handedness；
  运行时不得用逐像素随机或隐式 fallback 掩盖错误切线；
- 生产路径身份是 **UE Legacy R / TT / TRT**（固定 IOR 1.55、`Shift=0.035`、`fiberRadius=5e-5 m`），
  这些常量**不允许被 MI 覆写**；论文 case 的结论必须建立在这个身份上；
- 论文 case 用灰色头肩模 + 独立发型，不使用完整真人角色作为标准 Hair 资产；
- 建议头部转向约 20 度、侧后方轮廓光、较暗中性背景。

## 2.5 Subsurface（Subsurface / PreintegratedSkin / SubsurfaceProfile）

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中（三套入口齐备，传播核未做数值验证） |
| 实现锚点 | `M_subsurface` / `M_preintegratedSkin` / `M_subsurfaceProfile`；`engine/subsurfaceLighting.glsl`、`engine/preintegratedSkinLighting.glsl`、`engine/subsurfaceProfileLighting.glsl`、`engine/subsurfaceProfileFilter.glsl`、`generator/subsurfaceLookupTables.comp` |
| 已完成 | `SC_paper_case_subsurface_models` 三行 × 五列对照；三套入口的材质合同 |
| 未完成 | 扩散 profile 数值验证、屏幕空间 filter 守恒、背光 case 的机位与灯光 |
| 阻塞 | 皮肤头模资产授权（纸面资产 `SC_marble_bust_01` 已有） |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Jensen et al. 2001, *A Practical Model for Subsurface Light Transport* | BSSRDF、dipole、扩散 profile | [书库 `#jensen-bssrdf`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#jensen-bssrdf) |
| Burley 2012｜normalized diffusion | 归一化扩散 profile | [书库 `#burley-disney`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#burley-disney) |
| GPU Gems 1 Ch 16, *Real-Time Approximations to Subsurface Scattering* | 实时 SSS 近似（wrap / 深度图卷积） | [书库 `#gpu-gems-1`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#gpu-gems-1) |
| GPU Gems 3 Ch 14, *Advanced Techniques for Realistic Real-Time Skin Rendering* | 实时皮肤渲染、预积分与纹理空间扩散 | [书库 `#gpu-gems-3`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#gpu-gems-3) |
| PBRT 4ed｜Volume Scattering | 体积散射、介质参数、离线推导 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) |
| UE [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)｜Subsurface / Preintegrated Skin | 两套实时入口的输入语义 | [书库 `#ue-shading-models`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-shading-models) |
| UE [Subsurface Profile](https://dev.epicgames.com/documentation/en-us/unreal-engine/subsurface-profile-shading-model-in-unreal-engine) | profile filter / 屏幕空间卷积语义 | [书库 `#ue-subsurface-profile`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-subsurface-profile) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| 扩散 profile / `SSP_*.json` 参数化 | `Common/Profiles/` 数据 + `engine/subsurfaceProfileLighting.glsl:7` | [Jensen 2001](#jensen-bssrdf) §3–§4｜dipole 扩散 profile | profile 参数语义必须与论文的 `σs'` / `σa` 一致 |
| profile 透射评价 | `engine/subsurfaceProfileLighting.glsl:14` | [Jensen 2001](#jensen-bssrdf)｜`R_d(r)` | 径向衰减必须单调递减且积分有限 |
| 屏幕空间 filter（水平 / 垂直分离） | `engine/subsurfaceProfileFilter.glsl:22`、`shader/glsl/pass/sssHorizontal.frag`、`sssVertical.frag` | UE Subsurface Profile 屏幕空间卷积；离线依据见 [Jensen 2001](#jensen-bssrdf) | 必须验证分离卷积不引入额外能量 |
| 像素半径 → 屏幕空间核宽 | `engine/subsurfaceProfileFilter.glsl:22` | UE Legacy 参考实现 | 需要验证透视缩放正确 |
| Preintegrated Skin LUT | `generator/subsurfaceLookupTables.comp`；`engine/preintegratedSkinLighting.glsl:45` | [GPU Gems 3 Ch 14](#gpu-gems-3)｜预积分皮肤；归一化扩散见 [Burley 2012](#burley-disney) | 与离线积分比对 |
| Legacy `Subsurface` 局部 wrap / backscatter | `engine/subsurfaceLighting.glsl:21` | 实时近似，见 [GPU Gems 1 Ch 16](#gpu-gems-1) | 局部模型，**不做跨位置输运** |
| 局部间接漫反射 | `engine/subsurfaceLighting.glsl:124`、`engine/preintegratedSkinLighting.glsl:240` | 各入口自身的 IBL 形式 | 与 direct 路径的一致性必须核对 |

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `U-01` | [Jensen 2001](#jensen-bssrdf) §3 式(7)–(9) · dipole `R_d(r)` | 引擎扩散 profile 与论文解析式在 `r ∈ [0, 8σs'⁻¹]` 上逐点比对 | 曲线 + 数值 | ☐ |
| `U-02` | [Jensen 2001](#jensen-bssrdf)｜吸收 / 散射系数 | `σs'`、`σa` 与 `SSP_*.json` / `skinLut` 参数单位一致（`1/m`），量级落在皮肤合理范围 | 架构核对 + 数值 | ☐ |
| `U-03` | [GPU Gems 3 Ch 14](#gpu-gems-3)｜预积分皮肤 | `subsurfaceLookupTables.comp` 的 LUT 与离线 Monte Carlo 积分逐点比对 | 数值积分（离线） | ☐ |
| `U-04` | [Burley 2012](#burley-disney)｜normalized diffusion | 预积分 LUT 在不同 `curvature` 下的单调性与端点 | 数值核对 | ☐ |
| `U-05` | UE [Subsurface Profile](https://dev.epicgames.com/documentation/en-us/unreal-engine/subsurface-profile-shading-model-in-unreal-engine) | 背光强度随 `N·L` / `N·V` 的单调区间与论文一致：正对背面光源 + 正对观察者时最强 | 曲线 + 端点 | ☐ |
| `U-06` | [Jensen 2001](#jensen-bssrdf)｜能量 | 屏幕空间 filter 前后总能量差 ≤ 设定阈值（分离卷积不造能量） | 数值积分（离线） | ☐ |
| `U-07` | `engine/subsurfaceProfileFilter.glsl:22` | 像素半径随距离 / FOV 的缩放正确：相同世界半径在近远机位下屏幕半径符合透视关系 | 数值核对 | ☐ |
| `U-08` | [GPU Gems 1 Ch 16](#gpu-gems-1)｜wrap 近似 | Legacy `Subsurface` 的 wrap / backscatter 端点：`wrap=0` 退化为 Lambert；掠射不产生负值 | 曲线 + 端点 | ☐ |
| `U-09` | 三套入口的语义边界（§2.5⑤） | 同一参数下三套入口结果**不同**，且各自与其论文 / 文档定义自洽（不许"看起来差不多"） | 单变量扫描 | ◐ 场景已建，**缺差异归因** |
| `U-10` | `SC_paper_case_subsurface_models` | 无 SSS → 三套 SSS 的结构对照：protrusion 处变亮、阴影边界变软 | 单变量扫描 | ◐ 已建，**缺数值判定** |
| `U-11` | 三套入口的间接漫反射 | direct 与 indirect 用同一 profile 参数，不出现"直接光软、间接光硬"的割裂 | 场景对照 | ☐ |
| `U-15` | 参数面审计（本仓库） | **三套入口的驱动力不同**：`M_subsurface` 的 `u_subsurfaceColorWeight` / `u_subsurfaceShape` 在材质里；`PreintegratedSkin` 的散射色 / 透射色 / `thicknessMax` 在 `PSL_*.json` LUT 资产里；`SubsurfaceProfile` 的 mean free path / kernel 在 `Common/Profiles/Subsurface/*.json` 里。三条 case 链的"参数从哪来"必须分开记录 | 架构核对 | ☐ **先做，否则 U-01/U-03 无从下手** |
| `U-16` | 参数面审计（本仓库） | 记录 V1 的 profile LUT 只有二维 `(N·L, thickness)`，**没有 `N·V` 轴**；`u_skinSurface.w(curvature)` 的 range 是 `[0,0]`（V1 强制 0）；`u_skinCharacterLighting.z/w` 保存进 GBufferE 但不参与着色 | 架构核对 | ☐ |
| `U-17` | `engine/subsurfaceProfileFilter.glsl` + `pass/sssComposition.frag` | bilateral filter **无 object ID rejection**：确认同 depth / normal / profile 的相邻物体会互相泄漏，并把"泄漏可接受的最大间距"写进结论 | 数值 + 场景对照 | ☐ |

**论文 case**

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `U-12` | [Jensen 2001](#jensen-bssrdf) Fig 9 · 大理石半身像**背面打光** | 扩散 profile 的邻域输运、通体透亮 | `SC_marble_bust_01` 已有 | ◐ 资产已有，**缺机位与灯光设计**；场景已经把同一胸像并排放 4 份（DefaultLit / Subsurface / PreintegratedSkin / SubsurfaceProfile），环境强度为 0——先把这个布置当成四联基线用起来 |
| `U-13` | [Jensen 2001](#jensen-bssrdf) Fig 10 · 牛奶 | 高散射介质的体积感 | 缺 | ☐ |
| `U-14` | 皮肤头模三变体（preintegrated / subsurface / profile） | 三套 SSS 路径的差异可解释 | 需头模资产 | ☐ |

### ⑤ 专属约定

- **三套入口必须分开解释**：Legacy `Subsurface`（局部 wrap / backscatter）、`PreintegratedSkin`（LUT closure）、
  `SubsurfaceProfile`（profile filter / 屏幕空间卷积）不是同一个模型的不同写法；
  `U-09` / `U-14` 的结论里**不得把它们混为一谈**；
- 大理石胸像不套用皮肤散射 profile；如 `SubsurfaceProfile` 需要专用参数，新增独立 `SSP_marble.json`，
  **不复用 `SSP_skin.json` 语义**；底座与背景用 `DefaultLit`；
- 皮肤头模：同一头模三份 MI 变体，同机位同灯光切换；公开资产入库时必须记录作者、原始页面、
  许可文本与再分发范围；
- `M_preintegratedSkin` 的实例必须带顶层 `"skinLut"` 字段（见 §1.8 第 4 条）。

## 2.6 TwoSidedFoliage

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中（闭包已实现，曲线与守恒未验证） |
| 实现锚点 | `M_twoSidedFoliage`；`engine/twoSidedFoliageLighting.glsl`；`materialFunction/mf_twoSidedFoliageInputs.glsl` |
| 已完成 | `SC_foliage_potted_plant_02` 顺逆光对照截图 |
| 未完成 | 透射因子曲线、能量守恒、双面翻转对称性 |
| 阻塞 | 无专门论文基准图（只有官方文档与书本章节） |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| UE [Two Sided Foliage Materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/two-sided-foliage-materials-in-unreal-engine) | 该 shading model 的输入语义与行为描述 | [书库 `#ue-two-sided-foliage`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-two-sided-foliage) |
| GPU Gems 3 Ch 16, *Vegetation in Crysis*（程序化风与叶片透光着色） | 叶片透光着色的原始实时做法 | [书库 `#gpu-gems-3`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#gpu-gems-3) |
| PBRT 4ed｜Volume Scattering（吸收 / 透射） | 薄层透射的离线推导 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) |
| Henyey & Greenstein 1941｜相位函数 | 前向散射相位函数 | [书库 `#henyey-greenstein`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#henyey-greenstein) |
| UE 5.8 Legacy `TwoSidedBxDF` 源码 | 闭包的精确形式（固定 `Wrap=0.5`、`roughness=0.6` 的 GGX scatter） | 实时参考实现 |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| `EvaluateTwoSidedFoliageTransmissionFactor` | `engine/twoSidedFoliageLighting.glsl:26` | UE 5.8 Legacy `TwoSidedBxDF`（实时参考实现）；物理背景见 [GPU Gems 3 Ch 16](#gpu-gems-3) | 固定 `wrap=0.5`；wrap 项 `(-N·L + w)/(1+w)²` |
| scatter 项（GGX `D`） | 同上 | [Karis 2013](#karis-ue4) 式(2) `D_GGX`，固定 `roughness=0.6` | `D` 内部已含 `1/π`，**不得再乘一次** |
| `EvaluateTwoSidedFoliageBacklight` | `engine/twoSidedFoliageLighting.glsl:55` | UE 5.8 Legacy；透射色语义见 [UE 文档](#ue-two-sided-foliage) | `radiance × transmission × subsurfaceColor` |
| 双面法线翻转基准 | `engine/twoSidedFoliageLighting.glsl:75` | UE 双面透光表现；几何法线翻转的取舍已在该处注释 | 刻意以 camera-facing 法线为基准，正反面响应才对称 |
| 相位函数 | **未实装** | [Henyey & Greenstein 1941](#henyey-greenstein) | 当前用 GGX scatter 近似前向散射 |

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `F-01` | UE [Two Sided Foliage](#ue-two-sided-foliage) + UE 5.8 Legacy `TwoSidedBxDF` | 透射因子 `T(N·L, V·L)` 与参考式逐点一致（含 `wrap=0.5`、`α=0.6`） | 曲线（新增 mode） | ☐ |
| `F-02` | 同上 · 端点 | `N·L = -1`（光从背面正射）且 `V = -L` 时透射最大；`N·L = +1` 时透射 → 0 | 端点 | ☐ |
| `F-03` | UE [Two Sided Foliage](#ue-two-sided-foliage)｜Double Sided | 正反面翻转时透射响应严格对称，不出现背面发黑 | 场景对照（旋转 180°） | ☐ |
| `F-04` | [PBRT 4ed｜White furnace](https://pbr-book.org/4ed/Reflection_Models) | `反射 lobe + 透射项` 的半球积分 ≤ 1；当前实现是否超能量需给数 | 数值积分（离线） | ☐ |
| `F-05` | `engine/twoSidedFoliageLighting.glsl:26` · `1/π` 约定 | 确认 scatter 项里的 `1/π` 只出现一次（`D_GGX` 已含） | 数值核对 | ☐ |
| `F-06` | UE [Two Sided Foliage](#ue-two-sided-foliage) + 阴影合同 | Alpha Clip 掩码在主 pass 与 shadow pass 一致，叶片阴影形状与视觉轮廓吻合 | Debug + 场景对照 | ☐ |
| `F-07` | UE [Two Sided Foliage](#ue-two-sided-foliage) | `subsurfaceColor` 的作用是染色而非增益：`= 0` 时透射为 0、`= 1` 时为满透射 | 单变量扫描 | ☐ |
| `F-08` | `SC_foliage_potted_plant_02` | 顺光 / 逆光：DefaultLit 叶片 vs TwoSidedFoliage 叶片 | 场景对照 | ◐ 已有截图，**缺数值判定**；场景本身就是 A-B 对（同一 mesh 两份 MI：`MI_..._leaves_pbr` → `M_pbr`、`MI_..._leaves` → `M_twoSidedFoliage`，`u_subsurfaceColor=[0.18,0.55,0.12]`），一帧可同时拍到两盆，**不要为 A/B 各拍一张** |
| `F-09` | UE [Two Sided Foliage](#ue-two-sided-foliage) | `Leaves → TwoSidedFoliage`，`Pot / Soil → DefaultLit`；材质槽边界正确 | 架构核对 | ◐ 已有，**缺记录** |
| `F-12` | 参数面审计（本仓库） | 背光 lobe 的两个核心量是**硬编码常量**：`WrapNoL` 的 `0.5` 与 scatter 的 `roughness = 0.6` 写死在 `engine/twoSidedFoliageLighting.glsl:26-52`，材质无法扫。这直接限定了 `F-01` / `F-10` 的可扫范围，必须先记录再判结论 | 架构核对 | ☐ **先做** |
| `F-13` | 架构核对（§1.5.1） | `materialAssetValidator.cpp:759-772` 把 TwoSidedFoliage 限制为 `Opaque` / `OpaqueClip` + `cullMode = None`，因此 `ShadeTwoSidedFoliageForwardSurface`（`forwardLighting.glsl:387-419`）**当前无可达调用者**。任何"forward 下的 Foliage"结论都无效 | 架构核对 | ☐ **先做** |

**论文 case**

| ID | 案例（基准） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `F-10` | [GPU Gems 3 Ch 16](#gpu-gems-3)｜叶片透光着色 | 程序化风 + 透光着色下的植被观感 | `SC_foliage_potted_plant_02` | ☐ |
| `F-11` | [Henyey & Greenstein 1941](#henyey-greenstein)｜相位函数 | 若未来引入相位函数，用于对照当前 GGX scatter 的方向分布差异 | —— | ☐ 仅在前向散射方向性成为问题时做 |

### ⑤ 专属约定

- 本模型**没有专门的论文基准图**，基础 case 的基准是 UE 官方文档与 UE 5.8 Legacy `TwoSidedBxDF`
  源码，`F-10` 的基准是 GPU Gems 3 Ch 16 的行为描述；**必须在记录里写清这一点**；
- 落地前必须先确认 `TwoSidedFoliage` 的**材质入口、RenderState、Alpha 约定与 ShadowCaster 路由**；
  不能仅把现有 `M_speedtree` 改名来模拟该模型；
- 真实叶片厚度、BSSRDF、多次散射、WPO / PDO 都不在当前闭包内；引入时必须建立独立版本，
  不能修改 Legacy 的固定公式与常量。

## 2.7 Eye

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中（实现齐备，缺数值基准） |
| 实现锚点 | `M_eye` / `M_eyeCornea` / `M_eyeInner` / `M_eyeDeferred`；`engine/eyeLighting.glsl`、`engine/eyeGeometry.glsl`；`generator/eyeCausticLut.comp` |
| 已完成 | 分层几何、角膜 / 虹膜 / 巩膜材质分区、Forward/Deferred/dual-shell 路径、LUT |
| 未完成 | Fresnel 曲线、折射视差解析对照、LUT 自洽、闭包能量审计 |
| 阻塞 | UE 官方明确 Eye 对 shader、材质、几何与 **UV 布局**有强依赖；无专门论文基准图 |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| UE [Shading Models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)｜Eye | Eye 着色模型的输入语义与几何 / UV 依赖 | [书库 `#ue-shading-models`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-shading-models) |
| UE [Digital Humans](https://dev.epicgames.com/documentation/en-us/unreal-engine/digital-humans-in-unreal-engine)｜Eye geometry / UV / iris refraction / limbus | 角膜-虹膜分层、折射视差、边缘环 | [书库 `#ue-digital-humans`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-digital-humans) |
| PBRT 4ed｜Dielectric BSDF | 角膜界面的 Fresnel / Snell 折射 | [书库 `#physically-based-rendering`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#physically-based-rendering) |
| Jensen 2001 | 巩膜 / 眼睑次表面 | [书库 `#jensen-bssrdf`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#jensen-bssrdf) |
| OpenPBR｜dielectric / subsurface | 界面与次表面语义的规范定义 | [书库 `#openpbr`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#openpbr) |
| UE 5.8 Legacy / Substrate Eye BSDF 源码 | 闭包的精确形式（实时参考实现） | 实时参考实现 |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| `EyeFresnel` | `engine/eyeLighting.glsl:48` | [Hoffman 2013](#hoffman-physics-math)｜Fresnel 与 F0；[PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF) | 必须检查 Schlick 近似的方向极点 |
| 虹膜折射 / 视差 | `engine/eyeGeometry.glsl`；`SampleEyeIrisColor/Normal/Mask` | [UE Digital Humans](#ue-digital-humans)｜iris refraction；[PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)｜Snell 折射 | 依赖 UV 布局，属于 shading contract |
| 巩膜漫反射近似 | `engine/eyeLighting.glsl:124` `ApplyEyeScleraDiffuseApproximation()` | [Jensen 2001](#jensen-bssrdf)｜次表面 | 是**近似**，需要单独量化 |
| 焦散 LUT / 角膜透射 | `generator/eyeCausticLut.comp`；`SampleEyeCaustic()` | 自定预积分（本项目）；物理依据见 [UE Digital Humans](#ue-digital-humans)｜caustic | 生产路径是否使用需核对；LUT 自洽必须验证 |
| profile 对编码 | `engine/gbufferCodec.glsl:192` `EncodeEyeProfilePair()` | 自定编码（本项目） | 编解码往返与版本位校验 |
| 分层几何契约 | `engine/eyeGeometry.glsl`；`documents/plan/rendering/archive/eye-shading-model.md` | [UE Digital Humans](#ue-digital-humans)｜几何与 UV | 几何与 UV 属于 shading contract，不能只换 ShadingModelId |

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `E-01` | [PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)；[Hoffman 2013](#hoffman-physics-math) | `EyeFresnel` 随入射角曲线：`F(0°)=F0(IOR)`、`F(90°)=1`、单调；与精确 Fresnel 的最大偏差 | 曲线（新增 mode） | ☐ |
| `E-02` | [UE Digital Humans](#ue-digital-humans)｜iris refraction | 边缘光线聚焦的后焦距与解析 `f = R/(n-1)` 的偏差；视差随视角变化方向正确 | 数值 + 场景对照 | ☐ |
| `E-03` | `generator/eyeCausticLut.comp` | 焦散 LUT 与离线路径追踪参考图逐像素比对 | 离线对照 | ☐ |
| `E-04` | [Jensen 2001](#jensen-bssrdf)｜巩膜散射 | 巩膜近似随半径单调衰减、能量有界 | 数值 | ☐ |
| `E-05` | `engine/gbufferCodec.glsl:192` | `EncodeEyeProfilePair` 编解码往返误差；非法版本位被拒绝 | 数值 | ☐ |
| `E-06` | 本仓库 Eye 合同 · 闭包审计 | 逐项列出 `ShadeEyeSurface()` 的每个能量项，检查无重复计入、无漏项（沿用 Legacy / Substrate 收敛后的同一 evaluator） | 架构核对 | ☐ |
| `E-07` | [PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)｜Snell | 进入角膜 / 离开角膜的折射方向与解析 Snell 解一致（含全内反射分支） | 数值对照 | ☐ |
| `E-08` | [UE Digital Humans](#ue-digital-humans)｜UV 布局 | 角膜 / 虹膜 / 巩膜 / 眼睑的 UV 分区与几何分层映射正确（不靠目视判断） | Debug + 数值 | ☐ |
| `E-09` | `documents/plan/rendering/archive/eye-shading-model.md`｜路径矩阵 | Forward / Deferred / dual-shell 三条路径下同一机位结果一致；差异归因到路径本身 | 场景对照 | ☐ |
| `E-12` | 参数面审计（本仓库） | 四个 Eye 材质参数面**完全相同**，差异只在 `renderMode` 与 `u_eyeLayer` 默认值：`M_eye`=0 / `M_eyeInner`=1 / `M_eyeCornea`=2，且 `eyeMaterialContract.cpp:230-243` 强制校验该层号与 render mode 匹配 | 架构核对 | ☐ **先做** |
| `E-13` | 参数面审计（本仓库） | 登记三个**声明但不驱动着色**的参数：`u_pbrFactors`（被 `M_eye.surface.glsl` 覆盖为 Eye 语义）、`u_eyePupilDilation`、`u_eyeGaze`（只写入 `MaterialInputs`，无 evaluator 读取）。它们不得被当作"已支持瞳孔 / 视线控制" | 架构核对 | ☐ |
| `E-14` | 参数面审计（本仓库） | 宏表声明了 `USE_ALBEDO_MAP / USE_NORMAL_MAP / USE_PBR_MAP`，但 `M_eye*.json` **没有对应贴图槽**（只有 iris/sclera/emission 五个）：打开这些宏无槽可绑 | 架构核对 | ☐ **阻塞项** |
| `E-15` | `pass/sssComposition.frag:42-122` + `pass/toneMapping.frag:78-147` | Debug 模式 42-63 **不是**原始 evaluator 输出（被 Eye 分支重写）；另需按 `IsRawDebugView` 区分哪些模式套了 bloom / exposure / tone mapping。做 Eye 数值对照前必须先确认用的是不是 raw 通道 | 架构核对 | ☐ **先做** |

**论文 case**

| ID | 案例（基准） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `E-10` | [UE Digital Humans](#ue-digital-humans) + [PBRT 4ed｜Dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)｜解析复现 | 角膜 Fresnel + 虹膜视差（无专门论文图，基准是官方文档 + 解析解） | `SC_simple_character` 已有 | ☐ 需定位眼球 |
| `E-11` | [Jensen 2001](#jensen-bssrdf)｜眼睑次表面 | 眼睑与眼窝的次表面输运 | 缺 | ☐ |

### ⑤ 专属约定

- UE 官方明确 Eye 对 shader、材质、几何与 **UV 布局**有强依赖；几何和 UV 属于 shading contract 的一部分，
  不能只换 `ShadingModelId`；
- 暂不新增外部资产，用 `SC_simple_character` 作为综合回归入口；
- **不重新创建已清理的探针场景**：`SC_subsurface_models`、`SC_cloth_models`、`SC_eye_probe`、
  `SC_eye_dual_shell_probe`、`SC_eye_deferred_probe`；
- 本模型**没有专门论文基准图**，`E-10` 的基准是 UE 官方文档加解析解，必须写进记录。

## 2.8 ThinTranslucent

### ① 对齐状态

| 项 | 内容 |
| --- | --- |
| 状态 | ◐ 进行中（语义扫描已做，折射 / 守恒未验证） |
| 实现锚点 | `M_thinTranslucent`；`engine/materialForwardOutput.glsl:218` `BuildThinTranslucentFallbackOutput()`；`documents/plan/rendering/archive/thin-translucent-shading-model.md` |
| 已完成 | `SC_paper_case_thin_translucent` 网格背景 + 透射色 / 粗糙度扫描 |
| 未完成 | Fresnel 与介质反射、内部往返、能量部分、路径一致性 |
| 阻塞 | 玻璃球 / 马 / 多层透明资产缺失 |

### ② 论文来源

| 来源 | 负责什么 | 链接 |
| --- | --- | --- |
| Walter et al. 2007, *Microfacet Models for Refraction through Rough Surfaces* | 粗糙电介质 BTDF、Jacobian、能量分配 | [书库 `#walter-microfacet-refraction`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#walter-microfacet-refraction) |
| Wyman 2005, *Image-Space Refraction* | 屏幕空间折射近似的误差边界 | [书库 `#wyman-refraction`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#wyman-refraction) |
| McGuire & Bavoil 2013, *Weighted Blended Order-Independent Transparency* | OIT 近似结构 | [书库 `#mcguire-oit`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#mcguire-oit) |
| Porter & Duff 1984, *Compositing Digital Images* | 合成代数 | [书库 `#porter-duff`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#porter-duff) |
| UE [Thin Translucent Material Output](https://dev.epicgames.com/documentation/en-us/unreal-engine/thin-translucent-material-output-in-unreal-engine) | Transmittance Color / Surface Coverage 语义 | [书库 `#ue-lit-translucency`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-lit-translucency) |
| UE [Lit Translucency](https://dev.epicgames.com/documentation/en-us/unreal-engine/lit-translucency-in-unreal-engine) | Surface ForwardShading 与路径限制 | [书库 `#ue-lit-translucency`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#ue-lit-translucency) |
| Filament｜transparency / refraction | 实时折射与透明度实现 | [书库 `#filament`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#filament) |
| OpenPBR | thin dielectric slab 的规范语义 | [书库 `#openpbr`](../../../../yyb-knowledge-book/src/content/docs/tools-resources/books/index.mdx#openpbr) |

### ③ 算法来源

| 实现 | 位置 | 论文 / 书本来源 | 备注 |
| --- | --- | --- | --- |
| 表面反射（与 DefaultLit 相同的 BRDF） | `engine/materialForwardOutput.glsl:218` | [UE Lit Translucency](#ue-lit-translucency)｜Legacy 闭包与 Default Lit 反射一致 | `Specular` 不参与这条路径 |
| 透射分支：零次内部往返 | 同上 | UE 5.8 Legacy Thin Translucent 闭包 | Legacy 结果 `0.9216`；无限内部往返为 `12/13 ≈ 0.923077` |
| 薄板 Fresnel / 界面 | 同上 | [Porter & Duff 1984](#porter-duff)｜合成代数；界面物理见 [Walter 2007](#walter-microfacet-refraction) | `Transmittance ≠ Refraction` |
| Transmittance Color | `M_thinTranslucent` 输入 | [UE Thin Translucent Material Output](#ue-lit-translucency) | 是颜色 / 吸收，不是几何偏移 |
| Surface Coverage | `M_thinTranslucent` 输入 | [UE Thin Translucent Material Output](#ue-lit-translucency) | 与 Transmittance Color **不能合并** |
| 屏幕空间折射 | **未实装** | [Wyman 2005](#wyman-refraction)；[McGuire & Bavoil 2013](#mcguire-oit) | 登记为未实装项 |
| Legacy blend state | `engine/materialForwardOutput.glsl:218` 区域 | UE 5.8 Legacy translucent base-pass blend-state | 透明排序限制必须记录 |

### ④ Case 列表

**基础 case**

| ID | 案例（来源） | 验证目标 | 形式 | 状态 |
| --- | --- | --- | --- | --- |
| `T-01` | [Walter 2007](#walter-microfacet-refraction)｜电介质界面 | 薄板 Fresnel 曲线：`F(0°)=F0`、`F(90°)=1`、单调；与精确 Fresnel 比对 | 曲线（新增 mode） | ☐ |
| `T-02` | UE [Lit Translucency](#ue-lit-translucency)｜Legacy 零次往返 | 确认内部往返截断在 `0.9216`，与无限级数 `12/13` 的偏差 ≤ 0.0015 并记录 | 数值核对 | ☐ |
| `T-03` | [Porter & Duff 1984](#porter-duff)｜合成代数 | 反射部分 + 透射部分 = 1（无能量漏项 / 重复计入） | 数值积分（离线） | ☐ |
| `T-04` | [Walter 2007](#walter-microfacet-refraction)｜rough dielectric BTDF | 若粗糙透射接入，BTDF 归一化与 Jacobian 满足论文式(17)–(20) | 数值积分（离线） | ☐ 仅在校准折射时做 |
| `T-05` | UE [Thin Translucent Material Output](#ue-lit-translucency) | Transmittance Color 扫描：只改变颜色 / 吸收，不产生几何偏移 | 单变量扫描 | ◐ 已建，**缺数值判定** |
| `T-06` | UE [Thin Translucent Material Output](#ue-lit-translucency) | Surface Coverage 与 Transmittance Color 独立作用：改变其一不影响另一条的语义 | 单变量扫描 | ☐ |
| `T-07` | `SC_paper_case_thin_translucent` | 表面粗糙度扫描：反射高光展宽，透射色不随粗糙度变化 | 单变量扫描 | ◐ 已建，**缺数值判定** |
| `T-08` | UE [Lit Translucency](#ue-lit-translucency)｜Surface ForwardShading | 同一 MI 在 Forward / Deferred 路径下结果可比；差异归因到路径本身 | 场景对照 | ☐ |
| `T-09` | [McGuire & Bavoil 2013](#mcguire-oit)｜排序 | 透明排序 / 混合下的边界行为已记录；不得把路径差异当成公式差异 | 场景对照 | ☐ |
| `T-15` | 架构核对（§1.5.1） | **本模型是 Forward-only**：Deferred 路径没有 case，GBuffer 也从未编码 `transmittanceColor` / `surfaceCoverage`。任何"Deferred 下的 ThinTranslucent"结论都无效 | 架构核对 | ☐ **先做** |
| `T-16` | 参数面审计（本仓库） | `u_surfaceFactors.z`（specular）**不参与表面反射**（表面走 Default Lit 的 `F0 = lerp(0.04, BaseColor, Metallic)`），只进入透射 Fresnel；`u_surfaceFactors.x`（roughness）不产生透射模糊。两条都要写进结论，避免误判成实现错误 | 架构核对 | ☐ |
| `T-17` | 参数面审计（本仓库） | 吸收用 `TransmittanceColor^(1/NoV)`，**没有单位厚度入口**：无法用材质参数扫"厚度 → 吸收"关系。Walter 的 thickness 类量在本模型不可复现 | 架构核对 | ☐ |

**论文 case**

| ID | 案例（论文图） | 展示什么 | 资产 | 状态 |
| --- | --- | --- | --- | --- |
| `T-10` | [Walter 2007](#walter-microfacet-refraction) Fig 1 · 蚀刻世界地图的玻璃球 | rough dielectric 折射与能量分配 | 缺 | ☐ |
| `T-11` | [Walter 2007](#walter-microfacet-refraction) Fig 10 / 11 · 毛玻璃 / 磨砂样品的 BTDF 拟合 | 粗糙透射的方向分布 | 缺 | ☐ |
| `T-12` | [Wyman 2005](#wyman-refraction) Fig 5 · 马（ray traced 参考 vs 近似） | 屏幕空间折射的误差边界 | 缺 | ☐ |
| `T-13` | [McGuire & Bavoil 2013](#mcguire-oit) Fig 4 / 5 · San Miguel 多层透明 | 顺序无关透明的近似结构 | 缺 | ☐ |
| `T-14` | 孔明灯纸罩（薄介质反射、透射色与背光轮廓） | 薄层介质在真实资产上的观感 | 待授权或自制替代 | ☐ |

### ⑤ 专属约定

- **Transmittance 不等于 Refraction**：前者是颜色 / 吸收，后者是几何偏移；
- **Surface Coverage 与 Transmittance Color 不能合并**；
- 孔明灯案例：`PaperShell → M_thinTranslucent`、`WireFrame → M_pbr`、`Burner / Flame → M_unlit`，
  三者保持**独立材质与独立几何**，不能把发光或反射结构烘进纸罩；
- 必须记录 Forward / Deferred 路径限制，不能把不同路径的结果当作公式差异。

## 2.9 Unlit 与辅助材质

**定位**：`Unlit`、`VertexColor`、`Shadow`、`MeasureGrid` 作为**辅助或调试材质**，不作为主 case，
也不为其建立论文 case。它们本身没有需要验证的着色模型；它们只服务于别的模型的 case。

用途：

- 论文 case 场景中的辅助几何（底座、展示台、背景、火焰、发光源）；
- 曲线探针本体（`M_brdfPlot` 就是 Unlit）；
- Debug 视图与测量网格（`M_measureGrid`）。

约定：

- 辅助材质必须在记录中明确标注，**不得与主案例结论混在一起**；
- 辅助几何不使用会污染主模型判断的发光或反射材质；
- 如果某个辅助材质开始承担"行为正确性"的验证责任，它就升级为正式 case 并回到对应模型章节。

---

# 3. 逐模型进度与执行顺序

## 3.1 推荐推进顺序

```text
DefaultLit  ->  Cloth / Sheen  ->  Hair  ->  Subsurface  ->  TwoSidedFoliage  ->  ClearCoat  ->  ThinTranslucent  ->  Eye
```

理由（2026-09-12 清理后重述）：

1. **DefaultLit 先行**：它是所有其它模型的比较基线，也是清理后**唯一还有实现**的模型（`M_pbr`），
   曲线 case 与测量链路可直接使用；重做时要一并处理 `D-20` / `D-21`；
2. **Cloth / Sheen 紧随**：§1.4.2 确认它是唯一含自创公式的模型，重建前必须先定路线
   （`S-23` / `S-29` / `S-31`）；它的曲线探针场景（`SC_paper_case_plot_cloth_solo`）与
   `common/clothBrdf.glsl` 都还在仓库里，测量链路可以直接复用；
3. **Hair**：`SC_paper_case_plot_hair_solo` 与 `common/hairPathScattering.glsl` 同样在位，
   但基准选择（`H-20`：论文 `T^p` vs UE 经验式）必须先定，否则后面的 case 没有对照对象；
4. **Subsurface / TwoSidedFoliage**：不是"补数值验证"而是**从论文重写**求值器 + profile/LUT 资产 +
   case 场景，工作量按实现级估算，放在测量链路稳定之后；
5. **ClearCoat**：有明确的参数缺口（膜厚 / 吸收），需要先做"是否新增参数"的决定；
6. **ThinTranslucent / Eye**：依赖新增资产最多（玻璃 / 多层透明 / 眼球几何与 UV 契约），放在最后，
   避免资产授权阻塞前面所有工作。

## 3.2 进度表

| Shading Model | 章节 | 基础 case | 论文 case | 状态 | 主要阻塞 |
| --- | --- | --- | --- | --- | --- |
| DefaultLit | §2.1 | 15 | 4 | ◐ | ✅ `M-07` / `M-08` 探针与测量工具已就绪（§1.3 / §1.5.3）；**`D-20` / `D-21` 审计发现待处理**（IBL 域的 G 用法与来源），`D-19` 未做 |
| ClearCoat | §2.2 | 9 | 4（其中 2 项 ⛔） | ☐ **待按论文实现** | 旧实现（`M_carPaint`）已删除；重建时先按 UE 冻结接口，再处理膜厚 / 吸收的参数缺口；缺 Jakob Fig 4 资产 |
| Cloth / Sheen | §2.3 | 27 | 3 | ☐ **待按论文实现** | 旧实现（含 5 项无出处公式）已删除；重建前先定路线（`S-23` A / `S-29` B / `S-31` C，见 §1.4.3）；缺双色天鹅绒圆柱与服装资产 |
| Hair | §2.4 | 16 | 4 | ☐ **待按论文实现** | 旧实现已删除；重建前先定 `H-20` 基准（论文 `T^p` 路径长 vs UE Legacy 经验式）；Hair tangent 需离线生成 |
| Subsurface | §2.5 | 14 | 3 | ☐ **待按论文实现** | 旧实现（三套入口）已删除；重建时按论文定 profile / LUT 参数面；皮肤头模授权 |
| TwoSidedFoliage | §2.6 | 11 | 2 | ☐ **待按论文实现** | 旧实现已删除；重建前先决定背光常量是否参数化；无专门论文基准图 |
| Eye | §2.7 | 12 | 2 | ☐ **待按论文实现** | 旧实现（含四个母材质与 dual-shell）已删除；重建时必须连几何 / UV 契约一起设计 |
| ThinTranslucent | §2.8 | 12 | 5 | ☐ **待按论文实现** | 旧实现已删除；重建时必须重新设计 renderMode 与双源输出路径（`base.frag.glsl` 里该分支现为编译期错误）；缺玻璃球 / 马 / 多层透明资产 |
| Unlit | §2.9 | —— | —— | —— | 辅助材质，不作 case |
| **合计** | | **114** | **27** | | |

> 另有**共享 case** 15 项（`M-01..M-08` 数学基础、探针与坐标系规范、`A-01..A-07` 架构一致性），不归属于任何
> 单个模型，但每个模型落地时都要回到它们核对一次。全文档 case 总数为 **159**；
> 其中 `S-23` / `S-29` / `S-31` 是 Cloth 的**三条可选路线**（各向同性 / microcylinder / 两套并存），执行其一；`S-30` 是知识库侧的条目维护。
> **已完成**：`M-07`（2026-09-12，含 `measure_plot_curve.py` / `run_paper_case.ps1` / `bmp_reader.py` /
> `console_inject.ps1` 四个工具）与 `M-08`（同日，探针坐标轴化 + 图头说明与图例 + 探针场景 scale 约定），
> 见 `shading-model-case-records.md`。
> **删除**：ClearCoat / Cloth / Eye / Hair / Subsurface ×3 / ThinTranslucent / TwoSidedFoliage 的旧实现与案例资产
> （2026-09-12，清单见 `archive/README.md`）。

### 3.2.1 每个模型的第一步（阻塞项）

这些 case 是**先做项**：它们不验证着色正确性，只负责把"判据本身"弄清楚。跳过它们会让后面的
数值结论失去意义。

**全局前置（三件，必须最先做）**：

1. ✅ **`M-07`**（2026-09-12 完成）—— texCoord V 方向与 `fov` 约定已在单次会话中重新钉死，
   "测量前先 `tonemap 0` + 关 Bloom"的流程与两个共享工具同时落地（§1.3 / §1.5.3，
   记录见 `shading-model-case-records.md#m-07`）。**§2.1 / §2.3 / §2.4 的 mode 0 / 1 / 2 曲线 case
   自此可以做绝对的逐列像素判定**（已实测 ≤0.5 px）；
2. **Cloth 整改路线定案（`S-23` A / `S-29` B / `S-31` C）** —— §1.4.2 审计确认 v2 各向异性路径含 **5 项论文里没有的公式**（`S-10` `S-11` `S-12` + `aspect=2^anisotropy` + 分层因子）；§1.4.3 补查已找到有出处的替代路线（microcylinder），且已确认 A / B 是**两种不同外观**。按 §0.3 这是**唯一被确认的自创公式区**，属最高优先级：实现本身不等于论文时，任何对齐结论都无效；
3. **DefaultLit 的 `D-20` / `D-21`** —— IBL 域的几何项用法与来源（Karis 明确禁止把 analytic
   调整用于 IBL；`k=α²/2` 在论文里不存在）。这两项会同时影响其它所有用到 `GeometrySmith` 的模型。

> `M-07` 完成后，**下一步就是第 2 件**（Cloth 整改路线定案），第 3 件与 `DefaultLit` 的曲线 case
> 可以并行推进——它们现在有可用的测量链路了。

**读法（2026-09-12 清理后）**：下表里"先做 case"的**含义变了**。原先它们的目的是"在已有实现上先弄清
判据"，现在对应模型没有实现，所以它们读作**重建该模型前必须先定下的设计决策**：

- 仍然有效且仍然必须先做：`S-23` / `S-29` / `S-31`（三条外观路线选一条）与 `S-24..S-27`、
  `D-20` / `D-21`（DefaultLit 仍在，且影响所有用 `GeometrySmith` 的模型）；
- **需要重新推导**：凡是把**已删产物**当作前提的行——`H-16` / `H-19`（"用 `M_hair` 还是 `M_hairProbe`
  做基准"，两者都已删除）、`C-13`、`U-15` / `U-16`、`E-12` / `E-14` / `E-15`、`F-12` / `F-13`、
  `T-15` / `T-16`。重建这些模型时要先在论文里确定判据与参数面，再据此定义新的探针材质与 case 锚点。

| 模型 | 先做 case | 为什么要先做 |
| --- | --- | --- |
| ~~**全部**~~ | ~~**`M-07`**~~ | ✅ 已完成（§1.3）：V 方向与 `fov` 约定已定，曲线类像素判定可用 |
| **Cloth** | **`S-23` / `S-29` / `S-31` 定一条**、`S-24` `S-25` `S-26` `S-27` | 整改路线未定 + 两项映射/分层因子决策；未完成前 Cloth 无法离开 ⚠️（**当前第一优先**） |
| **DefaultLit** | **`D-20` `D-21`** | IBL 几何项会影响所有走 `GeometrySmith` 的模型（含 ClearCoat） |
| DefaultLit | `D-11` `D-19` | `M_brdfPlot.json` 的 `u_plotMode` range 已扩到 `[0,3]`（已核对生效）；`D-19` 的校验规则要按改后的 range 判，**仍未做** |
| DefaultLit | `D-02` `D-03` | 测量链路已就绪，可直接重测；`D-03` 需要新增"`k=α/2` direct 变体"的 mode |
| ClearCoat | `C-13` | 先记录哪些 coat 参数真正进入 shader，否则 `C-04` / `C-05` 的扫描变量可能根本没生效 |
| Hair | **`H-20`** | 基准选择（论文 `T^p` vs UE Legacy 经验式）决定后面所有 case 的对照对象 |
| Hair | `H-16` `H-19` | 先确定用 `M_hair` 而不是 `M_hairProbe` 作为论文基准，并决定要不要为探针补场景 |
| Subsurface | `U-15` `U-16` | 三套入口的参数来自三个不同地方，不先理清就无从设计扫描 |
| TwoSidedFoliage | `F-12` `F-13` | 常量硬编码 + forward 不可达，直接限定 case 的可行范围 |
| Eye | `E-12` `E-14` `E-15` | 层号契约、宏与贴图槽、Debug 模式是否 raw，三者都会让判定读错数 |
| ThinTranslucent | `T-15` `T-16` | Forward-only 与 specular 语义边界决定结论的适用范围 |

## 3.3 Case → 论文图索引（快速查阅）

| 论文 / 书本 | 图 / 节 | 用在哪个 case |
| --- | --- | --- |
| Karis 2013 | Fig 2 | `D-02` `D-03` |
| Karis 2013 | Fig 3 / 4 / 5 | `D-17` `D-15` |
| Karis 2013 | §3 式(5)、§5 式(7) | `D-04` |
| Burley 2012 | Fig 12 | `D-05` |
| Neubelt & Pettineo 2013 | Fig 1 | `D-16` |
| Neubelt & Pettineo 2013 | Fig 5 / Fig 6 | `S-01` `S-12` `S-15` |
| Estevez & Kulla 2017 | Fig 1 / 2 / 3 / 4 | `S-02` `S-11` `S-16` |
| Heitz 2014 | §3–§5 | `D-01` `M-03` |
| PBRT 4ed | Reflection Models / Microfacet / Dielectric / Volume | `M-02` `M-04` `D-06` `D-07` `U-*` `E-01` `T-*` |
| Jakob 2015 layerlab | Fig 4、adding equations | `C-02` `C-03` `C-09` |
| Weidlich & Wilkie 2007 | Fig 3 / Fig 5 | `C-10` `C-11`（⛔） |
| Marschner 2003 | Fig 5 / 12 / 13 | `H-01` `H-12` `H-13` |
| d'Eon 2011 | Fig 4 / 12 | `H-03` `H-14` |
| Chiang 2016 | Fig 2 / 7 | `H-15` |
| Jensen 2001 | Fig 9 / Fig 10 | `U-12` `U-13` |
| GPU Gems 1 Ch 16 / GPU Gems 3 Ch 14 / Ch 16 / Ch 23 | 各章 | `U-08` `U-03` `F-10` `H-07` |
| Walter 2007 | Fig 1 / 10 / 11 | `T-10` `T-11` |
| Wyman 2005 | Fig 5 | `T-12` |
| McGuire & Bavoil 2013 | Fig 4 / 5 | `T-13` |
| UE 官方文档（Shading Models / Digital Humans / Thin Translucent / Subsurface Profile / Lit Translucency / Two Sided Foliage） | 各小节 | `A-01` `A-04` `U-05` `E-02` `E-08` `F-01` `T-05` `T-06` |

---

# 4. 通用验收与风险

## 4.1 每个 Case 的通用验收

- [ ] 场景可以从 `config/config.json -> resourcePath` 正确解析全部资源。
- [ ] 所有 `SM_*.json`、`MI_*.json` 与纹理引用无悬挂路径。
- [ ] 主模型使用计划指定的 `M_*.json` 与 Shading Model ID。
- [ ] 辅助几何使用明确的 `DefaultLit` / `Unlit` 材质，不污染主案例结论。
- [ ] 相机、主光、轮廓光、环境与曝光配置可重复。
- [ ] 资产来源、许可证与转换记录齐全。
- [ ] **曲线 / 分布类 case 必须有像素级数值对照记录**，误差 1 像素量级；只有截图与识图描述的不算通过。
- [ ] **论文 case 必须绑定对应论文图（或说明基准来自官方文档），并给出差异归因**。
- [ ] 被验证的**算法本体在论文 / 书本 / 官方规范里有出处**，且已写明图号 / 式号 / 节号；

## 4.2 非目标

- **不自创算法**：算法本体（分布、visibility、衰减、相位函数、中间近似）一律取自论文 / 书本 /
  官方规范；找不到出处就走 §0.3.1 阶梯，最后一格是"声明做不了"。这条优先于其它所有非目标；
- 不重新实现 Hair、Cloth、Eye 或 Subsurface 的**光照公式**（对齐是"照论文抄对 + 验证"，不是自行设计）；
- 不引入实时布料模拟、发丝动力学、DCC 插件或模型在线下载器；
- 不把 `Strata` 或 `SingleLayerWater` 提前包装成已支持的正式案例；
- 不清理与案例无关的普通学习场景；
- 不为了凑 case 而给模型加参数——参数缺口（例如 ClearCoat 的膜厚）先作为 ⛔ 记录，
  是否新增由功能需求决定，不由验证需求决定；
- **不新增运行时测试命令**。旧文档里出现过的 `--framesmoke`、`--hair-validation-test` 在代码里
  不存在，其引用的场景与产物也不存在；新 case 脚本一律只使用 §1.5.4 列出的参数。

## 4.3 主要风险

| 风险 | 影响 | 处理方式 |
| --- | --- | --- |
| 把识图模型的定性描述当作验证证据 | 得出错误结论（本项目已发生） | 曲线类一律用像素级数值对照（§1.9.1） |
| 沿用已被证伪的隐式约定（V 方向 / fov） | 曲线判定整体偏移，误差目标不可达 | `M-07` 先重新探明；在此之前只做相对比较 |
| 用近似参数冒充论文参数 | 宣称"已复现"但实际不是同一张图 | 记录差异：`k=α/2` vs IBL 变体、膜厚 vs coat 权重、UE Legacy vs pbrt 参数化 |
| 自创公式被当成"论文实现" | 结论无法追溯到任何外部基准 | 算法本体只认论文 / 书本 / 官方规范；不参与数值的约定才进 §0.3.2 登记表 |
| 论文没给的形式自行补全 | 得到一个"看起来对"的中间式，无法验证 | §0.3.1 阶梯：换论文写了的东西 → 引用其它已发布来源 → 登记工程约定 → 声明做不了 |
| 把自定近似混进"已验证" | 后续无法判断该不该改 | §1.4 审计表 + ⚠️ 待整改状态；审计项不清零不得转 ☑ |
| 把"资产渲染得好看"当成模型对齐 | 论文 case 结论不可信 | 论文 case 必须绑定对应论文图与特征 |
| 跳过基础 case 直接做论文 case | 资产上的差异无法归因 | §0.5 流程：来源冻结 → 基础 → 论文 |
| 用被后处理过的 Debug 通道做判定 | 读到的不是 evaluator 原始输出 | 先确认该模式是否 raw（§1.6.1）与 `IsRawDebugView` 名单 |
| 在 Release 构建里做 Debug 类 case | `ENABLE_DEBUG_VIEW` 不生效，全部读出 0 | 确认构建类型与 `NDEBUG`（§1.6.1） |
| 外部资产授权不完整 | 不能进入正式资源库 | 先记录为参考，改用授权清晰的替代资产 |
| 绘图面板留有背景 | 像素测量被背景污染 | 面板铺满画面；用四角像素检查（§1.7.2.1） |
| 复杂场景遮蔽主效果 | 结论不可复现 | 先验收独立 case，再组装综合 Gallery |

---

# 5. 本计划的完成定义

以下条件全部满足后，本计划才可从"计划"迁移为验收记录：

- [x] `M-07` 已完成：texCoord V 方向与 `fov` 约定在同一次会话中被重新钉死，并写入 §1.3
      （2026-09-12；顺带修正了 `M_brdfPlot` 的竖直镜像，见 §1.3 与 case 记录）。
- [x] §1.5.3 的工具（`measure_plot_curve.py`、`run_paper_case.ps1`，另加 `bmp_reader.py`、
      `console_inject.ps1`）已入库并自测通过。
- [x] `M-08` 已完成：探针图带完整坐标系（轴名 + 主次刻度 + 由刻度值格式化出的数值标签 + 绘图区参数），
      曲线是**等宽屏幕宽度**的描边，测量脚本会从图上核对轴框位置；探针场景 scale 统一为精确填满画面
      （§1.5.2 / §1.7.2.1）。
- [ ] **`D-20` / `D-21` 已处理**：IBL 域的几何项要么改用论文允许的形式，要么按 §0.3.1 登记为
      "论文未指定"；不存在"用了论文明确禁止的变体却没记录"的情况。
- [ ] **Cloth 整改路线已执行（`S-23` / `S-29` / `S-31` 之一）**：`anisotropy ≠ 0` 路径不再含任何论文里没有的公式；若选方案 C，另需满足"两个 variant、指定生产臂与对照臂、两套能量不交叉"三条硬约束。
- [ ] **`H-20` 已定**：Hair 的基准（Marschner 论文式 vs UE Legacy 经验式）已明确声明，且记录里不混称。
- [ ] §3.2 中 8 个正式模型的每一行都是 ☑ 或 ⛔，没有 ☐ / ◐ / ⚠️ 残留。
- [ ] 所有曲线 / 分布类 case 都有像素级对照记录，误差在 1 像素量级，且由 §1.5.3 的脚本产出。
- [ ] §2.x 的「算法来源表」里每一项都写着 ✅ 式号 或 🔎 待核对，**没有 ❌ 论文里没有**；
      §1.4.2 审计表的 ❌ 项全部清零。
- [ ] 每个 ⛔ 受限模型都写清了缺失的参数名与它对应的论文量（例如 ClearCoat 的膜厚 / 吸收）。
- [ ] 每个模型的论文 case 都绑定了对应论文图，或注明基准来自官方文档，并给出差异归因。
- [ ] `SC_sphere_array` 与 `SC_car_showcase` 继续作为稳定基线，未被单模型 case 改动语义。
- [ ] 皮肤、石材、植物、Hair、Cloth 与 ThinTranslucent 的独立场景全部有明确状态。
- [ ] 每个已入库资产都有可追溯授权与转换记录。
- [ ] Beauty、Backlight、Debug 三类观察证据已归档，辅助材质没有混淆主结论。
- [ ] 资源引用、shader 编译、场景加载与允许的 runtime smoke 均通过。
- [ ] 尚未完成的 Eye、Foliage authoring、Water 与 Strata 边界仍被明确标记。
