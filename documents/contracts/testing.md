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
| `py -3 tool/validate_scene_schema.py` | 资源工作树可访问 | 每个真实场景都能被 `schema/scene.schema.json` 接受；**schema 不能比 `SceneAssetValidator` 更严**，否则会拒绝引擎实际接受的场景 | 改 `schema/scene.schema.json`、场景 JSON 或 `sceneAssetValidator.cpp` 后 || `pwsh -File tool/architecture-boundary-audit.ps1` | 无 | 65 条命名架构边界规则（禁止词、cutover、所有权、生命周期句柄采用等） | 改 `source/**` 后 |
| `pwsh -File tool/architecture-final-validation.ps1` | 已构建 | 边界审计 + 最终验证矩阵（它调用上面的审计脚本） | 提交前 |
| `cmake --build build --target <target> -j8` | 已 configure | 编译该目标 | 改任何被它编译的源码后 |
| `ctest --test-dir build -R <regex> --output-on-failure` | 已构建对应目标 | 模块测试 | 见下"改动 → 验证" |
| `.\build\bin\main.exe --dev-ui` | 已构建、`config/config.json` 可解析 | 端到端渲染/交互 | UI 或渲染路径改动后（见"手工与像素验证"） |

## 测试目标实测规模

| 可执行文件 | 测试数 | 覆盖 |
| --- | --- | --- |
| `editor_ui_tests` | 276 | 编辑器 UI：组件、停靠、布局持久化、内容浏览器、JSON 资产库、actor 树、可见性、颜色转换、右键菜单 |
| `vl_offline_adapter_tests` | 101 | 离线场景/网格/显示/相机适配 |
| `editor_viewport_tests` | 34 | 视口几何、命中、坐标映射、aspect，以及编辑器网格的几何生成 |
| `vl_offline_coordinator_tests` | 13 | 离线会话协调 |
| `vl_renderer_backend_tests` | 12 | 离屏显示输出契约、swapchain 帧结果 |
| `material_instance_editor_landed_tests` | 10 | MI 已落地接口 |
| `material_instance_editor_tests` | 7 | MI 命令/文档服务 |
| `material_instance_editor_production_tests` | 5 | MI 生产路径（预览边界、直接 render-state 路由） |

`editor_ui_tests` 里 14 个套件；`ctest` 用套件前缀选择：

```powershell
# 编辑器 UI 全套（当前 315 项通过，含 ctest 展开的用例）
# 注意 EditorViewportGridGeometry 与 EditorViewportAxisIndicator 来自 editor_viewport_tests 目标，
# 不是 editor_ui_tests：
# 它必须显式列入，否则改坏编辑器网格不会被这条选择集拦住（2026-10-12 发现并补上）。
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorViewportGridGeometry|EditorViewportAxisIndicator|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure

# 材质实例编辑器全套（当前 22 项通过）
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

`JsonAssetLibraryTest` 有 4 项按设计 skip（符号链接用例，需要权限）。

## 改动 → 验证

| 改动范围 | 最小验证 | 额外风险验证 |
| --- | --- | --- |
| `documents/**`、`AGENTS.md`、`README.md` | `py -3 tool/validate-docs.py` | 路由覆盖是否仍完整（脚本会 note 未被任何路径路由的契约） |
| 局部实现（单模块内） | 该模块的测试目标 + `ctest -R <该套件>` | `tool/architecture-boundary-audit.ps1` |
| `source/ui/**`、`source/editor/**` | 编辑器 UI 全套 + 构建 `main` | 真实出图（见下） |
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
