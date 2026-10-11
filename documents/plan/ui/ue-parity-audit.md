# UE 风格编辑器：能力对照与轮次审计

日期：2026-10-11。分支：`codex/rmlui-editor`。

这份文档是**工作审计**，不是设计契约。它记录"哪些能力在 ImGui→RmlUi 迁移中丢失了"、
每一轮的验收证据、以及尚未达成一致的判断。正式契约仍在
`documents/architecture/ui-component-system.md`，路线图仍在
`documents/plan/ui/rmlui-editor-plan.md`。

## 为什么需要这份文档

迁移完成后，代码里零 TODO/FIXME，缺口全部表达成显式拒绝或空状态，因此
**"看起来完整"和"确实完整"在源码里无法区分**。判断缺口的唯一可靠方法是把
迁移前的实现当作能力基线逐条对照。

基线命令：

```powershell
git show HEAD:source/editor/ui/materialInstanceAssetEditorPanel.cpp
```

## 能力对照表（HEAD = ImGui 版）

| 能力 | HEAD（ImGui） | 迁移后（RmlUi） | 处置 |
| --- | --- | --- | --- |
| 顶部菜单栏 | 无 | 无 | 第 1 轮新增 File/Edit/Window/Help |
| 视口工具条 | 无 | 无 | 第 1 轮新增；仅 view mode 有真后端 |
| 面板显隐 | 无 | 无 | 第 1 轮新增 |
| 数值滑块 | `ResolveSliderRange` + `ImGui::SliderFloat` | 纯文本框 | 第 3 轮恢复 |
| 颜色色块 / 十六进制 | `LinearToSrgb`/`ToSrgbHexByte` + 色块列 | 无 | 第 3 轮恢复 |
| 颜色取色器 | `ImGui::ColorPicker4` 弹窗 | 无 | **未恢复**，改用方案待定 |
| 折叠分类 | `CollapsingHeader`：General / Render States / Parameters / Texture Bindings | 三个平铺 Heading | 第 3 轮恢复 |
| 资源编辑器返回按钮 | 不适用 | `resource-json-close` 分支存在但无触发控件 | 已删除（见下） |
| 纹理选择弹窗 | `BeginPopup("MITexturePicker")` | `<select>` 下拉 | 不恢复：下拉覆盖同一需求 |
| 场景对象树 | 无（只有 MI 模型列表） | 无 | 未排期 |
| gizmo / 变换编辑 | 无 | 无 | 未排期，需先改场景数据模型 |
| Undo / Redo | 无 | 无 | 未排期；Edit 菜单已留禁用占位 |

**HEAD 也没有**的东西不要记成迁移损失：菜单栏、视口工具条、面板显隐、场景树、
gizmo、Undo/Redo 在迁移前同样不存在。它们属于"向 UE 对齐"的新增工作，不是回归。

## 第 1 轮：菜单栏、面板显隐、视口工具条

- 菜单栏位于 `#editor-toolbar` 行内左侧；`DockWorkspace::Panels()` 未变。
- 面板显隐由 `EditorDockingController` 拥有：隐藏保留布局树位置、隐藏激活标签
  移交激活、整栈全隐藏则折叠出几何、拒绝隐藏最后一个可见面板；布局文件新增
  可选 `"hidden"` 数组，缺该键的旧文件照常可读。
- 视口工具条只有 view mode 接真实后端（`UiActionType::SetDebugViewMode`，
  Lit=0 / Unlit=1）。变换工具与变换空间是**纯呈现状态**，只切高亮、不发命令。
  Wireframe 无后端，按钮禁用并标注，未伪造。

### 证据

- 构建 `editor_ui_tests` / `editor_viewport_tests` / `main` 成功。
- 文档化 ctest 选择 **229/229 通过**，4 项既有 symlink 用例按设计 skip。
- 断言未削弱：一处既有测试的探测像素从 `{50,100}` 移到 `{50,200}`，
  因为原坐标落在新增的视口工具条上。

## 第 2 轮：死代码与命令陷阱

- 删除 `source/editor/executor/materialInstanceEditorCommandExecutor.{h,cpp}`
  （全仓库无引用，`source/CMakeLists.txt` 走 GLOB_RECURSE，无需改构建）。
- `SetMaterialRenderStateOverride` / `ClearMaterialRenderStateOverride` 此前在
  runtime 顶层 switch 无 case，直接提交被判 `InvalidCommandType`，只能经受
  batch。现已直接路由到 `ExecuteEditorCommandBatch` 逐项调用的同一对 service
  方法，并纳入自动预览刷新条件。
- 陷阱由**修复前的真实失败**证明：先加测试 → 失败（`unsupported editor command
  type`）→ 修源码 → 通过。产物 `material_instance_editor_production_tests`
  **22/22 通过**。

## 第 3 轮：恢复迁移丢失的 Details 控件

滑块（含 `ResolveSliderRange` 语义：通道范围 > 参数范围 > 由当前值推导的固定兜底）、
sRGB 色块与十六进制、可折叠分类。色块为只读；HSV 取色器不在本轮范围，原因见上表。

## 待处置项

### `resource-json-close` 死分支：已删除

`resource-json-close` 分支在全仓库没有发出者：`ui/editor-workspace.rml` 只有
`json-close` 与 `resource-close`，`EditorJsonInspectorController` 对前缀
`resource-json` 只发 `expand/search/prev/next`。

它的分支体是 `resourceEditorVisible = false; RefreshResourceEditor();`，
而这两件事已经完整包含在 `resource-close → CloseActiveResource()` 中
（后者还会关闭标签、在没有剩余标签时重新激活视口、清空状态行）。
它既没有独立的触发控件，也没有独立语义，属于重复死代码，因此**直接删除**，
而不是补一个"返回"按钮出来——资源 JSON inspector 当前恒可见，
一个没有第二视图可返回的返回按钮只会制造困惑。

### 截图 readback 可能被半截写入（已观察，未定位）

实测中出现过一个 1267200 字节的 BMP，应为 2764800 字节（1280×720×3 + 54）。
`WriteScreenshotBmp`（`source/render/backend/rendererBackendVulkan.cpp:44-138`）
是同步写完整帧后才返回，因此更可能是**读取发生在写盘过程中**，而不是写盘被中断。
影响限于验证流程：任何读截图做像素断言的脚本必须先校验文件长度，
否则会把未写完的图当成渲染结果。`artifacts/rmlui-editor/bmp_probe.py`
现在会输出实际载荷字节数并对截断像素报错。

## 像素验证方法

本仓库的 agent 模型多无图像输入，`describe_image` 也可能不可用，因此
**界面状态必须用像素值证明，不能用"看起来对"描述**。

```powershell
py -3 artifacts/rmlui-editor/bmp_probe.py <bmp> x,y x,y ...
```

已知对照点（1280×720）：

| 点 | 值 | 说明 |
| --- | --- | --- |
| 顶部工具栏 | `#28282c` | 不透明 CSS 色精确回读 |
| 面板主体 | `#303034` / `#222225` | 同上 |
| 运行时页 `#control-panel` | `#111a2e` | 不透明，非半透明 |
| 状态栏 | `#222225` | 文字像素另计 |

**不透明 CSS 颜色能被精确回读**，这证明 CSS→线性→视口编码的往返正确；
配合 `uiColorTransform.h` 的还原-再预乘语义与 premultiplied 渲染管线，
半透明表面的合成在解析上是正确的。

### "UI 半透明背景不与场景合成"——已定位为 RmlUi 的 `rgba()` 解析缺陷

第 1 轮曾把该现象记为既有限制。**结论是错的，真实原因已实测定位。**
半透明合成一直是正常的；失效的是 `rgba()` 的 **alpha 解析**。

RmlUi 6.2 的 `PropertyParserColour` 在 RGBA 分支对每个分量统一走整数路径
（`extern/RmlUi/RmlUi/Source/Core/PropertyParserColour.cpp` 的 RGBA 循环），
该路径只接受 `0`–`255` 整数或百分比，**小数 alpha 被 `atoi` 截断为 0**。
于是 `rgba(0, 0, 0, 0.55)` 静默变成全透明，没有任何报错。

受影响的声明共 8 处，全部静默失效，现已全部改写为 `#RRGGBBAA`
（`#RRGGBBAA` 走独立的十六进制分支，alpha 正确）：

| 文件 | 原声明 | 现值 | 原本意图 | 可见后果 |
| --- | --- | --- | --- | --- |
| `ui/runtime-control.rcss` | `rgba(12, 18, 34, 0.76)` | `#0c1222c2` | HUD 卡片底色 | 仅在非 developer UI 模式可见 |
| `ui/runtime-control.rcss` | `rgba(127, 166, 255, 0.45)` | `#7fa6ff73` | HUD 卡片描边 | 同上 |
| `ui/runtime-control.rcss` | `rgba(73, 105, 157, 0.45)` | `#49699d73` | 运行时页控件描边 | 运行时页可见 |
| `ui/runtime-control.rcss` | `rgba(92, 126, 184, 0.7)` | `#5c7eb8b3` | 运行时页控件描边 | 运行时页可见 |
| `ui/editor-workspace.rcss` | `rgba(0, 0, 0, 0.55)` | `#0000008c` | About 对话框背板 | 对话框完全没有压暗 |
| `ui/editor-workspace.rcss` | `rgba(0, 0, 0, 0.55)` | `#0000008c` | 脏关闭对话框背板 | 同上 |
| `ui/editor-workspace.rcss` | `rgba(71, 153, 126, 0.30)` | `#47997e4d` | 停靠落点预览 | 拖动时看不到落点高亮 |

`ui/runtime-control.rcss` 另有两处 `rgba(0, 0, 0, 0)`（本意全透明）已改写为
`transparent` 关键字，避免以后被人"顺手修成"小数 alpha 而静默失效。

#### 验证证据（全部为像素实测）

受控扫描：把 `#editor-status` 底色依次替换后各启动一次并截图，
读取 `(900, 706)`：

| 声明 | 回读值 | 判定 |
| --- | --- | --- |
| `#222225` | `rgb(34,34,37)` | 不透明精确回读 |
| `#ff00ff` | `rgb(255,0,255)` | 不透明精确回读 |
| `#ff00ff80` | `rgb(189,15,189)` | 正确半透明；与线性空间 50% 混合解析值一致 |
| `rgba(255,0,255,0.5)` | body 底色 | alpha 被截断为 0 |
| `rgba(255,0,255,0.25)` | body 底色 | 同上 |
| `rgba(255,0,255,0.5)`（无空格） | body 底色 | 同上，与空格无关 |

背板功能性验证（`Help → About` 真实鼠标路径）：打开对话框前后同一坐标，
背板外像素比值约 **0.62–0.70**；"完全不合成"应读回约 1.00。
不同初值下比值一致，符合线性空间 55% 黑的预期（sRGB 非线性使比值高于 0.45）。

**验证方法上的两个教训**（已固化进
`artifacts/rmlui-editor/bmp_probe.py` 与各探针脚本）：

1. 截图可能是**半截文件**。实测出现过 1267200 字节而应为 2764800 字节的 BMP；
   读像素前必须校验文件长度，且要通过"大小不再增长"来判断写盘完成。
2. 判断样式是否生效应优先**像素回读**，不要凭观感下结论。
   本轮一度在"`.hud-card` 设成不透明红仍不可见"上绕了很久，
   最后发现是 `UiSubsystem` 在 developer UI 可见时把 `#hud` 显式设为
   `display: none`——元素本身按设计不显示，与颜色格式无关。

## 尚未实现能力清单（2026-10-11 核对）

以下在 HEAD 与当前分支中都不存在，属于新增而非回归：

- gizmo、变换编辑、场景对象增删改 / 重命名 / 重父级
- Undo / Redo（Edit 菜单为禁用占位）
- 真正的世界大纲：模型树只列出携带有效 MI 的网格，无灯光 / 相机 / 地形 / 环境，
  无层级、无眼睛图标、无搜索、无重命名
- 资产缩略图、模型几何预览、纹理像素查看
- 原生多窗口、IME 输入
- 视口按 dock 分辨率渲染（场景仍按完整 swapchain extent 出图，UI 用完整 UV 采样）
- 材质实例纹理 override 的 live 生效（保存 JSON 后才生效）
- HSV 取色器
