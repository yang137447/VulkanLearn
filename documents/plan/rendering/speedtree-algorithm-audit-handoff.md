# SpeedTree 植被算法知识库审计交接

> 知识库：`D:\YYBWorkSpace\GitHub\yyb-knowledge-book`
>
> 迁移基线：`documents/plan/rendering/speedtree-knowledge-base-migration.md`
>
> 当前进度：**10 / 16 已通过，6 项待审计**
>
> 分组审计：**16 / 16 已核对**；学习内容仍按上面的逐项进度验收。

## 审计标准

每个算法逐页确认，未经过人工学习体验确认的页面不得标记为通过：

- 算法拥有独立页面，不向读者暴露跨算法切换器。
- 先说明实际用途和输入输出，再进入公式。
- 包含核心公式，并逐项解释变量、范围、单位和边界条件。
- 交互示例只演示当前算法；适合植被语境时，用树、枝条、叶片或风动数据说明。
- 示例明确区分源数据、编码数据、中间量和最终效果，避免把多个阶段画成一个含混流程。
- 验证桌面与窄屏布局、边界输入、控制台和 `npm run build`。

## 分组审计结论

16 项算法的范围已经重新核对（Billboard 独立专题与 Main/Shadow 一致性专题已在后续审计中移除）。等距一维 LUT 已从 `program/algorithms` 迁入 `rendering-math`；植被页只保留与 Oak / Runtime SDK 还原直接相关的项目：

- `rendering-math` 的 8 项都是可脱离植被成立的数学、编码或采样算法。
- `rendering/vegetation` 的 7 项覆盖风变形、Alpha coverage 与风动速度；游戏常用 LOD 过渡方案已迁至 `rendering/performance`。
- SpeedTree 的 byte offset、固定码本、`9 x 9 x 3`、20 点 profile、Branch/Ripple 顺序和 Gust 参数继续只属于 `rendering/speedtree-final-quality`。

`program/algorithms` 的独立运行时专题和实验台已全部删除；基础小公式仍可按需保留在该目录，领域算法的公式和验证下沉到对应植被或 SpeedTree 页面。

容易混淆的页面按“通用算法核 -> 植被领域组合”成对维护：

| 通用算法核 | 植被领域页 |
| --- | --- |
| Value Noise | 多尺度风噪声、分层风变形 |

内容边界处理：SpeedTree/Oak 的方向量化、Mixed-radix 与 20 点 LUT 只作为明确标注的领域案例，精确字段与消费关系仍以 SpeedTree 专题为准。相位去相关已完成实际拆分：通用页使用周期信号轨道，树群示例只存在于植被实例相位页。

### 容易误分项的复核

| 页面 | 最终归属 | 判定依据 |
| --- | --- | --- |
| 球面方向量化 | `rendering-math` | 输入是单位方向，输出是码本索引与量化误差；Branch direction 只是领域案例。 |
| Mixed-radix 离散打包 | `rendering-math` | 输入是有限范围整数元组，输出是可往返解码的整数；`9 x 9 x 3` 才是 SpeedTree 专用实例。 |
| 等距一维 LUT | `rendering-math` | 区间索引、局部参数和线性插值不依赖风或植被；规范页已从 `program/algorithms` 迁入数学组，旧 URL 仅作兼容入口。 |
| Value Noise、相位去相关 | `rendering-math` | 输出分别是连续信号和稳定相位差；树冠频段、实例风层与枝条消费关系全部下沉到植被页。 |
| LOD 过渡方案 | `rendering/performance` | 页面核心是游戏通用的表示切换成本与连续性策略；Oak `lodPosition` 只作为 Morph 的资产案例。 |
| Alpha Coverage | `rendering/vegetation` | 页面输入、输出和验证围绕叶片 coverage、mip 与 Pass 一致性。 |

复核采用保守原则：一个公式“还能用于别处”不足以保留独立算法页。只有页面的输入、输出、公式解析和验证都能脱离植被语义成立，并形成独立学习边界，才进入 `rendering-math`。

## rendering-math（8 / 8）

- [x] 球面方向量化
  - 页面：`src/content/docs/rendering-math/spherical-direction-quantization.mdx`
  - 结论：方向索引、查表与实时计算的关系已讲清。
- [x] 离散数据打包
  - 页面：`src/content/docs/rendering-math/discrete-data-packing.mdx`
  - 结论：mixed-radix、位宽利用率、取值范围和 SpeedTree noise offset 案例已讲清。
- [x] UNORM / SNORM 归一化整数编码
  - 页面：`src/content/docs/rendering-math/numeric-quantization.mdx`
  - 结论：同一 byte 的不同解释、端点规则和编码/解码过程已讲清。
- [x] binary16 半精度浮点
  - 页面：`src/content/docs/rendering-math/binary16-quantization.mdx`
  - 结论：half 位布局、normal/subnormal、ULP 和存储带宽取舍已讲清。
  - 后续增强：同一 float32 源值并排比较 `UNORM8 + range`、binary16、float32，尚未实现，不影响本轮算法通过结论。
- [x] 等距一维 LUT / 分段线性插值
  - 页面：`src/content/docs/rendering-math/piecewise-linear-lut.mdx`
  - 结论：`N` 个采样点对应 `N - 1` 个区间，SpeedTree 20 点曲线使用 `x * 19`。
- [x] 非等距曲线采样
  - 页面：`src/content/docs/rendering-math/nonuniform-curve-sampling.mdx`
- [x] 程序噪声
  - 页面：`src/content/docs/rendering-math/procedural-value-noise.mdx`
- [x] 相位去相关
  - 页面：`src/content/docs/rendering-math/phase-decorrelation.mdx`
  - 结论：通用稳定相位与植被实例风层已拆开；共享频率/振幅、稳定 offset 与每帧随机抖动的边界清楚。

## rendering/performance（0 / 1）

- [ ] 游戏常用 LOD 过渡方案
  - 待验收：原 Geometry Morph 页扩展为游戏常用 LOD 过渡方案，覆盖硬切换/迟滞、逐顶点 Morph、Dither Cross-fade、Billboard/Impostor 与组合管线；交互模式为 `lod-morph` / `lod-dither` / `lod-billboard` / `lod-combined`；Oak `lodPosition` 保留为 Morph 字段与 runtime 未接入的边界。

## rendering/vegetation（2 / 7）

> 7 项已完成分组核对；其中 2 项已由用户验收，另外 5 项保持待验收。

本轮内部修正：枝条受风示例收窄为横向分量与力矩，移除混淆主概念的 $\beta$，直接对比 $0^\circ$、$90^\circ$、$180^\circ$ 夹角下的弯曲作用；多尺度风噪声用“只有共享低频 / 叠加 Branch 与 Leaf/Ripple”的树体对照替换抽象波形，并明确三层结构权重；Alpha Coverage Mip 增加 raw/corrected 对照、实际缩放搜索和离散阶梯边界；A2C 改为四级 sample coverage；分层风补充各层独立贡献。

### 运行时风与结构变形

- [ ] 分层风变形
- [ ] 保长弯曲
- [x] 枝条受风的横向分量
  - 结论：用绕分叉点的力矩解释横向风，移除干扰主概念的 $\beta$；$0^\circ$、$90^\circ$、$180^\circ$ 边界与树枝弯曲趋势已讲清。
- [x] 多尺度噪声
  - 结论：用同一棵树的“只有共享低频 / 叠加 Branch 与 Leaf/Ripple”对照替代抽象波形，三层信号、结构权重与 LOD 高频衰减关系已讲清。

### 叶片、LOD 与远景表示

- [ ] Alpha coverage mip
  - 待验收：页面已覆盖 base/raw/corrected coverage、固定 cutoff、二分搜索与离散阶梯边界；页面收窄为离线 mip 生成单一主线；交互已改为真实 box 下采样并用珊瑚色高亮恢复的像素；桌面/窄屏与 canvas 绘制已检查。
- [ ] Alpha clip / A2C / dither
  - 待验收：已覆盖 Hard clip、A2C sample mask 近似、Dither 空间 pattern 的输入输出与样本域；补充 `n(x)` 范围与硬件 A2C 映射边界；桌面/窄屏与 canvas 绘制已检查。
- [ ] 风动 Velocity
  - 待验收：页面已覆盖用途、current/previous 完整重求值、NDC 速度公式与表示切换历史；交互显示 current/previous 树形、平均位移与速度；补充 NDC 与屏幕像素空间的说明；桌面/窄屏与 canvas 绘制已检查。

## 移除记录：Main/Shadow Coverage 一致性

用户审核结论：删除独立算法页及交互模式。`M_Main(x)=M_Shadow(x)` 的正确性是所有 Masked foliage Pass 的工程约束，不需要单独包装成算法页；相关语义继续保留在 SpeedTree 材质专题、光照与阴影专题的“Main 与 Shadow 必须重放同一棵树”等小节中。

## 移除记录：速度与相位的时间积分

用户审核结论：删除独立算法页及交互模式。`state += velocity * dt` 与加速度、速度、位置的基础积分关系相同；当前内容没有形成值得单独维护的算法边界，反而增加学习负担。需要该公式的领域页面可在具体上下文中直接说明。

## 移除记录：Influence 烘焙

用户审核结论：删除独立算法页及交互模式。Runtime SDK 和 `Standard.lua` 只能证明两套 Branch 顶点记录的字段、打包方式与消费顺序，不能证明 Modeler 如何把任意树形拓扑生成或压缩为这些记录。旧页面把运行时 anchor 公式反推成 authoring 算法，并与“保长弯曲”重复。可靠内容分别保留在资产语义、SpeedTree 风变形和保长弯曲页面。

## 移除记录：动态风 Bounds

用户审核结论：删除独立算法页及交互模式。风后扩张 bounds 不参与 Runtime SDK 风公式，当前 VulkanLearn 也没有主相机对象级 frustum 或 occlusion culling；静态 SpeedTree 资产 bounds 仍必须保留给 noise offset extent 与 Shared 高度权重。强风可能超出 CSM 的静态 caster bounds，继续作为 `lighting-shadows` 与性能验证中的后续风险，不包装成独立算法。

## 移除记录：向量与坐标变换

用户审核结论：删除独立页面和交互模式。核心内容与 `rendering-math/transforms/` 的坐标变换和法线矩阵推导重复；二维反射示例不足以形成独立算法页，继续维护只会增加学习路径负担。植被页面改为直接引用 `transforms`。

## 移除记录：随机事件状态机

用户审核结论：删除独立算法页及交互模式。该内容未形成超过基础有限状态机的独立学习边界；随机触发、seed 和 rise/hold/fall 包络只有在具体事件语义中才有价值。SpeedTree Gust 的触发参数、时间参数和状态布局继续在 `rendering/speedtree-final-quality` 专题中说明。

## 移除记录：参数平滑

用户审核结论：删除独立算法页及交互模式。一阶指数响应是常用实现细节，没有形成独立的工程决策或验证链路；目标值、响应时间和方向插值的具体约定应在风响应、镜头跟随或材质参数等领域页面中就地说明。

## 移除记录：通用运行时专题

用户审核结论：删除 LOD 阈值与迟滞、Cross-fade / Dither、可见性剔除、当前/上一帧快照、数据作用域设计、GPU/CPU 数据布局、不变量验证、分阶段性能测量的独立页及运行时实验台。它们在本知识库中更适合作为植被或 SpeedTree 页面中的具体工程约束，单独维护反而让学习路径脱离实际对象和渲染问题。


## 移除记录：Billboard / Impostor 独立专题

用户审核结论：删除独立算法页及交互模式。Oak 的 `block 3: 78 vertices / 156 indices / stride 16` 与 16-byte 顶点解码仍属于 SpeedTree 资产语义和 LOD/时间域专题的事实，不作为独立通用植被算法维护；atlas、frame、renderer 与 LOD 链路属于未来还原状态，不在知识库中包装成已实现算法。

## 移除记录：非还原范围的植被专题

用户审核结论：从本次 SpeedTree 还原清单和知识库页面中移除实例相位打散、风与 LOD 联动、Leaf Card 朝向、Billboard 帧混合、Billboard 法线重建、Branch-aware LOD、双面薄层光照，以及十项程序化建树与几何生成算法。Alpha Coverage Mip 作为叶片覆盖率还原质量项明确保留。
