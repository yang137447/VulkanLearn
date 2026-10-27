# 坑点与设计约束

> 状态：**经验记录，不是契约**。每条都给出适用场景、失败表现、根因、规避动作与证据。
> 规则失效时删除或标注迁移去向，不要保留误导性旧结论。

## 范围与维护

- 覆盖范围：本仓库中**反复出现、代价高、且无法仅从局部代码看出**的约束
- 主要来源：本分支（RmlUi 编辑器与 actor 树工作）已定位并修复的缺陷
- 收录门槛：必须有可复现的失败表现与已确认的根因。**只有一次性的调试过程不收录**

## 记录规则

每条规则应能回答：什么时候触发、会造成什么、根因是什么、应如何规避、证据在哪里。
只适用于某个模块的规则应优先放进该模块契约文档，这里只保留跨模块的共享约束。

## 设计约束与避坑

### [rcss-alpha] RmlUi 的 `rgba()` 小数 alpha 会被静默截断为 0

- 适用场景：任何写 `ui/**/*.rcss` 的地方，尤其是半透明底色、描边、遮罩、拖动预览
- 失败表现：元素**完全透明**，且**没有任何报错**；表现为"样式没生效"，而不是解析错误
- 根因：RmlUi 6.2 的 `PropertyParserColour` 在 RGBA 分支对每个分量统一走整数路径
  （`atoi`），该路径只接受 `0`–`255` 整数或百分比，**小数 alpha 被截断为 0**；
  `#RRGGBBAA` 走独立的十六进制分支，alpha 正确
- 规避动作：RCSS 一律用 `#RRGGBBAA` 表达半透明；需要完全透明时写 `transparent` 关键字。
  **不要用 `rgba()`**，也不要写 `rgba(0,0,0,0)`（虽然它当前"正确"，但改动时会踩坑）
- 证据：受控扫描 —— `rgba(255,0,255,0.5)` / `0.25` / 无空格三种写法回读均为页面底色（alpha 归零），
  而 `#ff00ff80` 回读 `rgb(189,15,189)`，与线性空间 50% 混合的解析值一致。
  本仓库曾有 8 处 `rgba()` 声明因此静默失效（两个对话框背板、停靠落点预览、运行时页描边、HUD 卡片）
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11
- 相关：`documents/contracts/ui-component-system.md` 的颜色格式约定段

### [screenshot-truncation] 截图 BMP 可能是半截文件

- 适用场景：任何读截图做像素断言或人工核对的流程
- 失败表现：读到错误的像素值，进而得出错误结论。实测出现过 **1267200 字节而应为 2764800 字节**的 BMP
- 根因：读取发生在写盘完成之前（`WriteScreenshotBmp` 本身是同步写完才返回，问题是**观察方**过早读取）
- 规避动作：读像素前确认文件大小**稳定且达到预期**（1280×720 的 24 位 BMP 载荷 = 2764800 字节）。
  `artifacts\rmlui-editor\bmp_probe.py` 会报告载荷大小并对截断像素报错
- 证据：一次"UI 半透明背景不与场景合成"的误判，其测量来源就是一张半截图；后续用受控实验证伪了该结论
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [no-quit-path] `main.exe` 不会自行退出，也没有退出命令

- 适用场景：任何脚本化启动 `main.exe` 的流程（截图、手工核对、冒烟验证）
- 失败表现：进程泄漏；`build\bin\main.exe` 被锁住，**下一次构建报 `Permission denied`**
- 根因：应用没有退出路径 —— `source/debugConsole.cpp` 没有 `exit`/`quit` 命令（只有
  debugview / tonemap / bloom / help / offline / loadworld / environment / screenshot /
  camera / shaderreload / shadercache），`Quit` 只是 `UiAction`
- 规避动作：启动后**必须自己终止进程**；等待截图落盘再 `Stop-Process`。
  报告时区分"渲染证据"与"正常关闭流程证据"——强制终止**不构成** teardown 证据
- 证据：本分支一次真实事故 —— 泄漏的进程存活约 25 分钟并锁住输出文件，使并发 agent 的构建失败
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [console-command-quoting] `--console-command` 的引号规则取决于调用方

- 适用场景：用脚本传 `--console-command "screenshot x.bmp"`（命令含空格）
- 失败表现：应用报 `Unknown launch option: x.bmp`，或控制台报 `Unknown command: "screenshot`
- 根因：PowerShell `Start-Process -ArgumentList` 把数组元素**不加引号**地拼接，所以命令串必须自带引号；
  Python `subprocess`（列表形式）会自行加引号，手写引号会作为**字面量**进入 argv
- 规避动作：PowerShell 写 `'"screenshot x.bmp"'`；Python 写 `'screenshot x.bmp'`（不加引号）
- 证据：两种写法在本分支都实际触发过上述两种报错
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [boundary-audit-scope] 边界审计脚本整批覆盖 `documents/`，文档里不能出现禁用词字面量

- 适用场景：写或改 `documents/**`、`README.md` 时
- 失败表现：门禁报 `Vulkan-native architecture wording: 'documents/xxx.md:NN' matches forbidden pattern ...`，
  即使这句话是在**说明这条规则本身**
- 根因：`tool/architecture-boundary-audit.ps1` 的 `Vulkan-native architecture wording` 规则扫描的是
  **整批文档路径**（`README.md`、`documents/` 全部 markdown、契约与计划文档），
  不区分"禁用词的使用"与"对禁用词的引用"
- 规避动作：在文档里描述禁用标识符时**不要写出字面量**，改为描述性的说法
  （例如写"由门禁脚本维护的禁用标识符表"而不是把那个词写进文档）
- 证据：2026-10-11 实测 —— 在 `documents/README.md` 里把某个禁用词当作例子写出来后，
  门禁立刻报该文件命中
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [boundary-audit-false-positive] 门禁的 `Source comment clarity` 规则曾被泛化中文词误伤

- 适用场景：在 `source/**` 写中文注释时
- 失败表现：4 处正常的技术注释被判失败，例如"临时转移焦点""临时视口标记""临时记忆""临时场景"
- 根因：该规则除了禁 `TODO`/`FIXME`，还禁了一批中文 TODO 式词，其中一个是**泛化的"临时"**。
  它在中文技术写作里是正常用词，不是待办标记
- 规避动作：规则已收窄为具体的 TODO 式搭配（"临时方案/临时代码/临时处理/临时绕过"）。
  **新增此类禁用词时应选具体搭配，不要用泛化单词** —— 泛化词会造成误报，而误报会让人开始
  无视门禁，比漏报更危险
- 证据：收窄前 4 处误报，`\bLegacy\b`/`\bmaybe\b`/`\bmigration\b`/`目前暂时` 均 0 命中，
  说明泛化词是唯一误报源；收窄后门禁通过且未损失捕获能力
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [intermittent-tests] 单次通过不代表稳定

- 适用场景：新增或改动涉及并发、生命周期、悬垂引用的测试
- 失败表现：ctest 偶尔失败；本仓库有过一次**约 20% 概率的 segfault**，单次运行看不出来
- 根因：测试自身缺陷（该例为 `sink.commands.clear()` 后的悬垂引用），与产品代码无关，
  但会掩盖真实回归
- 规避动作：新增或改动此类测试后**至少连跑两遍**再声明通过；测试失败时先区分真实回归、
  环境问题与偶发失败，**不得为让数字变绿而削弱断言**
- 证据：该 segfault 由连跑发现，用 gdb 定位后修复，随后 12/12 与 30/30 连跑干净
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

### [gtest-filter-blindspot] 名字不含关键字的测试会被过滤器静默跳过

- 适用场景：用 `--gtest_filter` 或 `ctest -R` 按名字模式挑选测试
- 失败表现：过滤器报"全部通过"，但**少跑了一个**；本分支实测两次：
  (a) 一个右键菜单测试原名 `BrowserAssetRightClickOpensTheMenuAndItsOpenItemUsesTheExistingCommand`
  不含 `ContextMenu`，`--gtest_filter="*ContextMenu*"` 报 12 个通过而实际有 13 个；
  (b) 新增的 5 个网格几何测试住在 `editor_viewport_tests` 目标、套件名
  `EditorViewportGridGeometry`，而编辑器 UI 的 ctest 选择集里没有这个前缀，
  于是它**从未进入那条门禁** —— 该选择集报 302/302 时并不包含它们。
- 根因：模式匹配是"包含"语义，不是"属于这个特性"；测试住在哪个目标与选择集写没写它
  是两件独立的事
- 规避动作：同一特性的测试**命名统一带同一个关键字**；新增测试目标时**立刻**把它加进
  `documents/contracts/testing.md` 的选择集并实测计数变化（302 → 307 就是加法证据）；
  选择集有两份副本（`testing.md` 与 `ui-component-system.md`），改一处要改两处
- 证据：2026-10-12 在编辑器网格与右键菜单两项上各踩一次，均由"数一遍列表"发现而非失败报警
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-12

### [header-inline-constexpr-not-rebuilt] 改头文件的 `inline constexpr` 可能不触发测试目标重编译

- 适用场景：改 `inline constexpr` 常量或 `inline constexpr` 函数，而消费方在另一个目标里
- 失败表现：源码明明改了，测试读到的仍是旧值。本分支在修编辑器网格顶点数时被它拖了数轮：
  同一个断言先后报 1200/612/204，每次数字都"证明"了一个不同的结论，而**那些数字来自不同年代的
  目标文件**。最刺眼的一次是探针打印出 `perQuad=6 actual=1224`，而同一份头文件里 `PushQuad`
  已经在出双份绕序 —— 探针读的是旧 obj。
- 根因：MinGW Makefiles 对这个头文件的依赖没有被完整跟踪；`cmake --build` 认为目标是最新的
- 规避动作：改这类常量后**显式删掉目标 obj 再构建**，或直接触碰 `.cpp`：
  `Remove-Item build\<target>\CMakeFiles\<target>.dir\**\*.obj -Recurse -Force` 后重新构建。
  更省事的判据：**如果同一个断言连续两次给出不同的数字，先怀疑目标文件，不要先怀疑代码。**
- 证据：2026-10-12 编辑器网格；清 obj 后同一份代码一次通过
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-12

### [headless-verification-model] 无图像输入的模型必须用像素回读证明渲染

- 适用场景：由无法查看图片的 agent 验证 UI 或渲染改动
- 失败表现：凭"代码看起来对"或"组件测试通过"声称视觉正确；本分支至少有 4 个真实缺陷
  （眼睛按钮零像素、分组折叠不隐藏子行、`display:flex` 丢文本子节点、隐藏控件导致命令乒乓）
  **全部通过了单元测试**
- 根因：单元测试断言的是命令与 DOM 状态，不是**屏幕上画出了什么**
- 规避动作：改动 UI 后必须出图并做**像素回读**，且先做稳定性对照（两次无命令的连拍必须零差异），
  再做 A/B 测量。断言"发出了命令"不能替代断言"屏幕变了"
- 证据：四个缺陷均由像素探针发现；`artifacts/rmlui-editor/` 下的探针脚本记录了方法与全部测量值
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-11

## 待确认候选

尚未完成证据核查或负责人确认的候选。**实现不得默认依赖这些内容。**

- `[row-height-contract]` `#scene-models .tree-row { height: 30px; }` 是停靠窗口化 spacer 数学的隐式前提。
  已有测量测试覆盖当前三种行类型，但"新增行类型必须保持等高"这条约束尚未写进契约文档的正式条款。
- `[scene-actor-identity]` actor 身份字符串（分组键 + 名称）的稳定性契约尚未明确：
  改名或改分组键会让选择/展开状态失效，当前只靠 World 换代时清空来兜底。

## 维护规则

- 规则失效时删除或标注迁移去向，不保留误导性旧结论。
- 规则改变模块边界、公开契约、资源格式或验证入口时，同步更新相关 `contracts/` 文档与
  `documents/routing.md`。
- 一次性调试过程、提交时间线与已失效的尝试**不收录**；`artifacts/` 被 gitignore，
  因此结论必须写进本文件才能持久化。
