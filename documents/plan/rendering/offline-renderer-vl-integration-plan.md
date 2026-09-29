# 离线渲染器集成 VL 开发计划

日期：2026-09-28

状态：P0-P4 首期范围已实现，独立 CLI 已退役，VL 是唯一出图入口；跨 GPU/驱动、超大纹理解码延迟与开发者 UI 故障注入仍是后续验证项。历史阶段记录按发生时状态保留，以末尾收口记录为准。

用途：交给新线程直接开展分阶段开发；每阶段完成后更新本文进度和证据。

> 2026-09-29 更新：本计划的 P0-P4 阶段和下文保留的 CLI/双入口要求
> 是实施时的历史决策，不再是当前合同。独立论文成图程序已退役；
> 当前唯一出图入口是 VL，纯 CPU 核心迁入 `source/render/offline/core/`，
> 模块测试归根 GoogleTest。现行边界见
> `documents/architecture/vl-offline-rendering.md`。

## 1. 目标与范围

### 1.1 已对齐的产品方向

VL 作为完整论文模型与实时近似模型的研究平台：

- 离线端逐步覆盖大量 Shading Model，保留完整模型的求值与路径积分能力。
- 实时端用于观察公式简化后的外观差距，未来用于评估实际游戏应用的成本。
- 本期只落实离线渲染集成、共享资源与编辑、模式切换和结果导出。
- 性能基准、性能预算、平台移植评估、完整画质分析工具均待定，不在本期建设。
- 不要求先实现完整模型的 GPU 版本；实时近似可以独立开发。
- GBuffer 表达不了的实时实现允许走 Forward；本期不改 GBuffer、不实现新的实时模型。

核心原则：

> 一个 VL 应用、一套作者资产和编辑状态、两条画面生成路径；离线核心与 CLI 仍能独立运行。

“共享”指资产身份、作者参数、导入/解析语义和有效编辑状态，不指 CPU/GPU 必须使用同一内存布局。
“独立”指算法核心和研究验证入口不依赖 VL 窗口或 Vulkan，不指重新维护一套场景和材质。

### 1.2 本期最终体验

- 加载一份 VL 场景，视口选择实时或离线模式。
- 共用相机、对象变换、材质编辑入口及资产引用。
- 离线渐进出图；调参后新任务替换旧任务，UI 不阻塞。
- 提供采样目标、反弹次数、随机种子、暂停/继续/停止，以及 Combined/Direct/Indirect 查看。
- 使用已成功应用但尚未保存的材质草稿；不要求先写 MI 文件。
- 模式切换不重新读取另一套场景、不重置相机、不丢失草稿。
- 导出线性结果和可追溯的参数/资源清单。
- 不支持的场景特性在启动渲染前报告，禁止静默跳过或替换为 DefaultLit。

### 1.3 首批支持范围

| 内容 | 本期边界 |
| --- | --- |
| 几何 | 静态三角网格、多实例、section 到材质槽映射；保留 CLI 原解析球/圆柱案例 |
| 材质 | 已有 Cloth A/B，加上布料案例所需的明确 Lambert 地面实现 |
| 相机 | VL 当前相机投影；CLI 固定正交相机保持原行为 |
| 光源 | 方向光；已有离线单面正方形面光源需要接入统一 VL 场景表达 |
| 纹理 | 首批明确支持的材质输入和 groom 方向；格式、采样及 UV 语义逐项验收 |
| 动画 | 静态快照；不承诺顶点动画、SpeedTree 风或连续动画路径追踪 |
| 环境 | 暂不实现环境照明；存在有效环境照明时必须报告不支持 |
| 其它 | 点光、聚光、地形、透明、体积、多层闭包、任意程序化 GLSL 材质暂不支持 |

不能把 Lambert 地面称为已支持整个 DefaultLit，也不能把两种 Cloth 当成彼此的降级版本。
首个集成测试场景应只包含支持的特性；不能为了让任意现有场景启动而偷偷关掉环境或删对象。
如果当前 World 默认强制带环境，应补一个明确的无环境表达，并记录资产迁移，而不是在适配器忽略它。

## 2. 新线程阅读顺序与现状

先读仓库 `AGENTS.md` 与 `README.md`，再按下表定位。路径相对仓库根目录。

| 主题 | 必读文件 |
| --- | --- |
| 当前离线算法与证据 | `documents/architecture/offline-paper-renderer.md`、`tool/offline-renderer/README.md`、`main.cpp`、四个 `*_test.cpp`、`verify_outputs.py` |
| 引擎与线程 | `documents/architecture/vulkanlearn-architecture.html`、`source/main.cpp`、`source/engine/engineLoop.*` |
| World 加载事务 | `source/world/loading/worldLoader.*`、`worldBuilder.*`、`worldGraphTransactionCoordinator.*` |
| 快照与渲染 | `source/world/worldSnapshot*`、`source/render/frontend/renderScene.*`、`source/renderSystem.*`、`source/renderGraph.*` |
| 资源 | `source/mesh/loader/common/meshAssetLoader.*`、`source/render/resource/rendererMeshLoader.*`、`rendererResourceCache.*`、`source/renderableObject.*` |
| 纹理 | `documents/rendering/texture-asset-json-v1.md`、`source/textureAssetLoader.*`、`source/resource/image/textureIO.*`、`hostImage.*` |
| 材质与编辑 | `documents/rendering/material-param-authoring-and-reflection.md`、`source/material/loader/materialInstanceResolver.*`、`source/editor/preview/*`、`source/editor/runtime/materialInstanceEditorRuntime.*` |
| 显示与 UI | `documents/architecture/game-ui-stack.md`、`source/ui/uiOverlayRendererVulkan.*`、`config/renderGraphConfig.json` |
| 测试规则 | `documents/architecture/coding-guidelines.md#test-governance`、根 `CMakeLists.txt` |

2026-09-28 开工前的代码观察（历史基线，不可误读为当前实现）：

1. 离线 `main.cpp` 集中了预设、相交、求值、积分、CLI 与整帧文件输出；当前不是可取消的渐进 session。
2. `WorldSnapshot` 是不可变的逻辑快照，但只带资源 handle，不带 CPU 网格、像素或冻结材质值。
3. `MeshAssetLoader` 已输出 CPU `ModelResource`。`RenderableObject` 仍私有持有顶点/索引，
   不要因没有 getter 就认定 CPU 数据已丢失，或直接增加重复的大型缓存。
4. `Texture` 主要暴露 GPU 资源；`TextureIO`/`HostImage` 是 CPU 解码基础，但不等于完整 CPU 材质采样器。
5. `RendererMeshLoader` 当前创建 renderable 并加载 GPU 材质。World 准备与 Vulkan 资源仍有耦合。
6. MI 编辑已有带 document revision 的草稿和 renderer-owned preview session；
   `Apply` 成功、文件保存成功、离线支持、实时支持是不同状态。
7. `RenderSystem::RecordAndSubmitCurrentRenderScene()` 当前执行环境更新、图形 Pass 与 UI，
   不能仅叠一张离线图片却继续默默执行整套实时场景渲染。
8. 当前使用 SDL3 及已有 UI 栈，不新增 SDL2 窗口或另一个 ImGui 应用。
9. 通用实时 Cloth dispatch 尚未恢复。集成离线不能伪装成完成实时 Cloth。

## 3. 架构边界

### 3.1 依赖方向

```text
VL 作者资产 / 编辑文档 / World
                |
     解析、验证、准备、提交有效状态
                |
   Frozen Scene + CPU Asset References
          /                      \
实时资源与渲染路径         OfflineSceneAdapter
                                  |
                           OfflineRenderSession
                                  |
                          OfflineRendererCore
                                  |
                    Immutable Linear HDR Result
                                  |
                Vulkan 上传 / 显示映射 / 现有 UI

独立 CLI -> 固定案例构造器 -> OfflineRendererCore -> 文件输出
```

只做两个具体渲染路径的调度，不趁机恢复跨图形 API RHI 抽象。
CPU 离线积分器不是 `RendererBackendVulkan` 的替代实现；离线模式仍使用 Vulkan 显示。
不强迫离线通过 GBuffer，也不从 GBuffer 回读构建离线场景。

### 3.2 推荐模块分工

目录是建议落点，P0 核对构建后可小幅调整；职责与依赖方向不能改变。

| 模块 | 建议落点 | 责任 / 禁止事项 |
| --- | --- | --- |
| 离线算法核心 | `tool/offline-renderer/core/`，独立 CMake library target | 数学、几何/BVH、采样、散射、积分、线性 film；不 include `source/`、Vulkan、UI、活动 World |
| CLI 与固定案例 | `tool/offline-renderer/` | 保持命令、固定输入与历史回归；不得复制一份核心算法 |
| CPU 资产数据 | 优先扩展现有 mesh/image/material 数据模块 | 冻结导入结果和有效参数，维护资源身份；不持有 UI 或 Vulkan 生命周期 |
| VL 离线适配 | `source/render/offline/` | 把 VL 冻结场景转为核心输入，检查能力与坐标/单位；不重写模型公式 |
| 离线会话协调 | `source/render/offline/` | 任务版本、取消、暂停、失效、结果投递；不让 EngineLoop 堆积算法分支 |
| 离线显示 | 现有 render/UI 边界内的独立组件 | staging 上传、HDR 显示、GPU epoch 退休；不计算路径积分 |
| 编辑入口 | 现有 editor command/document/preview 体系 | 参数修改与状态反馈；不让 UI 直接访问 render worker |

Core 使用自己的小型纯 CPU 输入类型；VL 适配器只转换必要数据。
不得把带 Vulkan include 的 `Vertex` 或 `MaterialInstance` 传播进独立 core。
重复定义少量边界类型可以接受，复制材质默认值、模型公式或资产加载规则不可以。

## 4. 共享资产与 World 加载

### 4.1 CPU 数据所有权

优先将既有 CPU 导入结果提升为不可变共享数据，或提供一次性冻结导出，
而不是新增一套重新读取源模型、忽略 VL 导入选项的离线 loader。

- 同一个模型的几何按资产身份与版本复用；不同对象保留独立实例变换。
- section、索引顺序、UV、法线、切线与 handedness 来自同一导入语义。
- 正常矩阵、负缩放、几何法线与 shading normal、背面处理必须有转换测试。
- OBJ 的局部旋转、只选 `Plane.001` 等旧 CLI 规则只留在固定案例构造器；
  不得隐式应用到 VL 所有模型。
- CPU 纹理数据按资产描述和版本复用；按需保留，明确缓存释放责任。
- CPU 数据无消费者且不被 session 持有时可以释放；GPU 资源由 GPU owner 独立退休。
- 解码数据可以共享，采样状态不得混用：同图像不同 sampler 仍是不同采样语义。

共享快照必须真正不可变。`shared_ptr<const Package>` 若内部指向可变 MI 并不等于冻结；
需复制有效参数值，或引用不可变的版本化材质对象。

### 4.2 Offline-only 模型是前置架构问题

目前 World 准备会加载 GPU 材质。如果 CPU 模型必须先有可编译 GPU shader 才能进 World，
“未来支持大量离线模型”的目标就没有实现。

P0/P2 必须确定并实现最小边界：

1. 资产解析、参数校验和逻辑对象/材质槽建立不依赖实时 pipeline 成功。
2. 实时资源准备按显式 capability 执行；没有实时实现是可报告的能力状态，不是伪造 shader。
3. 当前选择实时模式且场景不可实时渲染时，给出明确诊断；不能落入 DefaultLit。
4. 选择离线模式可以加载 CPU 支持的场景；无需先创建虚假的实时材质。
5. 共用 World 生命周期和既有事务，禁止维护平行的 OfflineWorld 或绕过事务修改活动资源缓存。

这不要求本期完成全引擎资产系统重构。只抽出静态网格/材质所需的数据准备边界，
保持已有实时 World 加载的成功/失败行为不变。实现前写清所修改的事务准备项。

### 4.3 纹理与相机合同

- 明确支持的输入格式、通道 swizzle、SRGB/Linear、FlipY、wrap、filter 和 mip 策略。
- 未支持的压缩格式或程序化贴图在入口拒绝，不能用白贴图替代。
- groom PFM 的 signed RG、repeat、双线性向量过滤和反向抵消规则保持现有合同。
  PFM 若未被 `TextureIO` 支持，应增加窄数据格式适配，不伪装成普通 SRGB 颜色图。
- VL camera ray 必须匹配当前投影、宽高比、坐标约定；不得沿用 CLI 固定正交射线。
- VL 透视相机的近远裁剪按相机前向深度约束主射线命中；裁剪面变化属于
  输入变化，必须重置累积并记入导出清单，不影响二次光路和 CLI 固定案例。
- CLI 原正交相机不受集成改动影响。VL 若需新增正交 authoring，另列增量，不为兼容 CLI 强加迁移。
- 对未实现的纹理 LOD 使用明确受限策略并在结果清单标注，不能宣称与实时采样完全一致。

## 5. 材质与模型扩展

### 5.1 一份作者数据，多个实现

- 参数默认值仍只有一个来源；沿用 M_ defaults + MI overrides 的优先级。
- 完整曲线等结构化数据若现有 M_ schema 无法表达，应设计共享的类型化模型 profile 资产引用。
  不得在两个 renderer 各复制一套表，也不得硬把任意数组塞进现有 scalar/vector UBO schema。
- profile 格式、引用位置、校验和编辑能力在 P0 定案；未实施前不得将草案字段当成现有格式。
- CPU/GPU evaluator 可以独立，参数语义、单位、模型身份和实现版本必须明确。
- 模型身份不能依赖当前 GBuffer 的 4-bit 编码；离线-only 模型不占用新的 GBuffer 槽位。
- 不支持任意 GLSL Surface Function 自动转 CPU。使用显式的 material-definition 到 CPU evaluator 映射，
  验证所有会影响外观的宏/输入；未知组合在 preflight 拒绝。
- 只注册模型名不足以表示支持，必须检查实际材质配置、贴图和表面计算能力。

### 5.2 可扩展但不过度抽象的核心

按模型组织参数和代码，例如 `models/cloth_sheen.*`、`models/cloth_fiber.*`。
使用显式注册表或有限分发连接 evaluator；不建设动态插件系统或新的通用材质语言。
不能让每次命中携带含所有模型字段的巨大结构，也不为每个像素做不必要的堆分配。

首期以现有两类模型验证散射接口：

- A 臂 BRDF 的积分权重是 `f * absCos / pdf`，按现有同侧约定求值。
- B 臂使用已包含其方向测度因子的 radiance kernel `K / pdf`，不能再乘宏观 `NoL`。
- 接口必须明确 Eval 返回的量、solid-angle PDF、方向支持域、delta 事件语义和几何侧。
- 推荐在模型适配层归一成“相对于立体角的方向贡献核”，同时保留原始论文函数供独立验证；
  该选择应经 A/B 数值和采样测试确认，不能仅凭命名统一。
- Eval/Sample/Pdf 使用同一冻结参数和离散化设置；不得只修改 curve sample 数的一侧。
- B 面光源 MIS 的样本数权重、续射命中 emitter 不重复加光规则保持不变。
- 不用离线核心自己证明自己的正确性；独立 scalar/reference 验证继续存在。

增加一个模型的正常修改面应是模型模块、参数适配、能力登记和对应测试，
而不是同时修改 World、UI、积分器和所有已有模型。
体积、分层、微观纤维多重散射等以后可能需要扩展接口，本期不虚构已支持。

### 5.3 草稿预览的提交语义

文件保存、document working draft、已应用预览状态分开管理。
沿用现有 command/revision 协议，在 owner 稳定边界准备有效参数，不让 worker 读取 editor document。

建议模式：

1. 一次 Apply 解析并校验草稿，冻结出候选参数和依赖版本。
2. 准备当前活动渲染路径需要的资源；成功后才发布新的有效预览版本。
3. 不支持的非活动路径记录能力状态，不阻止离线-only 参数应用。
4. 活动路径准备失败时保留旧有效版本和旧图；working draft 保留供修正，但不能报告 Apply 成功。
5. 切换路径前按当前有效版本准备目标资源，失败时保留当前模式并提示原因。
6. Restore baseline 走相同流程；World 换代使旧 session 失效。

不要直接向现有 GPU `Apply` 后追加一个可能失败的 CPU 调用，造成“一半成功”。
如果初期仅实现活动路径原子提交，非活动路径必须标记待同步，不能展示不同版本图像却称为同一状态。
既有编辑器状态字段的必要扩展在 P0 记录，不另建一套离线材质面板。

## 6. 任务、线程与版本

### 6.1 调度边界

- GT/现有 owner 稳定边界捕获 World 和已应用材质版本，准备候选。
- CPU workers 只读冻结 scene/asset package，不访问 Vulkan、活动 World、编辑文档或 GPU cache。
- RT 接收不可变结果，管理 staging、image、descriptor、上传和 GPU 退休。
- 昂贵 BVH/解码准备放入可取消的 CPU prepare 工作；不能在 UI slider 回调里同步构建。
- 任务队列采取 single-latest/coalescing，禁止调参越快积压越多过期任务。
- 线程创建、错误汇报与 join 有明确 owner；不为每次参数变化创建无人管理的线程池。

推荐状态：Idle、Preparing、Rendering、Paused、Completed、Canceled、Failed。
暂停保留 scene、RNG/sample 索引和 accumulation；停止取消任务并保留最后一张图。
模式切回实时默认取消离线计算、保留带版本标签的最后结果；再次进入离线重新启动。
持久 checkpoint/跨进程恢复不在本期。

### 6.2 失效规则

| 变化 | 必要动作 |
| --- | --- |
| 相机、分辨率 | 重建射线设置和 film，重置累积，复用几何 |
| 材质、groom、灯光 | 更新相应冻结数据，重置累积，复用未变几何 |
| 实例变换 | 更新实例数据和必要加速结构，重置累积 |
| 模型或拓扑 | 重建受影响几何/BVH，重置累积 |
| World 提交成功 | 使旧任务/结果失效，准备新 World |
| World 候选失败 | 当前任务与有效数据不变 |
| AOV/显示曝光 | 只改显示，不重置累积 |
| 提高目标 spp | 在输入版本不变时继续累积 |
| seed、反弹次数、积分器设置 | 重置累积 |

当前离线视口对相机运动采用交互调度：首次观测到相机变化就将本轮目标
降为半宽、半高（奇数向上取整）的视口预览；准备超时或预览完成后实测
超过 30 ms 时按每轴 1/2、1/4、1/8、1/16 逐级降档，连续三张低档预览
低于 5 ms 才升一级，给上一级约四倍像素量预留余量。已进入积分的预览
不在 30 ms 边界取消，等待已领取 tile 完成并发布有效像素。
只对新接受的预览计时一次，输入换代不沿用旧计时。上一张预览仍准备或渲染时，后续纯相机变化只
保留最新姿态，不逐帧取消任务。上一张完成后再提交最新相机；最后一次
相机变化后连续 200 ms 无变化，恢复原分辨率和用户目标 spp。全尺寸
film 与半分辨率预览是不同输入，必须重算，不复用预览累积；同一尺寸、
相机和输入版本下提高目标 spp 才能继续累积。World、资产或积分参数
变化不等待纯相机请求的合并。显示端只从同代有效像素填补部分预览并放大，
不改变正式 film 或导出清单。截止时间是协作式软上限，单条光路及准备
可能超时；新结果未发布前显示保留的旧图。

不得以每帧递增的 `frameIndex` 作为重置依据。
需要明确 World generation、资源内容版本、材质 revision、相机/灯光内容、渲染设置和 task generation。
内存 task counter 与持久内容身份是不同概念；持久摘要沿用稳定 BLAKE3-256，
不使用 `std::hash`、时间戳或裸指针作为持久身份。

取消只停止计算不够：每份结果必须带 task generation，发布端和显示端拒绝旧代结果。
窗口 resize、scene reload、模式切换与退出都需要覆盖这个规则。

### 6.3 渐进计算与安全发布

- 按可取消的 tile/sample batch 工作，不能等完整高 spp 帧结束才响应取消。
- film 每像素保留 sum、sample count 及所需 AOV；展示平均值，不能把平均值再次累加。
- 未完整处理的 tile 使用自己的 count 或只发布完整阶段，避免未采样区域被错误归一。
- sample seed 继续由像素、样本索引和 seed 决定；保持每像素累加次序。
- UI 读取的是完整不可变发布结果或受 ownership 保护的交换缓冲，不读取 worker 正在写的数组。
- RT 不等待完整离线路径追踪，只消费已经就绪的最新结果；没有新结果时继续显示旧图。
- 数据竞争测试与分批/单次结果一致性优先于增加花哨调度。

## 7. 显示、光源与模式切换

### 7.1 显示路径

在现有 BeginFrame/Present/UI 生命周期内提供离线显示分支：

```text
实时：现有 scene passes -> 最终显示 -> UI
离线：上传线性 film -> 显示映射 -> UI
```

不要把 CPU 计算变成阻塞式 RenderGraph pass。
可将显示记录做成专用 pass 或渲染组件，选取与现有 graph/framebuffer 生命周期最一致的方案，
但不要为每批结果重建 RenderGraph，也不要让离线模式继续执行 geometry/shadow/IBL 的整套工作。
现有资源热重载、World 事务的安全边界仍需维护，不能靠停止显示使它们失效。

线性 HDR 显示只做一次 tone mapping / SRGB 编码。首期不默认加 bloom 或其它改变参考外观的效果。
旧 CLI indirect PPM 的 4x exposure 仅是显示约定，不能写入线性输出或 Combined。
GPU 上传使用现有 frame fence/epoch 与退休机制；禁止每次上传 `device.waitIdle()`。
首个新任务结果到达前可显示带旧版本状态的旧图；不同任务不能混合积累。

### 7.2 单面方形面光源

这不是离线面板的私有灯。需一起补齐场景 asset 类型/校验、World 表达、snapshot 和适配。
资产中的方向、尺寸、颜色/强度单位、单面发光以及 camera-visible 行为要明确。
不要把 emitted radiance 与现有 directional irradiance 直接复用同一无语义字段。

实时暂不支持该光源时必须报告能力缺口，不自动转换成点光。
P0 明确类型命名及 authoring 字段后再实施，不在本文臆定已存在的 JSON 字段。
需要无面光源的共同场景来验收来回切换；面光源场景用于离线能力验收。

### 7.3 模型路由规则的范围

长期目标：有 GPU 实现但 GBuffer 不支持的配置可走 Forward；
仅有 CPU evaluator 的模型不能因为选择 Forward 就被视为实时可用。
本期只增加必要能力诊断，不建设完整实时近似管理、Forward/Deferred 重路由或 GBuffer v2。

仓库当前“模型同时补两条 dispatch”的规则与长期 Forward-only 方向存在差异。
等实施实时路由时再同步修改相应合同/AGENTS；本期不能用该差异为理由绕过现有实时校验。
离线-only 支持由独立的明确 capability 表达，不把未实现模型偷偷送进当前 shader dispatch。

## 8. 输出、复现与旧案例

- 集成模式输出在 `resourcePath/Generated/` 下的明确子目录；固定 CLI 研究输出继续使用
  `artifacts/offline-renderer/`。不把结果写进仓库 `resources/`。
- 线性 Combined/Direct/Indirect PFM 保留；展示图与线性数据分开，EXR 可作为后续增量。
- 每次导出固定一次结果快照，图像与 manifest 必须对应同一 generation/sample count。
- 清单记录场景身份、相机、有效参数（含未保存覆写）、profile/texture/mesh 内容身份、
  模型实现版本、积分设置、seed、样本数、AOV、显示设置与结果是否完整。
- 正式结果的 `inputDigest` 在纳入网格内容摘要后计算；任务 generation、
  已完成样本数和展示设置不属于这个冻结输入身份。
- 默认值发生变化后仍需能还原当次有效输入；不能只保存 MI 路径或依赖当前磁盘草稿。
- 使用结构化 JSON 清单；旧 CLI 文本清单可保留兼容。导出临时文件完成后再发布清单，
  避免把半写结果报告为完整导出。
- asset 只记录引用和 hash 时，应注明重放仍需原版本资产；不承诺清单本身就是自包含场景包。

旧 CLI 模型导入、浮点精度、相机和 sample 顺序先保持不变，以保护固定 hash。
VL 适配器可能使用不同三角化/切线生成，不能要求它与旧 OBJ loader 自然逐位一致；
应单独验证坐标/材质语义和受控简单场景，不能重写历史 hash 掩盖差异。

## 9. 分阶段开发与验收

所有任务初始未完成。不要按写完代码就勾选，通过对应验收后再更新状态。

### P0：边界定案与基线

- [x] 检查当前工作树，保护已有未提交代码与离线工具；记录基线 commit 和相关本地变更。
- [x] 运行现有构建/离线回归，记录已有失败，确认输入资产与输出位置。
- [x] 明确 CMake core target 位置、root/standalone 双入口以及无 Vulkan 依赖检查。
- [x] 记录 CPU 几何/纹理 ownership，避免复制 `RenderableObject` 已有数据。
- [x] 定案 offline-only World 加载边界、草稿提交与模式切换失败语义。
- [x] 定案模型 profile 最小格式、灯光/无环境表达、首批纹理与相机支持范围。

验收：在本文追加决策记录和文件修改面；没有“CPU 要求 GPU 假材质”或
“worker 读取 live MI”等未解决的架构前提。格式如需用户选择，先询问再实施迁移。

### P1：提取独立核心

- [x] 从 `main.cpp` 拆出 core、固定案例构造与 CLI，保持行为与命令兼容。
- [x] 分离 Cloth A/B 模块，明确 Eval/Sample/Pdf 测度。
- [x] 提供独立 CMake library，VL 与 CLI 链接同一目标，无算法复制。
- [x] 聚焦迁移旧模块测试到 GoogleTest，保留原来的独立 reference 语义。

验收：独立 CLI 可构建运行，A-arm 两个固定 hash 与 B directional Velvet hash 保持；
数值、PDF、灯光/MIS、groom、AOV 和线程确定性回归通过。此阶段不顺手改公式。

### P2：共享场景与资产准备

- [x] 冻结 CPU 资产包、材质有效值与 VL 场景适配器。
- [x] 打通 offline-only 的逻辑资产与 World 准备，保持事务提交隔离。
- [x] 支持静态实例、材质槽、VL 相机、方向光及明确无环境场景。
- [x] 补单面方形面光源的统一 scene/World/snapshot 表达。
- [x] 接 Cloth A/B 参数与 profile/groom，输出结构化 preflight 诊断。

验收：同一 VL 输入可由 core 输出线性图；不需第二份离线场景或参数配置。
未知材质/光源/动画/贴图拒绝有具体对象和资产路径；候选失败不改变 live World。

### P3：渐进会话与交互

- [x] 版本化任务、single-latest 调度、取消、暂停/继续/停止与错误通道。
- [x] 稳定 seed/sample 序列，逐像素或完整批次正确归一，安全结果发布。
- [x] 接已应用草稿、Restore baseline、相机/灯光/资源变化失效。
- [x] World 更换、resize、模式切换和退出的取消/join 生命周期。

验收：分批与一次性相同输入结果一致；不同 worker 数结果一致；
过期任务不覆盖新图；失败 Apply 不改变有效输入；暂停继续无重复样本或丢失样本。

### P4：VL 视口与导出闭环

- [x] 离线显示分支、上传资源与 GPU epoch 退休、现有 UI 叠加。
- [x] 模式切换保持 World/相机/草稿；目标路径不支持时不破坏当前模式。
- [x] Combined/Direct/Indirect、显示曝光及会话控制。
- [x] 线性输出、展示图和同版本结构化 manifest 导出。
- [x] 更新已落地架构合同、用户入口说明和本文阶段证据。

验收：支持的共同场景来回切换不重载资产、不丢编辑状态；离线-only 场景可正常加载渲染；
UI 持续响应，图像无重复 tone mapping，退出无悬挂线程和 GPU use-after-free。

## 10. 测试与验证纪律

### 10.1 模块测试

新增测试使用 GoogleTest，通过根 `vulkanlearn_add_gtest(...)` 注册，按模块集中：

- 几何/相机适配：section、多实例、负缩放、法线与切线方向、投影 ray。
- 材质解析：默认值/覆写/profile、CPU capability、未知 Surface Function 拒绝。
- 冻结与事务：版本捕获、Prepare 失败、World 更换、草稿 Apply/Restore 失败保留。
- 会话：取消、暂停/恢复、旧结果拒绝、batch/sample 确定性、AOV 求和。
- 核心：A/B 独立 reference、采样 PDF、面积灯/MIS、groom 和几何侧。
- 显示数据：归一化、AOV 选择、曝光不修改线性 film、导出版本一致性。

旧离线 CMake 使用手写 test `main()` 与 `add_test()`，与现行治理有差异。
不得复制这一模式扩展新测试；P1 聚焦迁移相关数值/解析检查到根 GTest。
standalone 保留生产 CLI 构建，模块测试由根构建运行，避免复制一套测试注册 helper。
纯命令输入验证可提取 parser 做 GTest；Python 图像/能量验证仍独立运行，不塞进 ctest。

### 10.2 图像与 Vulkan 验证

- 旧 `verify_outputs.py` 和独立 `tool/validation/verify_cloth_sheen_albedo.py` 继续保留。
- 集成后另做受控场景的线性/AOV、坐标与材质适配验证，不能只凭截图“看起来差不多”。
- 人工/外部操作覆盖连续调参、模式切换、World 更换、窗口 resize、渲染中退出与资产失败。
- 实时回归至少构建通过，并在原场景确认原路径未被离线分支改变。
- 不新增 runtime test flag；现有三类 runtime tests 若因事务或 GPU 生命周期改动需要运行，
  必须串行。需要新增/扩展 runtime test 先获得用户明确同意。
- 对不能自动验证的 GPU/交互项目如实记录，不把模块测试通过写成完整端到端验收。

## 11. 不做的事与防止范围扩散

- 不做性能测量平台、硬件基准、FPS 目标或游戏引擎适配。
- 不做 GBuffer 扩容、材质表大迁移、实时近似版本管理和模型 GPU 重建。
- 不做另一个场景编辑器、离线专属资源目录或重复材质 UI。
- 不做全量 GLSL 到 CPU 翻译、材质 DSL、动态模型插件或跨 API backend。
- 不引入去噪、radiance clamp、隐式环境补光来掩盖噪声或公式问题。
- 不删除旧固定案例、独立验证脚本或重置 hash 来让迁移“通过”。
- 不顺手重构所有资源加载器、清理全部历史文档或修改无关 shader。

## 12. 文档迁移与交付

当前 `architecture/offline-paper-renderer.md` 描述的是独立工具事实。
本计划实施前不要将其改成已集成；P4 将落地合同迁入正式 architecture/rendering 文档，
保留论文案例和证据归属，说明“核心独立、VL 共享输入、CLI 保留”的新边界。

每阶段交付记录：

| 阶段 | 状态 | 修改摘要 | 验证命令/结果 | 尚存问题 |
| --- | --- | --- | --- | --- |
| P0 | 完成（边界决策） | 基线、候选事务修改面与格式/失败语义已定案 | 独立构建、27/27 CTest、19/19 黑盒 | 决策尚须由 P2-P4 实现和端到端验收 |
| P1 | 完成 | 纯 CPU core、A/B 模型、固定案例/CLI 两目标、根 GoogleTest | 独立 27/27、黑盒 19/19、根 main/CLI 构建及 GTest 5/5 | 独立构建保留历史 CTest main；后续迁移不得复制 |
| P2 | 首期完成 | World 事务冻结 CPU mesh、有效 MI/profile、颜色与 groom 原始字节；offline-only 逻辑对象不建假 GPU 材质；支持静态实例、相机、方向/方形面光、显式无环境、Lambert/Cloth A/B 的显式能力预检 | 根 Adapter GTest；共享 Lambert 双路径、Cloth A/B 方向/面光、sRGB checker 与 signed-RG groom 场景的 1 spp 成图/清单 | 论文外观定量对照不属于本期集成验收 |
| P3 | 首期完成 | 单最新 prepare/积分 worker、代数取消和发布、暂停/停止/续积；GT 快照和草稿有效包变更自动重启，World/resize/退出管理 worker 生命周期 | 会话/协调器 GTest；单/双线程标准压力与 1000000 spp 中途退出均通过；失败 Apply/Restore 最近邻测试 | 超大贴图预检时延和长期压力统计未测 |
| P4 | 首期完成 | Vulkan 线性 film 显示+UI 分支，AOV/曝光/设置、线性 PFM/展示图/JSON 同版本导出；实时/离线用同一 World 资产 | 同进程双向切换、1 spp 导出与视口截图；开发者 UI 模式拒绝/成功草稿与 Restore 人工验证；根 CTest 100/100 | 未做不同 GPU/驱动矩阵和 UI 故障注入 Apply |

### 2026-09-28 实施记录

- 基线 commit：`90a83c3898691742e009f0c0670b191193177da2`。开始时 `AGENTS.md`、`README.md`、`documents/README.md`、shading-model 计划和记录、两份 GLSL 有本地修改；本计划、离线渲染器整个目录、论文架构文档等未跟踪。没有重置或清理这些内容。
- 基线：独立 `cmake -S tool/offline-renderer -B build/offline-renderer -G "MinGW Makefiles"` / `cmake --build build/offline-renderer -j 8` 成功；独立 CTest 27/27、`py -3 tool/offline-renderer/verify_outputs.py` 19/19。固定输入在 `tool/offline-renderer/assets/`，验证输出在忽略的 `artifacts/offline-renderer/verification/`。
- 构建决策：`tool/offline-renderer/core/` 持有纯 C++17 的 `vl_offline_core` 静态库；独立入口仍能构建 CLI，根构建也构建并链接同一库。头文件不依赖 `source/`、Vulkan、UI；根构建新增聚焦的 `vulkanlearn_add_gtest` 测试。现有独立 CTest 是迁移前的历史测试，不能作为新模块测试的模板。
- CPU ownership 观察：`MeshAssetLoader::ImportSections` 返回 `ModelResource`，但 `RendererMeshLoader` 随即把 section 复制进 `RenderableObject` 并加载 GPU MI；`WorldSnapshot` 只有 handle。P2 应在候选准备包中保存按资产身份共享的不可变 CPU 导入结果，不从 Vulkan buffer 回读，也不让 worker 持有 live `RenderableObject` 或 MI。CPU 解码纹理应由同一候选包按描述版本持有，sampler 单独冻结。
- P2 事务准备项（尚未实现）：`WorldLoader` 的 scene/mesh/MI 逻辑解析与 renderer resource preparation 分开；候选包一并持有 World、CPU 资产、已解析有效材质版本和能力诊断；活动路径的 GPU 或 CPU 资源准备失败均保持旧 World。`RendererResourceLoadCoordinator` 目前无条件初始化 Vulkan 全局资源、pass MI 和 mesh GPU MI，`WorldBuilder` 从这些结果生成逻辑 mesh 对象，必须先拆开该依赖，才可允许 offline-only 材质进入 World。不能给它配置假 shader。
- 草稿提交（待实现）：沿用 editor document revision；在 owner 稳定边界复制 M_ 默认值与 MI 覆写后的有效参数，活动路径准备成功再发布版本；非活动路径标记待同步。失败的 Apply/切换保留旧有效版本与当前模式，Restore 走相同提交。worker 只接不可变版本。
- 格式选择（开工时的决策）：结构化曲线采用单独版本化 `modelProfile` JSON 引用，由 M_ 声明默认引用、MI 稀疏覆写引用；不把数组塞进 UBO 参数。场景用 `environment.type = "none"` 显式无照明、`squareAreaLight` 独立对象，尺寸为世界单位边长、强度为线性 emitted radiance、朝向为法线并单面发光。字段现已进入解析/World，但无环境和面光源在实时路径明确拒绝，尚无可加载的离线模式。纹理首期拟限定非压缩 Png/Tga/Exr/Hdr 和独立 signed RG PFM groom、明确线性/sRGB、repeat/clamp 与 nearest/linear；mip/LOD 仅 level 0，未支持的配置 preflight 拒绝。VL 相机以 `CameraSnapshot` 当前投影和宽高比构建透视 ray，固定 CLI 正交行为不变。
- P1 后续：历史 OBJ 解析、groom PFM 导入、案例球阵构造和六种论文预设已移到 `cases/legacyCases.*`；公式保留于 `core/models/`，根 GoogleTest 重用独立标量参考与 groom 检查。P2-P4 的 World、会话、显示和导出验收尚未发生。根构建链接 core 只证明依赖方向可用，不代表 VL 已提供离线模式。

### P0 决策记录（尚待 P2-P4 落地）

- **CPU ownership 与事务修改面**：`MeshAssetLoader::ImportSections()` 的 `ModelResource` 在候选包中按 `modelCacheKey` 共享为 `shared_ptr<const ModelResource>`；同一候选的 Vulkan `RenderableObject` 仍独立拥有其上传源数据。CPU 包不回读 GPU，也不持有可变 `RenderableObject`。纹理解码后按描述路径及内容版本共享不可变像素，sampler 状态按材质绑定另存；World 切换后旧包由运行中的 session 持有至退出，不依赖 GPU epoch。候选失败直接销毁包，绝不修改活动缓存。
- **World 与路径能力**：`WorldLoader` 保留 JSON 验证；候选阶段先由 CPU mesh/material resolver 构建逻辑 section 与有效 MI 值，再按当前路径能力选择 GPU 准备。`WorldBuilder` 仅消费逻辑 plan。`WorldGraphTransactionCoordinator` 在所有活动路径准备成功后才发布 World/graph/CPU 包；实时不支持的材质或面光源在切换前给结构化诊断，不交给 DefaultLit 或假 shader。离线-only 候选仍走同一 World generation 与提交事务。未来实时路径保持当前失败行为。
- **草稿与模式切换**：有效状态是成功 Apply 的 document revision 与 M_ 默认值、MI 覆写合并后的冻结 JSON/类型化值，不是文件保存状态。活动路径资源成功准备后才提交；非活动路径标记待同步和能力诊断。Apply/Restore/切换准备失败保留旧有效版本、旧模式与最后结果；World 成功换代才取消旧 session。worker 不读取 document/live MI。
- **profile v1 合同**：M_ 可声明 `modelProfile` 默认资源路径，MI 可用同名顶层字符串稀疏覆写；无默认且无覆写即没有 profile。引用的 JSON 须含 `type: "offlineModelProfile"`、`version: 1`、`model`；Cloth B 的结构化值为 `eta` 与两个 `yarns`（每个有 coverage、线性 dye RGB、kd、gammaS/gammaV 角度、offsets 角度数组、lengths 数组），数组数量与 `controlPoints`/采样规则由 profile validator 保证。路径是资源身份，不是编进 UBO 的数组。A 臂标量/颜色优先现有 M_ 参数；未知模型或多余外观宏必须拒绝。字段已由 validator/resolver 接受，但 profile 仍未接入编辑 UI。
- **scene v1 增量**：`environment: { "type": "none" }` 是明确关闭环境照明；缺省仍按旧程序天空以保留实时案例。`squareAreaLight` 对象字段为 `name,type,position,normal,size,color,intensity`；`size` 是正的世界单位边长，`normal` 为非零世界空间发光面法线，`color * intensity` 为线性 emitted radiance，单面、相机可见。此灯实时无实现时诊断拒绝，不换成点光；字段已进入 SceneAssetValidator/World/snapshot/CPU adapter，但离线 World 加载尚未实现。
- **首批输入**：静态 mesh、M_ 受支持的 A/B 与明确 Lambert 地面；2D 非压缩 PNG/TGA/EXR/HDR 的显式 SRGB/Linear、通道、FlipY、repeat/clamp、nearest/linear，以及 signed RG 线性 PFM groom。只用 LOD0；压缩、隐式 LOD、程序化贴图拒绝。VL 相机是 snapshot 的水平 FOV 透视射线，右/上/前基正交且宽高比来自目标尺寸；CLI 保持正交。范围外资产由带对象/路径的 preflight 拒绝，不替换白色贴图。上述支持须在 P2 的解码/采样测试通过后才能宣布可用。

### 2026-09-28 续做记录

- `core/renderSession.*` 增加 single-latest CPU worker、generation、暂停/恢复/停止、完整样本批次的不可变平均线性 Direct/Indirect 发布；每像素 seed 与 CLI 公式一致，透视射线由水平 FOV 构建。它可消费新增的 core 实例列表，但尚没有从活动 VL World 生成该列表。根 session 测试覆盖代数/确定性/暂停与失败预检保留旧任务；独立历史 27/27、`verify_outputs.py` 19/19 均通过。未运行 Vulkan 交互验收。
- `core/renderer.*` 新增显式 `useAuthoredGeometry` 的静态多实例路径，命中材质按 section 选择 Lambert/Cloth A/B；共享 mesh 的实例矩阵允许负缩放。`source/render/offline/offlineMeshAdapter.*` 使用既有 `MeshSection` 的索引、UV 和法线构造 core BVH，不再次解析源 OBJ。平面包围盒的 slab 接触改为可命中，根 Adapter GTest 覆盖零厚度三角形、负缩放、实例共享、空场景不回退到旧球阵、Lambert 方向光及非法输入。该路径尚未连到 World 候选加载，也没有完成贴图/材质/光源能力预检。
- 后续候选 World 保留一次导入的不可变 mesh 引用和 M_/MI 合并后的有效 JSON；`modelProfile` 在候选中冻结 JSON/BLAKE3 摘要，提交前复核。材质草稿预览在 live MI 数值提交或 GPU render-state 替换前准备离线候选包，失败保留原有效版本。profile 外部文件目前仍不能直接在现有 UI 编辑。
- `squareAreaLight` scene 字段为 `name,type,position,normal,size,color,intensity`；`normal` 是发光面世界空间正面，`size` 是正方形边长，`color * intensity` 是线性 emitted radiance。World 与快照保存原语义，离线适配器映射到 core 单面 emitter；当前实时资源准备会明确拒绝此类型，尚无可加载面光源的离线模式。无方向光 WorldBuilder 不再解引用空指针；缺省环境仍是旧程序天空，只有显式 `environment.type=none` 才允许离线 preflight。
- CPU session 达到目标后保留累积与样本索引，`ExtendTargetSamples` 在同一 generation 继续；模块测试确认分段 2→5 spp 与一次性 5 spp 相同。该能力尚未接入 VL 编辑/UI。

### 2026-09-28 集成续验

- 旧记录中“尚未接入”是当时的阶段事实；当前 `source/render/offline/` 已有
  World 冻结、模型/纹理适配、CPU prepare/session owner、显示及导出。离线启动
  `--offline --initial-scene Maps/SC_offline_shared/SC_offline_shared.json`
  与同一场景的实时启动均提交了 generation=1 的 World/graph 事务；实时启动后
  `offline on` 命令成功。受控资产新增在独立资源仓库
  `D:/YYBWorkSpace/GitHub/VukanLearnResources/Maps/SC_offline_shared/`，
  不更改 `config/config.json` 的默认场景。
- 冻结颜色图与实时 `Texture` 统一执行一次 CPU 行翻转，UV 不改；两行 TGA
  模块回归确认方向。数值 Apply 提交前复核 profile 与纹理摘要；旧代三角形
  转换/BVH 在批次/递归边界取消。草稿更换后清除不再引用的冻结依赖，避免
  旧资产变动阻止新草稿提交。根 `cmake --build build -j 4`、CTest
  68/68；独立 `ctest --test-dir build/offline-renderer` 27/27，
  `verify_outputs.py` 19/19。并行重建独立 CLI 时与黑盒脚本竞争 Windows
  可执行文件锁导致链接失败，脚本完成后串行重建成功。
- 草稿解析现在直接消费活动 World 冻结的 M_ 定义，不因磁盘 M_ 在 World
  提交后变化而混入新默认值；`MaterialSchema.PreviewUsesFrozenDefinitionDefaults`
  锁定解析合同，根 CTest 更新为 69/69。
- 同进程控制台执行 `offline samples 1`、`offline export`、`offline off`、
  `offline on` 均成功，World generation 保持 1。导出清单记录
  `samples=1`、`targetSamples=1`、`complete=true`，四个图像文件均有摘要。
  1280×720 三份 PFM 的 Combined 非零通道 146912、峰值约 1.55514，
  `Combined-(Direct+Indirect)` 最大误差约 `8.94e-8`。截图
  `resourcePath/Generated/Screenshots/artifacts/offline-display-smoke.bmp`
  可见两个球体实例与 UI；窗口关闭打印 `EngineLoop teardown complete`。
  这些结果确认受控 Lambert 路径的基本 GPU 显示/导出闭环；后续条目
  分别记录 Cloth、纹理和交互边界，不把它们误归入这次 Lambert 对照。
- 资源仓库增加 `Maps/SC_offline_cloth/`，同一 UV 双片各使用 A 臂
  ClothSheen 与 B 臂 ClothFiber，Linen profile 为版本化资产；方向光与
  单面方形面光场景分别启动并提交 World。面光场景 1 spp 导出 1280×720
  有限线性图，左右半幅 RGB 能量约 520330 与 160879，截图
  `resourcePath/Generated/Screenshots/cloth-area-smoke.bmp` 可见两片材质。
  不把这些代理平片当作论文 Fig. 24 原图或定量模型校准。
- 在面光场景执行 `offline off` 得到显式
  `Realtime does not support squareAreaLight`，加载不存在的 World 失败后
  仍导出原 generation=1、1 spp 的完整结果。双线程共享 Lambert 场景
  连续改 samples/bounces/seed 并 resize；新导出为 `1008×601`、
  `seed=9`、`bounces=2`、`samples=1`，窗口关闭正常打印 teardown。
  本轮没有操作材质编辑 UI 的失败 Apply，也未进行长时间多驱动压力测试。
- `SC_offline_cloth_texture.json` 将既有 `Common/Textures/T_UV_Checker.json`
  绑定到 A 臂 baseColorMap，1 spp 导出和视口截图
  `resourcePath/Generated/Screenshots/cloth-texture-smoke.bmp` 显示 UV 字母
  与色块方向；清单含 T_ 描述与 PNG 源摘要。`SC_offline_cloth_groom.json`
  使用由 `generate_validation_groom.py` 创建并归档为场景源资产的 2×2
  signed-RG PFM，B 臂 1 spp 导出完整，Combined 1280×720 全部有限、
  非零通道 1532508，清单含 groom 描述与源摘要。首次 15 秒时未满一批，
  `offline export` 正确拒绝半成品，随后完整发布并正常退出。
- `build/bin/main.exe --world-graph-transaction-test --exit-after-tests
  --worker-thread-count 1` 串行返回 0；已有运行时矩阵确认故障注入回滚、
  graph-only 事务、minimize/resize、shader/M_ 交错与 epoch 退休排空。
  它不是新增离线 runtime flag，未声称覆盖材质编辑 UI 的人工 Apply。

### 新线程启动指令

> 请按 `documents/plan/rendering/offline-renderer-vl-integration-plan.md` 开发。
> 先读 AGENTS 和计划的现状/边界，从 P0 基线与前置决策开始，按阶段实施并记录验收。
> 保留当前工作树变更，不能重建第二套作者资产或材质 UI，不能让离线-only 模型依赖虚假 GPU shader。
> 本期不做性能平台、GBuffer 改造或新实时模型。不能省略线程/资源生命周期和失败事务验收。
> 需要用户选择或新增权限时明确提出；其余按已有代码模式推进，不把计划当作当前已实现合同。

### 2026-09-28 本轮收口与验证

- P2 预检现在会解析所引用的冻结颜色图与 signed-RG groom 字节，并在
  World/Apply 提交前以 MI 路径和对象名报告损坏数据；三角形转换与 BVH
  仍由可取消的 CPU prepare worker 负责。Cloth B profile 入口拒绝负
  dye、负曲线长度及超出 `[0,1]` 的 `kd`，不在积分器逐像素补救。
  当前预检解码发生在提交边界，超大纹理的 UI 延迟仍需专门测量。
- P3 会话修正 prepare 期间提高 spp 的目标丢失、降低 spp 后旧累积
  复用、暂停/续积状态及停止后发布；行内也检查取消。显示端按 acquired
  image 保存上传代数/样本/AOV，拒绝旧代或尺寸不符的 film，不长期钉住
  CPU 像素；曝光和目标 spp 元数据变化不重传线性图。协调器的 4 个
  直接 GTest 覆盖快速目标变更、换输入旧代拒绝、降低目标重算、停止/
  重启；快速目标用例重复 30 次通过，但不是强制卡住 prepare 的测试。
- P4 材质草稿的实时预览使用活动 World 冻结的 M_ 定义和 MI 来源，
  GPU/CPU 候选消费同一有效值；跨 World 或改指另一 M_ 的草稿在发布前
  拒绝。开发者 UI 按会话状态启停控制，提交数值前校验，模式拒绝后保留
  当前模式并提示查看 Diagnostics；具体拒绝原因尚未进入 UI 快照。
  导出清单的颜色纹理采样描述已与单次 CPU 行翻转合同一致。
- 串行根 `cmake --build build -j 4` 成功，根 CTest `86/86`；
  独立构建及 CTest `27/27`，`verify_outputs.py` 黑盒 `19/19`，
  `PYTHONIOENCODING=utf-8` 下的 `verify_cloth_sheen_albedo.py`
  `24/24`。未设置 UTF-8 时后一个脚本曾因 Windows GBK 控制台
  无法编码数学符号而中断，非数值断言失败。
- 串行 `--world-graph-transaction-test --exit-after-tests
  --worker-thread-count 1` 返回 0。`--offline --initial-scene
  Maps/SC_offline_shared/SC_offline_shared.json --no-dev-ui
  --worker-thread-count 1` 同进程执行 `offline samples 1`、
  `offline export`、`offline off`、`offline on`；导出校验为
  `1280×720`、1 spp、Combined 峰值约 `1.55514`，窗口关闭显示
  `EngineLoop teardown complete`。输出在配置资源根的
  `Generated/OfflineRenderer/SC_offline_shared-5df43fc2e60a-1-2/`。
- 本轮没有人工操作材质编辑器的失败 Apply/Restore，也没有长时间
  连续调参、多驱动 GPU/窗口状态压力验收；P2-P4 的阶段勾选仍保持
  未完成，不能将模块测试和单次受控运行等同于全部交互验收。

### 2026-09-28 Swapchain 压力回归

- 外部驱动 `tool/offline-renderer/stress_vl_integration.py` 串行注入采样目标、
  反弹次数、seed、暂停/继续/停止/重启、双向模式切换、AOV 与 resize；
  不新增引擎 runtime test flag。首次在 worker 1/2 的 resize 后分别遇到
  `vk::Queue::presentKHR: ErrorOutOfDateKHR`，两个进程均未完成最终导出。
- RHI 将 acquire/present 的 `SuboptimalKHR` 与 `ErrorOutOfDateKHR` 分类；
  未取得 image 不重置 fence/登记提交 epoch，present 已提交则保留 epoch；
  backend 向 GT 发出 swapchain 重建请求，GT 在 RT 空闲边界按 drawable
  像素尺寸重建，成功后清除请求。`SwapchainFrameResult` 四个模块测试
  覆盖成功、次优、过期和其它致命错误的分类。
- 修复后 `py -3 tool/offline-renderer/stress_vl_integration.py --workers 1 2
  --timeout 120` 串行通过：两种 worker 均 resize 到 640×360，最终清单
  均为 generation 1、seed 17、bounces 2、1 spp、task generation 13，
  输入摘要一致；`verify_vl_export.py` 均验证线性输出/AOV/清单，
  两进程均 exit 0 并记录 `Quit event received` 与
  `EngineLoop teardown complete`。日志与时间线位于忽略目录
  `artifacts/offline-renderer/vl-stress/20260928-213358-817842-33580/`。
  串行 `--world-graph-transaction-test --exit-after-tests
  --worker-thread-count 1` 也返回 0，覆盖既有 resize/退休测试。
- 失败 Apply/Restore 的代码与最近邻测试保留旧有效 revision、会话和
  图像，不在 `Failed` 显示状态下误连新预览；但此处仍不声称已经人工
  操作开发者 UI。长时间高 spp 退出、不同 GPU/驱动和更多窗口状态
  仍待独立验收。

### 2026-09-28 首期收口

- 标准压力序列与渲染中退出各在 worker 1/2 串行通过：
  `py -3 tool/offline-renderer/stress_vl_integration.py --workers 1 2
  --scenarios standard render-exit --timeout 120`。标准序列的最终
  640×360、seed 17、bounces 2、1 spp 输出均经
  `verify_vl_export.py` 校验；中途退出在 1000000 spp 目标下已有
  7 spp 的不可变部分 film，两个进程均由 WM_CLOSE 退出 0、记录
  Quit 与完整 teardown，stderr 无错误。证据位于忽略目录
  `artifacts/offline-renderer/vl-stress/20260928-214117-896826-53380/`；
  压力脚本纯逻辑测试 11/11。
- UI 选择器原先将 `squareAreaLight` 场景误判为不支持，现允许该
  对象类型，同时继续拒绝旧 `sunLight` 和未知类型。开发者面板承接
  UI 模式命令的拒绝原因；模式及结果始终以引擎有效快照为准。
  人工开启面板首次暴露了 `ImGui::InputInt` 与
  `EnterReturnsTrue` 不兼容导致的断言退出；移除非法 flags 并在
  共享 flags 处防回归。导航/离线控制上下停靠于加宽的左栏，
  检查器仍在右栏，真实 Vulkan 画面留在中央。
- `--dev-ui` 启动面光源离线场景后，经 F10/System 的应用内按钮
  打开面板，点击 Realtime 可直接看到
  `Realtime does not support squareAreaLight`，Offline 和已发布
  图像不变。选择 ClothFiber 材质槽、修改未保存参数使累积重启，
  Restore Baseline 也触发重启；窗口正常 exit 0、teardown。
  失败 Apply/Restore 的旧有效 revision 保留由编辑器最近邻
  GoogleTest 验证，尚未在 UI 人工注入对应准备失败。
- 最终 `cmake --build build -j 4` 成功；根 CTest 100/100、
  独立 CTest 27/27、`verify_outputs.py` 19/19、
  `PYTHONIOENCODING=utf-8` 下独立 Cloth A 数值检查 24/24。
  受控资源场景仍位于独立资源仓库且未保存人工草稿。
  本期阶段勾选以这些受控输入、当前 Windows/Vulkan 环境为界；
  多 GPU/驱动、长时统计和超大贴图同步预检时延不是已验证结论。

### 2026-09-28 相机运动调度补充

- `OfflineCameraMotionPolicy` 在 GT 依据相机内容、最近变化时间和会话状态
  选择 1 spp 或用户目标；第一段运动立即切到 1 spp，等待已接受的
  1 spp 完成期间合并纯相机请求，停稳 200 ms 后恢复目标。非相机
  输入变化仍及时进入协调器。协调器将完整清单、BLAKE3 持久摘要及
  mesh 摘要计算放在有效候选的 prepare worker，不再每帧做这些工作。
- 本轮构建 `vl_offline_adapter_tests`、`vl_offline_coordinator_tests`、
  `vl_offline_core_tests` 和 `main` 成功，根 CTest 109/109 通过；
  其中运动策略新增 4 项模块测试。双线程面光场景通过控制台修改相机
  位置至 `(0.75, 0, 4)` 后，实机导出清单显示新相机和 `6/32 spp`，
  确认停稳后恢复目标并继续渐进发布；进程正常退出并重新启动。
  该结果不代表实际移动时的帧率：连续鼠标移动期间的 1 spp 发布
  节奏和交互延迟仍待人工观察。

### 2026-09-28 共享场景移动预览

- `SC_offline_shared` 的双球 Lambert 场景仍有明显延迟：此前即使
  1 spp 也是单 CPU worker 在 1280×720 逐像素积分，窗口 FPS 不能表示
  新视角的 film 发布频率。现改为每轴半分辨率、1 spp 的交互预览，
  样本内最多 8 路行并行并保留逐像素随机流；停稳后重启原分辨率目标。
  固定的 Cloth 方向反照率 LUT 改为进程内一次构造、会话间只读共享，
  避免 Lambert 相机任务也反复计算约 78 万次方向积分。
- 显示端只接受全尺寸、等宽高比缩小尺寸及奇数窗口的半尺寸取整，
  放大只写当前 swapchain image 的 staging；不把低分辨率结果当成
  完整尺寸导出，也不让半成品 film 跨代发布。根 `main` 构建和
  CTest 116/116 通过；改动前的串行 worker 1/2 标准压力回归分别
  PASS，含 resize、模式切换及导出校验。LUT 复用后再次构建并跑根
  CTest；压力序列尚未重复执行。
- `SC_offline_shared` 双线程 Debug 构建实机：将目标设为 1 spp 后修改
  相机至 `(0.7, 0, 4)`，约 120 ms 后提交的导出得到该姿态的
  `640×360`、1 spp、`complete=true` 清单；停稳后导出得到同一姿态
  `1280×720`、1 spp 清单。两者摘要不同，没有把预览冒充全尺寸。
  这验证任务发布与恢复；连续鼠标移动的体感仍需在用户窗口确认，
  不由窗口 FPS 或单次命令延迟推断恒定帧率。

### 2026-09-28 自适应交互预览

- 交互目标仍为 1 spp，按 GT 单调时钟观测提交到完整样本完成的等待，
  包含 prepare 和积分。默认每轴 1/2，超过 80 ms 逐级降至每轴
  1/4、1/8、1/16；低档连续三张快于 15 ms 才升一级，
  重复读取 Completed 不重复计数。1/16 每轴只有原视口 1/256 的
  像素量（1280×720 为 80×45），以移动响应换清晰度。
  停稳 200 ms 恢复原尺寸和用户 spp；低档是独立输入，不混入全尺寸
  累积。预算只控制调度，不保证每张预览必定在 80 ms 内完成。
- 奇数窗口允许上述每轴向上取整的 film，显示端仍只放大完整
  发布的线性像素，清单/导出记录实际积分尺寸。暂停会丢弃本次计时，
  samples-only 或 resize 即使缩小后尺寸相同也立即消费新请求；
  World、冻结资产和积分参数换代都会清除旧计时。
- 双线程 Debug 构建的共享 Lambert 场景连续注入 120 个相机位置：
  导出包含 `640×360` 与 `320×180` 的完整 1 spp，停稳后导出
  `1280×720`、相机 `(0.3846, 0, 4)` 的完整 1 spp。运行中请求导出
  可能因当前代尚未完成样本被拒绝；已有图仍用于显示，不冒充新结果。
  Vulkan 运动截图确认低尺寸结果正常放大，窗口关闭记录 Quit 与
  完整 teardown，stderr 为空。日志位于忽略目录
  `artifacts/offline-renderer/adaptive-preview/split-worker-2/`；
  不从注入脚本间隔推断实际鼠标移动帧率。这是扩展到 1/8、1/16
  之前的实机记录，不能据此声称最低两档已在窗口触发。

### 2026-09-29 四档移动预览验证

- 扩展后根 `main` 与 `vl_offline_adapter_tests` 构建成功，根 CTest
  155/155；外部标准压力序列的 worker 1/2 串行通过，包含 resize、
  模式切换和最终全尺寸导出，日志在
  `artifacts/offline-renderer/vl-stress/20260929-000942-762599-59900/`。
- 双线程 Debug 的 `SC_offline_shared` 连续注入 150 个相机位置，
  间隔约 24 ms 并周期性请求导出。成功清单实际出现四档完整
  1 spp：`640×360`、`320×180`、`160×90` 和 `80×45`；
  停止注入后为 `1280×720`、`complete=true`。受控运行正常退出，
  记录 Quit/teardown，stderr 为空；日志在
  `artifacts/offline-renderer/adaptive-preview/sixteenth-worker-2/`。
  请求导出时也可能没有当前代的已发布结果，不能把导出成功率当成
  鼠标移动的帧率。显示端仍上传全尺寸 RGBA32F staging（1280×720
  为 14,745,600 字节）；降档减少 CPU 积分，不降低当前上传量，
  因此没有保证固定的 80 ms 交互时延或流畅度。

### 2026-09-29 移动预览旧视角回闪修复

- Vulkan 显示资源按 swapchain image slot 分配；先前新 film 只上传当前
  acquired slot，其他 slot 仍保存旧相机画面。协调器换代时 `Capture`
  暂时为空，全局最新 generation 又禁止旧 film 重传，轮转显示会短暂
  倒退到更旧视角，表现为移动画面突然抖回去。
- 显示端现在保留最近一次完整且有效的不可变 film；新 film 发布后，
  每个 slot 在下一次 acquire 时补传同一代。空输入、旧 generation
  或更少样本都不能覆盖已展示的更新结果；已有图继续显示直到新结果
  完整发布，swapchain 资源释放时清空保留状态。新增三个 slot 轮转
  和样本/AOV 规则模块测试，根 CTest 157/157 通过。
- 修复版的外部标准压力序列在 worker 1/2 串行通过（resize、模式切换、
  导出）；日志位于 `artifacts/offline-renderer/vl-stress/
  20260929-021906-549368-39888/`。双线程 Debug 共享场景连续注入
  90 个相机位置，导出可见 1/4、1/8、1/16 的完整 1 spp 及停稳后的
  1280×720；正常 Quit/teardown 且 stderr 为空，日志位于
  `artifacts/offline-renderer/adaptive-preview/jitter-fix-worker-2/`。
  这些自动检查不能代替用户的连续鼠标移动体感验收。

### 限时视口预览实现

- 移动相机的请求带 `interactivePreview` 标记；GT 接受请求时记录绝对
  截止时间，准备阶段已经消耗的时间不会重新计入。完整渲染仍使用原有
  `FilmSnapshot` 和逐样本发布路径，部分预览单独使用
  `PreviewSnapshot`，不会出现在正式清单、spp 累积或导出接口中。
- 预览按分散的 8×8 tile 领取任务，固定像素随机流不变。截止时间到达后
  停止领取新 tile；正在执行的 tile 完成后才发布不可变的有效像素掩码。
  发布点再次检查 generation，旧相机、取消任务和新输入不能污染当前预览。
  这是协作式软截止，不会在一条正在执行的光路中间强制终止；极慢的
  单条路径仍可能令实际完成时间超过预算。
- Vulkan 显示端只接受有效且缓冲区完整的同代 `PreviewSnapshot`，用固定
  邻域顺序的多源 BFS 对缺失像素取同一预览的最近有效像素；不把上一相机
  的 film 混入新相机。零有效预览保留上一张已显示图，各 swapchain slot
  追上同一代预览，随后完整 film 优先。
- 本轮新增限时预览核心、协调器隔离、显示填洞、交换链追帧、取消/暂停
  和相机策略测试；根 CTest **169/169** 通过。外部标准压力序列 worker
  1/2 均通过。双线程 Debug 共享场景连续注入相机位置的截图确认部分
  预览能显示，停止后导出恢复 `1280×720`、完整 `1 spp`；运行正常
  Quit/teardown，stderr 为空。实机日志位于
  `artifacts/offline-renderer/timed-preview/smoke-worker-2/`，截图位于
  资源仓库 `Generated/Screenshots/timed-preview-64.bmp` 和
  `timed-preview-100.bmp`。

### 2026-09-29 交互截止调整为 30 ms

- 应用户体验反馈，GT 接受请求至 CPU tile 停止领取的同一预算从
  80 ms 调为 30 ms；降档判断共用这个常量。连续三张快预览升档
  阈值从 15 ms 调为 5 ms，估算升一级像素工作量约四倍时仍留有余量。
  已进入积分的预览不在 30 ms 时被策略取消，而是等待已领取 tile
  发布，再按完成耗时降档；准备阶段超时仍可提前换档。
- 根 CTest 174/174 通过；双线程 Debug `SC_offline_shared` 连续注入
  100 个相机位置，运动中截图显示 30 ms 部分预览，停稳后导出
  `1280×720`、完整 1 spp，正常 Quit/teardown、stderr 为空。
  日志在 `artifacts/offline-renderer/timed-preview/thirty-ms-smoke/`，
  截图在资源仓库 `Generated/Screenshots/thirty-ms-moving.bmp`。
  外部 `stress_vl_integration.py --workers 1 2 --scenarios standard` 的
  单线程和双线程标准序列均通过，日志在
  `artifacts/offline-renderer/vl-stress/20260929-103359-370743-69816/`。
  图片清晰度较 80 ms 预览下降；脚本不能证明实际鼠标交互帧率。

### 2026-09-29 架构审查修正

- 独立审查发现离线透视相机未使用 World 快照中的近远裁剪面。
  现在适配器冻结并预检 `clipNear/clipFar`，core 只按相机前向深度
  裁剪主射线的表面和可见面光源；阴影/反弹射线及 CLI 正交案例不变。
  相机策略和协调器把裁剪面变化视为输入换代，清单记录实际裁剪范围。
- 正式 `inputDigest` 现在先纳入冻结网格的 `meshDigests`，再对完整
  输入计算 BLAKE3-256；样本数、任务代数与显示设置不参与。
  同一 World generation 换入另一网格导入版本时也不会复用旧 BVH。
- 根构建与 CTest 188/188、独立构建与 CTest 27/27、CLI 图像/哈希
  黑盒 19/19 均通过。外部标准和渲染中退出压力序列在 worker 1/2
  均通过，证据在
  `artifacts/offline-renderer/vl-stress/20260929-110904-420173-58400/`。
  透视主射线额外进行命中检查；30 ms 移动预览的性能影响尚未做
  相同输入下的对照测量，不能从脚本总耗时推断鼠标交互帧率。

### 2026-09-29 独立论文成图 CLI 退役

- 应当前单应用、单作者资产方向，删除独立 CLI、固定 OBJ/球阵/圆柱
  案例构造、旧手写 CTest、CLI 黑盒/研究成图脚本及其专用资产。
  `vl_offline_core` 搬至 `source/render/offline/core/`，只消费 VL
  冻结的 `MeshInstance`；空实例不再回退到固定球阵。根构建不再
  注册独立 CMake 工程。VL 导出与压力脚本搬至
  `tool/validation/offline/`，不增加引擎 runtime test flag。
- 原独立标量 Eq. 2-15、Table II、groom 协变、PDF/采样、面光 MIS
  等仍有价值的数值检查迁到根 GoogleTest，测试固定参数只在 fixture
  持有。旧 CLI 参数解析、固定 OBJ、球阵/圆柱图像哈希与专属 CSV
  不再运行；历史研究结论移入 `archive/offline-paper-renderer-standalone.md`，
  不再当作当前实现合同。
- 根构建与 CTest **198/198** 通过，VL 验证脚本纯逻辑测试
  **11/11**；迁移后的外部 `standard`、`render-exit` 在 worker
  1/2 均通过，日志在
  `artifacts/validation/offline/vl-stress/20260929-113716-242026-73096/`。
  独立 CLI 源码与构建入口已退役；本地 ignored 目录仍有旧 CLI 构建、
  可执行文件与 Python 字节码。`artifacts/offline-renderer/` 约 645 MB
  的混合结果既含旧论文图，也含近期 VL 集成验证证据，因此暂时保留，
  待按证据归属选择性清理；当前新增 VL 验证输出归
  `artifacts/validation/offline/`。
