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

### [rmlui-shrink-to-fit-max-width] 收缩盒上的百分比 `max-width` 会被 RmlUi 按 0 解析

- 适用场景：`position: absolute` + `width: auto`（以及 float / inline-block）的 RmlUi 元素同时写了 `max-width: N%`
- 失败表现：元素内容宽度为 0，只剩左右 padding；配 `word-break: break-all` 时文本**逐字一行**，
  看起来像"标题变成竖向"。写 `white-space: nowrap` 会掩盖症状：盒子仍然是 0 宽，只是文字横向溢出，
  肉眼看起来像正常
- 根因：RmlUi 6.2 `BlockContainer::GetShrinkToFitWidth()` 钳制宽度时把 containing block 宽度传死为 0
  （`LayoutDetails::GetMinMaxWidth(min_width, max_width, computed, box, 0.f)`），百分比 `max-width`
  因此解析成 0；`LayoutDetails::GetShrinkToFitWidth()` 再把结果交给 `BuildBoxWidth()`，后者虽然用真实的
  containing block 重算一次上限，但已经救不回被钳成 0 的结果
- 规避动作：百分比上限只写在**有确定宽度**的外层。需要"贴合内容 + 到窗口比例换行"时用两层：
  外层 `position: absolute` + `width: 75%` + `overflow: hidden`，内层 `display: inline-block` 收缩包裹，
  内层不写百分比 `max-width`（RmlUi 6.2 不支持 `width: fit-content`）
- 证据：`ui/runtime-hud.rcss` 的 `#hud` 在 `max-width: 75%` 下内容宽 0、高 450（25 行逐字）；
  改成 `max-width: 961px` 或删除后 173×18；`max-width: 100%` 复现 0 宽。
  运行截图的面板 24px 宽（= 左右 padding）、466px 高，与脱离引擎的 RmlUi 探针逐像素一致
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-10-23
- 相关：`ui/runtime-hud.rcss` 的 `#hud` / `#hud-panel`、`[rcss-alpha]`

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

### [uncached-readback-bandwidth] `HOST_VISIBLE` 不等于 CPU 能快速读

- 适用场景：feedback / readback ring、任何常驻 map 后由 CPU 逐 texel 解析的 GPU buffer
- 失败表现：GPU pass 只有 2–5 ms，但每帧 `submitPresent` 固定多出 ~50 ms；9-tile 1K 地形
  只有 11–14 fps。`TryConsume` 内部 64 KiB cover feedback 约 11 ms、256 KiB layer feedback
  约 36 ms，和算法复杂度完全不相称
- 根因：`HOST_VISIBLE | HOST_COHERENT` 可能落在 write-combined/BAR 型内存，CPU 逐字节
  读取会跨 PCIe 做未缓存访问。`std::map` 不是主因；换成 flat table 后耗时几乎不变，
  换成 `HOST_CACHED` 后同一次回读降到 0.23 ms / 0.38 ms
- 规避动作：CPU 要读的回读 buffer 优先请求
  `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`；没有 cached 型时再退回 coherent。
  聚合用定长/flat 表，不要在每 texel 上做 `std::map` 或分配。不要用“GPU 才几毫秒”
  推断渲染不是瓶颈——先看 readback 内存类型和 CPU 读带宽
- 证据：2026-11-06，RTX 3080，`SC_terrain_scale_09.json`（9×1K 分层地形）；
  修复前 `coverRead≈11 ms / layerRead≈36 ms`，修复后 `≈0.23/0.38 ms`，
  同一场景 CPU `submitPresent` 从 ~50 ms 降到 ~5 ms，稳定态约 40–46 fps（另一次运行
  受 chunk streaming 的 `other` 影响为 ~24 fps），GPU 始终只有 2–4 ms
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-06
- 相关：`documents/contracts/terrain.md` 的 feedback readback 与 summary 段

### [runtime-editor-frame-products] 运行时帧不要构建编辑器专用快照

- 适用场景：`--no-dev-ui`、运行时 HUD、gameplay/线上帧路径
- 失败表现：GPU 只有 3–4 ms，但 `worldSnapshot` 固定约 10.5 ms；9-tile 1K 地形
  在 RTX 3080 上只有 ~49 fps，继续提高帧率时发现 CPU 还在为编辑器做无用工作
- 根因：`PublishSnapshotFromActiveWorld` 每帧无条件构建完整 picking `RenderScene`，
  即使开发者 UI 不可见、根本不会拾取；同时无条件深拷贝整份 `WorldSnapshot` 给
  `currentOfflineSnapshot`，即使不是 offline；`WorldSnapshotBuilder` 还对同一 LOD 组
  的每个级重复计算 AABB/距离/切换状态，并每帧重建 RuntimeId 字符串
- 规避动作：picking scene 只在 `developerUiVisible` 时构建；offline 快照副本只在
  offline 分支创建；LOD 组状态每帧每组算一次；RuntimeId 按 World 代际缓存。
  任何“只服务编辑器/导出”的帧产品都必须显式挂在对应模式上
- 证据：2026-11-06，`SC_terrain_scale_09.json`；修复后 `worldSnapshot`
  `10.6 ms → 6.2 ms`，CPU `20.3 ms → 17.2 ms`，9-tile 稳定段从 ~49 fps
  提升到 53–60 fps（多数帧 58–60），GPU 仍为 3–4 ms
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-06
- 相关：`source/renderSystem.cpp` 的 `PublishSnapshotFromActiveWorld`、
  `source/world/worldSnapshotBuilder.cpp`

### [present-mode-masks-frame-cost] 60 fps 不一定是渲染慢

- 适用场景：GPU 利用率很低、`worldSnapshot`/`submitPresent` 很小，但 FPS 固定在 60 附近
- 失败表现：Release 9-tile 地形 `gpuTotalMs≈0.5`、`worldSnapshot≈0.32`、
  `submitPresent≈1.1`，但 `other≈14 ms`、FPS≈60；容易被误判成 CPU/驱动瓶颈
- 根因：`fifo`/DWM/窗口失焦会按显示刷新率阻塞 present，等待时间被
  `cpuMs - 已测阶段` 计入 `other`；这不是引擎计算
- 规避动作：启动日志读取 `[Vulkan] Present mode: ...`；性能上限验收用
  `--present-mode immediate` 并保持窗口前台；不要把 Debug/validation 的 CPU
  数字当作 Release 上限
- 证据：2026-11-06，Release，`SC_terrain_scale_09.json --no-dev-ui
  --present-mode immediate`；前台稳定 `240.000 fps`（`worldSnapshot≈0.32`,
  `submitPresent≈1.1`, `gpuTotalMs≈0.5`, `other≈2.7`），窗口失去前台后降到
  `60–66 fps` 且 `other≈14 ms`
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-06
- 相关：`documents/contracts/gpu-pass-timing.md` 的 Present 模式与 `other` 段

### [streaming-throughput-follows-fps] 页流式吞吐 = 每帧上传页数 × 帧率，Debug 构建会把"加载慢"放大百倍

- 适用场景：地形层页（VT）补页慢、`terrainlayer stats` 的 `dirtyPages` 越积越多、
  `pending` 不下降
- 失败表现：`--perf-jsonl` 里 `gpuTotalMs≈0.8`（GPU 几乎空转），`submitPresent`
  窗口平均 374→610 ms 且随帧数上升；`terrainlayer forcemip 0` 之后 31 s 仍有
  `dirtyPages=88`、`pending=80`、`uploadedPages=92`，`lastValidEpoch` 只有 11
- 根因：补页速率是 `maxLayerPageUploadsPerFrame(8)` × 帧率。`build/` 是
  `CMAKE_BUILD_TYPE=Debug`（`-O0`、无 `NDEBUG`），同样的代码一帧要几百毫秒，
  帧率掉两个数量级，8 页/帧就退化成十几页/秒；这不是流式架构慢
- 规避动作：性能与流式验收只能用 Release（`build-vscode/release`，`-O3 -DNDEBUG`）；
  现在开启 `--perf-jsonl` 或 `--gpu-stats` 时，非优化构建会在启动期打印
  `[Diagnostics][Warning] This build is not optimized ...`
- 证据：2026-11-08，`SC_terrain_pegasus_4x4.json --no-dev-ui`。
  Release：`terrainlayer forcemip 0`（需求 260 页）后 1.3 s `resident=260`、
  `dirtyPages=80`，2.7 s 时 `dirtyPages=0`、`uploadedPages=260`。Debug：同一命令
  31 s 后 `resident=180/512`、`uploadedPages=92`、`dirtyPages=88`、`pending=80`
- owner / status / last_verified：仓库维护者 / 已确认 / 2026-11-08
- 相关：`source/render/resource/rendererTerrainLoader.h` 的
  `maxLayerPageUploadsPerFrame`、`documents/contracts/terrain.md` 的层页预算段

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
