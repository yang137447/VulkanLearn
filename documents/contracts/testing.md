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
| `editor_ui_tests` | 643 | 编辑器 UI：组件、停靠、布局持久化、内容浏览器、JSON 资产库、标准资产修改、actor 树、属性编辑、场景持久化、保存确认、可视化、颜色转换、右键菜单、RmlUi GPU 缩略图桥接、C# 放置/蓝图资产拖放、组件 Details、独立蓝图配置标签、3D 拾取/变换/辅助图示/吸附、文档历史、类型化属性与实例权限、物理配置、编译入口、逻辑组件、Quaternion 原子编辑、Time Scale、Log 页签、四处递归自定义数据编辑、冻结 HUD 文本绑定、C# 类浏览/源码打开请求及一类多 BP 配置创建 |
| `script_tests` | 301（C# ON；OFF 为 75 项无宿主选择集） | 蓝图场景/资产/组件校验、实例权限、地面放置、输入映射、源码 URI 边界；ON 时使用真实 hostfxr/CLR 验证生命周期、Spawn、运行时组件/代际句柄、异步加载/取消/租约、Jolt 层与掩码/compound/静态 mesh/约束/扫球、角色控制、计时器/事件、故障隔离、World 换代、冻结快照、类型化属性、灯光、程序集重载、资源源码编译/监视及 DLL/PDB 完整字节确定性、PDB 空类/partial 源码定位、私有运行时部署、自定义逻辑组件、FixedTick/时钟/性能采样、递归自定义数据、显式播放中深值迁移、增量同步/空回调过滤/千实例负载、HUD 所有权/回滚/迁移、受控程序集限定类名及混合 CLR/Jolt/WorldSnapshot 负载 |
| `vl_offline_adapter_tests` | 101 | 离线场景/网格/显示/相机适配 |
| `editor_viewport_tests` | 41 | 视口几何、命中、坐标映射、aspect、坐标轴投影及编辑器网格 |
| `vl_offline_coordinator_tests` | 13 | 离线会话协调 |
| `vl_renderer_backend_tests` | 44 | 离屏显示输出、GPU 缩略图资源、swapchain 帧结果及同 World 新增绘制对象的资源发布契约 |
| `material_instance_editor_landed_tests` | 10 | MI 已落地接口 |
| `material_instance_editor_tests` | 7 | MI 命令/文档服务 |
| `material_instance_editor_production_tests` | 11 | MI 生产路径（预览边界、直接 render-state 路由、场景/非活动 MI 独立保存）及标准资产 JSON facade/Save Content、C# 草稿连续保存、独立蓝图草稿/保存反馈和历史集成 |
| `game_export_tests` | 19 | Scene/BP/SM/MI/T 依赖闭包、动态资产清单、模型 sidecar、包内配置、PE 原生依赖、私有运行时、源路径约束与失败保留 |

`ctest` 用套件前缀选择：

```powershell
# 编辑器 UI 选择集（缩略图、预览等另有专项套件；全量可直接运行 editor_ui_tests）
# 注意 EditorViewportGridGeometry 与 EditorViewportAxisIndicator 来自 editor_viewport_tests 目标，
# 不是 editor_ui_tests：
# 它必须显式列入，否则改坏编辑器网格不会被这条选择集拦住（2026-10-12 发现并补上）。
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|EditorActorProperty|EditorActorVisualization|SceneDocumentTest|AssetDocumentTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorViewportGridGeometry|EditorViewportAxisIndicator|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure

# 材质实例编辑器全套（当前 28 项）
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

`JsonAssetLibraryTest` 有 4 项按设计 skip（符号链接用例，需要权限）。

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
FixedTick/Jolt 交替与暂停余量、Time Scale、性能快照容量、
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
