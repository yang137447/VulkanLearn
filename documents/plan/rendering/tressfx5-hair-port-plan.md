# TressFX 5.0 for UE 5.4 头发移植开发计划

状态（2026-10-24）：本地分支 `codex/tressfx5-hair-port` 已实现 CPU 数据层、真实 Female
骨架/蒙皮与 Idle/Walk 重定向、GPU guide 模拟、单根骨绑定、骨骼胶囊和逐帧蒙皮代理 SDF。
完整逐发根绑定/贴图、Marschner、抗锯齿和发丝阴影尚未完成。这是 `plan/` 层，不是实现契约；
已实现边界见 [TressFX 资产导入](../../contracts/tressfx-asset-import.md)。资产路线与选项记录见
[hair-pipeline-options.md](hair-pipeline-options.md)。

## 当前执行重点：物理交互（2026-10-24）

用户确认暂缓渲染，先完成复位、连续运动/坐标空间与碰撞验收。主线程规划管理，
子任务分别负责复位、空间/时间审计与外部验证；不新增 runtime test 标志。
验收记录见 [物理交互执行记录](../../rationale/tressfx5-hair-physics-record.md)。

| 分项 | 本轮目标 | 验收方式 | 当前状态 |
| --- | --- | --- | --- |
| 跳变复位 | 首次姿态、seek、clip 切换同步重置 current/previous/prevPrev，清除旧速度 | GTest 状态/参数断言；源码三 buffer 赋值审查、实际 GPU 事件日志与外部截图 | 已实现，模块/事件/图像烟测通过；GPU 逐点 trace 已可用（验证记录 §5.1），reset 专项 trace 尚未单独采集 |
| 空间一致性 | 根骨、胶囊、代理在同一发丝空间内；placement 变化时三份历史保持世界语义 | 世界空间等价推导、独立 Verlet oracle、正式 Female 烟测 | 已实现局部历史 + placement 换基与 5 项世界历史/惯性测试；共同 Actor 移动的运行时夹具已落地（正常 Console `mesh transform/offset/sweep`，60 Hz 短采集命令全被接受、placement 窗口 0 reset），GPU 逐点 trace 已实现（验证记录 §5.1），placement 专属数值惯性已用 v2 trace 采集：200 cm sweep 根点差 0.0016 cm、发梢最大 32.97/均值 9.68/尾段 7.62 cm；旋转/teleport 未做，见 [世界历史记录](../../rationale/tressfx5-hair-world-history-record.md)、[验证记录](../../rationale/tressfx5-hair-verification-record.md) |
| 时间控制 | 冻结快照传递游戏时钟，区分动画暂停与游戏暂停 | 时钟模块测试；外部控制以实际能力为准 | 已实现，帧时钟及 ScriptRuntime 冻结时钟过滤用例通过；正式普通 mesh 烟测通过 |
| 连续运动 | 连续 Idle/Walk、seek 前后、切片与侧/背接触审阅 | 30/60/120 Hz 短烟测；可复用 600 s 长烟测 | 修正 +Z 机位后三档 27 图完成，旧失败图保留；600 s 未执行 |
| 碰撞质量 | 穿脸/颈/肩背、接触抖动与胶囊/SDF 顺序冲突 | 数值模块测试加真实动画审阅；像素差不代替穿透深度 | 现有位置投影语义保持；接触 smoke 已拍，完整数值质量未验收 |
| 源步长/参数 | 源 0.0125 s + 最多 2 次循环、阻尼、VSP 140 cm、曲线与碰撞语义 | 固定源行号的闭式推导、纯函数测试、契约对照 | 原生调度已实施（2026-10-24）：GPU/CPU 均按 n=0/1/2 × 0.0125 s、reset 首轮继续后续 pass、SDF clear/splat 每帧一次而 collide 每 substep；`maxDt` 仅兼容旧 `Step`。VSP/LocalShape 逐顶点作者曲线数据路径已对齐（可选数组、800 B UBO、flags、长度与 0.95 校验、CPU/shader 消费），临时 SM 的 32 项曲线烟测已确认 `vspcurve=32/localshapecurve=32`、finite=0；正式资产无曲线数据仍走 fallback。新增 9 项调度/动态/历史测试；阶段 4a 再增 12 项曲线/UBO/导入测试，阶段 3 再增 5 项 trace 格式/往返测试，多实例再增 1 项 key 测试（`hair_asset_tests` 全量 254）。60 Hz Debug 启动烟测 shader `failed=0`、ResetPositions 有 `substeps=`、无 VUID、stderr 0。见 [时间/参数记录](../../rationale/tressfx5-hair-timing-params-record.md) §3.3/§6 |
| 验证准备 | 三目标的验证矩阵、采集/检查工具、缺口 | 外部 Console 烟测、证据检查器、GPU readback 方案 | 工具与矩阵已就绪；placement 夹具已落地；一次性 `hair trace` GPU 只读回读已实现并跑通 120 帧/40 guides 短 trace（finiteCount=0、段长比 p99/max、trace 开关 dispatch 一致），逐帧 dt 分布、接触抖动与 VSP A/B 仍未实现；v3 trace 已读回碰撞前 SDF/胶囊最大穿透，见 [验证记录 §5.1](../../rationale/tressfx5-hair-verification-record.md) |
| 后续物理能力 | 摩擦/速度响应、真实 `.tfxbone`/作者曲线数据、专门快速转头夹具 | 独立设计与对应验收 | 多实例独立状态已实现（per-placement RenderableObject，双发丝烟测 objects=2/guides=80668）；`.tfxbone` parser/映射 + GPU 加权 `InitialPos`/`SkinningQuat` 已接入（临时 75/25、100% Chest 烟测，无 VUID），但本地无真实 `.tfxbone` 资产、正式 Female 仍单根骨；摩擦/真实曲线导出/快速转头夹具仍未纳入 |

### UE5 物理效果对照决策（2026-10-24）

用户补充目标为“尽量复刻 UE5 里面的头发物理效果”。接下来的直接算法/参数基线固定为
`GPUOpenSoftware/UnrealEngine` 的 `TressFX5-5.4.4`、
revision `61f8dd9b51b88d41937ef8d73618aac778642f41`；不能仅凭参数同名或截图相似
宣称对齐。UE 内置 Groom 同时作为效果与能力参照，分别记录资料依据和未核实部分，
不把它与 AMD 插件称为同一套 solver。

Epic UE5.4 文档描述的内置 Groom 物理为 Niagara/GPU 上的 XPBD，提供 Rods/Springs
模型，以及静/动摩擦、黏性、local linear/angular velocity scale 和 teleport distance
等控制；这些用于整理能力差异，不直接作为当前 TressFX 参数的同义替换。
资料见 §13 的两份 UE5.4 官方物理文档。
介绍文档只能说明能力与概念；未读取对应源码的部分保持未核实，不能据此认定
某个具体求解、摩擦或惯性实现已被证明，更不能与当前 TressFX 参数直接等同。

用户随后明确：**UE 侧主要看代码与物理分析，不准备、构建或启动 UE，
不采集 UE 截图或运行轨迹。** 现有参考检出只有插件、engine patch、Maya 资产和工具，
README 提到的 `Asset/Project/UE544TFX` 未提交；可运行工程不再作为本轮前置条件。
主要对照证据是固定 revision 的 host/shader 公式及单位、空间、历史、调度审计，
配合由公式推导的纯 CPU oracle 和独立数值分析。VL 的实际运行、数值采集与场景验证
可独立推进；已完成的 30/60/120 Hz 图片、复位日志及模块测试边界继续以执行记录为准。
“效果接近”按同输入源码语义和可测物理响应评估，不宣称与 UE 实测一致。
“忠实复现源行为”与“物理合理性”分别记账：即使源常量/循环看似特殊，也先追调用上下文、
单位与累计物理时间，再判断它的含义，不机械照抄或未经论证更改。

源码语义与物理响应对照先冻结一份输入及推导清单：

| 对照项 | 固定内容 |
| --- | --- |
| guide 几何 | 同一组曲线、顺序/身份、每根点数、rest length、根固定点与绑定；发生重采样时另记误差 |
| 角色运动 | 同骨骼动画/采样时刻、根运动模式及 Actor placement 轨迹；甩头与整体移动分别测 |
| 单位与空间 | 厘米/米转换、轴向、模型/世界空间、碰撞代理及骨骼变形保持可追溯对应 |
| 参数与外力 | TressFX 数值/公式逐项对照，固定重力、风、阻尼与约束；Groom 能力另表记录，缺源码的实现不作等价断言 |
| 时间与初始化 | 固定输入 dt 序列、预热时长/步数、开始姿态、三份历史及 seek/clip/teleport 的 reset 约定 |
| 分析与观察 | 源公式推导可检验的数值期望；VL 固定相机/采样时刻观察 guide、接触与轮廓，不依赖 UE 出图 |

实施优先级先由固定源的遗漏决定，再由能力差异决定：

1. **共同 Actor placement 运动是最高优先源对齐缺口。** 源 TressFX 的蒙皮目标与位置历史
   包含世界变换：patch `ApplyVertexBoneSkinning` 在 8304..8308 行组合
   `PrimitiveData.LocalToWorld`，Integration 在 8432..8433 行把 world cm 转 world m，
   SDF 代理也组合世界变换（9765..9766 行）。VL 当前相对 palette 的空间一致性测试只证明根点/碰撞体对应，
   不能证明共同移动时的发丝惯性。先审计并修复仿真空间、历史位置和整体平移/转身的关系；
   这属于恢复既有 TressFX 运动语义，不能归入额外 Groom 能力。
2. **源主路径步长/循环差异（2026-10-24 已实施）。** `TressFXCoreSimulation.cpp` 在
   935 行保存组件 dt、941 行把 `Instance->TimeStep` 设为 0.0125、1013 行最多循环两次，
   每次给 Integration/Length 传该固定值，1217 行再从局部输入 dt 扣 0.0167。
   152/271 行的 Integration 参数 clamp 不能代表上层实际调度。VL 现在 GPU 与 CPU
   共用 `PlanGuideSubsteps`：按冻结 delta 得 0/1/2 个 0.0125 s substep，reset 首轮
   继续后续 pass，风帧/时钟/历史每帧只提交一次；`maxDt` 只保留给旧 `Step`。
   剩余风险（n==0 帧不推进 `Guides.Frame`、每 substep 无随机风幅值、30/48 Hz
   dispatch 翻倍的性能）见时间/参数记录 §6.5、§8。
3. 建立甩头、转身、整体启停、脸/颈/肩背接触及传送夹具。分别覆盖连续运动与应复位跳变，
   对同一输入留存固定源的公式/空间/时序推导、纯 CPU oracle 与 VL 验证记录；
   不要求 UE 运行，Groom 能力参考另列。
4. 采集选定 guide 的根到梢轨迹、发梢相对头部的滞后、启停后的振幅衰减、
   段长伸长比、碰撞穿透深度及静置接触抖动。报告峰值/分位值与时间曲线，
   有限值检查独立于图片；数值采集尚未具备时明确未验收。误差阈值按量纲、解析特例、
   独立 oracle、约束边界和测试数值容差确立，场景质量目标另列，不等待 UE 采集。
5. 补齐其余源参数面差异：VSP 的 `length(new-2*old+oldOld)` 未除 dt²，是二阶
   位置差幅值。源默认 1.4 world m 对应 140 cm，VL 当前 5000 cm 不等价；
   源 VSP/LocalShape 作者曲线按 `i/N` 采样，VL 已提供逐顶点数组数据路径、
   800 B UBO 与 CPU/shader 消费，但真实 Maya 曲线尚未采样导出，正式资产仍走
   VSP 线性/LocalShape 单标量回退。默认 global shape/VSP 开关与强度、多骨
   `.tfxbone`、MT 根三角绑定等也需分别对照，不将当前简化形式记作源参数完整对齐。
6. 对照固定 revision 的 host 参数、StrandLevel/VSP、积分/约束、碰撞和 reset 路径，
   先补源已有但未移植的行为。源 SDF 对 previous 的写回在 patch 8051 行已注释，
   因此不能把添加 previous 速度响应/摩擦称为恢复该源分支；源未提供的摩擦、体积、
   CCD/self-collision 或 XPBD 等按独立能力设计，并标注为效果扩展。

帧率适配与物理时间策略分开决策。2026-10-24 起 VL 改为源原生调度：n=0/1/2 个
固定 0.0125 s substep，每帧物理时间只能是 0/0.0125/0.025 s；`maxDt` 不再限制
GPU 路径，只服务旧 `GuideSimulation::Step`。30/60/120 Hz 烟测仍不等于帧率无关性
通过，逐帧 dt/dispatch 计数与逐点轨迹读回尚未实现。不为贴近 Groom 立即改成
XPBD 或给现有参数重新赋义。

本轮只收敛目标与验收，不改 solver。渲染继续延后，只保留足够审阅 guide 轨迹、
轮廓和身体接触的可视化；Marschner、透明/抗锯齿和阴影不作为物理对照的前置门槛。

下文 M0..M4 是原始完整移植路线；其中 UE 出图/参考工程条目保留为历史路线，
本轮由上述源码与物理分析决策覆盖，不作为实施或验收前置条件。
不能用原先的渲染优先顺序阻塞本轮物理交互。
以下各批记录按当时范围保留；其中“没有模拟/没有骨架”不代表当前状态。

## 首批落地范围（2026-10-22）

- 纯 CPU `vl_hair_assets`：`.tfx` 读取、属性/bounds、结构化错误、静态切线/rest length/ribbon 索引。
- `vl_tfx_inspect`：只读检查 JSON 报告；`hair_asset_tests`：根 GTest 模块套件。
- 以本地 5.0 exporter 核实：产品版本是 5.0，实际 `.tfx` header version 仍为 **4.0**；
  `.tfxbone` 实际按 bone count/name/strand weights 流写入，不能直接套未使用的
  `TressFXTFXBoneFileHeader`。
- 首批当时没有真实女性 `.tfx` 导出；当时 M0 资产验收、M0.5 shader/渲染 spike 与 M1 均未完成。
- 2026-10-21 已关闭参考仓库 sparse checkout；该分支只有插件、引擎 patch、Maya 资产和工具，
  README 所称 `Asset/Project/UE544TFX` 未提交。后续 UE 画面对照依赖参考工程另行准备。
- 验证：隔离工作树 MinGW Debug 的 `hair_asset_tests` / `vl_tfx_inspect` 构建通过，
  36/36 项 CTest 通过；中文路径的合成 `.tfx` 报告为 2 guides / 8 points / 36 indices，
  缺失文件返回 `Hair.TfxRead`、exit 1。证据在 gitignored 的
  `artifacts/tressfx5-import/`。首次独立配置误选 Clang，改为显式 gcc/g++ 后通过；
  依赖源码复用本机下载目录，构建输出独立。共享目录的 main 构建曾通过，
  迁移后的隔离目录未重编完整 main，也未执行 Vulkan 发丝出图或三项 runtime gate。

## 第二批落地：正式网格路径的静态 guide

本段更新首批的实施状态；下文 groom 专用资产、pass 和完整 M1 仍为规划。

- 场景继续使用 `type: mesh`，`SM_*` 描述符使用 `type: tressfx`、`.tfx` 和显式
  `strandRadius`。通过 `TressfxModelImporter → MeshAssetLoader → RendererMeshLoader`
  复用 World 候选、上传、快照与资源退休，没有新增 groom 场景对象。
- `shader/glsl/engine/tressfxVertexFactory.glsl` 迁移 patch 中
  `TressFXVertexFactory.ush::ComputeViewAlignedWorldPosition` 的统一 ViewForward
  展开语义；局部中心线在 GPU 展开，保留旋转/缩放实例和材质工厂身份。
  下文旧拆解顺序的 `TressFXBasePass` 是概括名称，不能当作真实 shader 文件。
- 先用 `hair/M_strandProbe` 的双面 Unlit、alpha=1、写深度探针进入既有
  `forwardTransparent`；没有恢复 Hair ID，也没有启用 MSAA/A2C 或像素最小半径。
  材质边界拒绝 deferred、蒙皮和 WPO；离线三角形路径明确拒绝该工厂。
- 隔离工作树完整 main、导入工具、hair/material/UI 测试目标构建通过。
  首次链接因 CMake 的 `/hair/` 排除规则误伤嵌套 importer 失败，收窄到
  `/source/hair/` 后完整构建通过。
- 合成探针为 96 guides ×32 points、每实例 17,856 indices，包含正面和旋转 55°
  的两个实例。真实 Vulkan 正面/转相机截图已检查，均为完整 1280×720 BMP：
  `artifacts/tressfx5-static/Resources/Generated/Screenshots/front.bmp`、
  `orbit.bmp`。运行 stderr 为空，窗口正常关闭并完成 teardown。
  这只验证几何/朝向/正式加载链，不代表真实女性头发、光照或亚像素覆盖验收。
- `hair_asset_tests` 49/49、`material_schema_tests` 16/16；
  `editor_ui_tests` 全量 681 项，676 通过、5 项符号链接权限环境跳过。
  三项既有 Vulkan 门禁串行通过（exit 0）。最初缺少 Common 测试纹理导致
  fixture 加载失败，补齐隔离资源后通过；未修改原资源目录或门禁实现。
  合成 fixture 和运行日志位于 gitignored 的 `artifacts/tressfx5-static/`。

当前没有 `GR_*` 或 `.groom` 格式；下文 §4.3/§5 的专用格式是后续设计候选，
不能据此另建与正式 SM 加载并行的作者链。M0 的女性资产导出、完整逐文件 port map、
M1 的 Marschner/覆盖率仍未验收。

## 第三批落地：Female 正式资产关卡

- 配套资源分支为 `codex/tressfx5-assets`，工作树在引擎工作树同级的
  `VukanLearnResources`，沿用 config 的 `../VukanLearnResources`。
  只检出 Common、默认 sphere array 和本关卡，未改原资源仓库的 master 工作目录。
- 正式入口为 `Maps/SC_tressfx_female/SC_tressfx_female.json`；三个 SM、三个 MI、
  OBJ/TFX 与来源许可归属该地图。Generated 只存导入报告、验证和截图。
- 本机没有 Maya。新增受限离线提取器，按 `.ma` 内容摘要锁定源文件，通过 SciPy
  B-spline 求值保存的 NURBS 曲线；这是静态保存几何迁移，不声称执行过官方 Maya exporter、
  绑定图或 UE 工程。主组实际 40,334 根；另组 308 根使用重名节点，必须按 DAG 路径区分。
- 每根 32 点、共 1,290,688 点；完整导入主组，未抽 guide 子集或补 follow。
  BodyModel/眼球保留保存的 Orig 输入网格，OBJ 由 Assimp 正式加载；
  源包缺失的两张外部贴图没有伪造，使用中性 PBR 和粉色 Unlit 发丝。
- 已通过正式 scene API 读取/能力/修改/校验/保存相机，重启消费最终构图。
  相机水平 FOV 为 38°，首次误按竖直视场估算距离而裁边，已将位置调整为
  `[35,162,140]`、目标 `[0,151,0]`。开启编辑器后，较矮视口仍有裁边，
  最终把正式位置改为 `[50,165,210]` 并重启验证，完整头顶与发梢均在默认编辑器画面内。
  Console `camera pose` 改的是 active World 相机，不能当成独立编辑器相机的构图验证。
- 实测 `vl_tfx_inspect`、三个 SM schema 与 scene/mesh 标准 API 通过；
  稀疏资源根场景 schema 为 2/2，不代表原资源目录全量扫描。
  独立重导入 10 文件字节一致、1 个作者保存 JSON 语义一致；已有目录明确拒绝。
  `female-final.bmp` / `female-side.bmp` / `female-editor-final.bmp` 已检查，1280×720，日志见
  `artifacts/tressfx5-female/`。Python 默认选到 free-threaded 3.14，安装的
  NumPy 扩展不能加载，改用标准 `C:\Program Files\Python314\python.exe` 后成功；
  首次离线 mesh 提取只消费了首个显式 polyFaces 块，并拒绝了源中的五至七边形，
  补齐后续省略 type 的块与 n-gon 支持后，全部 face 数量和边闭环通过。
- 第三批仍不完成完整 M0/M1：没有 Maya/UE 对照、贴图、骨骼/follow、Marschner、
  发丝覆盖抗锯齿或阴影；静态关卡不等于完整角色移植。当前实现细节见
  [正式静态关卡契约](../../contracts/tressfx-asset-import.md#female-正式静态关卡)。

## 1. 目标与范围

目标：把 **TressFX 5.0 for UE 5.4** 示例里的粉发女性资产搬进 VulkanLearn，
先做静态头部肖像，再逐步接入 guide/follow 蒙皮、发丝模拟、阴影和 LOD，
最终接近 UE 5.4 参考画面的观感。

### 1.1 版本基线

- **目标版本：TressFX 5.0 for UE 5.4**（2025-08-04 发布，分支
  `TressFX5-5.4.4`）。
- 资产：该 patch 的 `Asset/Maya2018/Female_Outside/Female_Outside.zip`；
  `.ma` 实际是 **Maya 2020**（文档写 2018，以文件为准）+ collision mesh。
- UE5.4 相对 UE4.27 的差异：Lumen 软/硬光追、deep shadow approximation、
  continuous LOD、IndirectDraw texture clear。
- **不使用 UE4.27 版作为目标**；`TressFX5.0_DeveloperGuide.pdf` 主要写
  UE4.26/4.27，架构/导出流程可参考，但 UE5.4 的差异以更新说明为准。
- **运行时基座改为拆 TressFX 5.0 for UE 5.4 工程**：抽 TressFX 核心/插件/shader，
  把 UE glue 重写到 VulkanLearn；4.1 standalone 只作算法对照与缺失模块 fallback，
  不再作为主基座。

范围：

- 资产：female avatar 的头/身体、头发、贴图、骨骼、collision mesh。
- 渲染：strand 几何、forward 透明、MSAA/alpha-to-coverage、Marschner 头发着色、
  deep shadow、OIT（ShortCut/PPLL）、LOD。
- 模拟：guide/follow、骨骼蒙皮、SDF 碰撞、VSP、约束求解。
- 不包含：UE 渲染器整体移植、TressFX 5.0 的 UE 插件原样编译、毛发/fur 全量支持。

## 2. 当前事实与约束

- **TressFX 5.0 for UE 5.4** 不是独立库，是 UE 5.4.4 的 patch；女性资产在
  `Asset/Maya2018/Female_Outside/Female_Outside.zip`（`.ma` 是 Maya 2020，
  40,334 guides + `tfxmesh`），需要 Epic UE GitHub 权限。
- 5.0 for UE 5.4 的运行时是 UE Plugin + Renderer hooks，绑在 UE deferred 管线上；
  不能作为 VulkanLearn 的构建依赖。
- 独立可用的 Vulkan 参考是 **TressFX 4.1 standalone**（MIT）：
  DX12/Vulkan、Maya exporter、strand 渲染/模拟/SDF/LOD。
- VulkanLearn 当前是 deferred GBuffer；`forwardTransparent` 已存在但 `needMsaa:false`，
  `graphicsPipelineBuilder.cpp` 的 `alphaToCoverageEnable` 固定为 false。
- 角色侧已有 GPU 蒙皮与 GASP 骨架；发根可以绑到头骨/头皮骨骼。
- Hair 着色模型在 2026-09-12 被删除；`common/hairPathScattering.glsl` 仍在。
- 已有 `.tfx` guide 导入、camera-facing ribbon、CPU/GPU guide solver、单根骨驱动与
  逐帧蒙皮代理 SDF；没有 `.tfxbone` / `.tfxmesh` 导入、follow 生成、Marschner或发丝阴影。当前契约以
  [TressFX 资产导入](../../contracts/tressfx-asset-import.md) 为准。

## 3. 总体路线

两条线并行，但以资产线为前提：

| 线 | 内容 | 关键依赖 |
| --- | --- | --- |
| 资产线 | UE5.4 patch → Maya 2018 → `.tfx`/`.tfxbone`/`.tfxmesh` + FBX + 贴图 | Epic UE 权限、Maya 2018 |
| 运行时线 | 拆 5.0 工程 → TressFX 核心/shader → VL strand 资产 → forward 渲染 → 蒙皮/模拟/阴影/LOD | TressFX 5.0 for UE 5.4 源码 |

运行时**以 TressFX 5.0 for UE 5.4 工程为主基座**：拆出 TressFX 核心/插件/shader，
把 UE glue 重写到 VulkanLearn；4.1 standalone 只作算法对照和缺失模块 fallback。
这样能避免“老基座 + 5.0 资产”造成的光照/阴影/透明路径不一致。

### 3.1 5.0 工程拆解边界

“拆”指**抽源码 + 重写 UE glue**，不是把 UE 代码直接编进 VulkanLearn。

| 5.0 工程部分 | 处理 | VL 落点 |
| --- | --- | --- |
| `Engine/Plugins/AMD/TressFX`（Asset/Importer/Simulation/SDF） | 抽核心数据结构与算法，去掉 UE 类型 | `source/hair/` |
| `Engine/Source/Runtime/Renderer/Private/TressFX`（BasePass/DeepShadow/Marschner/Composition） | 重写为 VL pass | `source/render/pass/` + `shader/glsl/hair/` |
| `Engine/Shaders` 的 TressFX `.ush`/`.usf` | HLSL→GLSL，替换 UE include | `shader/glsl/hair/` |
| UE Material/VertexFactory/SceneProxy hooks | 丢弃，换 VL material/vertex layout | `source/material/`、`source/pipeline/` |
| `Asset/Maya` + `.tfx`/`.tfxbone`/`.tfxmesh` | 保留资产管线 | `tool/hair/` |
| `Tools` Maya exporter | 原样使用 | 外部工具 |

不搬：UE RHI、Lumen、UE 材质图、`SceneTextures`/`LightBuffer`、UE 阴影管线。
UE5.4 的 Lumen 光追不在本期范围；只保留 TressFX 自己的 deep shadow/opacity。

拆解顺序：

1. **依赖审计**：列 5.0 工程文件、include、UE 类型；产出 port map（3–5 天）。
2. **数据层**：`TressFXAsset`/Importer + `.tfx` 解析 → VL groom asset。
3. **静态渲染**：`TressFXBasePass` 的 strand VS/PS + Marschner → VL forward hair。
4. **阴影**：DeepShadowMap/DeepOpacityMap → VL compute/raster pass。
5. **模拟**：`TressFXSimulation` / SDF / VSP compute → VL compute。
6. **合成/材质**：`TressFXComposition` / 材质参数 → VL pass/material。

拆解 spike 的验收：`.tfx` 能读、一根 strand 能在 VL 画出、构建里没有 UE 代码、
依赖 port map 完整。达不到就先退回“5.0 资产 + 4.1 运行时”的混合方案。

初步盘点见 [tressfx5-port-map.md](tressfx5-port-map.md)。

## 4. 资产管线

### 4.1 拉取

- 仓库：`GPUOpenSoftware/UnrealEngine`，分支 `TressFX5-5.4.4`。
- 只取资产：sparse checkout `Asset`，避免完整 UE 源码。
- 仓库当前没有预导出的 `.tfx`/`.tfxbone`/`.tfxmesh`；女性资产只有 `.ma`，
  必须走 Maya 导出。
- 只有 `.ma`/`.mb` 时，用 **Maya 2020** 打开（依赖 `hairPhysicalShader`/`mtoa`），
  再用仓库 `Tools/Maya2022/TressFX_Exporter.py` 导出 `.tfx`/`.tfxbone`/`.tfxmesh`。
  仓库没有预导出的 `.tfx`，必须走 Maya。

### 4.2 导出清单

| 资产 | 格式 | 用途 |
| --- | --- | --- |
| 头/身体/头皮 | FBX（带骨架） | 角色网格、发根绑定 |
| 头发 guide | `.tfx` | 位置、UV、宽度、颜色 |
| 骨骼蒙皮 | `.tfxbone` | 发根/发丝骨骼权重（5.0 最多 16 根） |
| collision mesh | `.tfxmesh` | SDF 碰撞 |
| 贴图 | PNG/TGA/DDS | hair/face 材质 |
| 参考图 | PNG | UE5.4 同机位对照 |

实际入口：`Asset/Maya2018/Female_Outside/Female_Outside.zip` → `Female_Outside.ma`
（Maya 2020，185 MB，40,334 个 XGen guide，13 个 hair joint，`tfxmesh`）。
这不是 Ratboy；Ratboy 只是插件默认内容。

### 4.3 离线转换

- `.tfx` 是简单二进制：header + positions(float4) + strand UV + vertex UV +
  thickness + vertex color；**只存 guide**，follow 运行时生成。
- `.tfxbone`：bone names + skinning indices/weights。
- `.tfxmesh`：collision triangles，用于生成 SDF。
- 写一个 `tool/hair/tfx_dump` 或 Python 转换器，输出：
  - `.groom` 二进制（VL 运行时格式）
  - `GR_*.json` 描述符（见 §5）
  - 统计/校验报告（strand 数、点数、bbox、骨骼数、SDF 分辨率）
- 5.0 的 `.tfx` version 可能高于 4.1；解析器要按 header/offset 做版本容忍，
  不直接复用 4.1 的版本拒绝逻辑。

## 5. 运行时数据模型

### 5.1 资产

新增 groom 资产（命名暂定 `GR_*`，最终以 `contracts/asset-organization.md` 为准）：

```json
{
  "name": "GR_female_hair",
  "type": "groom",
  "source": "Source/female_hair.groom",
  "guideCount": 40334,
  "followCount": 20000,
  "pointsPerStrand": 32,
  "scalpMesh": "SM_female_head",
  "skeleton": "SK_female",
  "material": "M_hair",
  "collisionMesh": "Source/female_hair_collision.groommesh",
  "sdfResolution": [64, 64, 64]
}
```

二进制 `.groom` 建议字段：

- header：magic/version/guideCount/followCount/pointsPerStrand
- positions `float3[guideCount * pointsPerStrand]`
- tangents（离线算或运行时算）
- width per point 或 per strand
- rootUV / strandUV
- bone indices/weights（可选）
- follow 生成参数（root radius、tip separation、clump）

### 5.2 场景对象

新增 `groom` 场景对象（或 mesh 的 `groom` 子对象），引用 `headMesh` 和 transform：

- `SceneAssetValidator` 增加字段校验；
- `worldLoader` / `worldBuilder` 增加构建计划；
- `schema/scene.schema.json` 与文档同步；
- 先支持静态/单实例，不做多 groom 合批。

## 6. 渲染管线

### 6.1 Pass 顺序

目标顺序：

```text
shadowCascade0..3        opaque shadow
hairSimulation           compute（V2）
hairDeepShadow           compute（V1/V2）
geometry                 deferred opaque
deferredLighting         采样 hairDeepShadow
forwardOpaque
sky
hairForward              forward + MSAA + alpha-to-coverage（V0 起）
forwardTransparent
editorGrid
autoExposure / bloom / toneMapping
```

V0 只加 `hairForward`；V1 加 `hairDeepShadow`；V2 加 `hairSimulation`。

### 6.2 Strand 几何

- 每个 strand 点扩成 camera-facing ribbon：2 顶点/点，2 三角形/段。
- 顶点着色器读取 `HairVertex` storage buffer + index buffer；
  不把每帧发丝位置写回静态顶点缓冲。
- 每顶点属性：position、tangent、width、strandUV、rootUV、strand index。
- 切线方向 root→tip；ribbon 宽度按视距/像素覆盖率钳制，避免亚像素闪烁。
- 索引缓冲离线生成；发丝数量/点数变化时重建。

### 6.3 Pass 类型与 MSAA

- 新增 `RenderGraphPassType::ForwardHair`（避免和已有 `forwardTransparent`
  单例行为冲突），或在 V0 复用 `forwardTransparent`，V1 再拆分。
- `needMsaa: true`；渲染目标用现有 MSAA source/resolve 资源。
- `graphicsPipelineBuilder.cpp` 增加 `alphaToCoverageEnable` 开关；
  `sampleCount` 用 pass 的 MSAA 配置。
- 无 TAA 时，MSAA 4x/8x + alpha-to-coverage 是底线；有 TAA 后再接
  velocity（`gbufferVelocity` 已有）。

### 6.4 着色

- Marschner R/TT/TRT + 多重散射；复用/重写 `common/hairPathScattering.glsl`。
- TressFX 5.0 的 Marschner shading model 作为参考；不要在 GLSL 里硬编码
  UE 的材质节点。
- 输入：base color / scatter / roughness / backlit / tangent；参数走
  `M_*.json` / `MI_*.json`，与现有材质系统一致。
- 先做单散射 + 简化多重散射；再按 TressFX 5.0 的 dual scattering/deep shadow 对齐。

### 6.5 透明与 OIT

- V0：alpha blend + MSAA；接受发丝间排序误差。
- V1：ShortCut（TressFX 3.1+ 的简化 OIT）或 per-pixel linked list（PPLL）；
  需要额外 buffer/atomic，作为 compute/raster 混合方案。
- 选 ShortCut 优先：实现量小于 PPLL，TressFX 4.1 有现成 shader。

### 6.6 发丝阴影

- V1 先做 deep shadow map / deep opacity map（每光源），或至少把发丝
  加入 CSM 的 alpha-tested 阴影。
- V2 对齐 TressFX 5.0：DeepShadowMap + DeepOpacityMap、每 TressFXComponent/光源；
  AMD 的 DeepShadow approximation 作为可选路径。
- 阴影必须用同一份模拟后位置，否则会拖影。

## 7. 模拟

### 7.1 设计

- guide/follow：只模拟 guide（约 512–2048 根），follow 由 guide 插值/程序化生成。
- 约束：距离（拉伸）、弯曲、local/global shape、碰撞；摩擦另行设计。
  VL 已按源原生调度落地：每帧 n=0/1/2 个固定 0.0125 s substep，物理时间只能是
  0/0.0125/0.025 s；`maxDt` 仅兼容旧 `Step`，见上述物理效果对照决策与
  [时间/参数记录](../../rationale/tressfx5-hair-timing-params-record.md) §6。
  原固定步长 + 2–4 substep 的 accumulator 候选仍不作为默认路线。
- 外力：重力 + 程序化风；阻尼分根/中/梢。
- 碰撞已使用普通 OBJ 代理的逐帧蒙皮 SDF + capsule；`.tfxmesh` 导入属于独立缺口。
- 蒙皮：发根随头骨；`.tfxbone` 的 16 骨权重映射到 VL 骨架。
- VSP：高速移动时传播根部速度，避免发丝拉长。
- LOD：V3 接 continuous LOD；V2 先固定 guide 数。

### 7.2 GPU 边界

- 模拟在 compute，current/previous/prevPrev 三份位置历史，正常帧 CPU 零回读。
- 发丝位置/切线写 storage buffer；渲染 pass 直接读。
- VL 已按源 n=0/1/2 × 0.0125 s 调度（GPU 与 CPU `StepFrame`）；冻结时钟保证
  同一帧只消费一次，n=0 帧不消费 reset/clock/history。游戏暂停/time-scale 的
  冻结语义保持，reset 首轮按源继续录制后续 pass。
- 模拟 pass 失败时保留上一帧姿势，不能破坏渲染。

## 8. 代码落点

### 8.1 资产/数据

- 5.0 拆解来源：`Engine/Plugins/AMD/TressFX`、
  `Engine/Source/Runtime/Renderer/Private/TressFX`、`Engine/Shaders`；
  M0.5 产出 port map（文件 → 依赖 → VL 落点）。
- `source/hair/tfxLoader.*`：`.tfx`/`.tfxbone`/`.tfxmesh` 解析（纯逻辑，可测）。
- `source/hair/groomAsset.*`：VL `.groom` / `GR_*.json`。
- `tool/hair/tfx_dump.*` 或 Python 转换器；输出报告。
- `source/mesh/loader/mesh/assimpModelImporter.*`：复用 FBX 导入。
- `source/animation/**`：骨骼名/权重映射。

### 8.2 渲染

- `shader/glsl/hair/hairStrands.vert`、`hairShading.glsl`、`hairForward.frag`。
- `shader/glsl/hair/hairSimulation.comp`、`hairSDF.comp`、`hairDeepShadow.comp`。
- `source/pipeline/graphicsPipelineBuilder.cpp`：alpha-to-coverage / sample count。
- `source/render/rendergraph/renderGraphPassType.h` + `renderGraphCompiler.*`：
  `ForwardHair` / `HairSimulation` 类型。
- `source/renderGraph.cpp`、`source/renderSystem.cpp`：MSAA 资源、pass 顺序、draw。
- `config/renderGraphConfig.json`：新增 pass/resource。
- `source/world/loading/worldLoader.*`、`worldBuilder.*`：
  `groom` 对象构建。
- `source/scene/validation/sceneAssetValidator.*`、`schema/scene.schema.json`：
  新对象校验。

### 8.3 文档/测试

- 落地后更新 `contracts/vulkanlearn-architecture.html`、`contracts/gbuffer-channel-ownership.md`、
  `contracts/shader-hot-reload.md`、`contracts/testing.md` 的相关条目。
- GoogleTest 放 `tool/hair-tests/`（或现有最近套件），不新增 runtime test 命令。
- 参考 `tool/validation/` 的截图/像素对照方式。

## 9. 里程碑与验收

### M0 资产可解析（1–2 天）

- 拿到 `.tfx`/`.tfxbone`/`.tfxmesh` + FBX + 贴图。
- `tfx_dump` 输出 strand 数、points、bbox、骨骼数、SDF 分辨率。
- 验收：数量/bbox 与 UE 内统计一致；无越界/NaN。

### M0.5 5.0 工程拆解审计（3–5 天）

- 列 5.0 工程文件、include、UE 类型依赖；产出 port map。
- 用最小 strand 数据验证“抽出的 shader/数据能进 VL 构建”，不接完整渲染。
- 验收：构建里没有 UE 代码；port map 覆盖 BasePass/DeepShadow/Marschner/
  Simulation/SDF；明确 full extraction 还是 hybrid。
- 如果 spike 失败，退回“5.0 资产 + 4.1 运行时”的混合方案，并记录卡点。

### M1 V0 静态肖像（1–2 周）

- female avatar + hair 在 VulkanLearn 静态渲染。
- forward strand pass、MSAA/alpha-to-coverage、基础 Marschner。
- 验收：同机位对照 UE5.4 参考图，轮廓/发丝密度/主高光位置一致；
  无明显缺丝/穿插/排序灾难。

### M2 V1 蒙皮与阴影（1–2 周）

- guide/follow 插值、骨骼蒙皮、转头不炸。
- hair deep shadow/opacity（至少 directional light）。
- 验收：头部转动/姿态变化时发根不漂移；发丝在脸上有正确投影；
  阴影不拖影。

### M3 V2 模拟（2–4 周）

- guide 模拟 + SDF 碰撞 + VSP + 重力/风。
- 验收：静置稳定、快速转头不拉长、发丝不穿脸/肩；
  连续 10 分钟无爆炸/NaN；GPU 时间可接受。

### M4 V3 对齐与 LOD（2–4 周）

- 连续 LOD、TAA/velocity、材质/光照对齐、性能优化。
- 验收：UE 参考图 ROI 像素误差在约定阈值内；目标分辨率下帧时间达标；
  LOD 切换无明显跳变。

> 工期是粗估，取决于资产导出、UE 参考环境、以及是否已有 TAA。
> 本轮按用户授权先推进 M3 物理交互，渲染里程碑暂缓；不得把基础模拟落地等同于 M3 质量验收完成。

## 10. 测试与验证

- **单元测试**：`.tfx`/`.tfxbone`/`.tfxmesh` 解析、偏移/越界、bbox、骨骼映射。
- **离线数值**：SDF 生成、约束求解、follow 插值；放 `vl_offline_core`。
- **历史渲染规划中的图像对照**：UE5.4 同机位截图 vs VulkanLearn；
  原设固定曝光/灯光及像素/ROI 统计。本轮 UE 侧不执行、不作为前置，
  物理验收按上述固定源码/独立数值分析与 VL 场景验证推进。
- **稳定性**：30/60/120 Hz 变量步长下运行、快速转头、传送/大幅位移；
  外部截图/日志烟测与长度/有限值/穿透的数值验证分别记账。
- **性能**：20k render strands / 2k guides 的 GPU 时间、overdraw、显存。
- **回归**：现有 forwardTransparent/GBuffer/shadow 路径不受影响；
  shader cache/hot reload 正常。

## 11. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| UE patch 访问/资产 license | 拿不到 female avatar | 先确认 Epic 权限；资产条款单独记录；个人学习与发布分开 |
| `.tfx` 5.0 版本差异 | 4.1 解析器拒绝 | 写版本容忍 parser；按 offset 读，未知字段跳过 |
| 5.0 工程 UE 耦合深 | 拆解超预期、进度失控 | M0.5 spike gate；只抽核心/shader；失败转 hybrid |
| 40,334 guides | 模拟/渲染/显存过高 | 导出时抽 guide 子集 + follow；V3 接 continuous LOD |
| UE shader 依赖 | 不能直接编译 | HLSL→GLSL；替换 `SceneTextures`/`LightBuffer`；以 5.0 工程为源、4.1 为对照 |
| UE5.4 Lumen/DeepShadow 依赖 | 无法完全对齐 | Lumen 明确不做；只保留 TressFX 自己的 deep shadow/opacity |
| 无 TAA | 发丝闪烁 | MSAA + alpha-to-coverage 先顶；V3 补 TAA |
| MSAA/alpha-to-coverage 未接 | 边缘碎 | 先改 pipeline builder；确认 sampleCount/attachment |
| OIT 复杂度 | 发丝排序错误 | V0 alpha blend；V1 ShortCut；PPLL 作为可选 |
| Deep shadow 成本 | 帧时间高 | 先 directional light；V3 再全光源 |
| `forwardTransparent` 单例 | pass 冲突 | 新增 `ForwardHair` 类型，V0 可临时复用 |
| 资产过大/流式 | 显存/加载 | 先离线裁剪到肖像范围；后续 LOD/streaming |

## 12. 待定决策

已定：运行时基座 = 拆 TressFX 5.0 for UE 5.4 工程，4.1 只作 fallback；
M0.5 spike 验证可行性后再冻结。

待定：

1. V0 是否新增 `ForwardHair` pass 类型，还是临时复用 `forwardTransparent`。
2. groom 资产是独立 `GR_*` 场景对象，还是 mesh 的子资源。
3. follow hair 用 TressFX 程序化生成，还是 Maya 额外导出 follow 曲线。
4. OIT 选 ShortCut 还是 PPLL（建议 ShortCut 优先）。
5. 模拟直接 port TressFX 4.1 compute，还是先写 P2 简化版再替换。
6. TAA 是本期做，还是 V3 单独做。
7. 资产 license 是否允许非 UE 引擎使用；不允许时是否只做个人学习分支。

## 13. 参考

- [TressFX 5.0 for UE5 更新说明（UE 5.4）](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/)
- [TressFX5-5.4.4 分支](https://github.com/GPUOpenSoftware/UnrealEngine/tree/TressFX5-5.4.4)
- [TressFX 5.0 Developer Guide（PDF，主要写 UE4.26/4.27）](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf)
- [TressFX 4.1 standalone（MIT）](https://github.com/GPUOpen-Effects/TressFX)
- [Epic UE5.4 Hair Physics Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/hair-physics-in-unreal-engine---overview?application_version=5.4)
- [Epic UE5.4 Enabling Physics Simulation on Grooms](https://dev.epicgames.com/documentation/en-us/unreal-engine/enabling-physics-simulation-on-grooms-in-unreal-engine?application_version=5.4)
- [hair-pipeline-options.md](hair-pipeline-options.md)
- [shading-model-alignment-plan.md](shading-model-alignment-plan.md) §2.4
- [animation-runtime-skinning.md](../../contracts/animation-runtime-skinning.md)
- [shader-hot-reload.md](../../contracts/shader-hot-reload.md)
- [gbuffer-channel-ownership.md](../../contracts/gbuffer-channel-ownership.md)
- [testing.md](../../contracts/testing.md)

## 14. Female/GASP 重定向姿态校准调研（2026-10-22）

本节记录资料、测量与下一步建议，**不是已实现契约**。
当前实现边界见 [动画播放与 GPU 蒙皮](../../contracts/animation-runtime-skinning.md)
和 [Female 原骨架导入](../../contracts/tressfx-asset-import.md)。

### 官方资料

- [Epic UE 5.4 IK Rig Retargeting](https://dev.epicgames.com/documentation/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine?application_version=5.4)
  的 Retarget Pose 节明确要求关注源/目标参考姿态差异，A/T pose 不一致时可以编辑独立
  Retarget Pose；姿态匹配能提高准确度。支持手工旋转、从动画指定帧或 Pose Asset 导入。
- [Epic UE 5.4 Auto Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-retargeting-in-unreal-engine?application_version=5.4)
  的 Auto Align Poses 根据 retarget chains 对齐整体、选中骨或子链，也提供按蒙皮网格对齐。
  自动结果仍需检查肘膝弯曲方向、手腕和末端；不能把方向一致当成扭转角已确定。
- [Epic UE 5.4 Runtime IK Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/runtime-ik-retargeting-in-unreal-engine?application_version=5.4)
  同样使用事先创建的 IK Retargeter，由 Retarget Pose From Mesh 消费运行时源姿态。
  运行时传递动作不会省去校准资产。
- IK Rig 文档把通常位于 pelvis/hip 的 Retarget Root、FK chain 传递和可选 IK 分开描述。
  该 Retarget Root 概念不能直接等同于 VL 播放器固定索引 0 的 locomotion root。

### 本地证据

当前 `source/animation/animationRetarget.cpp` 的 `CompiledRetarget` 直接缓存源/目标
ozz rest pose 的模型空间旋转，并应用
`R_source(t) * inverse(R_source(rest)) * R_target(rest)`。
这支持不同骨轴的参考增量传递，但没有单独作者的 source/target Retarget Pose、
chain 插值或 IK；源 rest 输入会得到目标自身 rest，而不是姿态已校准的目标。

在正式 `SK_UEFN_Mannequin.json` 与 `SK_Female_Outside.json` 中按父链连乘
`restLocal`，用关节模型空间位置之差定义骨段方向，两份数据均为厘米/Y-up。
取 `acos(dot(normalize(sourceSegment), normalize(targetSegment)))`，得到：

| 左侧骨段 | GASP 与 Female 绑定方向夹角 |
| --- | --- |
| upperarm→lowerarm / Shoulder→Elbow | 14.0838° |
| lowerarm→hand / Elbow→Wrist | 18.7573° |
| thigh→calf / Hip→Knee | 3.9590° |
| calf→foot / Knee→Ankle | 2.4920° |

这些是绑定骨段方向差，不是动画重定向误差，也不能直接逐骨取反作为校准量；
父级旋转会改变子链，骨段方向本身还不能唯一确定轴向 twist。
本次正式运行截图完成 bind、Idle、Walk 0.25/0.75 s，Walk ROI 有 53,297 个像素
变化超过 4 byte，C# 请求的是 in-place Walk。这只证明导入/播放/蒙皮链路可用；
证据在本工作树 `artifacts/tressfx5-female-skin-v1/`，不构成姿态质量或足底接触验收。

### 建议实施顺序

1. 为 retarget profile 增加独立、可命名的源/目标校准姿态，明确每骨旋转偏移的空间、
   四元数顺序和父链传播；原始 skin bind/inverse bind 继续专属于网格蒙皮。
2. 先为 Female 对齐 GASP 的肩、上臂、前臂、手腕和腿链，提供校准姿态与骨架叠加对照；
   可从链方向生成 swing 初值，再单独作者 twist，不能只依赖骨名或 A/T 标签。
3. 同一份校准用于 Idle/Walk 运行时采样；验证校准参考姿态对应关系、目标骨长、
   未映射骨继承，以及没有校准配置时的既有行为。
4. 之后分别设计 pelvis 运动、身高比例及足底 IK/接触处理。当前只传索引 0 根位移，
   不传 pelvis 平移；不能把参考姿态校准说成完整解决脚滑或 Motion Matching。

### 实施补记（2026-10-22）

以上“本地证据”为校准前的历史状态。现已加入可选 source/target 参考姿态，
Female 的 14 个偏移与独立只读 Retarget 标签；运行时和查看页共享参考姿态计算。
首轮脚尖方向硬对齐的实景出现高跟鞋上翘，已改为保留 Female 原脚部模型参考朝向；
其余链方向与手掌框架仍对齐，脚型差异继续在查看页显示。
准确当前行为与边界以 [Female 导入契约](../../contracts/tressfx-asset-import.md)
及 [Retarget 标签](../../contracts/ui-component-system.md#retarget-参考姿态标签) 为准。
