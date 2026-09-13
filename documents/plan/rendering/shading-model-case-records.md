# Shading Model 验证 Case 记录

本文件是 [`shading-model-alignment-plan.md`](shading-model-alignment-plan.md) 的**证据归档**。

- 每个 case 结束时在这里追加一节，字段固定为 plan §0.4 的七项：
  **ID / 来源 / 验证目标 / 形式 / 实现锚点 / 判定 / 差异归因**（归因不能写"无"）；
- 执行流程见 plan §0.5，状态语义见 plan §0.1；
- `artifacts/` 在 `.gitignore` 里（每次运行重建），**结论只以本文件为持久载体**，
  artifacts 与 `<resourcePath>/Generated/Screenshots/` 下的路径是当次运行的现场证据。

---

## M-07 — 重新钉死 texCoord V 方向与相机 fov 约定

| 字段 | 内容 |
| --- | --- |
| **ID** | `M-07` |
| **来源** | 本仓库探针工件（plan §1.2 / §1.3）；`Common/Source/Models/plot_quad.obj` 的顶点与 UV 定义；`source/sceneNode.cpp:188` `Camera::SetProjection()`；`shader/glsl/M_brdfPlot.surface.glsl` 的 mode 3 分支 |
| **验证目标** | 在**同一次进程、同一次 `tonemap 0` + bloom 0 会话**里，用 mode 3 UV 探针实测 `texCoord ↔ 屏幕像素` 映射，据此判定：① texCoord.y 在屏幕上朝哪边增大；② 相机 `fov` 是水平还是垂直视场角（`halfWidth` / `halfHeight` 各等于什么）；③ 实测曲线行位置与解析期望行位置能否达到 1 像素量级 |
| **形式** | 探针（单次会话四连拍）+ 像素级数值对照 |
| **实现锚点** | `shader/glsl/M_brdfPlot.surface.glsl:144-152`（`plotY` 映射，本次修正处）；`tool/validation/measure_plot_curve.py`（`uv-map` / `curve` 子命令）；`tool/validation/run_paper_case.ps1`（采图 harness） |
| **判定** | **通过**。mode 3：实测映射与相机模型最大逐点误差 `6.5e-4` uv（≈0.9 px）。mode 2（`MI_plot_smith_schlick`，roughness 0.6）：Schlick 161 列平均 **0.249 px** / 最大 **0.484 px**，Smith 23 列平均 0.189 px / 最大 0.418 px。mode 0（`MI_plot_ggx_charlie`，roughness 0.35）：GGX 129 列平均 0.194 px / 最大 0.486 px，Charlie 171 列平均 0.157 px / 最大 0.447 px（每张图另丢弃 3 列两曲线交叉列）。全部 ≤ 1 px，达到 plan §1.9.1 的门限 |
| **差异归因** | ① **探针 shader 的竖直镜像（已修正）**：先前 `plotY = 1.0 - uv.y` 的前提（"uv.y 沿屏幕向下增大"）是错的，整张探针图被竖直镜像——实测同一批数据在镜像假设下误差 ~700 px、在直立假设下 ≤0.5 px，两者相差三个数量级。② **旧的 g1 截图与探针不在同一状态**（见下方自测表），因此 plan §1.3 记的矛盾不成立：矛盾来自"用两套不同 shader / 场景状态的截图互相推断"。③ 无参数面缺口，本 case 不涉及受限项 |

### 实测结论（已写回 plan §1.3）

```text
mode 3 UV 探针（SC_paper_case_brdf_plot_uv，quad scale 4.6x2.6，相机 z=6，fov=45）：
  uv.x =  0.00042240·x + 0.229985     (rms 1.7e-3)
  uv.y = -0.00074636·y + 0.767818     (rms 1.7e-3)
  ⇒ uv.y 斜率 < 0：**texCoord.y 沿屏幕向上增大**，uv.y = 1 在画面顶部
  ⇒ 反推可见范围 halfWidth = 2.4871（模型 2.48528）、halfHeight = 1.3972（模型 1.39797）
  ⇒ 半宽/半高 = 1.780（aspect = 1.778）：**fov 是水平视场角**，
     halfWidth = distance·tan(fov/2)，halfHeight = halfWidth / aspect
  ⇒ 面板铺满整帧（蓝通道=0 覆盖 99.904%），四角为 sRGB 编码后的 uv 值，
     证明输出链路是 pixel8 = 255·sRGB_encode(clamp(shaderLinear))
```

### 由本 case 带出的实现修正

`shader/glsl/M_brdfPlot.surface.glsl` 的纵轴映射改为 `plotY = clamp(uv.y, 0, 1)`（去掉翻转）。
修正前 mode 0 / 1 / 2 的曲线探针图全部是**上下颠倒**的：值 1 被画在屏幕底部。
修正后同一场景重测，误差从 ~700 px 降到 ≤0.5 px。

### 复现方式

```powershell
# 单次会话四连拍（tonemap 0 + bloom 0，切场景用控制台注入）
.\tool\validation\run_paper_case.ps1 -Name 'M-07' -PanelModes uv,curve,curve,curve `
  -Scene 'Maps/SC_paper_case_brdf_plot_uv/SC_paper_case_brdf_plot_uv.json' `
  -Shots  m07_mode3_uv, m07_mode2_g1, m07_mode0_cloth, m07_mode1_hair `
  -Scenes '', 'Maps/SC_paper_case_brdf_plot_g1/SC_paper_case_brdf_plot_g1.json',
           'Maps/SC_paper_case_plot_cloth_solo/SC_paper_case_plot_cloth_solo.json',
           'Maps/SC_paper_case_plot_hair_solo/SC_paper_case_plot_hair_solo.json'

# 判据
py -3 tool\validation\measure_plot_curve.py uv-map <shot> --scale 4.6 2.6 --distance 6 --fov 45
py -3 tool\validation\measure_plot_curve.py curve  <shot> --mode mode2 --roughness 0.6 `
      --curves schlick smith --scale 4.6 2.6 --distance 6 --fov 45
```

现场证据（当次运行）：`artifacts/M-07/20260912_161433/`（`evidence-uv-map.txt`、`evidence-mode2-g1.txt`、
`evidence-mode0-cloth.txt`、`01-run.final.log`、四张 bmp）。
该会话日志确认：单进程内 4 次 `World/graph transaction committed`（generation 1→4）、
`Shader build: hits=21, misses=0`、`Tone mapping mode set to 0`、`Bloom strength set to 0`。
截图同时落在资源库 `<resourcePath>/Generated/Screenshots/m07_*.bmp`。

### 残留与后续

> ⚠️ **版式已于同日由 `M-08` 升级**：本节的截图与数字是"整幅面板当绘图区"那一版的产物。
> 曲线结论（V 方向、fov、镜像修正）不受影响；`M-08` 之后探针面板改为**精确填满画面**
> （scale = halfWidth / halfHeight），uv 变成 `(x+0.5)/W` 与 `1-(y+0.5)/H`，
> 曲线改画在内缩的绘图区里——重测后的数字见 `#m-08`。

- **mode 1（Hair）只采到图，没有数值对照**：它的期望值需要把 `EvaluateHairUeR/TT/TRT` 整条链路
  在脚本里复刻，而这套参数化正是 `H-20` 待定的基准问题（论文 `T^p` vs UE Legacy 经验式）。
  在 `H-20` 定案前不对 mode 1 下结论；
- **`--no-dev-ui` 不等于没有 UI 叠加**：运行时 UI（RmlUi）仍会在画面左上角画一条约 186×14 的
  控件（实测 bbox `x[31..216] y[32..45]`，模式 0 图上延伸到 x≈232）。它近白、会污染"按描边色找曲线"
  的判定。测量工具因此加了两道闸：UI 盒子跳过 + 彩度门限（通道差 ≥0.15）。
  做像素测量时**不要**把测量区放在左上角；
- 旧的 `brdf_plot_g1_only.bmp` / `brdf_plot_three_cases.bmp` / `brdf_plot_two_cases.bmp` 是
  修正前、且场景 scale 还在调整期的样本，**不得**再作为竖直朝向的参照。

---

## M-08 — 探针图的坐标系（轴名 + 刻度数值 + 绘图区约定）

| 字段 | 内容 |
| --- | --- |
| **ID** | `M-08` |
| **来源** | 本仓库探针工件；数学制图规范（盒式轴框、朝内刻度、主 / 次刻度、十倍程标签、竖排 y 轴名）；`shader/glsl/M_brdfPlot.surface.glsl` / `M_brdfPlot.json`；`Common/Source/Models/plot_quad.obj` 的 ±1 顶点定义 |
| **验证目标** | 探针图必须**自带坐标系与曲线身份**：能直接从图上读出"横轴是什么量、纵轴是什么量、刻度值是多少、**哪条线是哪条**"，且"图上写的数"与"曲线定义域"同源（标签由刻度值格式化，图例与曲线用同一份颜色与线宽）。同时把绘图区矩形做成材质参数，让测量脚本用同一组值换算并**从图上核对轴框位置**——版式与脚本脱节时必须报错，而不是给出错误的误差表 |
| **形式** | 架构核对 + 像素对照 |
| **实现锚点** | `shader/glsl/M_brdfPlot.surface.glsl`（`PlotLayout` / 5×7 点阵字体 / `PlotDrawAxes*` / `PlotDrawLegend`）；`shader/glsl/M_brdfPlot.json`（`u_plotRectPixels`、`u_plotTextPixels`）；`tool/validation/measure_plot_curve.py`（`PlotLayout`、`detect_frame`、从材质 JSON 读版式参数） |
| **判定** | **通过**。轴框位置：声明 `x[92,1262] y_screen[85,668]`（上留白 85 / 下留白 52）vs 图上检出偏差 ≤ **1 px**；四角仍是底板色；**等宽描边**在斜率 0→1.96 上折算后的垂直宽度恒定（改前 8.8→16.5 px，肉眼可见"粗细不一致"），最终粗细取**网格线的 2 倍**（半宽 1.44 px）；逐列误差 mode 2 最大 **0.474 / 0.472 px**、mode 0 最大 **0.476 / 0.492 px**，镜像假设仍差 600 px 量级；图头文字与结论数字都用独立方法核对过（见下）。四个探针场景在**单次会话**内一次采齐（`PASS: 4 shot(s) captured`） |
| **差异归因** | ① **描边粗细随斜率变化（已修正）**：原实现是"恒定纵向厚度"的带子（`|value−axisY| ≤ 0.007` 轴坐标），垂直宽度在陡处按 `1/sqrt(1+slope²)` 收缩——实测 mode 2 的 Schlick 在平缓段 16.5 px、陡段 8.8 px。改为把纵向距离除以屏幕梯度长度得到**像素距离**（SDF 标准做法），宽度只由"网格线的倍数"决定；② 由此带出的三类**模型驱动**列剔除（交叉 / 贴边 / 平台过薄）是判据的一部分，剔除数量逐项报出，不做隐式挑选；③ 描边变细后亮核平台从 12 px 降到 4~5 px，逐列误差略升（0.465 → 0.550 px）但仍在 1 px 门限内，属可接受的取舍；④ mode 1（Hair）只验证了"图与坐标轴渲染正确 + 四角检查通过"，**没有数值对照**——期望值模型仍等 `H-20`；⑤ mode 3 按设计**不画任何标注**（叠加会污染 uv 读数），这是刻意的例外 |

### 轴语义与曲线身份（直接对应 §1.5.2 的表）

```text
mode 0  x = θh (deg) 0..90，主 15 / 次 7.5      y = D 归一化 log10 轴 1e-3..1（十倍程 + 2x/5x 次刻度）
        图例: GGX / Charlie
mode 1  x = sinθl  -1..1，主 0.5 / 次 0.25      y = Mp 归一化 log10 轴（同上）
        图例: R / TT / TRT
mode 2  x = cosθ   1→0，主 0.25 / 次 0.125      y = G1 线性 0..1，主 0.25 / 次 0.125
        图例: Schlick (engine) / Smith (exact)
mode 3  无坐标轴、无图例（UV 诊断图，保持纯净）
版式    u_plotRectPixels=(92,52,18,85) px（上留白 85 px = 说明行 + 3 行图例；下留白 52 px = x 刻度值 + x 轴名）；
        u_plotTextPixels=(2,8,1.2,5) px；全部按 fwidth(uv) 换算；轴框 1170x583 px
图头    绘图区**外面、上方**：说明行（这是什么 + 现算的结论）+ 图例各一行（色线颜色与粗细
        都和真曲线一致），都在曲线之前绘制（曲线仍压在最上层）
字体    5x7 点阵，47 个字形（为图例补了 C H R S T X k x z :），逐字形 uint 行掩码
```

**为什么图例必须存在**：只靠颜色区分曲线时，图上没有任何东西说明"绿的是哪条"——
本 case 的第一版就是这样，判定与归档都依赖文字描述。图例把曲线身份放回图内，
`Schlick (engine)` / `Smith (exact)` 的括号注记还顺带写明了 Karis Fig 2 的对照点。

**为什么图头必须在绘图区外面**（这一条是实测出来的，不是审美偏好）：
图例色线的颜色与粗细和真曲线**完全一致**（这是它的价值），所以它会被逐列测量误认成曲线。
实测把图例放进绘图区左下角后，mode 0 的绿色图例线被当成 GGX 曲线，平均误差从
**0.153 px 涨到 4.990 px**、最大 599.9 px。当时的处理是加一个 `u_plotLegendPixels`
保留区参数让脚本跳过——但更好的做法是把图头移到绘图区外面（最终方案）：
既不用加参数，也不用底板与边框，测量脚本的扫描范围天然到不了它。

**结论数字是现算的，并且被独立复算核对过**：

| mode | 图上写的结论 | 独立 python 复算 | 核对 |
| --- | --- | --- | --- |
| 0 | `GGX vs Charlie: normalized peak ratio 13.1` | 13.113679（同 257 点采样约定） | ✅ |
| 2 | `Schlick vs Smith: max G1 deviation 0.226` | 0.226282（逐样本扫 cosθ ∈ (0,1]） | ✅ |
| 1 | `Hair R, TT, TRT: normalized log10 Mp` | ——（描述性文案，无数字） | —— |

**图上文字本身也核对过**：解法是"解出 shader 里的点阵字体表，按字形单元从截图里反向识别"，
三种 mode 的说明行分别还原为
`GGX vs Charlie: normalized peak ratio 13.1` / `Hair R, TT, TRT: normalized log10 Mp` /
`Schlick vs Smith: max G1 deviation 0.226`，与预期字符串逐字一致；图例行另用"墨点数 = 字形
'#' 像素数 × 4"交叉核对（mode 1 的 `R` / `TT` / `TRT` 长度也由此确认）。
这条检查同时证明了"现算数字"确实写进了图里，而不是被漏画。

### 由本 case 带出的三条工程约定

1. **探针面板必须精确填满画面**：`plot_quad.obj` 顶点 ±1，所以探针场景 mesh
   scale 必须取 `(halfWidth, halfHeight)` = `(2.485281, 1.397971)`（相机 z=6、fov=45、16:9）。
   这样 uv 0..1 正好覆盖整幅画面、uv 与屏幕像素一一对应，材质里那些"像素"参数才等于屏幕像素。
   四个探针场景已统一；
2. **标注必须无彩色**：轴框 / 刻度 / 文字 / 网格的通道差 ≤ 0.08，曲线是强彩色。
   测量脚本用彩度门限（≥0.15）把标注排除在曲线判定之外——这条是材质与工具之间的接口，
   以后加任何标注都要遵守；
3. **曲线描边是等宽屏幕宽度，粗细以"网格线的倍数"表达**：实现用
   `|Δ轴坐标| / |∇(Δ轴坐标)|` 得到像素距离；`PLOT_CURVE_WIDTH_IN_GRID_LINES`（当前 **2.0**）
   × `PLOT_GRID_STROKE_SCALE`（0.6）× `u_plotTextPixels.z` 就是半宽（默认 1.44 px）。
   改粗细只动这一个倍数，网格随之不动。测量脚本缺省时**从材质 JSON 反算同一公式**，
   不要在两处各写一个数。

### 实测中踩到并修掉的坑（都写进了代码注释）

- **`layout` 是 GLSL 保留字**：不能拿它当变量名，`PlotLayout layout;` 会编译失败（改用 `plot`）；
- **结构体成员不能叫 `length`**：与 GLSL 内建函数同名，glslang 会把 `text.length` 解析成函数调用；
- **曲线必须裁剪到绘图区**：`PlotAxisX/Y` 对框外片元会 clamp 到 0/1，
  "端点值正好等于 0 或 1"的曲线（mode 2 的 G1）会把整片留白涂成曲线色；
- **轴框必须在网格之后画**：网格主刻度与边框共线，先画框会被盖成网格色，边框核对直接失配；
- **平台中心的偏置来源**：曲线贴到绘图区上下边界时带子被裁一半、两条描边互相覆盖时亮核被啃掉，
  都会让"逐列平台中心"偏移 2~3 px。处理办法是把这三类列**按模型期望**剔除并报数
  （交叉列整列丢；贴边 / 过薄按曲线各自丢），而不是在结果里解释掉。

### 复现方式

```powershell
# 单次会话采齐四张带坐标系的图
.\tool\validation\run_paper_case.ps1 -Name 'M-08' -PanelModes uv,curve,curve,curve `
  -Scene 'Maps/SC_paper_case_brdf_plot_uv/SC_paper_case_brdf_plot_uv.json' `
  -Shots  m07ax_mode3_uv, m07ax_mode2_g1, m07ax_mode0_cloth, m07ax_mode1_hair `
  -Scenes '', 'Maps/SC_paper_case_brdf_plot_g1/SC_paper_case_brdf_plot_g1.json',
           'Maps/SC_paper_case_plot_cloth_solo/SC_paper_case_plot_cloth_solo.json',
           'Maps/SC_paper_case_plot_hair_solo/SC_paper_case_plot_hair_solo.json'

py -3 tool\validation\measure_plot_curve.py uv-map <uv shot>
py -3 tool\validation\measure_plot_curve.py curve <g1 shot> --mode mode2 --roughness 0.6 --curves schlick smith
py -3 tool\validation\measure_plot_curve.py curve <cloth shot> --mode mode0 --roughness 0.35 --curves ggx charlie
```

现场证据：`artifacts/M-08/20260912_184915/`（最终版：坐标系 + 绘图区外**上方图头**（说明行含现算结论 + 图例）
+ 2 倍网格线宽，`evidence-*.txt`、`panel-*.txt`、`summary.txt` = PASS、四张 bmp 与 `01-run.final.log`）。
截图同时落在资源库 `<resourcePath>/Generated/Screenshots/m08h_*.bmp`；更早几版分别是
`m08m_*`（图例在下方竖排）、`m08o_*`（图例单行在下方）、`m08f_*`、`m08lb_*`、`m08lg_*`、
`m08cw2_*`、`m08cw_*`、`m08_*`、`m07ax_*`。

---

## 共享 A｜工具交付（plan §1.5.3）

| 工具 | 位置 | 职责 |
| --- | --- | --- |
| `bmp_reader.py` | `tool/validation/` | 24bpp BMP 读取（负高度 top-down、行 4 字节对齐、BGR 通道序），统一成屏幕坐标；提供 sRGB 编解码 |
| `measure_plot_curve.py` | `tool/validation/` | `uv-map`（实测 UV 映射、核对相机模型与"面板铺满"约定）、`curve`（逐列曲线行位置 vs 解析期望 + 轴框交叉核对）、`panel`（四角 / 铺满检查） |
| `run_paper_case.ps1` | `tool/validation/` | 启动 main.exe、排队 `tonemap 0` / `bloom 0`、按场景序列采图、校验产物、跑四角检查、归档到 `artifacts/<name>/<timestamp>/` |
| `console_inject.ps1` | `tool/validation/` | 往运行中进程的控制台注入命令行（`AttachConsole` + `WriteConsoleInput`），使"一个进程内切场景多拍"成为可能 |

### 工具自测（plan §1.5.3 要求的两张旧图）

| 样本 | 结果 | 说明 |
| --- | --- | --- |
| `plot_cloth_solo.bmp`（mode 0） | GGX 132 列在**镜像**假设下平均 0.232 px / 最大 6.0 px；直立假设 ~378 px | 该图摄于 `plotY = 1 - uv.y` 时期，工具正确识别出它是镜像的 → 采图与判定链路自洽 |
| `brdf_plot_g1_only.bmp`（mode 2） | 只测到 44 列（Smith），直立假设平均 23.9 px；Schlick 未测到 | 该图摄于 12:20:47，早于 12:23:31 的 shader 修改与随后的 framefill 调整：**场景 scale 与 shader 都不是当前状态**，不能作为朝向参照；同时它走的是默认 ACES 色调映射，描边色已不是 shader 的线性色，所以彩度/描边匹配更差 |

自测的两条结论都写进了 plan §1.5.3：① 旧截图只在"工具能否识别朝向/失效样本"这一件事上可用；
② **测量前必须 `tonemap 0` + 关 Bloom**，否则连"按描边色找曲线"都会退化——这正是 §1.5.3 早就写下、
本次被实测再次确认的要求。

---

## 2026-09-12 清理后的复验（`M-07` / `M-08` 结论不变）

旧 shading model 实现与案例资产被整体删除（清单见 `archive/README.md`），其中**直接动到了探针链路**：
`sssComposition` 等 5 个 pass、6 张中间资源、set 3 的 binding 10..13（Hair / Eye / Cloth LUT）全部下线，
`deferredLighting` 回到单输出 `sceneColor`；`base.frag.glsl` 的 ThinTranslucent 输出分支改为编译期错误。
因此必须复验探针结论是否仍然成立。

复验方式：重跑 `run_paper_case.ps1` 的 M-08 四连拍（`artifacts/M-08-postcleanup/20260912_205853/`），
并用同一支 `measure_plot_curve.py` 重算数值。

| 量 | 清理前（本文件记录值） | 清理后复验 | 结论 |
| --- | --- | --- | --- |
| mode 3 UV 映射最大误差 | `6.5e-4` uv（≈0.9 px） | **`1.105e-04` uv**（门限 2.0e-3） | 通过，且更小 |
| 反推 `halfWidth` / `halfHeight` | 2.4871 / 1.3972（模型 2.48528 / 1.39797） | **2.48542 / 1.39807** | 与模型一致 |
| mode 2 Schlick（252 列） | 平均 0.249 px / 最大 0.484 px | **平均 0.179 px / 最大 0.474 px** | 列数一致、误差同量级 |
| mode 2 Smith（248 列） | 平均 0.189 px / 最大 0.418 px | **平均 0.182 px / 最大 0.472 px** | 同上 |
| mode 0 GGX（128 列） | 平均 0.194 px / 最大 0.486 px | **平均 0.168 px / 最大 0.476 px** | 同上 |
| mode 0 Charlie（178 列） | 平均 0.157 px / 最大 0.447 px | **平均 0.149 px / 最大 0.492 px** | 同上 |
| 镜像假设 | ~600 px 量级偏差 | 298 / 400 px（mode 2）、318 / 335 px（mode 0） | 仍被明确排除 |

**判定：通过。** 结论与三条含义：

1. **探针结论（texCoord V 向上、fov 为水平视场角、面板 scale 约定）与清理无关，继续有效**；
   逐列像素误差仍在 0.5 px 量级，plan §1.9.1 的 ≤1 px 门限不受影响。
2. **渲染图收缩没有改变颜色结果**：探针面板走 deferred 路径，清理前它经 `deferredLighting`
   （4 路输出）→ `sssComposition` 合成，清理后由 `deferredLighting` 直接写 `sceneColor`。
   两者数值一致，说明"删掉 3 路拆分"对非 SSS 表面是恒等变换——被删的 `sssComposition.frag`
   在非 SSS 分支里做的正是 `diffuse + nonDiffuse + transmission` 相加。
3. **保留场景未受影响**：同批次另跑了一次保留场景连拍
   （`artifacts/cleanup-verify/20260912_205607/`：`SC_sphere_array` → `SC_car_showcase` →
   `SC_speedtree` → `SC_sifi_head`，四张图均正常出图、日志无 `[Error]`）。

> 复验过程中的一次自伤值得记账：把 `base.frag.glsl` 的 ThinTranslucent 分支换成 `#error` 时，
> 旧的 `#else` 分支体与 `#endif` 残留成了重复的输出块，直接导致所有 forward 材质编译失败
> （`error: 'outSceneColor' : undeclared identifier`），而 **C++ 构建当时是绿的**。着色器只在启动期
> 编译，所以"构建通过"不能替代"跑一次"——这正是 plan §1.5.4 第 3 条要提醒的事。

**2026-09-13 第三轮（死代码与文档清理）后复测**：删掉 `engine/subsurfaceProfileFilter.glsl`、
`engine/virtualLight.glsl`、两个无引用 MF、过期的 pass 产物与生成 include，并移除已死掉的
`ForwardEyeInner` / `ForwardEyeCornea` pass 类型之后，用同一组命令重跑（`artifacts/M-08-final/20260913_123416/`）：

| 量 | 本轮复测 | 与上表 |
| --- | --- | --- |
| mode 3 UV 映射最大误差 | `1.105e-04` uv | 完全一致 |
| mode 2 Schlick / Smith | 平均 0.179 / 0.182 px，最大 0.474 / 0.472 px（252 / 248 列） | 完全一致 |
| mode 0 GGX / Charlie | 平均 0.168 / 0.149 px，最大 0.476 / 0.492 px（128 / 178 列） | 完全一致 |


