# 测试与验证

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

改动前先用 `documents/routing.md` 按源码路径找到必读契约，再用下表找到对应的最小验证。**不要凭记忆选命令**：
下面的命令与测试数量都是从当前构建图与 `--gtest_list_tests` 取出的实测值。

## 验证入口

按成本从低到高。所有命令从仓库根目录执行；构建目录是 `build`（MinGW Makefiles）。

| 命令 | 前提 | 验证内容 | 何时运行 |
| --- | --- | --- | --- |
| `py -3 tool/validate-docs.py` | 无 | 文档分层：路由引用的文档存在、源码 glob 匹配到真实文件、路由里的门禁规则名逐字存在于审计脚本、契约层无未来时断言、全树相对链接有效；另含图源证据、生成物过期与孤儿契约检查 | 改任何 `documents/**` 或 `tool/architecture-boundary-audit.ps1` 后 |
| `py -3 tool/generate_boundary_rule_index.py` | 无 | 从审计脚本重新生成 64 条规则索引（生成物入库，`--check` 只校验不写） | 改 `tool/architecture-boundary-audit.ps1` 的规则后 |
| `py -3 tool/validate_scene_schema.py` | 资源工作树可访问 | 每个真实场景都能被 `schema/scene.schema.json` 接受；**schema 不能比 `SceneAssetValidator` 更严**，否则会拒绝引擎实际接受的场景 | 改 `schema/scene.schema.json`、场景 JSON 或 `sceneAssetValidator.cpp` 后 |
| `pwsh -File tool/architecture-boundary-audit.ps1` | 无 | 65 条命名架构边界规则（禁止词、cutover、所有权、生命周期句柄采用等） | 改 `source/**` 后 |
| `pwsh -File tool/architecture-final-validation.ps1` | 已构建 | 边界审计 + 最终验证矩阵（它调用上面的审计脚本） | 提交前 |
| `cmake --build build --target <target> -j8` | 已 configure | 编译该目标 | 改任何被它编译的源码后 |
| `ctest --test-dir build -R <regex> --output-on-failure` | 已构建对应目标 | 模块测试 | 见下"改动 → 验证" |
| `.\build\bin\main.exe --dev-ui` | 已构建、`config/config.json` 可解析 | 端到端渲染/交互 | UI 或渲染路径改动后（见"手工与像素验证"） |

## 测试目标实测规模

| 可执行文件 | 测试数 | 覆盖 |
| --- | --- | --- |
| `editor_ui_tests` | 723（714 后加 7 项 Performance 页、1 项标签接线与 1 项旧布局迁移） | 编辑器 UI：组件、停靠、布局持久化、内容浏览器、JSON 资产库、标准资产修改、actor 树、属性编辑、场景持久化、保存确认、可视化、颜色转换、右键菜单、RmlUi GPU 缩略图桥接、C# 放置/蓝图资产拖放、组件 Details、独立蓝图配置标签、3D 拾取/变换/辅助图示/吸附、文档历史、类型化属性与实例权限、物理配置、编译入口、逻辑组件、Quaternion 原子编辑、Time Scale、Log 页签、Performance 页（冻结快照、5 Hz 节流、空状态、图序表格）、四处递归自定义数据编辑、冻结 HUD 文本绑定、C# 类浏览/源码打开请求、一类多 BP 配置创建及骨骼预览值请求/资产校验；公共 DrawDebug 投影/裁剪/叠层顺序、可选深度、转义文字、过滤菜单；Motion 值请求/布局、特征表和 MM 冻结候选/代价刷新、被聚焦控制器时间轴 seek/play/pause 与离线/无播放器禁用、查询回放历史帧选择/回显/禁用；SpringArm 字段刷新、默认值与编辑提交、组件/末端挂接空间、图示/拾取/gizmo 一致性、多层 Mesh section 空间与重挂父级；TressFX 类型识别、标准保存保留半径与场景引用；retarget source/profile 保存保留与引用/源类型拒绝；参考姿态校准、ozz 索引、SK 身份/根路径边界、冻结发布/失败清空、独立页签/轴向/选骨/取景/辅助骨和多尺寸布局；成对动画身份发布、控制/seek、模式切换、重载隔离和首次取景区域；共用 DocumentTab 稳定 key/独立关闭、临时最大化/恢复、跨类型关闭与 MI 异步确认 |
| `script_tests` | 当前注册 448（当前构建 XML 实测；含 2 项成对原地动画预览；SpringArm 运行时共 40 项；C# ON；注册数不等于实跑数；本轮范围见 Retarget 动画页验证。OFF 先前实测注册 89 项） | 蓝图场景/资产/组件校验、实例权限、地面放置、输入映射、源码 URI 边界；ON 时使用真实 hostfxr/CLR 验证生命周期、Spawn、运行时组件/代际句柄、异步加载/取消/租约、Jolt 层与掩码/compound/静态 mesh/约束/扫球、角色控制、计时器/事件、故障隔离、World 换代、冻结快照、类型化属性、灯光、程序集重载、资源源码编译/监视及 DLL/PDB 完整字节确定性、PDB 空类/partial 源码定位、私有运行时部署、自定义逻辑组件、Tick/LateTick 每帧时序/时钟/性能采样、递归自定义数据、显式播放中深值迁移、增量同步/空回调过滤/千实例负载、HUD 所有权/回滚/迁移、受控程序集限定类名及混合 CLR/Jolt/WorldSnapshot 负载；SpringArm schema/编辑保存/实例覆写、同帧物理/晚更新、旋转继承、位置/旋转 lag 与子步、碰撞/移动墙体、诊断/传送复位、重载与回滚、非相机子级/子局部偏移与嵌套碰撞空间、CLR/原生含 shear 矩阵一致性；动画请求/原生反馈、独立时钟、状态机及实际运动/碰墙回归、Actor 有界转向与世界关节/相机连续性、正式控制器稳态步频开关与原根行程对照及曲线筛选踝漂移诊断；静态接地探测过滤、18° 横坡参考点/法线独立测量与 pre-IK/MM 隔离；走跑双参考点动态支撑、台阶边缘单支点、惯性化与视觉根偏移、冻结源支撑窗滑移/漂移/穿入质量夹具；足部 Auto/Ball/Ankle 支点模式与 C# `FootLock`；Motion 调试历史回放（帧选择、不追加、退出恢复）；被聚焦脚本控制器时间轴的 seek/play/pause 身份隔离、代数/片长/动作校验；历史 Pause/Stop、实际碰撞轨迹与近似预测；DrawDebug 十形状、生命周期/发布者清理、批次回滚和深度参数跨桥；成对预览 clip/时钟、原地根变换、section 去重和场景门控 |
| `debug_draw_tests` | 27 | 十形状展开、生命周期、冻结、身份/过滤/预算、默认 Overlay 与可选深度传播、投影近远面及屏幕裁剪深度、Console 控制 |
| `hair_asset_tests` | 139 | `.tfx` 字节/属性/预算校验、文件读取、坐标与逆质量保留、静态切线/rest length/ribbon 拓扑、SM 半径/身份与导入流、simulation CPU/GPU backend 解析、变量步长（`maxDt` clamp、一帧一步）、`bendStiffness`/阵风/global shape/VSP 作者字段与 224-byte 参数打包、已删除 `fixedStep`/`maxSubsteps` 的拒绝、`simulation.skinning` 作者校验（source/rootBone/backend/未知键/缓存身份）、根骨解析（命中/缺骨/缺骨架，错误带骨名+资产路径）、不可移动点 `M*rest` 纯数学、五个 pass 的纯数学（距离约束 4 种 multiplier、风力方向与单位归一化、指数阻尼、global shape range 门控与拉回、VSP 端点/线性 stiffness、strand 变换与伪加速度阈值、直/弯 rest 的 local-shape 旋转、速度 clamp）、胶囊投影/固定点/长步数接触与 collision 校验；`collision.mesh` 解析（path/cellSize/margin 范围、未知键、backend 限制、缓存身份、OBJ 顶点/多边形/负索引/行号错误、缺失代理）；有符号 SDF 网格 CPU 镜像（magnitude-first FloatFlip 编码、每帧变形域/退化 extent 钳制/固定 48..64 分辨率、clear+splat、三线性采样、中心差分梯度、退化梯度跳过、2 cm 内部点被推到表面外、0.5 cm 外部点只在 margin 内移动、盒/球/远点、蒙皮矩阵带动场）；离线 SDF 生成（球/盒解析距离、角度加权伪法线、锐尖顶点、开放边报告、字节确定性、1 vs 4 线程并行字节一致、文件往返与截断/损坏拒绝）；离线碰撞代理（规则立方体聚类与绕序、退化/重复三角形剔除、区域包含规则、输出字节确定性、开盒开放边、最小三角形下限）；合成夹具，模块套件不含真实女性资产或 GPU dispatch（GPU 路径由 main 启动烟测覆盖） |
| `vl_offline_adapter_tests` | 101 | 离线场景/网格/显示/相机适配 |
| `editor_viewport_tests` | 41 | 视口几何、命中、坐标映射、aspect、坐标轴投影及编辑器网格 |
| `vl_offline_coordinator_tests` | 13 | 离线会话协调 |
| `vl_renderer_backend_tests` | 63（51 后加 12 项 Performance 快照/JSON） | 离屏显示输出、GPU 缩略图资源、swapchain 帧结果、同 World 新增绘制对象的资源发布契约，以及逐 pass GPU 计时的纯逻辑（tick→毫秒、`end < begin`、窗口平均/淘汰、名字顺序、表格字符串）；CPU+GPU 共享快照合并、部分窗口、缺失/负值样本、`valid`、副本独立、百分比、JSON 固定键序/3 位小数/`null`/拒绝 NaN |
| `engine_launch_options_tests` | 6（4 后加 2 项 `--perf-jsonl`） | `--gpu-stats` 默认关闭/可接受/与既有参数组合；`--perf-jsonl` 默认关闭、路径必填、与 `--gpu-stats` 组合；未知参数仍被拒绝 |
| `material_instance_editor_landed_tests` | 10 | MI 已落地接口 |
| `material_instance_editor_tests` | 7 | MI 命令/文档服务 |
| `material_instance_editor_production_tests` | 11 | MI 生产路径（预览边界、直接 render-state 路由、场景/非活动 MI 独立保存）及标准资产 JSON facade/Save Content、C# 草稿连续保存、独立蓝图草稿/保存反馈和历史集成 |
| `game_export_tests` | 19 | Scene/BP/SM/MI/T 依赖闭包、动态资产清单、模型 sidecar、包内配置、PE 原生依赖、私有运行时、源路径约束与失败保留 |
| `animation_preflight_tests` | 291（285 后加 6 项 skin bounds：逐关节预计算比对、快/精确路径包含性、跨关节混合、空关节/空 section 与退化 palette；动画工具 ON） | SK/Sequence/Montage 三层资产与播放：SK 拓扑/槽位/曲线注册表、`skinning.root` 拒绝与强制 `skeleton`（`AnimationSkeleton` 17）、sidecar 帧信息/曲线/notify/采样（`AnimationSequenceData` 14）、Montage 解析/跨文件绑定/缓存键/`rotationMethod`（`AnimationMontage` 24）、catalog/窗口装配/`PlayMontage` 拒绝矩阵/旋转透传/Static pose/section link/入口 startTime（`AnimationScriptData` 30）、notify/notifyState 区间执行/feedback（`NotifyPlayback` 11）、旋转与 Static warp point（`MotionWarping` 28）、完整权重导入 30；ozz 采样、只读 UE 盘点、FBX 审计、动态/保守 bounds 与导入期逐关节 AABB 比对、播放/暂停/seek/循环、原地根 TRS 消除、一次性完成、无效请求保持、独立时钟、冻结姿态及实际渲染提交历史；Motion 特征空间/速度/SE(3) 循环/完整窗口、总体统计、输入身份/失效、固定频率历史/epoch/重置与预测；精确搜索、继续/重选策略、运行时关节速度搬运、局部姿态交叉混合、原根周期均速与有界步频、按实际速率采样的继续候选；两骨 IK、ball/heel 双参考点、静态锚点/坡面法线、走跑滚动支点/释放、Auto/Ball/Ankle 显式支点与重置、摆动脚骨盆间隙、pre-IK/混合源/旧快照隔离；有限时惯性化 C1/端点/重复中断、有界视觉根偏移与查询隔离；显式 retarget 映射/拓扑、不同骨长/rest axes、target palette/混合、source Montage/catalog 与不兼容 source/profile/MM 拒绝；参考姿态偏移解析/拒绝、源参考旋转差及未映射骨继承 |

`ctest` 用套件前缀选择：

```powershell
# 编辑器 UI 选择集（缩略图、预览等另有专项套件；全量可直接运行 editor_ui_tests）
# 注意 EditorViewportGridGeometry 与 EditorViewportAxisIndicator 来自 editor_viewport_tests 目标，
# 不是 editor_ui_tests：
# 它必须显式列入，否则改坏编辑器网格不会被这条选择集拦住（2026-10-12 发现并补上）。
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|EditorActorProperty|EditorActorVisualization|SceneDocumentTest|AssetDocumentTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorPerformancePanelTest|EditorViewportGeometry|EditorViewportGridGeometry|EditorViewportAxisIndicator|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|EditorRetargetPreview|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure

# 材质实例编辑器全套（当前 28 项）
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

`JsonAssetLibraryTest` 有 4 项按设计 skip（符号链接用例，需要权限）。

### Performance 页与机器通道验证（2026-10-23）

隔离工作树 `codex/tressfx5-hair-port` 中 Debug MinGW `build` 与 Release `build-rel` 的 `main`
构建通过；`editor_ui_tests` 注册 723 项、`vl_renderer_backend_tests` 注册 63 项、
`engine_launch_options_tests` 注册 6 项。

Debug（validation 开启）以 `--initial-scene
Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json --gpu-stats --frame-limit 60
--console-command "perf json" --console-command-delay 120` 运行：stderr 为空、0 个 VUID；
`artifacts/perf-json-debug/performance.png` 显示已选中的 Performance 页，且同一快照的三项摘要
与 `perf json` 完全一致：FPS `56.559`、CPU `17.681 ms`、GPU total `14.290 ms`。
该行 JSON 为
`{"fps":56.559,"cpuMs":17.681,"gpuTotalMs":14.290,"valid":true,"passes":[{"name":"hairSimulation","ms":1.258},...]}`
（20 个 pass，完整行见 gitignored 的 `artifacts/perf-json-debug/stdout.log`）。

Release 的 `artifacts/perf-json-release/performance.png` 同样显示已填充的 Performance 页；
命令输出 `{"fps":60.027,"cpuMs":16.659,"gpuTotalMs":14.365,"valid":true,...}`。
截图面板显示 `72.443` / `13.804` / `14.425 ms`，这是 5 Hz 面板刷新取到的相邻快照，
不是两套数值规则；同一快照由 Debug 截图与 JSON 行逐字对上。

JSONL：`--perf-jsonl artifacts/perf-jsonl-auto/sub/run.jsonl` 运行约 12 秒，得到 12 行，
每行 `ConvertFrom-Json` 解析成功，父目录自动创建。首行在 GPU 首次读回之前写出，因此是
有效空状态：`{"fps":60.000,"cpuMs":16.667,"gpuTotalMs":null,"valid":false,"passes":[]}`；
后续行包含完整 pass 表。两次采集进程都由外部工具强制终止，不作为正常 teardown 证据；
终止后没有遗留 `main` 进程。


### Retarget 参考姿态页验证

2026-10-22，本地隔离 `codex/tressfx5-hair-port` 工作树中，Debug MinGW 的
`main`、`editor_ui_tests` 与 `animation_preflight_tests` 构建通过。
UI 全量 701 项：696 通过、5 项符号链接权限环境跳过、零失败；
动画全量 285 项全部通过。Retarget worker 的根路径约束、缺失元数据和冻结发布/
失败清空 3 项连续两遍通过；960×540、1280×720、1920×1080 布局由 RmlUi 模块检查。
证据为 `artifacts/retarget-tab-ui-final-tests.xml`、
`artifacts/retarget-tab-animation-final-tests.xml`。

真实窗口打开正式 `RT_Female_From_GASP`，检查 24 对映射、91/105 个关节、
0/14 个参考姿态偏移；实际点击 Bind/Calibrated、Front/Side/Top、手腕选择/
取景、Fit 和 All bones。截图为 `artifacts/retarget-tab/overview-final.png` 与
`wrist-final.png`；关闭窗口后日志记录 Quit 和 teardown complete，stderr 为空。
这只验证参考姿态检查，不代表姿态编辑、动画叠加或足部接触已经完成。

正式 Female 高跟鞋策略的 Bind/Idle/两帧 Walk 截图位于
`artifacts/tressfx5-female-footwear-v1/`，两帧 Walk 角色 ROI 中差值大于 4 的
像素数为 61846，config 原始字节已恢复。截图进程由采集工具终止，不充当正常退出证据；
旧 `tressfx5-female-calibrated-v1` 强制脚尖对齐结果已否决，不作为当前效果。
正式 RT 通过 `validate_scene_schema.check` 的 draft-07 子集校验；
模块解析测试另外约束四元数、骨骼身份和映射语义。

### Retarget 动画页验证

2026-10-22，隔离工作树中 Debug MinGW 的 `main`、`editor_ui_tests` 和
`script_tests` 构建通过。UI 全量 708 项：703 通过、5 项符号链接权限跳过；
动画全量 285 项通过。`RetargetPreviewPlayback` 两项通过，验证共享 clip/时间控制、
原地根变换、section 去重及非配对场景拒绝；连同三项既有 World 动画批处理测试共 5 项通过，
未全跑当前 448 项脚本测试。
证据位于 `artifacts/retarget-animation/` 的 UI、animation 和 playback XML。
首次取景区域修正后，5 项 Retarget 动画 UI 专项再次通过。

真实窗口已打开正式 RT，显示两套实际蒙皮、Idle 和 Walk 片段以及普通取景；
日志确认 source/target 同时收到 Walk 请求，随后正常 Quit/teardown，stderr 为空。
本轮未完成实窗逐项暂停、拖时间轴及环绕验收，这些操作的请求/状态通过模块测试覆盖。
此记录不作为高跟鞋接地、脚滑或 IK 效果验收。

性能预览复核（2026-10-22）：独立 `build-vscode/development` 的
RelWithDebInfo/C# ON 构建通过，主程序和动画库均为 `-O2 -g -DNDEBUG`，
Vulkan validation 关闭。1280×720、双线程、60 Hz 上限的真实窗口中，
Female 单模型待机约 38–39 FPS，RT 双模型 Idle/Walk 约 35–37 FPS；
暂停稳定后约 58–59 FPS。数值来自窗口 FPS，不是 GPU 时间或完整性能基准，
切换构建同时改变 CPU 优化、校验与 Shader 策略，不能据此单独归因。
实窗检查切片段、暂停和继续，窗口保留播放 Walk；构建与运行日志位于
`artifacts/retarget-animation/development-*.log`，stderr 为空，config 字节未变。

### GASP P2 蒙皮验证

本地 2026-10-18，C# ON / 动画工具 ON / MinGW Debug 全目标构建通过。
根 CTest 共 1360 项，1354 通过、6 项符号链接权限环境跳过，零失败，耗时 449.53 秒。
动画/资产/UI 相关 101 项选择集连续两遍零失败；最后的标量 bounds 优化后，
`SkeletalImport` 与 `SkinRenderHistory` 共 24 项再次连续两遍通过。
实测目标规模为 UI 648、动画 42；下方 C# 专项段落保留各轮历史数量。

正式 `Maps/SC_gasp` 的 Vulkan CPU/GPU Beauty 与 Normal 对照、单/双线程及
确定性 seek 对照在角色/阴影 ROI 内字节误差均为 0。
暂停几何的非零速度像素为 0，播放几何为 5610。
Surface/Shadow 与 Compute reload 在正式 GASP 场景串行通过；
World/graph 事务在默认场景、无编辑器模式下分别以单/双线程串行通过。
四个进程退出码均为 0，日志确认正常 teardown；
Compute stderr 含测试故意改变 workgroup size 的 ABI 拒绝，其后恢复通过。
39 个正式场景 schema、65 条边界规则和文档校验通过。

编辑器实窗截图已核对；时间轴、片段选择与播放控件有 RmlUi 模块覆盖，
未作真实鼠标操作验收。截图进程由工具终止，不充当正常退出证据。
完整数据、命令、失败修正与范围见
[P2 执行记录](../rationale/gasp-p2-skinning-record.md)。

### GASP P4 轨迹与特征验证

本地 2026-10-18，动画完整 61 项、脚本完整 316 项均连续两遍通过。
UI 完整 653 项连续两遍均为 648 通过、5 项符号链接环境跳过，零失败。
视口/轨迹投影选择集 43 项通过。World/Graph 事务 runtime gate 退出码 0，
含正常 teardown；单/双线程轨迹截图验证通过，但截图进程的强制终止不计 teardown。
Debug 全目标、Development main 已构建；其余构建结果见
[P4 执行记录](../rationale/gasp-p4-trajectory-and-features-record.md)。

`MotionHistory`、`MotionPrediction`、`MotionFeatures`、`MotionDatabase`、
`MotionDatabaseInputs` 和 `SkeletalImport` 覆盖采样与构建；
`ScriptAnimationTest` 覆盖真实 CLR/Jolt 实际历史和暂停/Stop；
`DebugDrawOverlay`、`ModelPreviewInputTest` 和 `EditorUiTest.Motion*`
覆盖冻结显示、裁剪、叠层顺序、值请求、静默刷新与多窗口尺寸。
正式数据库冷重建/命中/损坏恢复/版本拒绝/schema/成员顺序失效由外部工具验证，
1261 帧、42 维；帧浏览器有 Playwright 实际片段/帧输入和桌面/手机布局验收。
原生编辑器没有新增真实鼠标验收；没有运行时 MM 搜索/混合/IK 完成的声明。

### GASP P4.5 公共 DrawDebug 验证

2026-10-18：纯 CPU 27 项、C# ON 脚本 332 项均连续两遍通过；
C# OFF 脚本 77 项与纯 CPU 27 项连续两遍通过。
UI 全量 659 项连续两遍均为 654 通过、5 项符号链接权限环境跳过，零失败。
可选深度覆盖逐像素线段遮挡、前后点和标签，Debug Vulkan validation 截图 stderr 为空，
单/双线程 ROI 字节相同；Development 过滤/清理和深度演示截图通过。
三项既有 Vulkan runtime gate 串行退出码 0，均记录正常 teardown；
截图进程的强制终止单独记账，不作为正常关闭或真实鼠标验收。
具体构建、临时资源与 shader 重载范围见
[P4.5 执行记录](../rationale/gasp-p4.5-debug-draw-record.md)。

### GASP P5 验证范围

2026-10-18 物理反馈修正版冻结后，Debug 全目标、Development main 与 C# OFF Release
main/script_tests 构建通过。动画 112 项/14 套件、C# ON 349 项/34 套件、
C# OFF 79 项均连续两遍全部通过；完整 UI 666 项连续两遍均 661 通过、
5 项符号链接环境跳过、零失败。7 项 MM UI 专项包括真实指针展开、转义及受阻标记冻结。
查询手算回归包括固定实际物理速度下零/双倍渲染位移；
真实 Jolt 覆盖无子步纯 yaw 保速及位置传送清速。
正式控制器动作/墙前共六例覆盖 30/60/120 Hz，要求各阶段末一秒稳定循环或 Idle，
并保留起停/转身/跳落的实际原生选帧断言。
Vulkan 生命周期与截图的版本/范围分别登记于
[P5 记录](../rationale/gasp-p5-motion-matching-record.md)；本轮未完整重跑根 CTest。
最后修正版反向/受阻的单/双线程 4 项实窗截图与目标反馈均通过并经主线程看图核对；
World/Graph gate 退出码 0、stderr 为空并正常 teardown。
文档、65 条边界、40 场景 schema 与 diff 空白检查通过；截图强制终止不充当正常关闭证据。

`MotionMatching` 用独立手算和全扫描 oracle 验证加权距离、稳定决胜、有界排名、
业务筛选、准确/量化继续、动作锁及重选/节流策略；
`MotionRuntimeQuery` 核对显示姿态局部速度在新 heading 和实际根速度下的搬运。
同套件还区分地面当前受阻的实际水平速度预测、自由移动 desired 和空中 desired，
不把其通过解释为未来碰撞预测完成。
`SkeletalBlend` 验证 ozz 局部 TRS 混合、旋转归一化、混合打断、时钟/完成状态、
inPlace 与冻结姿态所有权。模块通过不等同于正式控制器的动作效果通过。

`ScriptAnimationWorld` 验证导入字节身份不匹配和批次失败原子性；
真实 CLR/Jolt 的 `ScriptAnimationTest` 验证正式统计绑定、冻结查询/排名、
空筛选、回调回滚、传送、锁定非循环尾段、历史预算优先级及独立暂停。
`FormalGaspControllerPublishesNativeSelectionsForGroundedAndAirborneActions`
实际执行资源侧 `Gasp.CharacterBlueprint` 并记录原生选帧，区别于旧的
`LegacyLocomotion*` 直接选片段夹具；动作类别曾出现不证明长时间稳态或碰墙策略。

`EditorUiTest.MotionMatching*` 验证同代查询/代价、Selected/Continue/前五名、
文字转义、固定槽位、展开状态的暂停静默、隐藏条件、短视口滚动/输入隔离和单行摘要；
真实 RmlUi 鼠标移动/按下/抬起覆盖实际 CSS hover、查询展开、42 维末行可读和固定数值节点；
冻结 `blocked` 标志及 actual/desired-no-collision/query 图例与候选排名同代。
`tool/animation/capture_gasp_p5.py` 运行独立 C# 输入夹具，通过生产
`Animation.Match`、Character/Jolt 和 Console 截图采集八类动作、单/双线程与多尺寸。
其报告分别记录截图完成、动作类别验收和截图时刻的原生反馈；只属于允许集合不算动作通过。
该脚本不注册为 CTest 或 runtime test，不执行正式控制器，也不替代真实鼠标/键盘验收；
强制终止的截图进程不计正常 teardown。
实际构建、最终重跑、失败修正与未覆盖项见
[P5 执行记录](../rationale/gasp-p5-motion-matching-record.md)。

### GASP P6 有界转向专项

2026-10-18：资源侧正式 C# 控制器新增有界最短角转向，默认 `540°/s`。
Debug 首轮 34 项运行 `405.102 s`，31 通过、3 项新 Stop 夹具失败；
Stop 按契约清空 runtime states，夹具误继续读取 owner，修正仅涉及测试。
最终 Debug 重编后复跑受影响的相机六项与 Stop 三项，九项全部通过，
耗时 `60.399 s`，退出 0；其余 25 项沿用首轮相同业务源码的结果。
因此本次合并覆盖 34 个不同用例，没有声称最终完整 34 项又跑一遍或重复两遍。
`--gtest_list_tests` 实际确认 C# ON 当前注册 377 项，本轮没有重跑全量 377、
UI 全量、动画 CPU 全量、C# OFF 或三项 Vulkan runtime gate。

新增 28 项包含 30/60/120 Hz 下 D 输入 `-90°`、S 输入固定正向 `180°`、
真实 Actor/World 模型角度界、冻结姿态 pelvis/双脚世界连续性、真实 render camera
forward 独立、打断、`179° → -179°` 短弧、Pause、TimeScale=0、
Stop/Play、传送和 TurnSpeed=0 的初始化拒绝；原有正式动作/墙前六项断言保留且通过。
未分别穷举左右 `±90°`、正负 `180°` 及反向跨界所有组合。
Development 正式 playground 启动、UI Play/Immersive 出图通过；
未形成可确认持续键输入的动态转向视觉验收，该项保留给用户实操。
具体日志、角步/到达时间与保留限制见
[P6 有界转向记录](../rationale/gasp-p6-bounded-turn-record.md)。

### GASP P6 稳态步频专项

2026-10-18：新增四项动画 CPU 用例与一项正式控制器行程对照，
扩展一项既有批请求回滚用例，没有新增测试目标、runtime 标志或 FPS 矩阵。
Debug 动画定向 16 项通过；脚本桥接/时钟/暂停/Stop/传送选择集七项连续两遍通过。
正式控制器行程对照与既有动作、受阻 `/60` 三项通过，合计耗时 `129.214 s`；
其中行程对照四模式各八秒、固定排除前三秒，占 `66.714 s`。
没有重跑完整 378 项脚本、UI 全量或三项 Vulkan runtime gate。
Debug 脚本/动画、Development main、C# OFF Release main/script_tests 构建通过；
OFF 只复跑受影响批请求用例两遍，均通过，不把既有 79 项称为本轮实跑。

按播放器实际时钟独立采样原根、补周期位移并与 Jolt Actor 行程比较：
Run 误差由 `49.9957` 降至 `0.000716 cm/s`，平均倍率 `0.900008`；
Walk 关闭/开启误差分别 `0.000425/0.000573 cm/s`。
四组源相位覆盖 100%，实际对照使用正式 C# 控制器及宽候选集合。
这只验证根行程匹配，未验证足底锁定。
首轮曲线筛选踝漂移质量测试失败的 XML/log 保留：
Run `36.33→33.51 cm/s`，未达到原改善线且有效区间偏少。
复核源蓝图的曲线重映射、FootPlacement 和 Leg IK 后，
该项改为踝部诊断，正式 `MatchSpeed` 默认关闭；未将失败解释为消滑通过。

Development 双线程 Run 开/关有实景截图，关闭组补采了稳态图并经主线程目视，
两组实际 Selected 均为 Run Loop、blend=1。反馈与 RT 图不保证同帧，
截图进程强制结束不计正常 teardown。config 已恢复，无自有进程残留。
65 条边界审计、文档与两仓 diff 检查通过。
具体日志、初始失败和下一接地分项边界见
[P6 步频记录](../rationale/gasp-p6-speed-matching-record.md)。

### GASP P6 静态接地专项

新增四项 `SkeletalBlend`、一项同步探测过滤和一项正式坡面对照，
其余复用最近的生命周期、动作与受阻用例，没有新增目标或 runtime 标志。
最终动画 CPU 16 项连续两遍、脚本生命周期选择集七项连续两遍通过；
真实 60 Hz 18° 横坡和动作、撞墙 `/60` 合计三项通过。
没有扩 30/120 Hz，也没有重跑全部当时注册的 380 项脚本、UI 集或三项 Vulkan runtime gate。

横坡开关各采集 240 个稳态双足样本，不按 planted 筛样本；
最大参考点误差为 `7.193770→0.272459 cm`，
平均误差为 `6.331438→0.233384 cm`，法线误差达到 5° 目标。
Actor、MM 反馈/查询、片段时钟逐帧一致，全部 pre-IK 骨矩阵差为零。
验收对象为绑定 ball 的单参考点，不是鞋底网格或走跑全周期消滑。
首次混编失败、重锚峰值失败及修复记录保留，见
[P6 足部接地记录](../rationale/gasp-p6-foot-placement-record.md)。

Development 双线程同坡面开关实景各一项完成并经主线程目视。
反馈与 RT 图片不保证精确同帧，采集使用强制结束，不作为正常 teardown 证据。
正式场景说明通过单一资产 CLI host 的五步生命周期保存，
独立深比较仅 description 改变，40 个场景 schema 检查通过。
Debug 脚本/动画、Development main、C# OFF Release main/script_tests 最终构建通过；
OFF 仅复跑受影响候选批次用例两遍，均通过，未重跑全量。
65 条架构边界审计通过，采集后 config 恢复，自有进程已结束。

### GASP P6 动态质量专项

新增一项正式 C#/Jolt/MM 动态质量夹具和八项动画模块用例
（三项惯性化、两项视觉根偏移、三项动态足部），
其余复用最近的生命周期、坡面与动作用例，没有新增 runtime 标志或 FPS 矩阵。
质量夹具固定 60 Hz、MatchSpeed 两组相同，走/跑各 10 s、六段台阶 6 s；
源支撑标签在读取输出前冻结，不按 solver planted 筛样本，阈值保持
50% 路径降幅、3 cm 漂移、1 cm 代理点穿入、95% 覆盖。

最终动画 `animation_preflight_tests` 129 项（含 Auto/Ball/Ankle 支点用例）连续两遍全部通过；
动态质量夹具连续两遍各 1/1 通过（`artifacts/gasp-p6-dynamic-quality-07.xml/-08.xml`）；
生命周期/坡面选择集 8 项连续两遍通过；C# OFF Release 79 项连续两遍通过。
C# ON Development 正式关卡完成关闭、足部开启、组合开启和 Stop/Blocked 四组截图，
Stop 取到根偏移 `smoothing`（-2.334 cm），Blocked 取到足部 `planted`（骨盆 -0.752 cm）。

Walk/Run 每足路径降幅 67.6%–85.6%，漂移 ≤0.808 cm，代理点穿入 ≤0.265 cm，
覆盖 1.00；台阶六段全部跨越，单参考点 25/42 帧、间隙 ≤0.292 cm。
真实鞋底网格最大穿入为 Walk 5.522 cm、Run 4.498 cm、Stairs 9.037 cm，
不计入通过项；完整鞋底、台阶整周期和移动平台仍未验收。
首轮 Run 零支撑窗、支撑窗拆碎、边界样本与跨踏面抬脚失败保留，见
[P6 动态质量记录](../rationale/gasp-p6-dynamic-quality-record.md)。

### GASP P7 时间轴、查询回放与独立包探针

新增两项编辑器 UI 用例和两项 World 定向用例，复用 Motion 面板、Pause 与
生命周期选择集；没有新增 runtime test 标志或 FPS 矩阵。
UI 用例覆盖时长/时间绑定、播放态禁用、seek 的网格身份/World 代数/秒值、
pause/play、离线/无播放器禁用，以及查询回放的复选框/历史帧选择/回显/禁用；
World 用例覆盖只改目标播放器、错误身份/旧代数/越界时间/非法动作拒绝，
以及历史回放不追加、退出恢复实时快照。

`editor_ui_tests` 668 项（Motion 11 项）连续两遍：663 通过、5 项符号链接权限跳过、
零失败；`script_tests` 384 项一轮通过，时间轴/回放定向 2 项两遍通过；
动画 129 项两遍、C# OFF 79 项通过。
独立包 `artifacts/gasp-p7-export-dev-01` 从仓库外 cwd 启动，World 提交成功；
空闲 clip=0，按 W 切到 clip=5 的 WalkLoop 并推进，松开回 clip=0，窗口 93 FPS；
`CloseMainWindow` 后正常 teardown，stderr 为空。
固定动作矩阵探针包 `artifacts/gasp-p7-probe-export-01` 从仓库外启动，
连续记录 Idle→Walk→Run→JumpStart→Fall→Land→Idle、yaw=180、
位置 z≈-1304、跳跃 grounded=False、落地 grounded=True、`P7_PROBE_FINISHED`；
探针场景 451 FPS，正常 teardown，stderr 为空。
正式 MM 探针包 `artifacts/gasp-p7-mm-probe-export-01` 记录正式状态机
Walk→Run→Jump→Fall→Land→180°→Idle、z≈-1000、yaw=180、459 FPS，
正常 teardown，stderr 为空；42 个场景 schema 检查通过。
正式 playground 包真实键鼠冒烟：空闲 clip=0、W clip=6、W+Shift clip=7、
Space clip=9、松开回 clip=0；无测试负载采样 97–117 FPS；
`ScriptAssemblyReloadTest.*`/`ScriptAssetReloadTest.*` 29 项与 World replacement
6 项通过。P7 时间轴/查询回放/独立包/生命周期技术验收分项完成；
历史姿态重演、多人回放、逐 pass 性能分项保留为后续增强，详见
[P7 时间轴记录](../rationale/gasp-p7-timeline-record.md)。

### GASP P8 障碍交互专项

新增场景夹具而非新的 runtime test 标志：1 m Mantle/Vault stand/walk/run、
1.6 m 高度拒绝、0.5 m 高度拒绝、顶棚净空拒绝、后方无地面的 airborne abort，
以及关闭 probe 的 F 输入场景。`Gasp.CharacterBlueprint` 用 SphereCastLocal
探测、头高向下找顶、70–130 cm 高度窗口和上方 180 cm 净空射线；动作期间
Jolt 跳跃承担竖直位移、水平前向速度越沿，播放 in-place 1 m 片段。

日志证据：`artifacts/gasp-p8-traversal-probe8.log`（Mantle 落到 y=190、
grounded=True）、`gasp-p8-traversal-vault2.log`（Vault 落到 y=90、z≈-290）、
`gasp-p8-traversal-abort-void2.log`（airborne abort）、
`gasp-p8-traversal-reject-high2.log`（height=160）、
`gasp-p8-traversal-reject-low2.log`（height=50）、
`gasp-p8-traversal-reject-clearance2.log`（clearance）、
`gasp-p8-traversal-reject-nohit.log`（no-hit）。
61 个场景 schema 通过。F 正式键鼠路径未取得稳定焦点证据；med/high 动作、
根运动逐帧消费、独立包和 P9 回归未做；P8 的 1 m stand/walk/run 固定子集技术验收完成，全阶段未完成。

### C# 类浏览与配置资产验证

本轮 C# ON Debug 的 main、script_tests、editor_ui_tests 构建通过。
真实 CLR 脚本集 301 项全部通过；UI 集 643 项中 638 通过、
5 项符号链接权限环境跳过，零失败。C# OFF Release 的 script_tests 构建及 75 项全通过。
类浏览覆盖筛选/搜索、更新移除、空闲不重建、不能拖放，
双击只发源码打开请求、右键创建选择正确类，以及菜单打开后类已移除的拒绝。
创建覆盖一类多资产、稀疏配置、无场景/编译器依赖、原有文件保留与内容域限制。
源码定位覆盖真实编译的空 partial 类、文件名与类名不同、URI 转义、缺失源码和目录边界。

文档分层校验、65 条架构边界审计及 diff 空白检查通过。
本轮没有操作桌面 VS Code 或 Vulkan 窗口，没有重跑根 CTest/三项 Vulkan runtime gate；
源码打开的 OS 关联与实际窗口启动未作手工验收。
桥接输出由构建和 CLR 共用，运行脚本集与重建桥接须串行，避免 Windows DLL 文件锁重试。
证据位于 gitignored 的 `artifacts/csharp-class-authoring-*`。

### C# 蓝图提交前复核

资源化提交时 C# ON Debug 全目标构建通过；根 CTest 1306 项中 1300 通过、
6 项符号链接权限环境跳过，零失败，总耗时 289.16 秒。
文档分层、65 条架构边界和 38 个真实场景的 schema 校验均通过。
配套用户源码与示例资产位于资源仓库 `VukanLearnResources` 提交 `3a865d3`。

提交前总脚本在构建完成后停留于进程等待，终止该次脚本后逐项直接执行剩余入口。
Shader 手动重载与 Compute 重载串行通过；Compute 的 stderr 包含故意改变
workgroup size 所触发的 ABI 拒绝诊断，最终恢复和 teardown 成功。
World/graph 事务在 `--no-dev-ui` 模式通过。
启用开发 UI 的同项检查退出码为 2：high-light 生成场景的
`../VukanLearnResources/Generated/Validation/...` 路径被草稿源路径校验拒绝，
报告 `Scene.DraftSourceChanged: Asset path must not contain traversal`。
该 UI 模式失败仍未修复，不能将其计为通过，也不由无 UI 模式的通过覆盖。
原始日志位于 gitignored 的
`artifacts/architecture-validation/20261018-022215/`。

### C# 用户源码资源化验证

2026-10-18：引擎保留常驻 Runtime/API 与项目模板，现有用户示例迁到资源仓库。
初始构建的 `vl_script_sources` 和编辑器编译共用源码收集器及显式 MSBuild 清单，
覆盖 Common/Maps 的 `.cs`（扩展名大小写不敏感）、排除生成目录、字面路径转义。
真实 CLR 验证资源侧 Blueprint 与普通自定义数据一起编译、新建模板进入类目录、
空用户项目正常启动、带空格/括号的资源路径、增加/改名/删除监视，以及编译期间改源拒绝发布。
既有候选 DLL/PDB 完整字节确定性用例仍通过。

C# ON Debug 的 main/script_tests/editor_ui_tests 构建通过；
脚本全套按 ScriptBuildTest 17 项和其余 282 项两组运行，299 项全部通过。
UI 全套 638 项中 633 通过、5 项符号链接权限环境跳过，零失败。
新增创建回归验证没有已编译类或场景实例时的值请求、GT 创建、路径/标识符/已注册类型拒绝、
已有文件保留，以及排除目录的混合大小写拒绝。
C# OFF Release 的 main/script_tests 构建通过，73 项全部通过。
本轮没有新增 Vulkan 实窗操作或重跑完整根 CTest；
模块证据位于 gitignored 的 `artifacts/csharp-resource-*`。
下面保留原计划收尾时的完整门禁和性能历史。

### C# 原计划收尾验证

本轮新增 32 项脚本回归与 2 项 UI 回归。C# ON Debug 全目标构建通过，
`script_tests` 288 项连续两遍全部通过；`editor_ui_tests` 629 项连续两遍均
624 通过、5 项符号链接环境跳过。Release 的 288 项连续两遍均
287 通过、1 项私有运行时部署前提跳过，不把该跳过算作部署通过。
独立复制当前 Debug 测试程序、原生依赖与完整 managed 目录后，
新进程从副本目录启动，PATH 仅含 Windows 系统目录、DOTNET_ROOT/X64 指向无效路径，
`ScriptDeployment.ExecutableRelativePrivateRuntimeHostsGameplay` 实际通过，
核对 CoreCLR 来自副本 managed/dotnet。

新增覆盖 HUD 的根/组件命名空间、所属线程/回调、预算、异常回滚、
禁用/移除/销毁/Stop/World 换代清理和 Paused 显式迁移后重建；
真实 RmlUi 文本绑定验证字面 markup 不成为元素、冻结文本及显示/隐藏更新。
受控程序集限定类名覆盖原生和直接 CLR 两条解析、默认值/权限、
错误程序集/版本拒绝及 Playing/Paused 迁移，作者 class 文本保持。
启动失败回归验证缺 host 的稳定错误代码、缺 Gameplay 的结构化失败与重试。
两次生产编译服务构建逐字节比较 Gameplay DLL/PDB/deps 清单与复制的桥接 DLL/PDB；
回归先真实失败，修正 Windows Roslyn PathMap 分隔符后 Debug 两遍与 Release 两遍通过。
明细采样回归核对 10 Hz、即时故障发布和 Pause 年龄冻结；
生命周期次数测试显式请求完整诊断，不依赖 UI 的定频采样。

最终性能测量未并行构建或运行其它测试。两个独立 Release 新进程各运行两遍，
每遍沿用预热 3 帧、测量 30 帧；原有
`VL_SCRIPT_IDLE_MAX_P95_MS=5` 明确启用，四次千空实例 p95 均通过：

| 负载（总计 1000 实例，无网格/刚体） | 平均 ms 范围 | p95 ms 范围 | GT 托管分配/帧 |
| --- | --- | --- | --- |
| 全部空实例 | 0.5070–0.6729 | 1.6779–1.8419 | 29.5–29.7 KB |
| 100 移动、900 空实例 | 1.0253–1.2424 | 2.3198–3.0419 | 84.1–84.2 KB |
| 全部逐帧移动 | 5.1690–5.9794 | 6.3373–9.0545 | 543.0–543.1 KB |

空实例每帧更新数为 0，平均请求约 0.77 KB、响应约 8.4–8.5 KB，
平均明细采样次数为 1/6。首遍最大帧仍为 4.68–4.69 ms，
这些有限样本不构成任意机器或全部帧的实时上界。
数值包含原生 Tick、JSON、World 同步和性能采样，不含渲染；
没有将 5 ms 的千空实例预算扩展为千移动实例或千刚体预算。

另一个真实 CLR/Jolt 混合 fixture 包含 64 动态球、静态地面、
64 逐帧移动 mesh owner、8 个重复 timer、跨 owner 事件、相机、灯光，
共 139 个 World CPU draw sections，首个球保留第二 section 的导入局部矩阵。
Release 三项回归连续两遍通过；混合帧每遍预热 3 帧、测量 30 帧，
Tick + WorldSnapshot 平均 6.2132 / 6.0402 ms，p95 7.9946 / 8.1012 ms。
验证冻结快照独立性、暂停/作者恢复、World 换代后服务/物理/历史清理；
没有创建 Vulkan 对象，不是 GPU 绘制或整帧性能验收。

本次未恢复桌面自动操作，没有新增 HUD 的 Vulkan 实窗交互验收记录。
日志及 XML 位于 gitignored 的 `artifacts/csharp-completion-*`。
最终 C# OFF Release 的 main/script_tests 构建通过，69 项连续两遍全部通过。
完整根 CTest 1286 项中 1280 通过、6 项符号链接环境跳过，零失败，
总耗时 292.86 秒。`--world-graph-transaction-test --exit-after-tests --no-dev-ui`
分别使用 `--worker-thread-count 1` 和 `2` 串行通过，均退出码 0、
stderr 为空且正常 teardown；回滚、同步提交、shader/M_ 交错、resize 和
epoch 退休排空通过。65 条架构边界与场景 schema 校验通过。
下面记录保留此前性能与验证历史，不覆盖本节的最终数据。

2026-10-17 Blueprint 性能增量测量：同一 Release 千空实例用例，
每遍预热 3 帧、测量 30 帧，未同时运行其它构建或测试。
修改前分段基线两遍平均 68.1667 / 61.0473 ms，原生 Apply 约 19 ms、
无刚体物理同步约 13.6 ms，每帧请求约 897 KB、响应约 750 KB。
增量同步、基类空帧回调过滤及分配削减后，两次独立进程的两遍记录如下：

| 负载（总计 1000 实例，无网格/刚体） | 平均 ms 范围 | p95 ms 范围 |
| --- | --- | --- |
| 全部空实例 | 1.3262–1.6940 | 1.4645–6.0734 |
| 100 移动、900 空实例 | 2.7149–2.9409 | 3.0721–3.5565 |
| 全部逐帧移动 | 8.0507–9.5213 | 9.2992–11.1396 |

空实例每帧实际状态更新为 0，请求约 0.75 KB、响应约 47.5 KB（主要为诊断），
托管 GT 分配约 142 KB；全移动每帧更新 1000 个位置，分配约 2.64 MB。
这些测量覆盖整个原生 Tick/JSON/World 同步/诊断，不含渲染或实际游戏逻辑；
没有将千空实例结果推算为千网格、千刚体或渲染帧率。
新 GoogleTest 固定检查更新数、传输字节和空实例分配预算。
Release 可用 `VL_SCRIPT_IDLE_MAX_P95_MS` 显式启用千空实例墙钟预算；
本次 5 ms 预算在新进程首遍两次失败（p95 5.8892 / 6.0734 ms），
同进程第二遍为 1.4645 / 1.5795 ms。保留失败日志和阈值，
不把平均耗时降低解释为冷 CLR 的严格尾延迟达标。
证据在 gitignored 的 `artifacts/blueprint-perf-baseline`、
`blueprint-perf-final-measure` 与 `blueprint-perf-final-repeat` 的 log/xml。

本轮新增 15 项回归，覆盖增量字段/物理基线、一次性指令、继承与显式空回调、
无帧回调的计时器/事件/组件清理、相机旧 owner 失效、禁用 owner 的异步取消，
以及暂停中重载的被动组件默认值发布。C# ON Debug 全目标构建通过，
`script_tests` 256 项连续两遍全部通过；Release 同套件连续两遍均为
255 通过、1 项私有运行时部署前提跳过。Debug 的部署用例实际通过，
不能把 Release 的跳过算作独立部署证据。
C# OFF Release main/script_tests 构建通过，67 项连续两遍通过。
完整根 CTest 1252 项中 1246 通过、6 项符号链接环境跳过、零失败，
总耗时 264.03 秒。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、同步提交、热重载交错和 epoch 退休排空检查通过，正常 teardown、stderr 为空。
未恢复桌面自动操作，本增量没有新增实窗交互验收。
验证日志位于 gitignored 的 `artifacts/blueprint-perf-*`：
`debug-final`、`release-final`、`off-tests`、`ctest-final` 与 `transaction-final`。

2026-10-17 内联自定义数据增量验证：
`script_tests` 全量 241 项连续两遍通过；`editor_ui_tests` 全量 627 项连续两遍均
622 通过、5 项符号链接环境跳过。新增 26 项脚本侧测试与 14 项递归 UI 测试，
覆盖嵌套默认值、对象稀疏合成/Reset、数组和字典整体替换、字面字典键
（含空串）、集合分页、焦点保护、只读展开及根实例权限。
真实 CLR 另验证实例间深拷贝、typed 动态组件配置、数据预算/错误路径、
运行时数据不参与逐帧配置遍历、深值迁移新增字段默认值/改类型拒绝，
以及递归元数据缓存不阻止旧程序集回收。
C# ON Debug 全目标及 C# OFF Release main/script_tests 构建通过，
OFF 的 67 项测试连续两遍通过。完整 CTest 1237 项中 1231 通过、
6 项符号链接环境跳过、零失败。递归 UI 实现从头文件提取后，MinGW COFF 节数超限已消除。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0、
stderr 为空，回滚、同步发布与 epoch 退休排空通过，正常 teardown。
本次未恢复桌面自动操作，递归控件的模块布局回归不代表实窗交互验收。

完整扫描后统一了单独解析入口的内部模板标记清理，防止绕过 inline 实例权限；
最终 ON 全目标与 OFF main/script_tests 再构建通过，ON 241 项和 OFF 67 项各再连续两遍通过。
Release 千空蓝图使用同一用例、每遍预热 3 帧和测量 30 帧，未并行运行其它构建或测试：
两遍平均 65.7463 / 57.4162 ms，p95 103.528 / 65.8416 ms，
最大 106.357 / 68.0142 ms。历史同用例平均为 211.152667 ms；
本次增加身份索引及复用响应缓冲区，但仍使用 JSON 完整状态批次，
这些数值包含 Tick/跨边界/World 同步/诊断，不是纯回调成本或渲染帧率。
测试日志位于 gitignored 的 `artifacts/blueprint-data-*`：
`ctest-all`、`ui-full`、`script-permission-final`、`off-script-final`、
`world-graph` 和 `release-performance`。

2026-10-17 Blueprint 基础与集中日志增量验证：
C# ON 全目标 Debug 构建通过；最终完整 CTest 1197 项，1191 通过、
6 项符号链接环境跳过、零失败。旧 `RuntimeControlAssets` 六项已改为验证
实际 workspace 控件，全部通过；下文六项失败记录只代表历史扫描。
其中 `script_tests` 为 215 项、`editor_ui_tests` 为 613 项；
最终专项另验证状态迁移/Scene 逻辑作者事务 25 项、日志/Quaternion/逻辑组件/
时间倍率/重载界面 27 项，全部通过。C# OFF Release 的 main/script_tests 构建
及 55 项无宿主测试通过。

新增覆盖包括独立组件权限与故障隔离、Quaternion 完整数值提交、
Tick/LateTick 每帧顺序、Time Scale、性能快照容量、
Playing/Paused 显式迁移失败保留、Spawn 刚体/速度保留、接触事件去重、
预加载模板成功重载和在途事件/结构命令拒绝，以及日志截断/容量/冻结/
筛选/虚拟列表和旧停靠布局补入 Log。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
stderr 为空，回滚、同步发布、shader/M_ 交错与 epoch 退休通过。
本增量没有恢复桌面自动操作；Log 页签与新增控件没有实窗交互验收记录。

C# ON Release 的 script_tests 构建及两项千实例性能测试通过。
1000 个空逻辑蓝图，预热 3 帧、测量 30 帧：
平均 211.152667 ms、p95 222.500000 ms、最大 227.324600 ms。
测量包含原生 Tick、JSON 桥接、World 同步与性能统计，显示该路径存在明显开销；
不是纯托管回调耗时，也不是渲染帧率。
1000 个动态球与地面、120 步、最终 1000 个接触：
总 506.328200 ms、平均 4.219402 ms、最大 15.751500 ms；
包含初始建体、JSON 同步及接触整理，不是纯 Jolt solver 耗时。
日志位于 gitignored 的 `artifacts/blueprint-next-*`：
完整扫描 `ctest-all.log`、最终构建 `build-final.log`、
专项 `migration-final`/`ui-final-focused`、OFF `off-script`、
原生事务 `world-graph` 与 Release `release-performance`。

2026-10-17 Blueprint P1/P2 七项集成验证：
蓝图视口实时变换/Self/辅助图示与吸附、显式组件实例权限、
运行时内置组件增删挂接、Jolt 扩展、编辑器编译、异步资产和游戏导出已落地。
真实 CLR 的 `script_tests` 177 项连续两遍通过；`editor_ui_tests` 587 项连续两遍均
582 通过、5 项符号链接环境跳过。最终完整 CTest 1133 项：1121 通过、6 跳过、
6 失败，仍仅为上述历史 `RuntimeControlAssets.*` 读取已删除控制页。
最终 CTest 包含新增复选框鼠标回归、MI production 11 项、backend 44 项、
既有 viewport 41 项和 exporter 19 项；没有新增失败。
C# ON 全目标构建与 C# OFF Release 的 main/script_tests/exporter 构建通过，
OFF 的 51 项无宿主测试连续两遍通过。

新增回归覆盖组件权限的作者/两条解析路径拒绝、运行时句柄换代/结构回滚、
异步完成/取消/租约/销毁/Stop、输入字节陈旧候选、动态资源依赖保留，
碰撞双方 mask、compound 重建、正式三角形静态碰撞、Fixed/Hinge 和扫球，
以及镜头使用本次物理后位姿、统一缩放的局部/世界距离换算。
编译用例使用生产 Gameplay 项目，检查隔离输出、成功重载、编译错误文件/行/列、
稳定源码监视、桥接源码变化拒绝及终止自有进程树。
实窗暴露相对资源根被 dotnet 的项目工作目录重解释；回归先失败，固定绝对路径后通过。
实窗另暴露 Snap/组件权限 checkbox 的 click 被提前截断；
回归改为 RmlUi 真正鼠标按下/抬起后先失败，修复后连续两遍通过。
复选框标签使用显式 span，避免 flex 布局吞掉裸文本。

Debug、Vulkan validation 开启、双线程实窗使用独立
`Maps/SC_blueprint_async_validation_20261017/`：
无预加载网格和 spawnAssets 的场景在 Play 中异步加载 BP，正式上传后生成真实贴图网格；
释放租约后活 Spawn 继续保留资源，延迟 Destroy 后记录 `Blueprint asset released`，
期间没有 World 事务替换，Stop 恢复作者场景。另一窗口核对工具栏编译成功和
`C# assembly reloaded`、蓝图标签真实网格拾取/平移、Undo 恢复干净草稿、
相机与碰撞辅助线和最大化布局。Self 工具及 Snap 修复后的桌面操作未完成：
用户物理 Esc 停止 computer-use 后没有恢复自动操作；对应行为有模块覆盖。
两窗口均正常退出，退出码 0、stderr 为空，验收修改未保存到既有资产。
最终 `--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、同步发布、shader/M_ 交错及 GPU epoch 退休检查通过。

独立游戏包实窗从工程之外的 cwd 启动，限制 PATH、提供无效系统 DOTNET_ROOT，
确认实际 CoreCLR/hostfxr/原生依赖来自包内，自动 Play、Spawn 刚体并正常关闭；
该窗口未记录进程退出码，不将正常日志等同于退出码 0。
最后用当前构建重新导出异步场景与显式动态 BP 清单：
1614 个文件、1157502947 字节，包内相对资源路径、autoPlay 和私有 .NET 8.0.32 齐备，
清单文件存在性/长度与主程序、Gameplay、Runtime 二进制一致性检查通过。
最后这个重导出包未再次做桌面启动，部署实窗证据来自前述同轮独立包。
独立千刚体测量为 1000 个动态球与地面、120 步、1000 个接触：
Debug 总 3693.3175 ms、平均 30.777646 ms、最大 225.1285 ms。
这是包含初始建体、GT JSON 同步和接触整理的整步耗时，不是纯 Jolt solver 或 Release 帧率。
38 个场景 schema、65 条架构审计、文档与 whitespace 门禁通过。
日志在 gitignored 的 `artifacts/blueprint-p1p2-*`、
`blueprint-interaction-*` 与 `blueprint-game-export-*`。

2026-10-17 Blueprint 命名迁移验证：C# API、脚本文件、原生作者/运行时接口、资产目录与前缀、
场景 `blueprintInstance`/`blueprintPath`、编辑器 `blueprint.*` 及示例资源统一迁移。
没有旧命令、资产类型或 C# 类型别名；旧场景类型、引用字段与 ACT 创建目标有拒绝回归。
`script_tests` 120 项连续两遍通过；`editor_ui_tests` 531 项连续两遍均为
526 通过、5 项符号链接环境跳过；MI production 9 项连续两遍通过。
离线场景类型支持选择集 5 项通过。C# ON 的 main、脚本/编辑器测试、CLI、production 和
offline adapter 目标，以及 Release C# OFF 的 main 构建通过。
CLI 对正式 BP 示例的 get_document/get_capabilities/validate 成功且未写盘；
旧 actor.get_document 返回 invalid_command_type。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，正常 teardown。
实窗核对 BP 浏览器条目、独立蓝图标签的组件树/3D 预览/Details，以及迁移后的 gameplay
场景六个 Spawn 刚体落地、碰撞日志和 Stop 从 11 个对象恢复到 5 个作者对象。
两个验收窗口均正常关闭，stderr 为空。拖放的 RCSS 类型选择器遗漏由原生 RmlUi 拖放回归发现并修复；
本轮未重复执行完整拖放/保存/重开实窗流程。日志在 gitignored 的 `artifacts/blueprint-*.log`。
本轮没有全量 CTest 扫描；下述历史扫描中的 RuntimeControlAssets 失败不在本轮选择集内。

2026-10-17 [C# Actor](csharp-blueprint-runtime.md) 最小实现验证：
真实 CLR 的 `script_tests` 15 项连续两遍通过；`editor_ui_tests` 连续两遍均为
474 项、469 通过、5 项符号链接环境跳过；MI production 8 项连续两遍通过。
新增回归覆盖独立实例属性、Play/Pause/Stop、作者/运行时 TRS 分离、
失败候选不影响活动 World、陈旧上下文拒绝、输入与逻辑 Actor、
回调异常隔离、Destroy、冻结快照，以及放置/属性动作的 generation/ID、
焦点保留、Scene 创建/保存/重开和 960×540、1280×720、1920×1080 表单布局。
小停靠区表单可滚动，提交按钮不溢出到 Details。
实窗发现再次编辑后保存被总线旧 revision 拒绝；新增 production 用例在修复前失败，
修复后验证连续两轮编辑/保存成功，ID 保持不变。

全部目标 C# ON 构建与 Development C# OFF 的 main 构建通过。
最终 CTest 832 项：820 通过、6 项符号链接环境跳过、6 项失败，
仍是下文已记录的 `RuntimeControlAssets.*` 读取已删除控制页失败。
文档校验、32 个真实场景 schema 检查、65 条架构审计和 diff whitespace 检查通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
事务回滚、提交和 GPU epoch 退休检查通过；没有新增 runtime test 标志。

Debug、Vulkan validation 开启、双线程实窗使用
`Generated/CSharpActorSmoke/SC_actor_smoke.json` 隔离场景，核对 Actor 根网格出图、
视口拾取到 Actor、Play 旋转、Pause 角度冻结、Stop 恢复零旋转，
新增无网格 PlayerBlueprint、Save Content 保存及 MoveSpeed 500→250 写盘。
1280×720 与最大化窗口的表单/Details 可用；Alt+F4 正常关闭，
日志含 Quit 和 teardown complete，stderr 为空。输入行为由模块测试覆盖，
未完成持续按键位移的桌面验收；离线自动 Stop、超限批次和大量实例性能未做专项实窗验收。
另以 `--worker-thread-count 1` 实窗核对同一场景出图、Play 旋转和 Stop 复位，
Alt+F4 后 teardown complete，stderr 为空。

2026-10-17 Actor 资产增量验证：`script_tests` 26 项连续两遍通过，
`editor_ui_tests` 483 项连续两遍均为 478 通过、5 项符号链接环境跳过；
MI production 8 项连续两遍通过。覆盖资产默认值变更、实例稀疏覆写与复位、
引用/内嵌互斥、重复键/深度/依赖边界、地面反投影、原生 RmlUi 拖放及取消、
真实 CLR 的生产作者适配器，以及 Scene 保存重开、新资产创建失败不覆盖原文件。
拖动图片 URL 的实窗警告修复后，原生拖放专项连续两遍通过。

双线程实窗使用 `Generated/Validation/CSharpBlueprintAssets/SC_actor_assets_smoke.json`
副本，核对同资产双实例出图、45 继承值、90 覆写的黄色边框、原生拖入第三实例、
复位回 45 并消除覆写边框，以及播放中的网格旋转。关闭窗口后日志含
Quit 和 teardown complete。本轮未完成新资产按钮、拖入后 Save Content 写盘、
重新启动及单线程的专项桌面验收；这些保存/创建语义由上述模块测试覆盖。
正式示例在独立资源仓库 `Maps/SC_actor_authoring/`，没有修改验证用副本之外的已有场景。
本轮日志位于 gitignored 的 `artifacts/blueprint-assets-*.log`。
全部目标 C# ON 与 Development C# OFF 的 main 构建通过；OFF 构建未启用 BUILD_TESTING，
没有在该构建目录运行 script_tests。33 个场景 schema、文档校验、65 条架构审计通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、提交、shader/M_ 交错和 GPU epoch 退休检查通过。
最终全量 CTest 852 项：840 通过、6 项符号链接环境跳过、6 项失败，
仍是 `RuntimeControlAssets.*` 读取已删除控制页；没有新增失败。
此前构建重链接期间一项缩略图测试出现 BAD_COMMAND，独立连续两遍及最终全量均通过。

2026-10-17 组件/Spawn/物理/ACT 重载增量验证：
真实 CLR 的 `script_tests` 50 项、backend 44 项、MI production 8 项均连续两遍通过；
`editor_ui_tests` 485 项连续两遍均为 480 通过、5 项符号链接环境跳过。
回归覆盖组件父链/环/完整矩阵、作者局部编辑、Spawn/Destroy/Stop、
Bullet 固定步进与接触 Enter/Exit、冲量单次消费、回调失败回滚、
ACT 摘要稳定采样/失败抑制，以及新对象先补齐模型 UBO/descriptor 再录制。
全部目标 C# ON 与 Development C# OFF 的 main 构建通过；
OFF 构建未启用 BUILD_TESTING，没有在该目录执行托管测试。
最终 CTest 880 项：868 通过、6 项符号链接环境跳过、6 项失败，
仍是上述 `RuntimeControlAssets.*` 读取已删除控制页，没有新增失败。
34 个真实场景 schema、文档校验、65 条架构审计及 diff whitespace 检查通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、同步提交、shader/M_ 交错和 GPU epoch 退休检查通过。

Debug、Vulkan validation 开启的双线程实窗使用新学习场景
`Maps/SC_actor_gameplay/SC_actor_gameplay.json`，核对组件旋转、
6 个定时生成刚体落地及各自碰撞日志、Pause 冻结、Stop 移除生成物并恢复作者姿态，
以及 1280×720 下组件树/Parent/局部 TRS 的滚动布局。
暂停时将新 ACT 的 ImpulseX 从 30 改为 60，没有发生 World 替换；
Stop 后自动提交 generation 2。无效类名候选报错后仍显示原场景，
恢复正确类名与 ImpulseX 30 后自动提交 generation 3。
初次实窗暴露 Spawn 对象缺少 GPU 包的问题，补齐发布路径后完成上述验收。
Alt+F4 正常关闭，日志含 Quit 与 teardown complete；
stderr 只有故意注入的无效类名失败，没有 Vulkan validation 错误。
单线程同场景另核对 6 个生成刚体落地、碰撞日志和 Stop 恢复为 5 个作者对象；
Alt+F4 后 teardown complete，stderr 为空。
ACT JSON 热重载不覆盖程序集或通用模型/材质/贴图；物理只支持 Actor 根 box/sphere。
未做大量实例性能、真实 GPU 包高水位或组件重挂后的桌面保存专项；
父链编辑和 Scene 保存语义由模块回归覆盖。日志位于 gitignored 的
`artifacts/actor-gameplay-*.log`。

2026-10-17 Jolt 迁移与游戏服务验证：
`script_tests` 74 项连续两遍通过，覆盖质量/单位/旋转约定、固定步进/追赶上限、
休眠接触保持、支撑体移走后唤醒、并存物理世界、Trigger 穿透/移除、
射线过滤/内部起点/延迟与失败回滚、胶囊角色落地/滑墙/跳跃/传送刷新及查询代理。
托管服务用例另覆盖命名输入、混合键鼠屏蔽、计时器暂停/重复跳积压/owner 清理、
Actor 查询快照、定向事件和失败回调丢弃消息。
`editor_ui_tests` 485 项连续两遍均为 480 通过、5 项符号链接环境跳过。
全量 CTest 904 项：892 通过、6 项符号链接环境跳过、6 项失败，
仍是 `RuntimeControlAssets.*` 读取已删除控制页；没有新增失败。
日志位于 gitignored 的 `artifacts/jolt-*.log`。
全部目标 C# ON 与 Development C# OFF 的 main 构建通过；
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
事务回滚、同步提交、shader/M_ 交错、GPU epoch 退休及正常 teardown 通过。
文档、34 个场景 schema、65 条边界审计与 diff whitespace 校验通过。

Debug/Vulkan validation 的单线程和双线程实窗均使用既有
`Maps/SC_actor_gameplay/SC_actor_gameplay.json`，确认组件旋转、6 个 Spawn
刚体落地及碰撞日志，Stop 从 11 个运行时对象恢复到 5 个作者对象；
双线程另核对 Pause 冻结。均由 Alt+F4 正常退出，日志含 Quit 与 teardown complete，
stderr 为空，没有修改正式资源。Trigger、射线和角色的行为由真实 CLR/CPU
模块测试覆盖，本轮没有对应的独立实窗交互场景或大规模实例性能验收。

2026-10-17 Actor 配置标签与实例权限增量验证：
`editor_ui_tests` 512 项连续两遍均为 507 通过、5 项符号链接环境跳过；
`script_tests` 95 项、MI production 9 项分别连续两遍通过。
回归覆盖 Actor 独立草稿、类切换清理、默认值与实例白名单、组件父链/局部变换/相机、
物理配置、刷新零修改、半成品输入保留、保存失败重试及关闭保存/丢弃/取消。
关闭回归另验证同帧修改后丢弃使用新 revision、回退拒绝保留弹窗，以及旧保存反馈
不能关闭更新的脏草稿。
真实 RmlUi DOM 在 960×540、1280×720、1920×1080 验证表单滚动及复选框尺寸/选中态；
这些模块检查不构成 Vulkan 实窗图像验收。
权限回归同时验证 GT 拒绝未开放项、ACT 合成前拒绝越权稀疏覆写、
inline `[Expose]` 不自动开放实例权限，以及 Scene 无法伪造内部权限元数据。
C# ON 全部目标和 Release C# OFF main 构建通过；headless CLI 对正式示例 ACT 的
get_document/get_capabilities/validate 三条请求成功，未写入文件。
文档、35 个场景 schema、65 条架构边界及 diff whitespace 校验通过。
日志在 gitignored 的 `artifacts/actor-tab-*.log` 和 `actor-tab-cli-smoke.jsonl`。
全量 CTest 扫描 950 项，938 通过、6 项符号链接环境跳过、6 项失败，
失败仍为 `RuntimeControlAssets.*` 读取已删除控制页；随后关闭流程新增三项回归，
受影响的 UI/production 选择集另行验证，没有修改这些既有失败。
此前桌面 Undo/Redo 验收被 Esc 中止，当时未恢复桌面自动操作；
该阶段独立 Actor 标签页尚未完成实窗点击和 Vulkan 图像验收，未实现 3D Actor 预览。

2026-10-17 Actor 布局后续验证：初次实窗恢复后确认 ACT 双击打开独立标签、
组件父链与属性白名单显示，默认速度 45→60 出现脏标记而磁盘仍为 45。
实窗发现根 rotation/scale 向量控件收缩，补齐控件列宽度后核对三轴正常显示。
用户再次按 Esc 中止桌面操作，后续没有恢复；保存/关闭/Scene Undo/Redo 的
实窗验收仍不完整。隔离副本位于资源仓库 `Maps/SC_actor_tab_acceptance_20261017/`，
该验收窗口保留运行，由用户接管；副本未保存到正式内容。
随后按用户要求改为左侧 Components 树、右侧 Details。
真实 RmlUi 回归在 960×540、1280×720、1920×1080 检查两栏边界、独立滚动、
三轴输入宽度与复选框选中态；新增三项覆盖实际指针选择组件、
稳定 ID 重排/删除回退，以及切换选择保留待提交编辑与 revision。
这轮两栏检查不等于新版 Vulkan 实窗验收，当时 Actor 3D 预览仍未实现。

2026-10-17 Actor 中央 3D 预览增量：UI 8 项新增回归覆盖 revision-gated 视觉请求、
完整父链矩阵的多网格取景、无网格 Actor 导航、匹配路径/revision 后才显示图像、
真实 RmlUi 滚轮导航、权威视觉草稿防抖、默认属性/白名单不重载几何及未接受输入不进入预览。
三种窗口尺寸继续验证组件树/中央视口/Details 的三栏边界和向量控件尺寸。
脚本 2 项新增回归使用真实 CLR 和生产 World，验证 FailCreate 配置在预览中不执行 OnCreate，
仍发布作者几何，并拒绝从预览启动 Play。
最终 `editor_ui_tests` 全量连续两遍均为 523 项、518 通过、5 项符号链接环境跳过；
`script_tests` 97 项连续两遍全通过。Debug C# ON main 与 Release C# OFF main 构建通过，
相关 production/viewport 选择集 50 项通过；文档、65 条边界与 diff whitespace 校验通过。
日志为 gitignored 的 `artifacts/blueprint-preview-*.log`。
用户 Esc 中止后没有恢复桌面操作，当前验收进程仍是旧版本；
新版中央预览的 Vulkan 像素、桌面相机手势及正常关闭尚未重新验收。

缩略图专项回归（注册在 `editor_ui_tests`，不需要 Vulkan）：

```powershell
ctest --test-dir build -R "^(ThumbnailJobTest|ThumbnailLibraryTest|ThumbnailSchedulerTest|EditorThumbnailGraph|RmlUiTextureTest)\.|^ContentBrowserTest\..*Thumbnail" --output-on-failure
```

覆盖内容缓存、可见请求取消、Refresh/重初始化代际、暂停与单次领取，以及离屏图派生。
`ThumbnailJobTest` 的 8 项验证非阻塞轮询、单次消费、陈旧结果/错误丢弃、
当前错误传播、单槽并发上限、缓存重建转移任务与退出收口；含 RmlUi 桥接和浏览器缩略图用例的专项共 46 项。
`ThumbnailSchedulerTest` 覆盖连续悬停不推迟出图、捕获暂停与交互后 750 ms 退避；
队列回归覆盖 Mesh/MI 等待时贴图继续、CPU/GPU 同时完成不覆盖、单次领取/发布背压、
可见集合取消与 Refresh 丢弃排队请求和未发布结果。
2026-10-17 调度修复验证：新增 8 项，完整编辑器测试连续两遍均为 458 项、
453 通过、5 项符号链接环境跳过；当轮 CTest 缩略图选择集 34 项通过。
修改前新增的混合队列用例因模型请求挡住贴图而失败，修改后通过。
Debug、Vulkan validation 开启、双线程实窗核对 Vilking 模型/材质及 Sifi 混合资源列表；
Sifi 初次搜索与显式 Refresh 后均观察到贴图已显示、部分模型仍在等待，随后模型/材质正常出图。
Alt+F4 正常关闭，日志记录 Quit 和 teardown complete，stderr 无 Vulkan validation 错误。
连续悬停退避由确定性时钟模块测试覆盖；未测量首次 GPU 准备耗时或逐帧性能分位数。

2026-10-17 GPU 直出验证：新增 10 项，覆盖 Mesh/MI 不发布 CPU 像素或写盘、
Refresh/取消/LRU 淘汰的 token 生命周期、浏览器原位更新，以及真实
`rmlUiInterfaces.cpp` 的 GPU 身份/尺寸、UNORM 采样策略、空发布撤销旧 source、
冻结快照保活和帧内延迟释放。该测试目标编译生产 RmlUi 桥接，但不创建 Vulkan 对象。
最终 `editor_ui_tests` 全量连续两遍均为 468 项、463 通过、5 项符号链接环境跳过；
上述专项 CTest 46 项、backend 模块 42 项通过，全部目标编译、文档校验和架构审计通过。
Debug、Vulkan validation 开启的单/双线程实窗均核对 Vilking 模型/材质 GPU 成图；
双线程另核对 Sifi 混合资源、Refresh 重新出图、最大化窗口后保留已完成缩略图、
列表与平铺往返复用。两次 Alt+F4 正常退出，日志确认 teardown complete，stderr 均为空
（`artifacts/gpu-thumbnails-20261017/`）。
当前仍使用专用同步 fence；未测量本次耗时收益，未进行旧路径像素对照。
UNORM 策略由模块测试覆盖，未另跑 UNORM surface 实窗；LRU 以假 token 验证，
未进行真实 Vulkan 图像池淘汰压力测试。

模型/MI 实际出图另用 validation 构建核对真实资产；2026-10-13 已验证青/红遮阳布材质球、
`SM_vilking_room`、窗口最大化后继续出图及 File → Exit 正常 teardown，无 Vulkan validation 错误。
当轮 `editor_ui_tests` 全量连续两遍均为 347 项、343 通过、4 跳过。

2026-10-13 actor 编辑验证的完整 CTest 扫描还发现 6 个 `RuntimeControlAssets.*`
用例仍读取已删除的运行时控制页（`ui_control_test.cpp` 的文件打开断言失败）。
该页在本次改动前已经不存在；这些用例未因本次 actor 编辑而恢复或改写。
`--world-graph-transaction-test --exit-after-tests` 在启用编辑器时出现 resize
GPU 资源计数不稳定，使用 `--no-dev-ui` 后通过全部事务/退休检查；
关闭编辑器的结果不能替代开启编辑器的交互验收。

actor 编辑的最终 `editor_ui_tests` 全量连续两遍均为 369 项、365 通过、4 跳过，
`editor_viewport_tests` 41 项通过；全部目标编译、文档校验和架构边界审计通过。
桌面实测扁平大纲、点光位置输入、灯光图标选择/双击取景、相机首次双击取景，
相机 FOV 80→40 后视锥变化但观察视角不变，以及 1280×720 / 最大化窗口、
辅助图形开关和 File → Exit 正常 teardown。验证未保存场景修改。
spot/area 图示尚无对应桌面场景或像素专项验收。

2026-10-13 保存机制新增 `SceneDocumentTest` 17 项：JSON 补丁/精度、返回基线、
lm↔cd、spot 锥角、面光尺寸/辐亮度、mesh TRS、CSM 默认复位、Camera look_at、
陈旧 revision、外部冲突、路径逃逸、草稿重放与真实替换 World、预览排除及 `scene.save` 协议。
面板回归覆盖单文件/场景与非活动 MI 组合列表、真实指针 checkbox 不重建节点、
取消不写盘、仅保存勾选项、失败重试、模态输入阻断、待编辑 revision 与长路径/20 文件
在 960×540、1280×720、1920×1080 的布局。生产 facade 回归验证场景/非活动 MI
独立保存、单调反馈与选择不变。

保存桌面验收使用独立临时 Scene/MI 副本，核对 1280×720 与最大化窗口：
星号出现/消失、右键单文件确认、组合列表取消勾选 MI 后只写场景、
未选 MI 的磁盘内容保持不变、随后 MI 保存并保留关卡页面、预览往返恢复未保存光源位置、
选择 actor 不抢回材质标签，以及 File→Exit 正常退出。临时资源已清理。
实际源文件冲突与磁盘写失败的桌面操作未验收；相关冲突/失败状态由模块回归覆盖。
最终 `editor_ui_tests` 全量连续两遍均为 397 项、393 通过、4 权限跳过；
MI 三套共 23 项及 viewport 41 项通过。构建、文档校验、架构审计与 diff whitespace 检查通过。

2026-10-13 Camera Details 曝光编辑新增 11 项回归：作者参数快照、逐字段原子校验、
旧 World/错误身份拒绝、physical 参数保留、模式可见性、刷新不提交与焦点保留、
静止帧零属性写入、稀疏场景保存/默认复位/替换 World 重放，以及三个窗口尺寸的按钮布局。
当前全量为 408 项，404 通过、4 权限跳过；viewport 41 项、MI 三套 23 项通过。
实窗 1280×720 验证 Manual/Auto 切换、EV100 0→9 改变渲染亮度、
自动曝光完整参数排版、窄 Details 滚动到目标灰度、所属 Scene 出现在 Save Content 列表、
Cancel 与 File→Exit 正常 teardown。未将验收曝光写入正式场景；
physical 作者配置保留与曝光写盘由模块测试覆盖，未做相应桌面保存操作。

2026-10-13 Debug 缩略图性能修复：GDB 抓到 GT 在预览球导入/MikkTSpace、
材质 shader 候选编译和 2048×2048 PNG 解码中阻塞。相同 `SC_sphere_array`、
Debug 无优化、validation 开启、两线程配置，修复前编辑器开启为 1–6 FPS，
`--no-dev-ui` 为 70–90 FPS；异步 CPU 准备后，从第 10 秒起 36 次每秒窗口标题采样
为 30–66 FPS，平均 59.1，任务完成后为 64–66。该采样不是逐帧时间分位数，
首次环境/GPU 准备及贴图上传/管线创建仍可有瞬时停顿。CPU 解码峰值 RSS 为
1063 MB，完成后约 473 MB；未将任意 MI/模型贴图永久缓存在会话中。
1280×720 截图载荷完整，场景与浏览器正常出图，无 Vulkan validation 错误，
窗口关闭退出码为 0，日志确认 teardown 完成。

最终 `editor_ui_tests` 全量连续两遍均为 416 项，412 通过、4 权限跳过；
缩略图与纹理专项 30 项连续两遍通过，backend 42 项与 shader build integration
8 项通过。全部目标编译、文档校验与架构边界审计通过。

2026-10-13 移动观察相机的独立性能修复：内容浏览器只显示 Common/Maps 文件夹、
选中网格时，启用相对鼠标并旋转可从约 65 FPS 降至持续 5 FPS，停止移动/释放捕获
仍不恢复。GDB 两次采样落在工作区布局/文字测量，`DirtyLayout` 断点进一步钉住
`RefreshActorVisualization → SetMarkup(viewport-actor-icons) → SetInnerRML`。
投影坐标进入整段 markup，导致节点替换；浮点末位变化让刷新继续发生。
修复后图标保留固定盒，线段保留未裁剪拓扑槽位，逐帧仅改 transform/visibility。
相同 Debug、validation、双线程、1280×720 实窗多次旋转后保持约 64–65 FPS；
包含捕获及旋转操作的 25 次每秒标题采样为 53–65，平均 60.84。
这是标题采样，不能视为连续运动的逐帧耗时分位数。
新增两项真实 RmlUi 回归覆盖观察移动的零布局/节点保持/变换命中，
以及选中点光跨近面与出屏后的固定线段槽位。全量连续两遍均为 418 项、
414 通过、4 权限跳过；既有真实指针双击回归也通过。
实窗另核对移动后的图标选择、作者 Camera 双击取景/视锥和点光双击取景/位置圆；
File→Exit 日志确认 teardown 完成，无 Vulkan validation 错误。该验收未保存场景修改。
提交前最终验证脚本的构建/架构审计通过；全量 CTest 759 项中 748 通过、
5 权限跳过，仍是上文记录的 6 项 `RuntimeControlAssets` 读取已删除页面失败。
脚本在 CTest 处停止，后续三个 Vulkan 事务测试未在此次提交前检查中执行。

## 改动 → 验证

TressFX 导入与材质边界使用 `vl_tfx_inspect`、`hair_asset_tests` 和
`material_schema_tests`：构建这三个目标后运行
`ctest --test-dir build -R "^(TfxLoader|TfxFileTest|StrandGeometry|TressfxDescriptor|TressfxImport|TressfxMaterial)\." --output-on-failure`。
hair 的 **139 项**覆盖 `.tfx` 字节边界、切线/ribbon 拓扑、SM 半径/身份、属性流、
simulation CPU/GPU backend 解析、变量步长（`maxDt` clamp、一帧一步）、
`bendStiffness`/`windGust`/`globalShape*`/`vsp*` 作者字段与参数打包、
`simulation.skinning` 作者边界/缓存身份/根骨解析/InitialPos 纯数学、
`collision.mesh` 作者边界/缓存身份/OBJ 解析、有符号 SDF 网格 CPU 镜像
（magnitude-first 编码/每帧变形域/clear/splat/三线性/中心差分梯度/投影/蒙皮）、
胶囊投影/固定点接触与 collision 资产校验；其中 `tressfxSolverMathTests` 的 16 项
覆盖五个 pass 的纯数学：距离约束的 0.5/0.5、1/0、0/1、0/0 multiplier，风力的
方向与单位归一化，指数阻尼 `exp(-DampingCoeff*dt*60)`，global shape 的 range 门控
与拉回，VSP 混合端点与逐顶点 stiffness 线性衰减，strand 刚体变换与伪加速度阈值，
直/弯 rest 的 local-shape 旋转、速度 clamp 与 std140 参数布局。
`GuideSimulationParams` 的 `static_assert` 固定 224 bytes 与 mat4 offset 160。
GPU compute dispatch 不在模块套件内，由 main 启动烟测覆盖。
`TressfxMaterial` 的 7 项覆盖工厂身份、透明写深度，以及不支持的模型/路径拒绝。
材质目标共 16 项。
当前改动 → 验证：

| 改动 | 命令 |
| --- | --- |
| `shader/glsl/hair/tressfxSim*.comp`、`tressfxSimCommon.glsl`、`tressfxSdfCommon.glsl` | 启动任一 GPU 发丝场景；shader 构建日志显示 9 个 `hair/tressfxSim*` 条目（含 SdfClear/SdfSplat/SdfCollide），`failed=0` |
| `source/hair/*`、参数布局 | `cmake --build build -j 8`；`build/bin/hair_asset_tests.exe --gtest_brief=1` |
| `schema/model_descriptor.schema.json`、`meshAssetValidator` | `hair_asset_tests`；`material_schema_tests.exe --gtest_brief=1` |
| `documents/contracts/**` | `py -3 tool\validate-docs.py` |
| 架构边界 | `pwsh -File tool/architecture-boundary-audit.ps1` |

GPU guide 模拟的实机证据来自作者场景
`Maps/SC_tressfx_female/SC_tressfx_female_dynamic.json`：其
`SM_female_hair_dynamic.json` 作者 `simulation.backend = "gpu"`。在仓库根运行：

```powershell
build/bin/main.exe --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_dynamic.json --frame-limit 60 --no-dev-ui
```

日志应出现 `World/graph transaction committed` 与
`[TressFX] GPU guide simulation dispatch: objects=1 guides=40334 points=1290688 passes=9 sdfObjects=0`，
stderr 无 Vulkan validation 输出。该日志行只在首次录制时输出一次，统计对象/
guide/点数量、pass 表大小与声明 `collision.mesh` 的对象数；每帧恰好录制一次
pass 序列（`vspCoefficient=0` 时不录制 VSP，无胶囊时不录制 Collision，无
`collision.mesh` 时不录制三个 SDF pass），没有固定步 accumulator 的 0..N 次
重复 dispatch。该路径没有自动退出标志，验证后手动结束进程。
同一 SM 还作者了头/颈/肩背四个碰撞胶囊；Collision pass 在 LengthConstraintsWind
之后执行。

根点骨骼蒙皮烟测使用资源仓库的
`SC_tressfx_female_hair_animated.json`：普通 `mesh` 角色
（`SM_female_skinned.json`，自动播放 Idle，不需要 Play）加
`SM_female_hair_skinned.json`（`backend=gpu`、`skinning.source=FemaleCharacter`、
`rootBone=Head_M`、`rootFixedPoints=2`）。在仓库根运行：

```powershell
build/bin/main.exe --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json --frame-limit 60 --no-dev-ui
```

日志应出现同样的 `World/graph transaction committed` 与
`[TressFX] GPU guide simulation dispatch` 行（`objects=1 guides=40334 points=1290688 passes=9 sdfObjects=1`），
stderr 为空、无 Vulkan validation 输出。该路径没有退出命令，验证后终止进程；
这不作为正常 teardown 证据。

两时刻截图用 Console 命令，两次运行分别使用不同的帧延迟（延迟从命令排队后开始计数）：

```powershell
build/bin/main.exe --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json `
    --no-dev-ui --frame-limit 60 --console-command-delay 2 --console-command "screenshot <root>\artifacts\tressfx-pass\animated_a.bmp"
build/bin/main.exe --initial-scene Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json `
    --no-dev-ui --frame-limit 60 --console-command-delay 30 --console-command "screenshot <root>\artifacts\tressfx-pass\animated_b.bmp"
```

两帧均为 1280×720；`Screenshot saved` 后终止进程。实测两帧平均绝对像素差
`13.284`、差值超过 8 的像素占 `18.32%`：第 2 帧发丝贴合头型，第 30 帧被作者风
吹向面部一侧，说明根点蒙皮、长度约束与风在 Idle 期间持续生效。Debug 构建下
动画场景约 0.6 FPS（发丝覆盖面部时 overdraw 高），这不是性能验收。截图位于
gitignored 的 `artifacts/tressfx-pass/animated_a.png` / `animated_b.png`。
静帧不能证明逐帧无抖动或零头皮漂移；模块套件也不覆盖 dispatch 结果。

#### `collision.mesh` 烟测与 A/B

在资源仓库的 `Maps/SC_tressfx_female/Meshes/SM_female_hair_skinned.json` 的
`collision` 块里作者：

```json
"mesh": {
    "path": "Maps/SC_tressfx_female/Collision/female_body_proxy.obj",
    "cellSize": 1.0,
    "margin": 1.0
}
```

同一动画场景启动后，日志在 `World/graph transaction committed` 之后出现：

```text
[TressFX] SDF collision buffer: object="...female_guides.tfx..." proxyTriangles=7391 resolution=48^3 cells=110592 bindCellSize=1.13073 margin=1 (domain is rebuilt per frame)
[TressFX] GPU guide simulation dispatch: objects=1 guides=40334 points=1290688 passes=9 sdfObjects=1
[TressFX] SDF domain: object="...female_guides.tfx..." dims=48x48x48 origin=(-26.81, 122.70, -24.73) cellSize=1.16395 margin=1 truncation=3.32791
```

`dims`/`origin`/`cellSize` 是**第一帧**的域；之后每帧 origin/cellSize/truncation
都随蒙皮后的代理包围盒更新，行只打印一次。`passes=9` 是 pass 表大小
（6 个 solver/胶囊 + 3 个 SDF）；该帧实际录制 Integration、StrandLevel、
LocalShape、LengthWind、Collision（4 个胶囊）、SdfClear、SdfSplat、SdfCollide
共 8 个 dispatch（VSP 关闭）。stderr 为空，无 Vulkan validation 输出。

符号数值检查（临时诊断，CPU 镜像 same field，**已移除**）：在正式代理最高的
三角形质心取“表面”，沿绕序法线做内外偏移：

```text
surface=+0.000177  inside1.5=-1.49978  inside2.5=-2.4965  outside2=+2.00012  farOutside=1e+10
```

表面约 0、内部为负、外部为正、带外为 +1e10（不碰撞），确认场是有符号的。
该诊断只读取 CPU 镜像，不是 GPU 回读；验证后已从源码删除。

A/B 证据：同一场景分别保留与移除 `collision.mesh`，在相同帧延迟截取 1280×720
截图并比较（`artifacts/tressfx-sdf/`）：

| 对比 | 平均绝对像素差 | 差值 > 8 的像素占比 |
| --- | --- | --- |
| 第 2 帧：SDF on vs off | 1.668 | 1.522% |
| 第 2 帧：同配置重复运行（噪声） | 0.421 | 0.340% |
| 第 30 帧：SDF on vs off | 2.498 | 5.453% |
| 第 30 帧：同配置重复运行（噪声） | 1.678 | 4.333% |

第 2 帧的 A/B 差异约为同配置噪声的 4.0 倍，热图集中在头顶与发丝-头皮接触带，
说明 SDF 碰撞在首帧起就改变发丝位置。第 30 帧的物理轨迹对帧时序敏感（Debug
约 0.6 FPS、变量步长），同配置噪声也升高，A/B 约为噪声的 1.5 倍；第 2 帧是
更干净的证据。符号正确性由上面的临时 CPU 镜像诊断覆盖；没有 GPU 读回。

原始 BMP/PNG、差异热图与 stdout/stderr 位于 gitignored 的
`artifacts/tressfx-sdf/`。资源 SM 保持启用 `collision.mesh` 的状态，未提交。


Female 正式关卡验证：`tool/hair/import_female_scene.py` 实际导入固定 Maya 归档，
`vl_tfx_inspect` 通过 40,334 guides / 1,290,688 points / 7,502,124 indices；
独立资源根重导入的 10 个文件逐字节一致，标准 scene API 保存过的 1 个 JSON
语义一致。已存在目录的导入请求 exit 1，未覆盖正式内容。scene API 完成
读取/能力/修改/校验/保存，三个 mesh 的读取/能力/校验 9/9 成功，
三个 SM 通过 `Test-Json` 模型 schema；稀疏资源工作树的场景扫描为 2/2，
只覆盖默认 sphere array 和新增 Female，不能称为原资源仓库全量扫描。
真实 Vulkan 的 `female-final.bmp` / `female-side.bmp` 均为完整 1280×720 BMP，
已检查角色与发根贴合、完整头顶/发梢和转相机展开；截图位于该资源工作树的
`Generated/Screenshots/`。首轮/最终无编辑器预览 stderr 为空，正常关闭有
`Quit event received` 和 `EngineLoop teardown complete`，截图临时 UI 配置已恢复。
默认启动脚本的编辑器模式也已出图检查：`female-editor-final.bmp` 中角色与完整头发位于
默认 viewport，Scene Outliner 为六个作者对象；stderr 为空并正常 teardown。
本批只改导入/启动工具、资产和文档，未更改引擎实现或新增 runtime test；
上一批的 GTest/三项 Vulkan 门禁结果不冒充本批重跑结果。
日志与重导入比对在 `artifacts/tressfx5-female/`。

公共 [DrawDebug](debug-draw.md) 的 CPU 套件为 `debug_draw_tests`：
几何、生命周期/帧命令时序、key/句柄、过滤、预算、投影和 Console 解析。
`ScriptDebugDrawTest` 使用真实 CLR 验证 API、回滚、发布者、
Pause/Stop/World/程序集生命周期与拒绝不影响游戏；
UI 的 `DebugDrawOverlay`、`DebugDrawLabelTest` 与 `EditorUiTest.DebugDraw*`
覆盖几何顺序、标签转义/槽位、静默控件和值请求及多窗口布局。
外部生产截图脚本 `tool/debug/capture_draw_debug.py` 不注册为 runtime test。

动画 P0 工具使用可选 `VULKANLEARN_BUILD_ANIMATION_TOOLS=ON`；
构建 `vl_animation_probe` 和 `animation_preflight_tests` 后，运行
`ctest --test-dir build -R "^(AnimationSampling|AnimationDependency|UnrealProjectInventory|AnimationFbxInspection|SkeletalImport|SkeletalBlend|AnimationRetarget|SkinRenderHistory|MotionHistory|MotionPrediction|MotionFeatures|MotionRuntimeQuery|MotionDatabase|MotionDatabaseInputs|MotionMatching)\." --output-on-failure`。
它验证依赖采样、只读盘点与 FBX 审计，不替代 GASP 导出或 VL 动态渲染验收。
范围见 [动画预检工具](animation-preflight-tools.md)。
P1 CPU 导入与采样、九影响权重及 MikkTSpace 顶点展开由 `SkeletalImport` 覆盖，
边界见 [动画资产导入](animation-asset-import.md)；真实资产数值对照是外部工具验收。
P2 播放、bounds 与提交历史分别由 `SkeletalImport`、`SkinRenderHistory` 覆盖；
GPU 画面对照使用 `tool/animation/capture_gasp_p2.py`，不注册为 CTest 或 runtime test。
当前实现见 [动画播放与 GPU 蒙皮](animation-runtime-skinning.md)。
P5 搜索、运行时输入绑定或查询变动还对应 `MotionMatching`、`MotionRuntimeQuery`、
`ScriptAnimationWorld` 和 `ScriptAnimationTest`；候选表对应 `EditorUiTest.MotionMatching*`。
只跑动画 CPU 集不覆盖 CLR/Jolt/World 桥接，只跑旧 P4 轨迹或直接选片段测试不覆盖正式 MM 控制器。
正式效果的外部截图入口为 `tool/animation/capture_gasp_p5.py`；
默认覆盖 idle/start/walk/stop/reverse/run/jump/land，每种动作保留原生反馈与实际截图，
失败报告和 fallback 截图保留，不能以“有图”替代目标动作验收。

2026-10-17 蓝图视口拾取、变换和文档历史增量：
`editor_ui_tests` 全量连续两遍均为 567 项、562 通过、5 项符号链接环境跳过；
`script_tests` 120 项与生产 facade 11 项连续两遍通过，既有
`editor_viewport_tests` 41 项通过。覆盖镜像/非均匀缩放/shear 父链的包围盒射线、
近远面裁剪、局部 Euler 连续旋转、缩放符号、真实 RmlUi 指针拾取与单次释放提交、
取消/零位移/旧预览门禁、历史 revision/冲突/预算/保存语义，以及失败请求完成反馈。
冻结 bounds 测试使用真实 World 与可见/隐藏 mesh sections，核对值副本不随 World 修改。
C# ON main、编辑器/生产测试与资产 CLI 构建通过，Release C# OFF main 构建通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、同步提交及 epoch 退休排空检查通过，正常 teardown。
文档校验与 65 条架构边界审计通过；本轮没有重新扫描全量 CTest，
此前记录的 6 项 `RuntimeControlAssets` 既有失败未作为已修复。

Computer Use 实窗使用既有独立 `SC_actor_tab_acceptance_20261017` 资源域，
在蓝图中央图像点击网格后 Components/Details 同步选中 visual；
移动 X 从 0 变为约 0.41355，Undo 恢复 0，Redo 恢复该数值。
随后旋转 Z 从 0 变为约 28.829°，缩放 Y 从 1 变为约 1.46714，
正式预览显示对应形变；逐步 Undo 恢复全部初始 TRS，dirty 标记消失。
1280×720 客户区及 1920×1008 最大化布局正常，Alt+F4 后日志确认
teardown complete，stderr 为空，验收修改没有保存。
Esc/失焦/旧预览取消路径有模块覆盖，本轮未在桌面逐一操作；
拖动中的模型连续刷新不在当前实现范围，候选轮廓与释放后正式预览分别验收。

2026-10-17 Actor 类型化属性、灯光组件与程序集重载增量：
`script_tests` 全量 118 项连续两遍通过；`editor_ui_tests` 全量连续两遍均为
528 项、523 通过、5 项符号链接环境跳过。覆盖向量分量连续编辑、枚举/稳定引用编码、
实例权限、父链灯光变换与冻结、故障隔离，以及同路径程序集替换、默认值继承、
失败保留、陈旧上下文拒绝和多轮卸载后的委托/计时器回收。
另将测试程序、原生 DLL 和完整 managed 目录复制到独立目录，以新进程单独运行
`ScriptDeployment.ExecutableRelativePrivateRuntimeHostsGameplay`，核对实际加载的
`coreclr.dll` 位于副本 `managed/dotnet` 下，1 项通过；不能用开发机 SDK 加载成功替代此证据。
C# ON 全部目标、Release C# OFF main、相关模块 CTest 72 项通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，
回滚、同步提交与退休排空检查通过，正常 teardown，stderr 为空。
本轮没有重新扫描全量 CTest；下文记录的 6 项 RuntimeControlAssets 既有失败不作为已修复。

Computer Use 实窗使用独立 `SC_actor_tab_acceptance_20261017` 资源域，核对
ACT 中央 Vulkan 图像、环绕/缩放、组件选择、局部旋转保存/重开、
保存后关闭/取消关闭、Scene 未保存草稿在预览往返保留、Undo/Redo 与最大化布局。
随后灯光场景核对 Play 脉冲、Pause/Stop、Reload C# assembly 成功反馈，
inline 点光强度 1000→100 后画面变暗，Expose Vector3 的 X 改为 0.25 后 Y/Z 保留。
灯光场景修改仅留在草稿，没有保存。两次窗口均通过 Alt+F4 正常关闭，
日志含 teardown complete 且 stderr 为空。
关闭丢弃、中键平移、枚举下拉选择和其它引用控件没有在本轮实窗操作，
其模块覆盖不等于桌面验收。此前中止的验收记录仍保留为历史。

2026-10-13 [标准资产接口](asset-modify-api.md) 新增 `AssetDocumentTest` 31 项、
生产 facade 1 项、Save Content 路由 1 项。覆盖三类协议/能力、完整候选与回滚、
stale revision、外部冲突、Win32 文件锁导致原子替换失败及重试、别名、
slot/actor 身份、嵌套环境校验、JSON 预算/重复键、共享 Scene 草稿和加载前 UI 编辑门禁。
其中 symlink 例因当前 MinGW 文件链接功能不可用跳过，不能作为已验收的链接重定向证据。

全部目标编译通过；`editor_ui_tests` 全量连续两遍均为 450 项、445 通过、5 跳过。
全量 CTest 792 项中 780 通过、6 跳过，仍有上述 6 项 `RuntimeControlAssets`
读取已删除页面失败。标准资产与相关模块没有新增失败。
文档校验、32 个场景 schema 检查、架构审计通过。
`--world-graph-transaction-test --exit-after-tests --no-dev-ui` 退出码 0，事务/退休检查通过；
该结果仍不代替启用编辑器的交互验收。

JSONL CLI 在隔离 Scene/SM/T 副本上执行读/改/校验/保存共 12 条命令，全部成功。
computer-use 实窗验收使用 `Generated/AssetModifySmoke-20261013/` 副本：
运行中 Console 六条 JSON 读/改命令成功；Save Content 同时列出 Scene/Mesh/Texture，
确认后分别写入三个副本。随后场景 Camera FOV 80→55 只暂存，
正式 `loadworld` 后 Details 显示 55、画面视角变化，而磁盘仍为 80；
再次明确保存才写入 55。File→Exit 正常关闭，日志含 teardown complete，
无 Vulkan validation 错误。正式资产未改，临时副本在验收后清理。

| 改动范围 | 最小验证 | 额外风险验证 |
| --- | --- | --- |
| `documents/**`、`AGENTS.md`、`README.md` | `py -3 tool/validate-docs.py` | 路由覆盖是否仍完整（脚本会 note 未被任何路径路由的契约） |
| 局部实现（单模块内） | 该模块的测试目标 + `ctest -R <该套件>` | `tool/architecture-boundary-audit.ps1` |
| `source/ui/**`、`source/editor/**` | 编辑器 UI 全套 + 构建 `main` | 真实出图（见下） |
| `source/script/**`、`script/**`、Actor 场景加载 | `script_tests` 连续两遍 + C# ON/OFF 构建 | UI/保存集成、实窗 Play/Pause/Stop、World 事务 |
| 共享接口、状态或资源契约（`source/world/**`、`source/renderGraph.*`、`source/renderSystem.*`） | 编译全部测试目标 + 相应套件 | `--world-graph-transaction-test`、边界审计 |
| 逐 pass GPU 计时与 Performance 快照（`source/core/performanceSnapshot.h`、`source/render/backend/gpuPassTimer.*`、`gpuPassTimings.h`、`source/renderSystem.*` 的 timestamp 录制） | `vl_renderer_backend_tests --gtest_brief=1`；Debug 构建 `main.exe --gpu-stats` 跑任一 GPU 场景，确认 `[GPU]` 表打印且 stderr 无 `VUID`；`--console-command "perf json"` 输出单行 JSON | Release 场景对照；`gpustats` 控制台按需采集；`--perf-jsonl <path>` 逐行解析 |
| Performance 页（`source/editor/ui/editorPerformancePanel.*`、`ui/editor-workspace.*`、`editorDockingController.*`） | `editor_ui_tests --gtest_filter=EditorPerformancePanelTest.*:DockRmlTest.*` + 构建 `main` | 实窗截图中 Performance 页已填充，摘要与 `perf json` 同快照一致 |
| 启动参数（`source/engine/launchOptions.*`） | `engine_launch_options_tests --gtest_brief=1` + 构建 `main` | 实际启动带该 flag 的场景 |
| shader（`shader/glsl/**`） | 编译 `main`（启动即编译 shader，缓存命中/失效见 `contracts/shader-build-cache.md`） | `--shader-reload-test`、`--shader-compute-reload-test` |
| schema（`schema/*.schema.json`） | 该 schema 的校验脚本（见 `schema/README.md`） | 加载对应资产的场景是否能起 |
| 门禁脚本本体（`tool/architecture-boundary-audit.ps1`） | 运行它自身 | `tool/architecture-final-validation.ps1` |

## 运行时测试命令（受限集合）

只有以下三个 runtime test 标志被支持，且**必须串行**执行（它们共用 `shader/spv/`）：

```powershell
.\build\bin\main.exe --shader-reload-test --exit-after-tests
.\build\bin\main.exe --shader-compute-reload-test --exit-after-tests
.\build\bin\main.exe --world-graph-transaction-test --exit-after-tests
```

不要新增 runtime test 标志，除非用户明确同意且能说明模块测试为何不足。退出码 0 = 通过，2 = 测试失败。

## 手工与像素验证

端到端渲染改动没有自动化门禁，必须出图核对。**两个已知陷阱**：

1. **`main.exe` 不会自行退出，也没有 quit 控制台命令。** 不自己终止就会泄漏进程并锁住
   `build\bin\main.exe`，使下一次构建失败。
2. **截图 BMP 可能是半截文件。** 读取像素前必须确认文件大小稳定且达到预期
   （1280×720 的 24 位 BMP 载荷是 2764800 字节）。

```powershell
# 启动、等截图落盘、再终止（PowerShell 下命令串需要自带引号）
$p = Start-Process -FilePath ".\build\bin\main.exe" `
    -ArgumentList '--dev-ui','--console-command','"screenshot check.bmp"','--console-command-delay','150' `
    -RedirectStandardOutput out.log -RedirectStandardError err.log -PassThru
$shot = "..\VukanLearnResources\Generated\Screenshots\check.bmp"
for ($i = 0; $i -lt 90 -and -not (Test-Path $shot); $i++) { Start-Sleep -Seconds 1 }
Start-Sleep -Seconds 1
Stop-Process -Id $p.Id -Force
```

`--console-command` 的引号规则与调用方有关：PowerShell `Start-Process` 需要手写引号，
Python `subprocess`（列表形式）**不要**加引号，否则引号会作为字面量进入 argv。

像素读取工具：`py -3 artifacts\rmlui-editor\bmp_probe.py <bmp> x,y x,y ...`（会报告载荷大小并对
截断像素报错）。`artifacts/` 被 gitignore，因此**结论必须写进文档**才能持久化。

**强制终止的进程不构成正常 teardown 的证据**，报告时必须区分"渲染证据"与"关闭流程证据"。

### 视觉回归检查（按需使用，**不进固定流程**）

> **不要把它当成每次改动都要跑的步骤。** 它要起视觉模型、走一次多模态往返，成本远高于上表里
> 任何一条命令。**只在"渲染出来的字"本身是争议焦点时用它**，例如：
> 有人报告某个标签不显示、某个面板标题对不上、或你刚改过标签栏/菜单栏的布局与 CSS。
> 常规改动靠上表的模块测试 + 像素探针就够了。

它的定位是**排查与佐证工具，不是门禁**。像素探针只能证明"某个坐标画了东西"——它曾把一个
渲染完全空白的 94px 选中标签报成"存在"。`tool/check_ui_visual_regression.py` 补上另一半：
让视觉模型读出**实际渲染出来的字**，再与 `ui/editor-workspace.rml` 里 author 的标签比对。

**期望值不写死在脚本里**，全部从 RML 提取（菜单标题、工作区标签、产品名），所以改 markup 后
不需要同步第二份清单。它检查两件事：

1. RML 里声明的每个菜单标题与标签都能在帧里读到；
2. 模型报出的**每个标签都必须有文字** —— 这正是"空标签"缺陷的判定条件。

三步流程（视觉模型在 harness 后面，所以接不进 ctest；也正因为成本高，**不要**接）：

```powershell
# 1. 打印要交给模型的提示词（含截图路径与回复 schema）
py -3 tool\check_ui_visual_regression.py prompt

# 2. 把该提示词交给 deepseek-v4-flash-vision-exp（见 deepseek-vision skill）
#    把它的 JSON 回复存成文件

# 3. 校验回复（不匹配则非零退出）
py -3 tool\check_ui_visual_regression.py check --reply artifacts\rmlui-editor\visual-reply.json
```

提示词**不告诉模型期望答案**，所以它能发现"界面变了"，而不只是"界面符合我的预期"。

**这个检查器的有效性是被注入测试证明过的**，不是只跑过一次绿灯：
`py -3 tool\test_ui_visual_checker.py` 会依次注入 5 种缺陷（空标签、标签被改名、工具条重复菜单
命令、菜单没渲染出来、多出一个未声明的菜单），逐一确认非零退出，并确认健康回复通过。
它是**纯本地比对，不调用视觉模型**，所以和上面三步不同，**随时可跑、零成本**。

**边界（不要混淆）**：视觉检查证明"我 author 的东西看得见"，**不构成"应该出现的东西都出现了"的证明**。
两者都不能单独成立：RML 里有标签但视觉读不到 → 可能是 CSS/布局没渲染；视觉读得到但 RML 里没有
→ 可能是运行期生成的（资产列表的类型图标就是这样，由面板按资产类型生成；资产编辑器标签同样
是按需创建的，但它在没有打开资产时**不该**出现，所以那里"视觉上不存在"才是正确的）。

## 测试不稳定时的处置

先区分真实回归、环境问题与偶发失败，**不得为迎合不稳定测试改业务行为**。

本仓库有过一次约 20% 概率的间歇性失败（新测试里的悬垂引用），只有**连跑**才抓到。因此：

- 新增或改动并发/生命周期相关测试后，至少连跑两遍再声明通过；
- 失败时保留报告，不要为了让数字变绿而削弱断言；断言确实需要调整时，在提交信息里说明原因。

## 维护

入口、前提、测试 helper 或验证范围变化时同步更新本表。测试目标或套件改名会让
`routing.md` 与本文的 ctest 正则失效——改测试结构时同时更新两处。

### GASP P9 独立包专项

首个包 `artifacts/gasp-p9-traversal-export-01`（1708 文件）从仓库外启动
`SC_gasp_traversal.json`；自动探针完成 1 m Mantle，日志
`P8_TRAVERSAL_END pos=0,190,-199 grounded=True`，窗口约 605 FPS，
正常 teardown，stderr 为空。
矩阵包 `artifacts/gasp-p9-traversal-matrix-01`（1714 文件）通过
`--dynamic-assets` 收入六个 Traversal 场景，覆盖 Vault 越障、
1.6 m/0.5 m 高度拒绝、顶棚净空拒绝、无目标 no-hit 和 airborne abort；
每次运行均正常 teardown，stderr 为空。
使用 `loadworld` 队列命令验证 generation=2 的 Vault World 换代；`shaderreload all` 记录 compiled=16、committed=true、retiredPipelines=18；无负载 1280×720 采样 589–639 FPS；`reloadassembly` 在包内 Editing 提交，Playing 受控骨骼正确拒绝。
gait 矩阵包 `artifacts/gasp-p9-final-package-01`（1725 文件）再验证
Mantle/Vault 的 walk/run 四个场景，均落地并正常退出。
已覆盖 1 m stand/walk/run；med/high、长时间 GPU/压力、真实输入与完整动作矩阵未在包内重跑；P9 的 1 m 独立包子集技术验收完成，完整 M2/最终交付未完成。详见
[P9 集成记录](../rationale/gasp-p9-integration-record.md)。

P10 2.5 m Climb：`SC_gasp_traversal_climb.json` / `_run.json` / `_walk.json`
障碍顶面 250 cm，probe 从局部 `y=120` 检测，`TraversalAction=Climb`
高度窗口 70–300。仓库 `P8_TRAVERSAL_END pos=0,340,-169 grounded=True`；
独立包 `artifacts/gasp-p10-climb-package-05`（1721 文件）为 stand/run/walk
`pos=0,340,-173` / `-151…-196` / `-193 grounded=True yaw=0`，run 连续 10/10
END、0 ABORT，stderr 为空。Mantle/Vault 回归
`190,-202` / `90,-290`。Climb 使用 root delta/dt 覆盖 CharacterVirtual
速度，不走 Jump；stand/run 后半段和 walk 全程竖直闭环，水平仍走根轨迹；
根 yaw 已消费（Climb 实测 0），pitch/roll 未做。2 m/3 m 地面 Mantle/Vault
无可用 clip，见 [med/high 资产审计](../rationale/gasp-medhigh-asset-audit.md)。
详见 [P10 记录](../rationale/gasp-p10-climb-root-motion-record.md)。

P11 2/3 m Hurdle gameplay stub：场景已从主资源移除（历史结果如下）。原 `SC_gasp_traversal_hurdle_2m/3m.json`，
障碍顶面 200/300 cm，1 m `Hurdle_1_0` 身体 + 根竖直 2×/3×，3 m 补有界水平。
仓库 `2m=0,290,-233`、`3m=0,90,-297 grounded=True`；包
`artifacts/gasp-p11-hurdle-package-01`（1724 文件）为 `-246` / `-306`，
stderr 空。该结果只是 gameplay stub，不是 2/3 m 资产验收；真实 clip 仍缺。
详见 [P11 记录](../rationale/gasp-p11-hurdle-stub-record.md)。
P12 输入注入：`injectinput <action> <down|up>` 通过
`InputSubsystem::InjectAction` 合并 gameplay action，不依赖 SDL 焦点。
`SC_gasp_traversal_input.json` 关闭自动探针，合成 `Interact` 触发：
`P8_TRAVERSAL_REQUEST` → `START` → `END pos=0,190,-202 grounded=True`。
`--focus-window` + 真实 F 在仓库/`gasp-p12-input-package-01`(1723) 均到 `END 0,190,-202`；合成 action 与 OS 事件分开记录。详见
[P12 记录](../rationale/gasp-p12-input-injection-record.md)。
P13 生成 2/3 m 资产：资产/场景已从主资源移除，仅保留历史记录。原记录：UE Python `set_bone_track_keys` 生成
`Mantle_2_0_Stub/3_0`、`Vault_2_0_Stub/3_0` 根轨迹，身体复用 1 m Hurdle。
Mantle2/3 落顶 `0,290,-184` / `0,390,-176`；Vault2/3 运行时仍未通过
（停在障碍前/顶面）。这是生成根轨迹资产，不是最终 2/3 m 身体动画。
详见 [P13 记录](../rationale/gasp-p13-generated-2-3m-assets.md)。
P14 Traversal Montage 脚手架：`GaspCharacter` 输出
`P8_MONTAGE_START/NOTIFY/END`，section 来自 UE `Default`，notify 名称来自
UE `Footstep/Handstep`；Mantle 运行到 `END pos=0,190,-202 grounded=True`。
notify 时间目前均分近似，不是 UE 资产时间；详见
[P14 记录](../rationale/gasp-p14-montage-runtime-record.md)。
P15 MotionWarping 核心数学（历史阶段）：`WarpTranslationToTarget` 根方向投影 +
5 个 GoogleTest（Identity/Double/Perpendicular/Zero/Lateral）全部 PASS。
当时尚未接运行时/warp window，P19 已接；详见
[P15 记录](../rationale/gasp-p15-motion-warping-core-record.md)。
P16 Chooser 脚手架：`TraversalAction=Auto` 按 UE 行输出
`P8_CHOOSER height/mode/choice`；1 m 选 Hurdle 并落顶 `0,190,-234`；
2 m 经低/高两档球命中，选 `Catch_Hurdle_high` 后记录
catch-runtime-not-implemented。详见
[P16 记录](../rationale/gasp-p16-chooser-scaffold-record.md)。
P17 UE Montage 时长/窗口：从 34 个 `AM_*` T3D 导出
Climb stand/run/walk = 2.75/3.25/3.6，MotionWarping window
0.405–0.732 / 0.366–0.885 / 0.310–1.294；VL 改时长后三个 gait
都 grounded `0,340,-178/-139/-198`。target 仍未换成 FrontLedgeLocation，
warp 未应用。详见
[P17 记录](../rationale/gasp-p17-ue-montage-timing-record.md)。
P18 运行时 Ledge/Warp：Climb 命中后计算
`P8_LEDGE_CONTEXT front=0,250,-120 backLedge=0,250,-260 backFloor=0,0,-280 depth=140`，warp target 用脚底换算
`0,340,-120`；按 UE 时长/窗口后 stand/run/walk 都 grounded
`0,340,-174/-198/-201`。warp 用 RootMotionRemaining + exact scale+shear；BackLedge/BackFloor 已算但未作为 Vault target。详见
[P18 记录](../rationale/gasp-p18-runtime-ledge-warp-record.md)。
P19 原生 MotionWarping：`MotionWarpRuntime` 在 C++ 拥有窗口/根位移/目标
空间换算，C# 只提交 target/window。Hurdle：
`P8_WARP_WINDOWS native=BackLedge[0.133,0.598] FrontLedge[0.157,0.235] BackFloor[0.164,0.399]`，
END `0,188,-266 grounded=True`。Climb/Mantle/Vault 窗口已提交原生；P19 段落记录的矩阵为
Hurdle `0,180,-280`、Climb `0,340,-207/-207/-210`、Mantle `0,190,-202/-201/-201`、
Vault `0,90,-290`，全 grounded；MotionWarping 11/11、全量 140/140（P19 时；P20 后
16/16、145/145；P22 后 16/16、224/224；P25 后 28/28、255/255）。P20 已裁定 `-280` 是迁移前旧三窗口/C# 提交配置结果、`-266` 是
native 结果（`final-hurdle.log` 16:29:35，资源提交 `91f98e7` 16:37:29）。详见
[P19 记录](../rationale/gasp-p19-native-motion-warping-runtime.md)。
P20 Hurdle 四窗口 + Bone provider：两个 FrontLedge 用 `attach`
（0.078149–0.234903、0.234925–0.398904），BackLedge/BackFloor 为 None；
`dOut=56.810280`（当时由曲线采样值内联，P22 后改由 `CurveValue` 计算），
`subtractRemaining` 全 false；Mantle/Vault 改单次消费原生 warped root：两个 FrontLedge
窗口（Vault stand 另有 BackLedge），两个 FrontLedge 均 `attach`，target 胶囊中心；
`abort_void` 坠到 `y≈-2676` 后 airborne abort，`abort_stall` 为 `stalled progress=10`。
P20 当轮的 Hurdle `0,90,-334`（作者轮 `-333..-354`）只是历史数字，已被 P21/P22 矩阵取代。

P21 场景 reach 重做（历史矩阵，`artifacts/gasp-reach-matrix/RESULTS.md`）：10 个 traversal
场景 ×3 次全部 grounded、无 ABORT、首窗 attractor 30/30 通过。END：Mantle stand
`-164/-174/-170`、walk `-202/-201/-201`、run `-202/-204/-209`；Vault stand
`-380/-377/-375`、walk `-348/-344/-334`、run `-346/-350/-335`；Climb stand
`-231/-188/-229`、walk `-204/-195/-213`、run `-187/-226/-230`；Hurdle(auto)
`-329/-320/-334`。拒绝/中断/输入语义保持：`reject_clearance` / `height=160` /
`height=50` / `no-hit`；`auto_high` `Chooser height=200 -> Catch` 拒绝；`abort_stall`
`stalled progress=10`；`abort_void` `airborne progress=289`；`input` 用
`injectinput Interact down` 触发到 END `(0,190,-173)`。**该矩阵已被 P22 清理后矩阵取代**，
旧值只作历史。

P22 Montage 迁移与清理：引擎 `0356b7f`/`b3a5e79` 执行 notify/notifyState 区间、
`e5a92f6` 接 catalog/`playMontage`/C# `CurveValue`/`Montage`/`PlayMontage`/feedback
`notifies`/`montage`，`0de7bcc` 删除 `skinning.root`（出现即拒绝）、强制
`skinning.skeleton`、缓存键只留 `|skeleton=`、C# 删除 `MotionWarpTarget` 与
`Play(warpTargets)`、`clip` 请求携带 `warpTargets` 显式拒绝；资源 `aa78096` 把 SM 接到
skeleton/22 个 sequence/10 个 montage，`GaspCharacter` 改 `PlayMontage` +
`Montage().Windows` + `CurveValue`，资源 `ac0fe0c` 删除残留 `root`。
Climb walk 真窗口为 `0.983966–1.293677` + `1.293831–1.603542`（21 个窗口与 T3D 0
mismatch；P17 的 `0.309711` 是 Duration）；修复 = 探针 walk 名义 500（reach 350）+
`climb_walk` z=+228 + 竖直闭环推迟到首窗，END `(0,340,-167/-165/-167)` grounded。
清理后矩阵（`artifacts/gasp-post-cleanup-matrix/RESULTS.md`）：10 个 traversal 场景 ×2
全部 grounded、无 ABORT；20/20 次 `P8_WARP_WINDOWS` 与全部 `P8_MONTAGE_NOTIFY` 与迁移后
基线逐字相同。END：Mantle stand `-160/-159`、walk `-202/-202`、run `-204/-202`；Vault
stand `-365/-376`、walk `-335/-340`、run `-346/-338`；Climb stand `-164/-162`、walk
`-168/-155`（extra `-167/-168`）、run `-182/-175`（extra `-163`）；Hurdle(auto)
`-325/-342`（extra `-326`）。特殊场景 8/8 语义保持：`auto_high` Catch、`reject_*`、
`abort_stall progress=10`、`abort_void airborne`（y≈-2435）、`input` END `(0,190,-176)`。
`reject_high` 首次 `exit=2` 为关闭路径交换链 `ErrorOutOfDeviceMemory`，复跑 2 次
`exit=0`，记为环境瞬态。`scriptAnimationTests` 动画相关 42 项（P22 时；`ScriptAnimationTest.*`
34，其中 7 项为 `CurveValue`/`Montage`/`PlayMontage`/notify feedback；
`ScriptAnimationWorld.*` 8，含 `RejectsClipRequestWithWarpTargets`）。Motion DB 的 SK
摘要进入 identity（17 项输入），旧缓存失效并自动重建，playground 运行命中且未改写缓存。
矩阵细节、抖动与未闭环见
[P22 Montage 迁移记录](../rationale/gasp-p22-montage-migration-record.md)。

P23 旋转 warp、Montage section/link 与 Static provider：引擎 `36de338` 执行旋转 warp
（`WarpTarget` 位置+可选旋转、`MotionWarpWindow.warpRotation/rotationTimeMultiplier/
rotationMethod`、`Apply(actorYaw, outYawDelta)`，只执行 `Slerp`），`a74e29f` 执行
section/link（Absolute/Relative/Ratio、最多 10 个 sub-step、段末跳转与 notify 边界/空洞
规则）与 Static provider（`staticPose` → `offset=Root(EndTime)*staticPose^-1`，只加平移），
`4d1475e` 把 Static 字段暴露给脚本数据层；资源 `b627bd0` 给 GASP `FrontLedge` 提交目标
朝向（`MakeRotFromX(-FrontLedgeNormal)` 语义），BackLedge/BackFloor 只提交位置。
旋转接线矩阵：Mantle stand / Climb stand / Climb walk / Hurdle(Auto) ×2 = 8/8 grounded、
无 ABORT，END yaw 0.0、x=0，`P8_WARP_ROTATION` 全为 `yaw=0.0`，窗口/触发/notify 与
P22 基线逐字相同；+20° 目标 yaw 诊断得到 END yaw 20.0、x=-14/-9（证明原生消费目标旋转，
诊断不属于验收）。`scriptAnimationTests` 动画相关 44 项（`ScriptAnimationTest.*` 35，
含旋转重载；`ScriptAnimationWorld.*` 9，含位置/位置+旋转 targets 的接线用例）；
`PlayMontageRejectsUnsupportedExecutionWithDiagnostics` 现为 6 项拒绝
（未知 montage、缺/多 target、非 Linear、loop、rate），多 section/Static 不再作为
script 拒绝项；C# 入口也已接受两者（`8e61949`）。
`animation_preflight_tests` 252（MotionWarping 28、AnimationScriptData 27、
AnimationMontage 24）。详见
[P23 旋转/section/Static 执行记录](../rationale/gasp-p23-warp-execution-record.md)。

P24 Catch 资产与运行时：资源 `3574a3a` 新增 9 个 Catch clip + 9 个 `AM_*`，SM 变为
31 clip / 19 montage（31 sidecar 829,022 B、19 montage 36,839 B、188 notify、
33 warp；`validation.json` `issues=0`）；引擎 `6963a9d` 给导出链加
`--clips`/`--continue-on-error`/`VL_GASP_ANIM_CLIP_IDS` 与 `audit_gasp_catch.py`；
资源 `90125e4` 实现 `MovementMode { Walking, Falling, Catch }`、Falling 入口与
`Catch_Hurdle_low/med/high`。运行期：`SC_gasp_catch_high.json` ×3 全部
`mode=Falling choice=Catch_Hurdle_high` → `P8_CATCH_LANDED y=290` →
`END (0,290,-164/-197/-176) grounded`；7 个既有场景回归通过（Mantle/Hurdle/input 正常、
`reject_high` 仍 `height=160`、`auto_high` 仍拒绝但 reason 变为
`catch-requires-falling`、`abort_void/stall` 保持）。父代理独立复核一次：
`CHOOSER height=176 Falling → Catch_Hurdle_high`、`CATCH_LANDED 2.736/4.333 y=290`、
`END y=290 z=-224`。`animation_preflight_tests` 保持 **252/252**（本轮无引擎改动）。
Catch 的 `BackFloor` 按顶面落点解释（`dOut=0`）是**有意偏离**地面 Hurdle 口径，
依据与未闭环见
[P24 Catch 记录](../rationale/gasp-p24-catch-runtime-record.md)。

P25 Catch 入口时间/家族/自动触发：引擎 `a97cc58` 给两个 `PlayMontage` 重载加
`float startTime = 0`（初始 seek：之前 notify 不补发、根位移从入口积分、窗口按
previousPosition 门控；越界/NaN 拒绝），`animation_preflight_tests` **255/255**（+3：
`PlayMontageStartTimeSkipsEarlierNotifiesAndRootMotion`、
`PlayMontageRejectsStartTimeOutsideSectionZero`、
`MontageSectionChainStartsFromEntryTime`），`script_tests` startTime 用例 3/3
（`ScriptAnimationTest.*` 36、`ScriptAnimationWorld.*` 11，共 47）。资源 `e0b7a8d`
接入口时间、Cliff/Mantle/Vault 家族与 Falling 自动触发，新场景
`SC_gasp_catch_jump.json`。运行期：`SC_gasp_catch_high` ×3 + final 全部
`P8_MONTAGE_START startTime=0.877 entry=PoseSearchBranchIn`，首条 `P8_TRAVERSAL time=1.12`
（旧 0.24，前摇消失），`CATCH_LANDED 2.082/2.198/2.131 y=290`、END y=290；
`SC_gasp_catch_jump` ×3 各 1 条 `P8_CATCH_AUTO_TRIGGER height=77 action=Catch_Hurdle_med`、
`startTime=0.834`、落地 1.702/1.759/1.702 y=290；家族各 3 次：Cliff high y=290、
Mantle med y=201、Vault high y=290（3 m 深台面）。回归：traversal END -174、auto y=90
z=-354、auto_high 仍 `catch-requires-falling`、reject_high `height=160`、abort_void
`airborne`、abort_stall `progress=10`、input Mantle、playground 无 Catch、
`jump_no_wall`/`CatchOnFall=false` 不触发、未知 family 给
`catch-montage-missing … tried=…`。父代理独立复核：`startTime=0.877`、
`CATCH_LANDED 2.064/3.456 y=290`、`END y=290`；jump `height=77`、`CATCH_LANDED
1.723/3.166 y=290`。截图 `artifacts/gasp-catch-entry/catch-jump-landed.png`
（1280×720，墙体 physics-only，画面只有角色）。详见
[P25 Catch 入口记录](../rationale/gasp-p25-catch-entry-record.md)。

P26 Catch 家族/跳跃 gating/视觉返工/PoseSearch 研究：资源 `026d480` 让 Chooser 按顶面
纵深选 Hurdle/Mantle/Vault 家族（阈值 2/3 个胶囊直径），加入 `jumpInitiated` gating
（`CatchOnFall` 默认 true 只认跳跃失败、`CatchOnWalkOff` 默认 false），5 个 Catch 场景
换成 GASP `LevelTextured` 资产（墙 `SM_gasp_055`/`MI_gasp_048`、地面
`SM_gasp_025`/`MI_gasp_018`，无 materialOverride），并新增
`SC_gasp_catch_family_hurdle/mantle/vault.json`；引擎 `1b203a8` 新增 script 测试
`MontageBranchInWindowsAreSamplingRanges`（9 个 Catch 窗口 ≤1.2/30 s、10 个非 Catch
`time==0`，只扫 montage 不扫 clip）。`animation_preflight_tests` 保持 **255/255**；
`scriptAnimationTests` 注册 **48**（`ScriptAnimationTest.*` 36 +
`ScriptAnimationWorld.*` 12）。运行期：家族各 2–3 次——Hurdle depth=100 y=290、
Mantle depth=160 y=288→290、Vault depth=280 y=290；`catch_high`
`family=Hurdle depth=120`、`catch_jump family=Hurdle depth=120 height=77`；
gating：走落默认不触发、`CatchOnWalkOff=true` 触发、`CatchOnFall=false` 不触发、
`jump_no_wall` 正常；回归 8/8。父代理独立复核
`family=Mantle depth=160 height=77 → Catch_Mantle_med`、`CATCH_LANDED 1.938 y=288`、
END y=290。截图 `artifacts/gasp-chooser-families/capture-mantle-landed.png`
（GASP LevelBlock + 地面 + 角色，1280×720）。失败保留：catch_jump 一次越顶
`ABORT stalled progress=180`；Mantle 一次丢窗口 `catch-missed-top`，重试通过。
PoseSearch 研究结论（`artifacts/posearch-branchin-research/REPORT.md`）：branch-in 只
钉采样范围、不选帧；Catch 窗口≈1 帧，VL 取下界与 UE 差 ≤1 帧；非 Catch 家族窗口
2–31 帧，B1（per-member 采样区间 + 独立 traversal DB + 一次性 query，约 4–6 人日）
才是收益点，C（完整 PoseSearch）不建议。详见
[P26 Chooser 家族记录](../rationale/gasp-p26-chooser-families-record.md)。

P27 视觉补齐、`--frame-limit` 与三帧率矩阵：新增启动参数 `--frame-limit <hz>`
（提交 `98d791d`；`source/engine/launchOptions.*`、`engineLoop.*`）。可取 `1..1000`；
省略整个参数时默认 0 = 不限速；只写 `--frame-limit` 不给值 exit 1
（`--frame-limit requires a rate in Hz between 1 and 1000.`），显式传 `0`、非整数或
`>1000` exit 1（`--frame-limit rate must be an integer between 1 and 1000 Hz.`），
两种失败都打印 usage。
它只限渲染主循环节拍：`Tick()` 后按 `SDL_DelayPrecise(remaining - 0.2 ms)` + 自旋
补足到 `1/hz`；`RuntimeClock` 仍按真实经过时间推进，`time.delta` 与动画/脚本 tick 的
dt 随帧间隔变化（P27 时 `ScriptRuntime` 内部仍有 1/60 accumulator 的固定物理子步，
P28 已改为每帧变 dt，见下节）。它是普通启动参数，不是 runtime test flag；
`RejectRuntimeTestConflict` 只覆盖三个 runtime test 标志，三项 Vulkan runtime gate 的
约束与串行要求不变。30/60/120 三档窗口采样 29–30 / 59 / 119（默认 718–781）；
三种无效输入实测都在 0.45 s 内退出、未创建窗口。资源 `882cc90` 之后 18 个
`SC_gasp_traversal*.json` 补 37 个与 physics 同 transform/尺寸的 GASP `LevelTextured`
可见 mesh（障碍 `SM_gasp_055`、地面 `SM_gasp_025`，无 materialOverride，physics 不变），
18/18 截图保存、18/18 期望标记命中，contact-sheet 1248×1626。
仓库内 21 场景 × 30/60/120 共 63 次：60=21/21、120=21/21、30=20/21；30 Hz 仅
`SC_gasp_catch_family_mantle` 确定性失败 2/2（`P8_TRAVERSAL_ABORT
reason=catch-missed-top progress=0`，缺 `P8_CATCH_LANDED`/`P8_TRAVERSAL_END`），
60/120 同场景 `CATCH_LANDED y=288`、`END y=290`——**仓库内 30 Hz 不按通过记录**。
独立包（21 个矩阵场景 + DB/PS，1810 文件，包内 `main.exe` 与仓库同一 SHA256）用同一
二进制跑完三档：60=21/21、120=21/21、30 首跑 20/21 + 重试 1 次后 21/21（唯一首跑
失败 `catch-family-hurdle` `P8_TRAVERSAL_ABORT reason=stalled progress=184
pos=-0,247,-294`，重试 PASS `END y=290`）；64 次运行全部 exit=0、stderr 0 B、包内
config SHA 不变、无残留。包内 30 Hz `catch-family-mantle` 首跑通过，**未复现**
仓库内的 2/2 失败；两个环境在 30 Hz 各暴露一例不同的 catch abort（`stalled` /
`catch-missed-top`），60/120 两边零失败。因此 30 Hz 是 catch 入口/warp 窗口对启动
与帧相位敏感的低帧率脆弱点，P9 三帧率门禁仍未全绿，**不按通过记录**。
`animation_preflight_tests` 保持 **255/255**、`script_tests` 注册 **401**（动画相关
48 不变，本轮未新增测试）；场景 schema **66/0**。详见
[P27 视觉/帧率记录](../rationale/gasp-p27-visual-frequency-record.md)。

P28 UE 帧语义对齐：引擎 `dd62695`（`【Mod】align gameplay simulation with UE frame
semantics`）删除脚本固定步。`ScriptRuntimeSubsystem::Tick` 每帧只调用一次
`tick(dt=frameDt*timeScale)` → `StepPhysics(dt)` → `late(dt)` → `TickScriptAnimations(dt)`
→ `StepPhysics(0)`（只同步 LateTick 指令/接触，不推进时间）；没有 accumulator、没有 `fixed` 回调，
`frameIndex` 每帧 +1。`ScriptPhysicsWorld::Step(states, declarations, dt)` 是唯一物理入口：
角色 `MoveCharacter(dt)` 按帧 dt 推进，内部按 UE CMC `MaxSimulationTimeStep=0.05` /
`MaxSimulationIterations=8` 分段；刚体按 `1/60` 子步、单帧最多 16 步，未达上限保留余量、
超限丢弃整步多余量；`dt=0` 只同步指令/接触。`EngineLoop` 对游戏/动画消费
`simulationDelta=min(rawDelta,0.1 s)`（UE `MaxUndilatedFrameTime`/`MaxSimulationTimeStep`
式 hitch 防护），FPS 与资产/脚本构建轮询仍用原始 dt。C# 删除
`Blueprint/Component.FixedTick`、`Time.FixedDeltaTime/IsFixedTick/FixedFrameIndex`、
NativeBridge `"fixed"` 命令与 FixedTick callback 位；Tick/LateTick 每帧一次、共用同一帧 dt。
测试：`script_tests` **400/400**（1278.8 s；原 401，删 3 个 fixed 用例、加 2 个
per-frame 用例，净 −1），`animation_preflight_tests` **255/255**；main 构建
0 error / 0 native warning（资源侧既有 nullable C# 警告发生在更早的 Gameplay 编译，
最终构建未重编译它）。
运行时只重跑 6 场景 × 30/60/120 Hz × 23 s：30 Hz **5/6**（仅
`catch-family-mantle` 2/2 `catch-missed-top`）、60 Hz **6/6**、120 Hz **6/6**；
矩阵 exe SHA256 `E7CB1432E4E0251A523DFD31BDAE7C2737FCAE213B0A8FFAD0CE4770029D474E`，
最终构建 `E9B95FC573DF9F89EFFF452EA33FCCA7FB57B9816A6F59B55217B8FD3C57AD60`（仅注释差异）。
`catch-high` 经资源 `b75b2de` 把 probe y 由 210 抬到 260 后三档全过（chooser
`mode=Falling family=Hurdle depth=120`、`CATCH_LANDED`/`END y=290`）；`catch-jump`
触发 height 记录为 30/60/120 = 87/78/77，断言已放宽为家族/纵深 + LANDED/END 290，
**height 不是 P28 契约**。30 Hz Mantle catch 是 P28 未闭环项（每帧变 dt + 0.1 s clamp
未修复该 warp 窗口）。**边界：P28 只重跑这 6 个场景；P27 的 21 场景 × 3 档全矩阵是
旧固定步语义，不能称为 P28 已重验。** 与 UE 的剩余差异：动画仍在 LateTick 后求值
（root motion 一帧滞后，本轮未改）、渲染插值未做、UE 引擎级 `bUseFixedFrameRate`
未使用；P28 不是“GASP M2 全部完成”。详见
[P28 帧语义记录](../rationale/gasp-p28-frame-semantics-record.md)。

SpringArm 相机臂基础：`script_tests` 新增 `ScriptSpringArmTest.*` 7 项（owner/arm 旋转组合、
SocketOffset、球扫缩臂、关闭碰撞、无 arm 的相机、schema 拒绝）和
`ScriptBlueprintAuthoringTest.SpringArmFieldsAuthoringRoundTrip` 1 项；注册数从 400 变为 **408**。
运行时冒烟 `Maps/SC_springarm_smoke/SC_springarm_smoke.json` 使用真实窗口 8 s + 截图注入：
exit 0、stderr 空、config 逐字节恢复、无残留 main，截图由 SpringArm 子相机接管
（证据 `artifacts/p28/springarm-smoke/`）。`schema/scene.schema.json` 同步接受
`springArm` 及五个字段，`validate_scene_schema.py` 检查 67 个场景、0 失败。
基础版本当时未做：物理后同步刷新（相机相对同帧刚体有一帧延迟）、相机滞后/平滑、
PawnControlRotation/旋转继承、调试绘制、运行时修改 arm 字段的 API、多 arm/相机优先级、
viewport 预览。本段为历史验证范围，当前能力以 C# 契约的 SpringArm 节为准。
引擎提交 `a76ba40`、资源提交 `7f8b101`；详见
[SpringArm 基础记录](../rationale/springarm-baseline-record.md)。

SpringArm UE 行为对齐（2026-10-21）：当前 `ScriptSpringArmTest.*` 34 项，加编辑保存与实例覆写各
1 项，共 36 项连续两遍通过；相关时钟/回调/增量/重载/物理/作者/资产/DrawDebug 195 项通过。
脚本注册数为 **436**，本轮没有完整重跑 436 项。UI 全量 **670** 项：665 通过，
5 项符号链接权限环境跳过，零失败。Debug MinGW 的 main/script_tests/editor_ui_tests 构建通过。
真实窗口 smoke exit 0、stderr 空、config 逐字节恢复，截图已检查；不作为 GASP 三帧率矩阵验收。
新增桥接内部 camera_prepare/camera_finish 不属于 runtime test 命令，也不推进物理时钟。
证据、首次失败与修正、数值口径及保留差异见
[SpringArm UE 对齐记录](../rationale/springarm-ue-alignment-record.md)。

GASP SpringArm 接入（2026-10-21）：正式角色与翻越探针通过 `blueprint.*`
读/能力/改/校验/保存迁移为 `springArm → camera`；资源控制器只提交世界 `ControlRotation`。
新增两项正式 CLR/Jolt 集成测试验证鼠标俯仰/yaw 环绕及后墙接触球心落点；
正式 GASP 夹具改为消费 BP 的真实组件父链。最终相机专项 42 项连续两遍全过
（34 项 SpringArm、2 项 GASP 集成、30/60/120 Hz 的 90°/跨 ±180° 转向各 3 项），
证据 `artifacts/gasp-springarm-final-tests.log`。没有全跑当前注册的 438 项；
较宽筛选在足部质量用例期间及挂点调整前主动终止，不能视为整套通过。
main/script_tests/vl_asset_edit 构建通过，67 场景 schema、架构审计通过。
实窗 Play/Pause/Stop 已检查；最终挂点改为胶囊中心后再次 Play/Stop，完整角色位于画面内，
stderr 为空。BP 页验证父链、450 cm 臂长和 12 cm 探测半径，窗口保留供检查；
此阶段编辑预览尚不执行臂端求值；后续静态挂接空间见下一段，仍不能作为运行时碰撞或相机 lag 的出图证据。

SpringArm 组件挂接空间（2026-10-21）：组件自身空间与供子级使用的臂端空间分开，
Camera、Scene、Mesh、Light 与嵌套臂共同消费父挂接空间，保留作者相对 TRS；
编辑预览的真实几何、图示、拾取与 gizmo 使用相同的静态空间链。
Debug main/script_tests/editor_ui_tests、C# OFF Release script_tests 与 Development main 构建通过。
UI 全量 679 项：674 通过、5 项符号链接环境跳过；SpringArm 运行时 40 项加内部状态
schema 拒绝 1 项连续两遍通过，OFF 全量 89 项连续两遍通过。
ON 全量首轮 445 项：442 通过、3 项 CustomData 失败：
`SparseObjectOverridesMergeDeclaredDefaultsAndContainersReplace`、
`MigrationCopiesNestedValuesAndAllowsAddedFieldsToUseNewDefaults`、
`MigrationRejectsNestedFloatToDoubleChangeAndKeepsOldData`；
第二轮已复现相同三项失败，未按全量通过记录。
证据 `artifacts/springarm-spaces-ui-final-tests.log`、
`artifacts/springarm-spaces-targeted-final.log`、`artifacts/springarm-spaces-off-tests.log`、
`artifacts/springarm-spaces-script-full-tests.log`。
Development 已加载正式 GASP playground 并启动渲染线程，stderr 为空；
BP 实窗检查被用户 Escape 中止，因此本轮未完成最终 BP 视口视觉验收。
空间选择与保留差异见 [组件挂接空间记录](../rationale/springarm-component-space-record.md)。
