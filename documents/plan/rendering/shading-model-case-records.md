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
| `read_plot_text.py` | `tool/validation/` | 从截图里**反向识别探针图上的文字**（图头说明行 / 图例第 1..3 条）：字体表从 shader 解析、位置按材质版式算出、按字模单元回读位图。用于核对"图上写的字符串就是预期字符串"；被运行时 UI 压住的字模标成 `?` 并按通配判定 |
| `verify_geometry_term.py` | `tool/validation/` | 几何项 `k` 变体的离线参考（`D-04`）：三支 `k` 的定义域扫描 + Karis `IntegrateBRDF` 的独立重算，给出"换一支 `k` 会让 split-sum LUT 的 A/B 偏多少" |

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

---

## D-20 / D-21 / D-04 / D-03 — DefaultLit 几何项的两条审计收口（2026-09-13）

计划书 §3.2.1 把「DefaultLit 的 `D-20` / `D-21`」列为全局前置第 3 件（IBL 域的几何项用法与出处），
§1.4.4 把它们记成「公式有出处但用错路径」与「论文里没有的 `α²/2`」。本次按 §1.4.1 的方法
（`pypdf` 抽出论文文本、逐个式号核对）重做，并加了一支离线数值脚本与一支新探针 mode。

**结论先行：两条都是审计标注错误，实现本身与论文一致。**

### 论文原文（本次从书库 PDF 抽出核对）

`yyb-knowledge-book/.../pdfs/karis-2013-real-shading-ue4.pdf` → 抽取文本 `tmp/pdftext/karis.txt`
（该目录在 `.gitignore` 里，按需重建）：

```text
§Specular G：
  "we chose to use the Schlick model [19], but with k = /2, so as to better fit the Smith model for GGX [21]."
     （抽取时希腊字母丢失，实际是 k = α/2 —— 正是 plan §1.4.1 提醒的那个坑）
  "We also chose to use Disney's modification to reduce "hotness" by remapping roughness using
   (Roughness+1)/2 before squaring. It's important to note that this adjustment is only used for
   analytic light sources; if applied to image-based lighting, the results at glancing angles will be
   much too dark."
  k = (Roughness + 1)^2 / 8          (4)
§Image-Based Lighting / §Environment BRDF：
  SpecularIBL() 与 IntegrateBRDF() 都调用 G_Smith(Roughness, NoV, NoL)（节内未印函数体）
```

⇒ 论文给的两支 Schlick 变体分别对应两条路径：**analytic 直接光 → `k=(r+1)²/8`**；
**IBL → `k=α/2`**（Disney 重映射被论文明确限定给 analytic，IBL 剩下的是未重映射的基础拟合）。

### 两条审计项的判定

| 字段 | `D-21`（LUT 里的 k 有没有出处） | `D-20`（analytic 变体有没有被用到 IBL 上） |
| --- | --- | --- |
| **来源** | Karis 2013 §Specular G（上引两段 + 式(4)） | 同上末句；计划书 §1.4.2 `D-b` |
| **验证目标** | `generator/brfdLut.comp` 的 IBL 域几何项到底是哪一支 `k`，它是否在论文里 | `common/lighting.glsl` 的 `GeometrySmith`（`k=(r+1)²/8`）是否被 IBL 路径调用 |
| **形式** | 公式溯源 + 数值积分（离线） | 架构核对（调用点枚举）+ 离线数值对照 |
| **实现锚点** | `shader/glsl/common/microfacetDistribution.glsl:61` `GeometrySchlickGGXIbl`（本次从 `generator/brfdLut.comp` 搬到共享头，数值未改）；`generator/brfdLut.comp` `GeometrySmithIBL()` | `common/lighting.glsl:431` 定义、`:568`（`EvaluateDefaultPbrLightLobes`）、`:614`（`EvaluateNeoXSkinDualSpecularLight`）；IBL 侧 `:273` `SampleEnvironmentBrdf()` → `brdfLut` |
| **判定** | **通过**。当时实现写作 `float a = roughness; k = (a*a)*0.5 = roughness²/2 = α/2`，与论文同式；与"`α/2` 写法"的逐点最大偏差 `0.000e+00`（判据 `1e-12`）。计划书写的 `α²/2` 与实现的偏差：`k` 上 `r=0.6` 时 `0.0648` vs `0.18`，`G1` 上最大 **0.554**。同日按 §1.4.5 第 5 条把变量名改成论文符号：现在是 `float alpha = roughness * roughness; float k = alpha * 0.5;`（纯改名，数值不变；改名前的写法在 `verify_geometry_term.py` 里以 `k_lut_legacy_literal` 保留，仍参与断言） | **通过**（= 与论文要求一致，不存在相悖用法）。全仓库 `GeometrySmith` 只有那两个调用点，**都是 analytic 光源**；`:614` 所属函数当前无任何调用者。IBL 路径不经过它。离线对照：两支变体在 `G1` 上最大差 **0.883**（`roughness 0.016 / cosθ 0.016` 的极端掠射），analytic vs 精确 Smith 最大差 **0.892** —— 也就是说"用错路径"的后果是被论文明确警告过的量级 |
| **差异归因** | **审计标注错误**：`brfdLut.comp` 当时的局部变量 `float a = roughness` 是**感知粗糙度**，不是 α（同一个文件里 `ImportanceSampleGGX` 的 `a = roughness*roughness` 才是 α），审计时按后者读成了 `α²/2`。实现数值无需改动；本次做了三件事：① 把 IBL 那支搬进共享头并命名为 `GeometrySchlickGGXIbl`；② 把变量名改成论文符号 `alpha`（`alpha = roughness²; k = alpha*0.5`），从命名上堵死这类误读；③ 把命名规矩写进 `AGENTS.md` 与 §1.4.5 第 5 条。**三处都不改数值** | **审计标注错误**：计划书把 `GeometrySchlickGGX`（`k=(r+1)²/8`）记成了"IBL 变体"，而它其实是论文式(4) 的 **analytic 变体**；代码注释与测量脚本 docstring 当时也沿用了这个错误标签。三处标签本次一并改正，判据是"哪条路径在调用它"而不是"哪支更像 IBL" |

### `D-04` — 两支 `k` 变体的离线量化

| 字段 | 内容 |
| --- | --- |
| **来源** | Karis 2013 §3 式(5) vs §5 式(7)（计划书原文的对比对象） |
| **验证目标** | 量化 LUT 的 `k` 与直接光 `k` 在 `(roughness, cosθ)` 上的差异。**对比对象按 `D-21` 修正为 `α/2` vs `(r+1)²/8`**，并把被误读的 `α²/2` 一起列出，好让"误读值离真实值有多远"有数 |
| **形式** | 数值积分（离线）：`tool/validation/verify_geometry_term.py` |
| **实现锚点** | `common/microfacetDistribution.glsl` 的 `GeometrySchlickGGX` / `GeometrySchlickGGXIbl` / `SmithG1Ggx`；`generator/brfdLut.comp` 的 `IntegrateBRDF`（脚本内独立重写） |
| **判定** | **通过**（脚本退出码 0）。① 字面写法 ≡ `α/2`：最大偏差 `0.000e+00`；② `G1` 最大偏差：`ibl vs analytic` **0.883**（极端掠射）、`ibl vs smith` 0.159、`analytic vs smith` 0.892、`α²/2` 误读 `vs ibl` 0.554；③ `k` 值域：analytic `[0.128937, 0.5]`、ibl `[0.000122, 0.5]`、误读 `[0, 0.5]`；④ 若把 analytic 变体用到 LUT 上：`max|ΔA| = 0.384`、`max|ΔB| = 0.627`（LUT 17×17、每纹素 512 样本）。`r=0.6` 时 `k_direct = 0.32`、`k_ibl = 0.18` |
| **差异归因** | 差异来自**论文两支变体本身的分工**（analytic 重映射 vs IBL 基础拟合），不是实现缺陷；计划书原句把它描述成"LUT 用了论文没有的式"，属标注错误（见 `D-21`）。数值留档的用途：以后有人想把两支合并成一支时，这两个数就是代价 |

### `D-03` — 新增 mode 4：两支 Schlick 变体 vs 精确 Smith

| 字段 | 内容 |
| --- | --- |
| **来源** | Karis 2013 Fig 2（Schlick vs Smith）+ §Specular G 的两支变体 |
| **验证目标** | 把引擎**实际使用的两支** Schlick（direct / IBL）与精确 Smith 画在同一坐标系里逐列对照，确认"引擎选的到底是哪条"，并让 `D-20` / `D-21` 的结论有一张可复核的图 |
| **形式** | 曲线（新增 `u_plotMode = 4`）+ 图头文字回读 |
| **实现锚点** | `shader/glsl/M_brdfPlot.surface.glsl`（`PLOT_MODE_G1_VARIANTS`、`PlotMaxG1Deviation(bool, float)`、`PlotCaptionGroup()`、mode 4 图例与文案、新增 4 个字形 `+ / ^ b`）；`shader/glsl/M_brdfPlot.json`（`u_plotMode` range 扩到 `[0,4]`）；`Maps/SC_paper_case_brdf_plot/Materials/MI_plot_g1_variants.json`；`Common/Meshes/SM_plot_quad_g1_variants.json`；`Maps/SC_paper_case_plot_g1_variants/` |
| **判定** | **通过**。`roughness 0.6`、1280×720、`tonemap 0` + bloom 0：`direct` 191 列平均 **0.175 px** / 最大 **0.474 px**；`ibl` 191 列平均 0.182 / 最大 **0.456 px**；`smith` 191 列平均 0.180 / 最大 **0.457 px**；镜像假设 242~380 px（被明确排除）；丢弃交叉列 96（三支曲线在 `cosθ→1` 与掠射两端收敛）；四角为底板色 `(101,105,115)`、轴框检出偏差 ≤1 px。图头文字回读（`read_plot_text.py`）：说明行解出 `G1 max dev vs Smith: direct 0.226 ibl 0.089`，与独立复算（256 采样约定）`0.226282` / `0.089427` 一致；图例解出 `Schlick (direct) k=(r+1)^2/8` / `Schlick (ibl) k=alpha/2` / `Smith (exact)` |
| **差异归因** | ① **图例第 1~2 行落在左上角运行时 UI 的包围盒内**（实测 `x[31..216] y[32..45]`）：被压住的字模像素亮度接近文字色，会被读成墨点（实测让 `c` 的最后一行多出一个 bit）。处理办法是文字回读工具把被遮挡字模标成 `?` 并按通配判定（legend0 / legend1 各有 11 个字模如此），而不是猜；② 为写出两支 `k` 的公式，字体表新增 `+` `/` `^` `b` 四个字形（`PLOT_FONT_CODES` 47→51、`PLOT_FONT_ROWS` 329→357，两个表必须同步）；③ 测量工具原先只按"颜色投影 ≥ 阈值"找曲线，而**青色在绿色方向上的投影是 1.13 > 0.98**，于是新图里整条青线会被当成绿线（实测 smith 平均误差 15.07 px / 最大 52.29 px）。补上"最近颜色身份判定"后为 0.180 / 0.457 px；④ 共享头搬迁（`GeometrySchlickGGXIbl`）与 mode 4 的加入对 mode 0 / mode 2 **零影响**——用新 shader 重新采图（`artifacts/D-03-regress/20260913_203814/`）测得 mode 2 Schlick/Smith 平均 0.179 / 0.182 px、最大 0.474 / 0.472 px（252 / 248 列），mode 0 GGX/Charlie 平均 0.168 / 0.149 px、最大 0.476 / 0.492 px（128 / 178 列），与 M-08 记录**逐项一致** |

### 本批带出的代码与工具变更（都不改数值）

1. **共享头新增 IBL 几何项 + 变量名按论文改**：`GeometrySchlickGGXIbl` 从 `generator/brfdLut.comp` 搬到
   `common/microfacetDistribution.glsl`；函数体逐行照搬，只做**改名**——
   形参 `NDotV` → `cosTheta`（与兄弟函数 `GeometrySchlickGGX` 一致），
   局部 `a`/`a*a` → `alpha`/`alpha*alpha`（论文的 α ≡ roughness²）。
   动机有两个：探针要画"引擎真正在用的那支"（数学定义只能有一份）；
   以及 **`a` 的双重含义正是 `D-21` 那次误判的来源，改名叫 `alpha` 才能让代码与论文式号直接对上**。
   同一批改名还覆盖 `DistributionGGX`、`brfdLut.comp` 与 `prefilterEnvMap.comp` 的
   `ImportanceSampleGGX`（全部是纯改名）。数值不变的证据：`verify_geometry_term.py` 的断言
   （当前写法与改名前的写法都 ≡ `α/2`）、以及 mode 0 / 2 / 4 探针用新 shader 重采后误差与
   归档记录逐项一致（见下一节表格）。
2. **三处错误的"IBL 变体"标签改正**：`common/microfacetDistribution.glsl`、
   `M_brdfPlot.surface.glsl` 的 mode 2 注释、`measure_plot_curve.py` 的 `geometry_schlick_ggx` docstring。
3. **变量名改成论文符号（同日，按 §1.4.5 第 5 条）**：α (= roughness²) 在代码里一律写 `alpha`，
   `a` / `a2` 不再用于表示 α。涉及 `common/microfacetDistribution.glsl`（`DistributionGGX`、
   `GeometrySchlickGGXIbl`、`SmithG1Ggx` 的注释）、`generator/brfdLut.comp`、
   `generator/prefilterEnvMap.comp` 的 `ImportanceSampleGGX` / `DistributionGGX`，以及两支工具的镜像实现。
   保留 `a` 的地方都写明理由：`pass/toneMapping.frag` 的 ACES 拟合系数 `a..e` 是**论文自己定义的符号**，
   改了反而对不上来源。
   **数值不变的证据（三层）**：
   - 离线断言：`verify_geometry_term.py` 里"当前写法"与"改名前的写法"都 ≡ `α/2`（偏差 `0.0e+00`）；
   - **像素级**：改名前后各采一遍 `SC_sphere_array` + mode 0 / 2 / 4 探针共四张图，
     **逐像素完全相同**（0 / 921600 个像素有差异、最大通道差 0）——这条同时覆盖了两支 compute 生成器
     （BRDF LUT 与预过滤环境贴图）与 deferred/forward 两条着色路径；
   - 逐列误差：mode 4 三支曲线 191 列最大 0.474 / 0.456 / 0.457 px，与改名前的记录逐项一致。
   现场证据：`artifacts/rename-alpha/`（`before/` 四张改名前的图、`geometry-term-after-rename.txt`、
   `measure-mode4-after-rename.txt` 与三条运行期测试日志）。
4. **`measure_plot_curve.py`**：新增 `mode4` 的期望曲线与配色、曲线身份判定（`color_distance`）、
   兜底 `DEFAULT_PLOT_RECT` 的上留白由 20 改为 85（与材质一致；此前只在读不到材质 JSON 时才会
   悄悄放宽剔除门限）。
5. **`read_plot_text.py`**（新）：把 M-08 的"反向识别图上文字"做成可复现工具，含半像素相位修正
   （shader 的 `pixel = uv * panelSize` 是像素中心坐标，不修正会踩到相邻字模行）与 UI 遮挡通配。

### 复现方式

```powershell
# 1) 公式级：三支 k 的定义域扫描 + split-sum LUT 独立重算（离线，不需要引擎）
py -3 tool\validation\verify_geometry_term.py

# 2) 像素级：单场景采图（自动排队 tonemap 0 / bloom 0）
.\tool\validation\run_paper_case.ps1 -Name D-03-mode4 -PanelMode curve `
  -Scene 'Maps/SC_paper_case_plot_g1_variants/SC_paper_case_plot_g1_variants.json' `
  -Shots d_03_mode4_g1

py -3 tool\validation\measure_plot_curve.py curve <shot> --mode mode4 --roughness 0.6 --curves direct ibl smith
py -3 tool\validation\read_plot_text.py <shot> --row caption --expect "G1 max dev vs Smith: direct 0.226 ibl 0.089"
py -3 tool\validation\read_plot_text.py <shot> --row legend2 --expect "Smith (exact)"

# 3) 回归：mode 0 / mode 2 的旧图重算（检测器改动不应影响它们）
py -3 tool\validation\measure_plot_curve.py curve <g1 shot>    --mode mode2 --roughness 0.6  --curves schlick smith
py -3 tool\validation\measure_plot_curve.py curve <cloth shot> --mode mode0 --roughness 0.35 --curves ggx charlie
```

现场证据（当次运行）：`artifacts/D-03-mode4/20260913_202924/`（`summary.txt` = PASS、`panel-*.txt`、
`measure-mode4.txt`、`regression-mode0.txt`、`regression-mode2.txt`、`text-readback.txt`、
`caption-recompute.txt`）、`artifacts/D-04/20260913_203003/geometry-term.txt`；
截图同时落在 `<resourcePath>/Generated/Screenshots/d_03_mode4_g1.bmp`。

---

## M-02 / M-03 / M-04 + D-01 / D-07 / D-08 / D-10 — DefaultLit 的公式级离线验证（2026-09-14）

这一批是"离线积分类"case：判据本身是**积分恒等式**（归一化、能量），像素只能证明"shader 画的曲线
与解析期望一致"，证明不了"这条曲线积分为 1"。所以证据形式是 **离线脚本 + 既有曲线探针** 的组合：

- 新工具 `tool/validation/verify_brdf_integrals.py`：把引擎公式在脚本里独立积分；
- 既有曲线探针（mode 0 / 2 / 4，≤0.5 px）证明 shader 里的同一份 `DistributionGGX` /
  `GeometrySchlickGGX` / `GeometrySchlickGGXIbl` 与这些公式逐点一致；
- 新工具 `tool/validation/measure_sphere_array.py`：球阵标尺的端到端数值判定。

python 侧的公式镜像不重写：`distribution_ggx` 等从 `measure_plot_curve.py` 导入，
`hammersley` / `importance_sample_ggx` / `integrate_brdf` 从 `verify_geometry_term.py` 导入——
数学定义在工具里也只有一份。

### `M-02` / `D-01`（NDF 归一化与 α 约定）

| 字段 | 内容 |
| --- | --- |
| **来源** | [PBRT 4ed｜Roughness Using Microfacet Theory](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)｜NDF 投影面积归一化 |
| **验证目标** | `∫ D(h)(n·h) dω_h = 1`，并据此钉死 `DistributionGGX` 的 α 约定 |
| **形式** | 数值积分（离线，确定性求积） |
| **实现锚点** | `common/microfacetDistribution.glsl:14` `DistributionGGX`；`tool/validation/verify_brdf_integrals.py` 的 `ndf_normalization` / `ndf_peak_identity` |
| **判定** | **通过**。6 个 roughness（0.05~1.0）上归一化偏差 **≤1.6e-9**（20 万点与 40 万点结果差 ≤1.2e-9）；峰值恒等式 `D(n) = 1/(πα²)`（α = roughness²）相对偏差 **≤4.7e-12** |
| **差异归因** | ① **网格问题被误当成公式问题两次，都记在代码注释里**：均匀 μ 网格 2e5 点时 r=0.05 只积出 **0.868**（峰宽是 α²=6.25e-6，不是 α，欠采样）；"均匀 w = (1-μ)/α²"与均匀 μ **是同一个网格**（雅可比抵消，两列数字逐位相同）——真正的问题是质量集中在 w≲1 而定义域到 1/α²≈1.6e5。改**对数网格**后收敛到 1e-9。② **下界不能省**：`∫₀^{w_min}2/(1+2w)²dw ≈ 2·w_min`，取 1e-6 会让所有 roughness 一致偏低 2e-6（"与 α 无关的固定偏差"正是截断特征）。③ **归一化证明不了 α 约定**：`∫D(n·h)dω=1` 对任何 α 都成立，所以判据补了峰值恒等式——漏掉平方会差 `r²` 倍（r=0.05 时 400 倍） |

### `M-03` / `D-01`（G1 值域 / 端点 / 互易性）

| 字段 | 内容 |
| --- | --- |
| **来源** | [Heitz 2014 JCGT｜Understanding the Masking-Shadowing Function](https://jcgt.org/published/0003/02/03/)｜§3–§5 |
| **验证目标** | `0 ≤ G1 ≤ 1`、`G1(c→1)=1`、`G1` 随 `c` 单调不减、`D·G/(4|n·l||n·v|)` 在 `l↔v` 下对称 |
| **形式** | 数值积分 + 端点扫描（离线） |
| **实现锚点** | `GeometrySchlickGGX` / `GeometrySchlickGGXIbl` / `SmithG1Ggx`（三种 G 变体都查） |
| **判定** | **通过**。三种变体：超出 `[0,1]` 的最大量 **0**、非单调最大回落 **0**、`|G1(1)-1|` **0**；互易性最大差 **2.2e-16** |
| **差异归因** | 无实现差异可归因；补记一条**判据设计**：`G1` 的三条性质对**近似**与精确解都成立，所以它只能证明"没有明显写错"，不能证明"近似够好"——后者由 `D-02` / `D-03` 的像素对照负责（analytic 变体与精确 Smith 在 `G1` 上最大差 0.892，见 `D-04`） |

### `M-04` / `D-07`（白炉；**含一条越界发现**）

| 字段 | 内容 |
| --- | --- |
| **来源** | [PBRT 4ed｜Reflection Models](https://pbr-book.org/4ed/Reflection_Models)｜White furnace test |
| **验证目标** | conductor 与 dielectric 的单次散射半球积分 ≤ 1（不允许凭空造能量） |
| **形式** | 数值积分（离线；NDF 采样估镜面、余弦采样估漫反射） |
| **实现锚点** | `common/lighting.glsl` 的 `fresnelSchlick` / `EvaluateDefaultPbrLightLobes`（`F0 = mix(0.08·specular, baseColor, metallic)`、`diffuseWeight = (1-F)(1-metallic)`）；`verify_brdf_integrals.py` 的 `specular_albedo` / `diffuse_albedo` |
| **判定** | **镜面 lobe 通过、组合项不通过**。镜面 lobe 在 conductor(F0=1) / dielectric(F0=0.04) / 半金属(F0=0.5) × 三种 G 变体共 9 组配置上最大 **1.0000**（判据 1+1e-3）；**diffuse+specular 组合**最大 **1.5458**（dielectric、精确 Smith、roughness 0.05、cosθv 0.1）——analytic/IBL/Smith 分别是 **1.0606 / 1.5325 / 1.5458** |
| **差异归因** | **越界来自"论文没规定的那一步"所用的平衡方案，不是某个 lobe 写错**——出处链已查清（2026-09-14，PDF 抽文本逐字核对）：<br>① **我们有这一项、Karis 2013 没有**：Karis 2013 §Shading Model 的 diffuse 是裸 Lambertian——正文原话 *"We evaluated Burley's diffuse model but saw only minor differences compared to Lambertian diffuse (Equation 1), so we couldn't justify the extra cost. … As a result, we didn't invest much effort in evaluating other choices."*，紧随其后的**式(1)** 为 `f(l,v) = c_diff`（*"Where `c_diff` is the diffuse albedo of the material."*，即纯 Lambert，没有任何能量耦合）；全文 `(1-F)` 只出现 2 次且都在 split-sum 代码里（`F = (1-Fc)*SpecularColor + Fc` 与 LUT 的 `A += (1-Fc)*G_Vis`）；**Burley 2012 全文 `(1-F)` 出现 0 次**。所以"实现偏离论文"这个猜测不成立——**Karis 根本没规定 diffuse 与 specular 怎么合并**。<br>② **这一项的真实出处**：**Neubelt & Pettineo 2013**（The Order: 1886 课程笔记，本计划的既有来源）§Diffuse BRDF **式(15)**：*"the diffuse term is balanced using the inverse of the Fresnel term from the specular component **Shirley [1991]**"*，原始来源是 **P. S. Shirley 1991 博士论文**（*Physically based lighting calculations for computer graphics*）。引擎写的 `(1-F(v·h))(1-metallic)` 就是式(15) 加 Disney 的 `(1-metallic)`。<br>③ **出处自己就写明了代价**：*"It should be noted that balancing the diffuse term in this manner **violates Helmholtz reciprocity**, which can cause issues with certain rendering techniques. If reciprocity is desired, an alternative balancing scheme is proposed in **Shirley et al. [1997]** that satisfies reciprocity at the cost of additional instructions."* —— 即"点取值 Fresnel 做平衡"是有已知缺陷的工程方案，不是能量守恒的推导结果。<br>④ **为什么必然在掠射越界**：掠射下 `v·h` 在大多数 L 方向上仍接近法线（cosθv=0.1 时实测 `v·h≈0.74` → `F≈0.04`），Fresnel 的上升没有传导到漫反射项：实测 diffuse **0.9398**，镜面再加 **0.6059**。物理上镜面已经拿走约六成能量，漫反射本应被压到 ≈0.4。<br>⑤ **越界大小的排序本身就是指纹**：analytic 变体的 G 偏小（多遮蔽、本身就丢能量）→ 组合 **1.0606**；精确 Smith 的 G 正确 → **1.5458**。**G 越"对"，越界越大**——缺口在 diffuse 侧的平衡方案，不在 G。<br>**候选出路**（2026-09-14 另起一节详列，含逐字引用、印刷缺陷提醒与"不是解"的方向）：**最小改动是 PG'97 的 coupled model**（可分离乘积 `kR_m(λ)[1 − R_f(θ)][1 − R_f(θ')]`，作者自称 *"the first model that produces the matte/specular tradeoff while remaining reciprocal and energy conserving"*）——代价只有两次 Schlick 求值，但那篇的守恒性是对**理想 δ 镜面**推导的，换成 GGX 后需先离线试算（见 A 类第 3 条保留）。**换模型一侧引用最扎实的是 `Kelemen & Szirmay-Kalos 2001`**——Burley 2012 说它 *"total albedo is always 1"*，**PBRT 4ed 的 Further Reading 又独立点名它**（*"a simple diffuse correction is generally unsatisfactory, since the precise amount of energy loss will depend both on the surface roughness and the angle of incidence. Kelemen and Szirmay-Kalos (2001) proposed an improved diffuse-like term that accounts for this dependence."*）；`Ashikhmin & Shirley 2000` 与 PBRT 4ed 的 `CoatedDiffuseBxDF` 分层做法也能解决，但前者属 anisotropic Phong、后者是**随机**分层 BSDF（书中 §14.3 从没声称它"能量守恒"，且不可直接搬到实时 lobe 模型）——都属换模型而非打补丁。另见 Filament 官方文档的旁证：他们的 lit 模型**连 `(1-F)` 都不做**，文档承认 fr+fd 问题但留成 TODO；把那一步去掉反而更糟（同配置 1.61 vs 1.55）。**改任何一条都会改动着色结果，球阵基线（`D-08` / `D-10`）需要重新标定** |

**顺带做的一条互证（③b）**：引擎 split-sum LUT 的 `A+B`（F0=1 时就是镜面反照率）与白炉积分（IBL 变体）
的最大差 **0.0000**——脚本内两条独立路径互相印证，说明 LUT 的估计量与白炉积分估的是同一个量。

#### `D-07` 的候选出路（2026-09-14 调研；"引用已发布做法"这一选项落到实处）

> **现状决定（2026-09-14）**：**维持现状，不动数值**。本 case 按"**有出处（N&P 式15 ← Shirley 1991）
> + 出处自承缺陷 + 实测边界（直接光 1.5458 / 间接光 1.5927）+ 候选与试算结果留档**"登记为已知受限；
> 等按 §0.6 重建 DefaultLit 时再照下面的排序决定是否采纳。本节的价值是**把"要不要改、改了会怎样"
> 变成可查的数字**，而不是留一句"以后再说"。

先把性质说清楚：**这不是"给公式打补丁"，而是"换不换模型"的问题**。候选分两类：

**A 类｜同一处公式的改进（改动最小）：PG'97 的 "coupled model"**

来源：**P. Shirley, B. Smits, H. Hu, E. Lafortune, "A practitioners' assessment of light reflection models", PG '97**
（Neubelt 2013 正文指向的那篇）。作者自存 PDF（原 `cs.utah.edu` 链接已 404，Wayback 可下，需 `id_` 前缀）：
`https://web.archive.org/web/20040409162330id_/http://www.cs.utah.edu/~shirley/papers/pg97.pdf`
——**本轮我自己下载并抽取核过全部引文**（同一文件 170,503 字节；数学是位图字体，散文部分清晰）。

| 关键点 | 原文（逐字） |
| --- | --- |
| **Shirley 1991 的原式用的是入射角**（不是半角） | *"Shirley attempted to simulate the change in the matte appearance with angle by explicitly dampening `R_d(λ)` as `R_s` increases [36]: `ρ = R_f(θ)ρ_s + R_d(λ)(1 − R_f(θ))/π`"* —— 注意是 `R_f(θ)`。**半角 `v·h` 在两篇 Shirley 文献里都不存在**；Neubelt 式(15) 的 `F(v·h)` 是他们自己的写法 |
| 原式的缺陷：**不互易** | *"The problem with this equation is that it is not reciprocal, as can been seen by exchanging θ and θ' which changes the value of the matte dampening factor because of the multiplication by (1 − R_f(θ))."*（原件的 "can been seen" 是原文笔误） |
| **PG'97 逐字写下了我们这种失效**（针对 Schlick 的混合角形式） | *"…where `R_d(λ)` is a matte coefficient and β is half the angle between incident and outgoing directions. **However, this form does not conserve energy for all incident angles**: for example, at θ = 90° the specular reflectivity goes to one, and the fraction of the hemispherical reflectance is still above zero (e.g,. plug in θ' = 0)."* —— 以及结论句 *"The Schlick model either defaults to the Lambertian-specular model, or it accounts for the Fresnel equation effects but **does not conserve energy**."* |
| **修法：可分离乘积形式**（每方向一个因子 → 天然互易） | *"An obvious candidate for the matte component `ρ_m(θ,θ',λ)` that will be reciprocal is **the separable form `kR_m(λ)f(θ)f(θ')`** for some constant k"*；由能量约束 `R_f(θ) + 2πkf(θ)∫f(θ')cosθ'sinθ'dθ' = 1`（式 3）得 `f(θ) ∝ (1 − (1 − cos θ)^5)`、`k = 21/(20π(1 − R_0))`（式 4），最终式(5)：<br>`ρ = [R_0 + (1 − cos θ)^5(1 − R_0)]ρ_s + kR_m(λ)[1 − (1 − cos θ)^5][1 − (1 − cos θ')^5]` |
| 作者的定位声明 | *"To our knowledge, it is the **first model that produces the matte/specular tradeoff while remaining reciprocal and energy conserving**."* |

**这对我们意味着什么（分析，非原话，已在下面逐条标注）**：

1. **我们的式子在 PG'97 的分类里属"Schlick 那一类"。** 我们的漫反射阻尼 `(1 − F(v·h))` 用的正是被批评的**半角因子**
   （Schlick 形式里 specular 取入射角、matte 取半角 β；我们两者都取 `V·H`，而反射几何下 `V·H = cos β`，
   所以那个"半角 matte 阻尼"是同一个东西）。**实测分解与 PG'97 的描述是同一机制**：镜面拿走能量的地方在
   `V·H ≈ NoV = 0.1`（实测 specular 0.6059 ≈ `F(0.1)`），而漫反射的补偿是在**整个半球上平均** `F(V·H)`
   （多数方向上 `V·H` 大得多 → `F ≈ F0`）→ 补偿点与消耗点不是一个地方。
2. **代价确实很小**：两次 Schlick 求值（正是 Neubelt 说的 *"cost of additional instructions"*），
   **不需要 LUT、不需要换模型族**。但**必须两处一起改**：PG'97 的 specular 用的是 `R_f(θ)`（入射角），
   只换 diffuse 权重、留着 `F(V·H)` 的 specular 就不是原文那个模型了（§0.3 的自拼装红线）。
3. **一个必须写下来的保留（我的分析）**：PG'97 的守恒性是**对理想 δ 镜面**推导的（式 3 里镜面的方向反照率就取 `R_f(θ)`）。
   我们的镜面是 GGX：`D-05` 已经实测过，α = 0.02 时精确 Smith 在 75° 的方向反照率是 **0.996**，而同一角度的
   `F(75°)` 只有 **0.25** —— 镜面 lobe 的实际"拿取量"远大于 `R_f(θ)`。所以把 `ρ_s` 从 δ 换成 GGX 之后，
   式(3) 那条约束不再自动成立，**照搬式(5) 未必能消掉我们的 1.55**；真正对得上"角度依赖"的做法是让 diffuse
   按**镜面的方向反照率**让路（PBRT 那句 *"depends both on the surface roughness and the angle of incidence"*
   说的就是这个），而 K&S 2001 正是为 Torrance-Sparrow 族给出这种依赖的耦合模型。**这一点必须先离线试算再决定**，
   不能凭"有出处"就上手改。
4. **引用时的印刷缺陷（三种抽取一致，别顺手"修"）**：式(4) 印的是 `k = 21/(20π(1 − R_0))`（与前面未编号的
   `[1 − R_f(θ)][1 − R_f(θ')]` 形式自洽），而式(5) 的 matte 括号里**没有** `(1 − R_0)` 因子——两式相差
   一个 `(1 − R_0)²`。照抄原文、把这个不一致记下来。
5. **Neubelt 那条互易性注脚疑似套错了自己的公式（分析）**：`v·h = l·h = (1 + l·v)/|l + v|` 对 l↔v 对称，
   所以 `(1 − F(v·h))` 以及 N&P 的完整 BRDF **本身是互易的**；他们那句"破坏 Helmholtz 互易性"更像是
   沿用了 Shirley **入射角**形式的缺陷描述。PG'97 画的取舍其实是反过来的：
   **入射角 ⇒ 守恒但不互易；半角 ⇒ 互易但可能不守恒**（后者正是我们踩的坑）。

**B 类｜换模型（改动大，且候选都属别的模型族）**

| 候选 | 依据（逐字核到的原话） | 注意 |
| --- | --- | --- |
| **Kelemen & Szirmay-Kalos 2001**（*A microfacet based coupled specular-matte BRDF model with importance sampling*, Eurographics Short Presentations 2001） | Burley 2012 相关工作：*"A coupled-diffuse model is also proposed such that the total albedo is always 1."* | 与我们的结构最近（同属微表面模型），但同句还说它的 G 是 Torrance-Sparrow 的**可微近似**——对得上出处的是**整个模型**，只搬它的 diffuse 权重就是自拼装 |
| **Ashikhmin & Shirley 2000** | Burley 2012 相关工作：*"presented a anisotropic Phong model that included a Fresnel-weighted diffuse and energy conservation guarantees."* | 它是 **anisotropic Phong** 模型：把 diffuse 权重单拎出来配 GGX 同样是自拼装（§0.3 不允许） |
| **PBRT 4ed｜介质界面 + 底层（`CoatedDiffuseBxDF` = `LayeredBxDF<DielectricBxDF, DiffuseBxDF, true>`）** | 结构上**完全不加权 diffuse**：界面反射是一个**相加项**（`f = nSamples * enterInterface.f(...)`），到达底层的能量靠路径吞吐 `beta` 经界面 BSDF/pdf 传递、出口用 BTDF + MIS 连接（§14.3.2） | ① **书上从没说它"能量守恒"**——§14.3 全节 `Fresnel`/`energy`/`conserv` **0 次命中**；② 它是**随机**的（`f()`/`PDF()` 是估计量、PDF 是近似、带 `maxDepth`/`nSamples`/MIS）→ **不可直接搬到实时 lobe 模型**；③ 书自己承认粗糙界面下单次散射仍**丢**能量（§9.6.5：*"objects with significant roughness may appear too dark due to this lack of multiple scattering"*），所以它不会出现我们这种**>1** 的失效模式，但那是因为它压根不做点取值 Fresnel 加权，不是因为它在能量上更强 |

**第三方实现给出的旁证（Filament 官方文档，2026-09-14 核）**

Filament（Google 的 PBR 引擎）恰好把这个问题**写在文档里但留成了 TODO**：

- §"Improving the BRDFs" → **"Energy gain in diffuse reflectance"** 整节只有两句（原文，含未填的占位符）：
  *"The Lambert diffuse BRDF does not account for the light that reflects at the surface and that is therefore
  not able to participate in the diffuse scattering event."* / *"**[TODO: talk about the issue with fr+fd]**"* ——
  即"diffuse 与 specular 相加"的问题他们**承认存在、但文档没给出结论**。全文没有任何一句说组合会超过 100%。
- Filament 的 lit 模型**不做**这一步衰减：diffuse 就是 Lambert（`Fd_Lambert() = 1/π`），材料指南里更明确写着
  *"Filament does not take a Fresnel share out of the diffuse lobe for an ordinary surface either"*；
  他们的 cloth 模型公式里印了 `(1 - F(v,h))`，但正文说 **故意不实现**：*"In practice we've opted to leave out
  the `1 - F(v, h)` term in the diffuse component. The effect is a bit subtle and we deemed it wasn't worth
  the added cost."* —— 这说明我们这一项是**主动选择**，不是行业统一做法。
- **顺带的量化**：若把这一项去掉（即裸 Lambert），同一配置（F0=0.04、r=0.05、cosθv=0.1）的组合 =
  镜面 0.6059 + 漫反射 `baseColor·(1-metallic)` = **1.61**（Lambert 的余弦加权积分恒等于 albedo，解析可得，
  无需采样）。也就是说这一步把越界从 **1.61 压到 1.55**——**有帮助，但远不够**。

**本轮离线试算结果（2026-09-14，`verify_brdf_integrals.py` ③c；**只动验证脚本，未改引擎**）**

先把 PG'97 的式子实现出来并自检——用 `k = 21/(20π(1 − R_0))` 配他们那个未编号的
`[1 − R_f(θ)][1 − R_f(θ')]` 形式：

- **自检①（δ 理想镜面）**：解析与数值积分（2 万步直接求积）最大差 **5.2e-10**，且 `R_m = 1` 时
  `ρhd ≡ 1.000000` —— 精确满足式(3)。顺带得到一条**解析塌缩**（我推导 + 数值验证）：
  `∫ k R_m [1−R_f(θ_l)][1−R_f(θ_v)] cosθ_l dω = R_m·(1 − R_f(θ_v))`，即这个模型就是
  "**镜面拿走 `R_f(θ_v)`，漫反射拿走剩下的 `1 − R_f(θ_v)`**"。这也正是 PBRT 那句
  *"depends both on the surface roughness and the angle of incidence"* 的具体含义。
- **自检②（按印出来的式(5)，即 matte 括号里丢了 `(1−R_0)`）**：ρhd = 1.0334 / 1.0753 / 1.0815 / 1.0817
  （自洽版恒为 1.0000）——**那处印刷不一致的量化**：照抄式(5) 会凭空多出至多 **+0.082**。

**试算（把我们引擎的 GGX lobe 接到 PG'97 的 matte 上，同网格对比）**：

| 材质 / G 变体 | 现行公式 | PG'97 全按出处¹ | PG'97 matte + 现有镜面² |
| --- | --- | --- | --- |
| conductor F0=1.00（三种 G） | 1.0000 | 1.0000 | 1.0000 |
| dielectric F0=0.04 / analytic `k=(r+1)²/8` | 1.0606 | **1.0057** | **1.0000** |
| dielectric F0=0.04 / IBL `k=α/2` | 1.5325 | **1.0165** | **1.0000** |
| dielectric F0=0.04 / 精确 Smith | **1.5458** | **1.0233** | **1.0005** |
| metal grey F0=0.50（三种 G） | 0.5000 / 0.7772 / 0.7946 | 0.5000 / 0.7770 / 0.7944 | 同现行（`R_m=0`，无漫反射项） |

¹ 镜面 Fresnel 也按出处改成**入射角** `F(θ_l)`；² 保留引擎现有镜面（`F(V·H)`）、只换 matte 项。
**引擎可 author 的介电范围**（`inputs.specular` 0.5→1.0 即 F0 = 0.04→0.08，R_m = 1，精确 Smith）：

| F0 | 现行公式 | PG'97 全按出处 | PG'97 matte + 现有镜面 |
| --- | --- | --- | --- |
| 0.04 | 1.5458 | 1.0233 | **1.0005** |
| 0.06 | 1.5540 | 1.0210 | **1.0004** |
| 0.08 | 1.5622 | 1.0191 | **1.0002** |

**结论与三条必须写下来的判读**：

1. **越界确实出在 matte 侧，而且 PG'97 的形状是对的**：换成可分离乘积后，越界从 **1.5458 → 1.0233**
   （超出量 0.5458 → 0.0233，降到 4.3%）；只换 matte、保留现有镜面则到 **1.0005**。现行公式的病根
   在这里被定量确认：镜面实际拿走 `A_spec ≈ 0.6059`，而漫反射的补偿写成"整个半球上平均
   `F(V·H)`"（≈0.94），**补偿量应该按镜面实际拿走的那一份算**。
2. **残余的 2.3% 在镜面侧，不在 matte 侧**：PG'97 的镜面用入射角 Fresnel，GGX 在大角度入射方向上
   `F(θ_l) → 1`，于是 `A_spec^{F(θ_l)}` 会略大于 `R_f(θ_v)`（最多 +0.023）。也就是说，
   **"全按出处"仍会小幅越界**，与 §A 类第 3 条保留（守恒性是对 δ 镜面推的）完全一致。
3. **"只换 matte"之所以站得住，是因为它落在该模型的适用条件内（不是随手拼装）**：PG'97 的推导前提是
   "镜面的方向反照率 = `R_f(θ_v)`"（式(3) 里就是这么写的），而**我们的镜面实测满足这个前提**——
   在整个 roughness × 视角网格、F0 = 0.04 / 0.06 / 0.08 上 `A_spec^{F(V·H)} ≤ R_f(θ_v) + 5e-4`。
   所以这一列可以论证为"**用 PG'97 的 matte 项配一个满足其前提的镜面**"，而不是自创公式；
   但它仍然是对论文原样的一处偏离，采纳与否需要显式决定并写在实现注释里。

**若采纳，引擎侧改动很小**（尚未实施）：把
`diffuseWeight = (1 - F(V·H))·(1-metallic)`、`diffuse = diffuseWeight·baseColor/π·radiance·NoL`
换成 PG'97 的 matte 项 `k·R_m·(1 − F(NoL))·(1 − F(NoV))`，其中 `k = 21/(20π(1−F0))`、
`R_m = baseColor·(1-metallic)`，**并且不再单独乘 `1/π`**（`k` 里已含）。代价：① 着色结果会变，
`D-08` / `D-10` 的球阵基线数字必须重测；② 五张探针图（mode 2/4/5/6/7）不受影响。

**必须改两条路径——间接光那条现在更差，而且这条修法不会自动覆盖它**（2026-09-14 试算补测）：

`CalculateDiffuseIbl` 是**裸 Lambert**（`irradiance·baseColor·(1-metallic)/π`，**没有 `(1-F)` 让路因子**），
所以均匀环境下 IBL 的总反照率 = `R_m` + split-sum 镜面 `(F0·A + B)`：

| 配置 | IBL 现状（裸 Lambert + split-sum） | 同一个 PG'97 matte 项 |
| --- | --- | --- |
| F0=0.04（dielectric，R_m=1） | **1.5927** | **1.0000** |
| F0=0.08（dielectric，R_m=1） | **1.6087** | **1.0000** |

三点判读：

1. **IBL 比直接光那条路径更差**（1.59 / 1.61 vs 1.55）：直接光至少还有个补偿项（哪怕补错地方），
   IBL 的漫反射**一点都不让路**。这与 Filament lit 模型的情形一致（对照组就是裸 Lambert）。
2. **同一个 matte 项对 IBL 也够用**：实测 split-sum 的镜面方向反照率 `F0·A + B ≤ R_f(θ_v)`（整个网格上取等），
   所以 `R_m(1 − R_f(θ_v))` 这一份"剩下的"就正好把总和压回 1.0000。
3. **但 IBL 侧要写清一处近似（重要）**：PG'97 的 matte 是 `k·R_m·(1−R_f(θ_l))·(1−R_f(θ_v))`，
   在 IBL 里要正确使用它需要**按 `(1−R_f(θ_l))` 加权过的 irradiance**（那是一次不同的 SH 卷积），
   而试算里用的是"把 `R_m(1−R_f(θ_v))` 当标量乘在现有 irradiance 上"——**这只在均匀环境下严格等价**
   （均匀环境下 `∫(1−R_f)cosθ dω = (1−F0)·20π/21` 是常数）。换成真实环境（比如一个方向很亮的太阳），
   这个标量近似会偏；要真做对，得再加一次加权 irradiance 卷积，或显式记录这条近似。

**越界的观感到底是什么（避免一个容易想错的结论）**

"掠射越界 ⇒ 更发白"**只对了一半**，要分情形看——以 F0=0.04、r=0.05、cosθv=0.1 那一格为例
（镜面 0.6059、漫反射 0.9398、总 1.5458，**比均匀环境的守恒上限多约 55%**），多出来的那 0.55
**全部来自没让路的漫反射**，而漫反射带 `baseColor` 的颜色：

| 情形 | 结论 |
| --- | --- |
| 白色 / 浅色表面 | 就是**更亮、更白**（本来就是白的，只是量多了），也更容易顶到显示范围 |
| 饱和颜色的电介质（例：albedo = 1.0 / 0.2 / 0.2） | **恰恰相反——越界让掠射更"偏色"**：现行 `(0.94, 0.188, 0.188) + 0.61 = (1.55, 0.80, 0.80)`，G/R = 0.52；修好之后（漫反射让路）`(0.39, 0.078, 0.078) + 0.61 = (1.00, 0.69, 0.69)`，G/R = **0.69**——**修完反而更白**，因为剩下的是占比升到 61% 的无彩色镜面 |
| 已经溢出 / 被 tonemap 压顶 | 这时才是字面意义的"洗白"：按通道依次压平，色相丢失。**越界让它更容易发生**，所以"过曝导致的发白"这个说法成立 |

所以准确的说法是：**越界本身让掠射更亮（对彩色表面还更偏色）；"发白"来自溢出/色调映射压顶那一步**，
而越界把那一带更容易推过阈值。另外别把它和**物理上本该有的掠射边缘亮边**（Fresnel → 1）混为一谈：
轮廓带发亮是真实的，**错的是漫反射没有让路、把这一带额外抬高了最多 ~55%**；IBL 那条路径更明显
（1.5927，漫反射一点都不让路）。

**第三方实现给出的旁证（three.js dev 分支，2026-09-14 核）——这条最有参考价值**

来源：`src/renderers/shaders/ShaderChunk/lights_physical_pars_fragment.glsl.js`（我下载原文逐行核过）。
**它把两条路径分开处理，而两条用的不是同一种耦合**：

| 路径 | three.js 的做法（原文） |
| --- | --- |
| **直接光** | `reflectedLight.directDiffuse += irradiance * diffuseBRDF * ( 1.0 - F );`，其中 `F = F_Schlick(f0, f90, dotVH)` —— **半角 `(1−F)`，与 UE4 / 我们一致** |
| **间接光（IBL）** | 注释原话 *"**Energy reflected by the specular lobe is not available to the diffuse layer**"*；先 `computeMultiscattering( material.dfg, ... )` 得到 `singleScattering = FssEss = Fr*dfg.x + F90*dfg.y`（**就是它本来就要采样的那张 DFG LUT**）与 `multiScattering`（Fdez-Agüera 式补偿），然后<br>`vec3 diffuse = irradiance * BRDF_Lambert( diffuseContribution ) * ( 1.0 - singleScattering - multiScattering );` |

**为什么这条比 PG'97 更贴我们的问题**：

1. 它就是"**用镜面的方向反照率给 diffuse 让路**"——即 PBRT 那句 *"depends both on the surface roughness
   and the angle of incidence"* 的工程实现，也正是 PG'97 用 `R_f(θ_v)` 去**近似**的那个量。
2. **在均匀环境下它精确守恒**：`R_m·(1 − A_spec) + A_spec ≡ 1`（`A_spec = F0·A + B` 就是 LUT 给的方向反照率），
   不像点取值 Fresnel 那样有残余。
3. **我们已经有那张 LUT**（`brdfLut` / `SampleEnvironmentBrdf`，`D-09` 那一批还验证过它与白炉积分一致到 0.0000），
   所以**改间接光这一条的代价极小**：`diffuse *= 1 - (F0 * A + B)`。
4. 它还顺带解释了**为什么直接光要用 `(1−F(V·H))` 这种点取值近似**：单条光线上拿不到"半球镜面反照率"，
   而 IBL 本来就在对半球积分、LUT 现成——**方向反照率式的严格耦合只在对半球积分的那条路径上可行**。
   这也解释了各家实现的分工：直接光用启发式、间接光用 LUT。
5. 顺带核到：three.js 的 diffuse 侧还新合入了 **EON（Portsmouth et al. 2025, JCGT,
   *A Practical Energy-Preserving Rough Diffuse BRDF*）**，那是补**粗糙 diffuse 自身**的多重散射
   （`multiScatter ∝ (1−albedoV)(1−albedoL)/(1−averageAlbedo)`），与本 case 的 diffuse/specular 分配是
   **两件事**，但它说明现代方向是"各 lobe 各自保能量"，而不是"用点 Fresnel 互相让路"。

**因此对 `D-07` 的建议排序更新为**（按"主流程度 × 代价 × 严格性"）：

1. **间接光改用 LUT 耦合**（three.js 做法）：`diffuse *= 1 − (F0·A + B)`，均匀环境下精确守恒，
   代价≈0（LUT 已有），**这是主流做法**；
2. **直接光**保留 UE 式 `(1−F(V·H))`（主流）；若要严格，再叠加 PG'97 的 matte 项
   （1.5458 → 1.0005，但偏离主流），或按 Frostbite 那样登记"重归一化 + ρhd 验证"的做法。

**第三方实现给出的旁证（Frostbite 课程笔记，2026-09-14 核）**

Frostbite（Lagarde & de Rousiers, *Moving Frostbite to Physically Based Rendering 3.0*, SIGGRAPH 2014 course notes，
自存 PDF `seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf`，122 页全文抽取）：

- **判据有第二个出处，而且就是我们用的那个量**（§3.1.3 "Energy conservation"）：*"In Frostbite we have chosen to keep
  the computation simple and only ensure the preservation of energy, by ensuring that the **hemispherical-directional
  reflectance** … is below one for our **whole BRDF (diffuse + specular term)**: `ρhd(v) = ∫Ω f(v,l)⟨n·l⟩ dl =
  ∫Ω (fr(v,l) + fd(v,l))⟨n·l⟩ dl ≤ 1`（式 6）"* —— 与 `D-07` 的白炉测法（定住 V、对 L 半球积分）**逐字同一个量**。
  比 Hoffman 式(3) 更进一步：Frostbite 明确写的是 **diffuse + specular 合起来**要 ≤ 1。
- **他们不用 `(1 − F)` 漫反射权重**：diffuse 是 **Disney diffuse + 能量重归一化**（Listing 1 的 `energyBias =
  lerp(0, 0.5, linearRoughness)`、`energyFactor = lerp(1.0, 1.0/1.51, linearRoughness)`）；Fresnel 只出现在镜面项，
  且确实取半角 `LdotH`（`float3 F = F_Schlick(f0, f90, LdotH)`）。他们**只对 Disney diffuse 自身的能量越界**
  （脚注 4：*"This is by design, as explained by Burley"*）做了重归一化，并用 ρhd 图（Figure 9）验证
  *"While not perfectly equal to one, it is close enough."*
- **幻灯片讲稿把同一件事说得更直白**（S3 slide 22 的 presenter script 层，逐字）：*"One issue we had with the
  original Disney diffuse term is that it does not respect energy conservation: in some cases, the reflected light
  can be higher than the incoming illumination. We have applied a simple linear correction to ensure that the
  **hemispherical-directional reflectance is below 1 when we add the specular and the diffuse terms together**."*
  同一页三张图的读数（OCR）："Original Disney Diffuse" 峰值 **1.5**、"Renormalized Disney Diffuse" **1.0**、
  "Diffuse + Specular" **1.0** —— 与我们的测法同构（峰值 1.55 是我们的组合，不是 Disney 那一项）。
- **方法论可引用**：Frostbite 的做法是"**把越界的那一项重归一化，再用同一个 ρhd(v) ≤ 1 的测法验证**"——
  与 PG'97 同属一个思路，区别是 PG'97 的常数 `k` 是**从能量约束推导**出来的（式 3），而 Frostbite 的
  `energyFactor` 是对 Disney diffuse **拟合**的（1/1.51）。
- **他们全篇没有讨论互易性**（`reciproc*` 0 命中），选"同一层"模型（diffuse 与 specular 共用 roughness）
  的理由是**粗糙度一致性**（脚注 3），不是能量；而 Frostbite **只在法线贴图预过滤处引用 Neubelt & Pettineo**，
  所以它**不能**佐证也不能反驳式(15) 的归属——那条归属的依据是 The Order: 1886 笔记本身（本轮已逐字核过）。
- 附：Appendix C 式(C.1) 里出现过 `(1 − F(l))(1 − Fdr)`，但那是**入射方向**的 Fresnel，且是理想化推导，
  不是他们发布的实现。

**PBRT 4ed 的 Further Reading 直接点名了候选（本计划的既有来源，逐字）**

`https://pbr-book.org/4ed/Reflection_Models/Further_Reading`：

> *"One issue with the specular term of the Torrance–Sparrow BRDF presented in Section 9.6.5 is that it only
> models a single scattering interaction with the microfacet surface, causing a growing portion of the energy
> to be lost as the roughness increases."*
> *"The original model by Torrance and Sparrow (1967) included a diffuse component to simulate light having
> scattered multiple times. However, **a simple diffuse correction is generally unsatisfactory**, since the
> precise amount of energy loss will depend both on the surface roughness **and the angle of incidence**.
> **Kelemen and Szirmay-Kalos (2001) proposed an improved diffuse-like term that accounts for this dependence.**
> Jakob et al. (2014a) generalized their approach to rough dielectric boundaries in the context of layered
> structures, where energy losses can be particularly undesirable."*
> *"Efficient approximate models for multiple scattering among microfacets were presented by **Kulla and Conty
> Estevez (2017)** and by **Turquin (2019)**."*

这条把 `Kelemen & Szirmay-Kalos 2001` 从"Burley 的一句话转述"升级为**两个独立来源共同指向的候选**，
而且 PBRT 那句 *"depends both on the surface roughness and the angle of incidence"* 与我们的诊断**逐字对上**
（我们的病根正是不随角度变化的点取值 Fresnel）。注意 PBRT 自己把 K&S 放在"补偿镜面**单次散射丢能量**"
的语境里，而 Burley 那句是"总量恒为 1"——两处描述的是同一个耦合模型的两种效果，
**"总量恒为 1" 才是能同时压住我们这种 >1 的说法**。

> 参考资料辨识的坑：PBRT 参考文献里的 *"Shirley, P. 1991. Discrepancy as a quality measure for sample
> distributions."* **不是** Neubelt 引的那篇——Neubelt 的 [1991] 是 **Shirley 的博士论文**
> *Physically based lighting calculations for computer graphics*（UMI Order NO. GAX91-24487）。同一作者同年两篇，
> 交叉引用时别混。

**明确不是解的方向**：Filament 的能量补偿确实存在，但**只补镜面**、而且**是加能量**——原文
*"Their idea is to add an energy compensation term as an additional BRDF lobe"*（Kulla & Conty 2017）、
*"They also propose to apply energy compensation by adding a scaled GGX specular lobe"*（Lagarde & Golubev 2018），
代码是 `energyCompensation = 1.0 + f0 * (1.0/dfg.y - 1.0)`（≥1）。它补的是粗糙金属单次散射**丢掉的**能量，
与本问题方向相反：加进我们的组合只会让越界更大。先前计划 §5 误区表里把 "Filament 的能量补偿 / Kulla-Conty"
列为候选的那句话**未经验证且指错了方向**，已删除。他们的白炉讨论也**只针对金属镜面**
（*"a purely reflective metallic surface (f0 = 1) should be indistinguishable from the background"*），
不覆盖 diffuse+specular 的组合。

**若决定动数值，代价清单**（先写在前面，免得低估工作量）：

1. 着色结果会变 → `D-08` / `D-10` 的球阵基线数字（端点差 ≥0.443、最小步长 0.016 / 0.006、
   `metallic=1` 不单调的观测）必须**重新测量**；
2. mode 2 / 4 / 5 / 6 / 7 五张探针图**不受影响**（它们走 `M_brdfPlot`，只 include 微表面与分布头，
   不经 `lighting.glsl`）——所以论文曲线的证据链不会被动摇；
3. `verify_brdf_integrals.py` 的 `diffuse_weight()` 与 ③ 白炉必须同步改，并留下改动前后的对照数字。

### `D-08`（参数化边界）

| 字段 | 内容 |
| --- | --- |
| **来源** | [Hoffman 2013｜Background: Physics and Math of Shading](https://blog.selfshadow.com/publications/s2013-shading-course/hoffman/s2013_pbs_physics_math_slides.pdf) |
| **验证目标** | `metallic = 1` 时可见 diffuse 为 0；F0 / albedo 不越界 |
| **形式** | 数值积分（离线）+ 球阵端点 |
| **实现锚点** | `f0_from_parameters` / `diffuse_weight`（镜像 `EvaluateDefaultPbrLightLobes`）；`measure_sphere_array.py` 判据 ① |
| **判定** | **通过**。`metallic=1` 时漫反射反照率最大值 **0.00e+00**（5 个 baseColor × 4 个视线角 × 6 个 roughness）；`F0` 映射偏差 **0.00e+00**（metallic=1 → `baseColor`，metallic=0 → 0.04）；球阵上每个 roughness 的 `metallic=1` 列比 `metallic=0` 列暗 **≥0.443** |
| **差异归因** | 无实现差异。补记：`specular`（独立 F0 作者入口）恒为默认 0.5，所以 `F0` 只能取 `0.04` 或 `baseColor`——这条参数面缺口仍登记在 `D-12`，本 case 只验证"给定参数面下边界成立" |

### `D-10`（球阵标尺的数值判定）

| 字段 | 内容 |
| --- | --- |
| **来源** | 球阵基线 `SC_sphere_array`（plan §1.7.1） |
| **验证目标** | roughness × metallic 的端点与单调性回归，让球阵成为其它模型可引用的标尺 |
| **形式** | 球阵（像素统计） |
| **实现锚点** | `tool/validation/measure_sphere_array.py`；场景 `Maps/SC_sphere_array/`（121 球 + 坐标轴；相机 `[0,0,16]` fov 80；sunset HDRI + dir 1.0 + point 250） |
| **判定** | **通过**。按相机模型投影出 121 个圆心（投影尺度 47.67 px/单位、球半径 16.7 px、圆盘采样半径 10.3 px），取圆盘平均线性亮度后：① 端点——每个 roughness 上 `metallic=1` 比 `metallic=0` 暗，最小差 **0.4427**；② 沿 metallic 递减的最小相邻步长 **0.0159**；③ `metallic=0` 沿 roughness 递增的最小相邻步长 **0.0060** |
| **差异归因** | ① **`metallic=1` 沿 roughness 并不单调**（实测峰值在 r≈0.43，0.4391；两端 0.2859 → 0.1918）——"瓣展宽"与"单次散射能量损失"在该 HDRI + 点光下的竞争结果，**场景相关、不是公式性质**，因此**明确不断言**这一条，只记观测，免得以后有人照"能量随粗糙度单调下降"去断言而误判。② 判据门限来自实测余量（0.002），不是拍的：图像本身可复现（同一场景重采逐像素相同）。③ 圆盘只取球投影半径的 62%，避开轮廓与相邻球间隙（球直径 33.4 px、间距 42.9 px） |

### 复现方式

```powershell
# 公式级（离线，不需要引擎）
py -3 tool\validation\verify_brdf_integrals.py                 # M-02 / M-03 / M-04 + D-01 / D-07 / D-08
py -3 tool\validation\verify_brdf_integrals.py --strict-total  # 把组合能量越界也算作退出码 1

# 球阵（需要一张 tonemap 0 + bloom 0 的球阵截图）
py -3 tool\validation\measure_sphere_array.py <rn_sphere_array.bmp>

# 采图（球阵不是全屏面板，所以面板检查要 skip）
.\tool\validation\run_paper_case.ps1 -Name sphere-array -PanelMode skip `
  -Scene 'Maps/SC_sphere_array/SC_sphere_array.json' -Shots rn_sphere_array
```

现场证据：`artifacts/D-01-D-07-D-08/run.txt`（离线脚本完整输出）与
`artifacts/D-01-D-07-D-08/sphere-array.txt`（球阵表与判据）。

---

## D-05 / D-06 / D-09 — 方向反照率、分布尾部、split-sum 误差（2026-09-14）

三个 case 的证据形式各不相同：`D-05` 与 `D-09` 是**离线积分**（新增/扩展到两个脚本），
`D-06` 是**新增 mode 5 曲线探针**（含像素级对照与图头文字回读）。

### `D-05` — 各 G 函数的方向反照率对照（Burley 2012 Fig 12）

| 字段 | 内容 |
| --- | --- |
| **来源** | [Burley 2012｜Physically Based Shading at Disney](https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf) §4.4 + **Fig 12**（图注：*"All plots use the same D (GGX/TR) and F factors. Left: smooth surface (α = 0.02); right: rough surface (α = 0.5). The 'no G' model **excludes the G and 1/(cosθl cosθv) factors**."*） |
| **验证目标** | 换 G 对方向反照率的影响有多大；论文的两条定性结论——"省略 G 会让掠射明显偏暗"、"Smith 在平滑表面上掠射反射显著上升"——在本引擎的 D/F 下是否成立 |
| **形式** | 数值积分（离线）：`verify_brdf_integrals.py` ⑤，NDF 采样估计量 |
| **实现锚点** | `DistributionGGX`（引擎真实 D）、F0=1 的 Schlick Fresnel、三种 G（analytic `k=(r+1)²/8` / IBL `k=α/2` / 精确 Smith）+ 论文定义的 "No G"（`f = D·F/4`） |
| **判定** | **通过**。全表最大方向反照率 **0.9996 ≤ 1**（论文："must be less than 1 for all angles"）。表（每 15° 一点，0→75°）：<br>α = 0.02：analytic `0.999 0.987 0.950 0.876 0.738 0.465`／IBL `1.000 0.999 0.996 0.991 0.980 0.944`／Smith `1.000 1.000 0.999 0.999 0.999 0.996`／noG `0.995 0.928 0.747 0.499 0.251 0.069`<br>α = 0.5：analytic `0.629…0.491`／IBL `0.662…0.625`／Smith `0.688…0.723`／noG `0.519…0.202`<br>75° 处 `Smith − noG` = **+0.9275**（α=0.02）/ **+0.5215**（α=0.5），与论文"noG 偏暗"一致 |
| **差异归因** | ① **α 约定必须先对齐**：Burley 用 Disney 的 `α = roughness²`，而引擎入参是 perceptual roughness，所以对照按 **α** 对齐（`roughness = √α`）——按 roughness 对齐会得到完全不同的曲线。② 论文 Fig 12 还画了 Walter 2007 / Ashikhmin-Shirley 2001 / Kurt 2010 / Ward 1992 四条臂，仓库里没有它们的可引用形式，**只做了有出处的那几条**（不做"看起来差不多"的自制版本）。③ 顺带量到一条与 `D-20`/`D-21` 呼应的数：**analytic 变体在平滑表面掠射下最暗**（75° 时 0.465，精确 Smith 0.996）——这正是 Karis 论文说"把它用到 IBL 上掠射会暗得多"的量化版本 |

### `D-06` — GGX vs Beckmann 的分布形状与尾部（新增 mode 5）

| 字段 | 内容 |
| --- | --- |
| **来源** | [PBRT 4ed §9.6.1 + **Fig 9.23**](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)（图注：*"Graphs of isotropic Beckmann–Spizzichino and Trowbridge–Reitz microfacet distribution functions … for α = 0.5. **Note that Trowbridge–Reitz has higher tails at larger values of θ**."*）；Beckmann 公式取 PBRT 3ed §8.4.1 **式(8.10)** 的等向形式（原始来源 Beckmann & Spizzichino 1963） |
| **验证目标** | 同一 α 下两条分布的形状；GGX 的"长尾朝哪边"——大角度处 GGX 必须高于 Beckmann |
| **形式** | 曲线（**mode 5**）+ 图头文字回读 + 逐列像素对照 |
| **实现锚点** | `common/microfacetDistribution.glsl` 的 `DistributionBeckmann`（**参考曲线，不参与着色路径**，与 `SmithG1Ggx` 同一待遇）；`M_brdfPlot.surface.glsl` 的 `PLOT_MODE_BECKMANN` / `PlotLogYAxisSixDecades` / `PlotLogYRange` / `PlotBeckmannTailRatio`；`MI_plot_ggx_beckmann` + `SM_plot_quad_beckmann` + `SC_paper_case_plot_beckmann` |
| **判定** | **通过**。α = 0.5（`u_plotRoughness = 0.7071`）：GGX **153 列最大 0.469 px**、Beckmann **69 列最大 0.451 px**（镜像假设与轴框核对均通过；118 个交叉列被整列丢弃——两条曲线在尾段都掉到轴底附近）。图头文字（关掉运行时 UI 后回读）：说明行 `at beckmann tail 1e-3: ggx/beckmann 116.5`，**与独立 python 复算逐位一致**；图例 `GGX (TR)` 与 `Beckmann` 逐字一致，并用 M-08 的墨点数法独立核对（预期 360 / 440 px，实测 360 / 440 px，差 0） |
| **差异归因** | ① **结论指标换过一次**（第一版被图上读数否掉）：原先报"两条分布各自掉到峰值 1e-3 的角度"，图上读出来是 `ggx 90.0`——因为 **GGX 在掠射极限非零**（D(90°) = α²/π，归一化后是 α⁴ = 6.25%），它永远不会掉到 1e-3，函数只能返回 90 这个哨兵值。现在报"在 Beckmann 的尾角处 GGX 高多少倍"，数字有界且直接对应论文结论。② **字体表插错位置**：为写 `Beckmann` 新增的 `B` 字形被插在 `C` 之后，而编码顺序是 66='B'、67='C'，于是 **B 与 C 的字形互换**——mode 0 的 caption 把 "Charlie" 画成了 "Bharlie"（逐像素回归时暴露：56 个像素差、全在图头文字区）。**为什么"回读"没发现**：编码与解码用同一张错表，自洽地把 `B` 读成 `C`；抓住它的是"编码 ↔ 字形标签逐项配对"检查与"与旧图逐像素对比"。两个字体表必须同时改**并且逐项核对标签**，只数行数不够。③ 顺带修正：`D-06` 原来引用的 `pbr-book.org/4ed/.../Microfacet_Distributions` 链接**已失效**（4ed 把 D 函数放在 §9.6），已更新。④ 两条分布的**峰值相同**（θh=0 处都是 `1/(πα²)`），所以"各自归一化"与"同一分母归一化"等价、两条曲线可直接比高低——这一点在注释里写明，免得被误当成"α⁴ 之差"（我第一版注释就写错了，被数值复核纠正） |

### `D-09` — split-sum 近似 vs 逐样本参考（真实 HDRI）

| 字段 | 内容 |
| --- | --- |
| **来源** | [Karis 2013](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) §5（split-sum：预过滤环境 × `(F0·A + B)` LUT） |
| **验证目标** | 量化这条近似相对逐样本积分的误差，并说明误差随环境频率、粗糙度、视线角怎么变 |
| **形式** | 数值积分（离线）：`verify_split_sum.py`——真实 Radiance HDRI + 四个解析环境；参考臂用与 LUT 相同的 NDF 采样对完整 BRDF 积分 |
| **实现锚点** | `generator/prefilterEnvMap.comp`（V=N=R、NDF 采样、按 NdotL 加权）、`generator/brfdLut.comp`（`IntegrateBRDF`，`k=α/2`）、`generator/equirectToCubemap.comp` 的 `DirectionToUv()` 采样约定 |
| **判定** | **通过（误差已量化）**。误差 = `abs(split-sum − 参考) ÷ 本配置信号量级`（信号量级 = 预过滤亮度 × (F0·A+B)）：<br>constant（对照组）**0.05%**／gradient 低频 0.05%／cosine 中频 51%／sun 高频 67%／**bistro 4k HDRI（真实）23.3%（conductor）、21.5%（dielectric）**<br>恒定环境里更严格的"相对误差"对照组自检 = **0.054%**（判据 ≤1%） |
| **差异归因** | ① **误差来源是近似本身，不是镜像**：恒定环境下 split-sum 与参考在数学上相等（两半各自就是 A/B），实测 0.054% 是估计量噪声地板（1024 采样 1.21% → 4096 采样 0.056% → 65536 采样 0.001%）。② **相对误差必须在"参考值有量级"时才报**：第一版用环境均值归一化，电介质的可判定配置被全部滤掉（镜面信号本来只有环境均值的百分之几）；改成**按本配置的信号量级**归一化后两侧都能判。③ **环境选择是受限的**：球阵场景的 `sunset.exr` 是 **DWAB 压缩**的 EXR（DCT 有损），离线脚本解不了；headline 数字用仓库里可解码的 `bistro_san_giuseppe_bridge_4k.hdr`，误差量级**依赖环境内容**，sunset.exr 的具体数字本 case 给不出。④ 镜像的简化：预过滤直接采 equirect（引擎走的是 cube + mip 层级 + 双线性），**mip 层级量化带来的额外误差没有建模**——真实引擎的误差只会比表中数字更大，不会更小 |

### 本批带出的实现与工具变更

1. **共享头新增参考分布**：`DistributionBeckmann`（PBRT 3ed 式(8.10) 等向形式），注释写明它不参与着色路径、
   只服务 `D-06` 的对照；峰值与 GGX 相同这一点也写在注释里。
2. **mode 5 + 深 log 轴**：`PlotLogYRange(value, decades)` 泛化了原来的三数量级映射
   （`PlotLogY` 现在就是 `PlotLogYRange(v, 3.0)`，行为逐位不变——mode 0 的逐像素回归为 0 差异），
   `PlotDrawLogYAxisSixDecades` 提供 1e-6..1 的轴；θh 的 x 轴拆成 `PlotDrawThetaHXAxis` 供 mode 0/5 共用。
3. **字体表新增 `B`**（`PLOT_FONT_CODES` 51→52、`PLOT_FONT_ROWS` 357→364）——见 `D-06` 归因②。
4. **`verify_brdf_integrals.py` 加第 ⑤ 节**（方向反照率）与 `specular_albedo_plain_d`（论文定义的 "No G"）。
5. **`verify_split_sum.py`（新）**：Radiance RGBE 解码 + 环境抽象（真实/解析）+ 两半镜像 + 误差表。
6. **`read_plot_text.py` 加 `--no-ui-overlay`**：短图例整行落在运行时 UI 包围盒里，默认跳过就读不到；
   配合"临时关 `ui.enabled` 采一张"即可逐字核对（关的是配置、采完立刻还原，`git status` 为空）。

### 复现方式

```powershell
# D-05 / （顺带）D-01 / D-07 / D-08：全离线
py -3 tool\validation\verify_brdf_integrals.py

# D-09：全离线（含 23MB HDRI 解码，约 2 分钟）
py -3 tool\validation\verify_split_sum.py --samples 8192 --prefilter-samples 4096
py -3 tool\validation\verify_split_sum.py --skip-hdr        # 只跑解析环境，秒级

# D-06：采图 + 逐列像素 + 图头回读
.\tool\validation\run_paper_case.ps1 -Name D-06 -PanelMode curve `
  -Scene 'Maps/SC_paper_case_plot_beckmann/SC_paper_case_plot_beckmann.json' -Shots d_06_mode5b
py -3 tool\validation\measure_plot_curve.py curve <shot> --mode mode5 --roughness 0.7071 --curves ggx beckmann
py -3 tool\validation\read_plot_text.py <shot> --row caption      # 图例被 UI 压住，需要下面这一步
# 短图例的文字核对：临时把 config/config.json 的 ui.enabled 改成 false，采一张，再还原
py -3 tool\validation\read_plot_text.py <ui-off shot> --row legend1 --no-ui-overlay
```

现场证据：`artifacts/D-05-D-06-D-09/`（`D-05-directional-albedo.txt`、`D-09-split-sum.txt`）、
`artifacts/D-06-final/20260914_110807/`（`measure-mode5.txt`、`text-readback.txt`）、
`artifacts/D-06-mode5-ui-off/20260914_110851/text-readback-ui-off.txt`（关 UI 后的逐字回读）、
`artifacts/D-06-regress/`（mode 0/2 的逐像素回归，0 差异）。

---

## 把离线结论搬进 VL——mode 6 / mode 7 与两条新发现（2026-09-14）

`D-05` / `D-09` 的结论原先只存在于离线脚本的输出里（"数字对，但看不见"）。这一批把它们做成
引擎里可以直接看的探针图：**mode 6 = 方向反照率，mode 7 = split-sum 相对误差**。
`D-06` 上一次已经有 mode 5，`D-20` / `D-21` / `D-03` 有 mode 2 / 4，至此 7 个 mode 覆盖了
DefaultLit 几何项的全部可画结论。

### 两张图的取数方式（与 mode 0–5 不同，必须写清楚）

mode 6 / 7 的曲线值**不在 shader 里现算**，而是 `#include` 离线脚本生成的表
（`validationAlbedoTable.glsl` / `validationSplitSumTable.glsl`）。理由：逐像素做半球积分在
shader 里做不到；而表一旦生成，"图上这条曲线"与"记录里那个结论"就是同一份数。
代价必须说明：**像素测量这一路只能回答"图有没有把表画对"**（双线性插值 / 轴映射 / 颜色身份），
"表算得对不对"由生成它的离线脚本独立负责（`verify_brdf_integrals.py` 的重要性采样积分）。
shader 只做插值这条是刻意的——让 shader 再实现一遍积分就会造出第二个"真值"。

### 实测证据

| 项 | mode 6（`D-05`） | mode 7（`D-09`） |
| --- | --- | --- |
| 场景 / MI / 参数 | `SC_paper_case_plot_albedo` / `MI_plot_albedo` / α = 0.02（r = 0.1414） | `SC_paper_case_plot_split_sum` / `MI_plot_split_sum` / F0=1、θv=60° |
| 表维度 | 5 α × 4 臂 × 18 角度（每格 8192 样本） | 4 环境 × 9 档 roughness（每格 4096 样本） |
| 轴 | x = θv（0..90°），y = 线性 0..1 | x = roughness（0..1），y = log10 六数量级（1e-6..1） |
| 逐列像素误差 | direct 68 列 **0.481 px** / ibl 68 列 **0.455** / smith 31 列 **0.439** / noG 68 列 **0.408**（判据 ≤2 px；镜像假设 ~400–570 px；轴框四边偏差 ≤1 px） | constant 209 列 **0.459 px** / cosine 147 列 **0.476** / sun 217 列 **0.475** / bistro4k 217 列 **0.468** |
| 图头回读（关 UI 后） | 说明行 `albedo at 75deg: Smith - no G = 0.927`（离线结论 0.9275 的三位小数）；四条图例 + 参数注记 `r=0.14 alpha=0.02` 逐字一致 | 说明行 `max rel err (F0=1, tv=60deg): 0.721`（表内最大 0.72052）；四条图例逐字一致 |
| 回归 | mode 5 / mode 6 与改动前的截图**逐像素相同**（0 / 921600，最大通道差 0） | 同上（同一批截图对比） |

### 两条新发现（都由"要把结论画出来"逼出来的）

1. **`D-09` 的判定原先缺一条绝对下限**（会产出假误差）。原判据只比"参考值 ≥ 本配置信号量级的 5%"，
   而镜面瓣指向环境暗处时**预过滤与参考同时趋零**——两个趋零的数相除照样通过该门限。
   实测（cosine、α = 0.0025、θv = 60°）：预过滤 `1.0e-5`、参考 `0.0e+00`，比值在
   4096 / 16384 / 65536 采样下给出 **395.7% / 559.1% / 285.8%** 三个完全不同的"相对误差"，
   换采样数就跳——这是噪声，不是近似误差。补上 `--prefilter-floor-fraction`（预过滤亮度必须 ≥
   环境平均亮度的 5%）后，该配置与 cosine 在 r ≤ 0.25 的两档一起被正确标为**不可判定**，
   而所有 headline 数字不变（原先的最大值都出现在信号充分大的 r = 0.7 处）。
   图上这些点**断开**而不画 0：画 0 会被读成"这个环境没有误差"。
2. **表的索引布局写反过一次，而像素测量抓不到**。生成脚本一行写一个环境（`[env][roughness]`），
   shader 与 python 镜像却都按 `[roughness][env]` 跨行取值。两边**自洽地错**，
   于是 `constant` 那条曲线依旧"测出 0.474 px 的最大误差"——它只是一致地画错了数据。
   抓住它的是"实测像素值反推出来的数在表里根本不存在"（反推得到 0.0625 / 0.1158 / 0.1466，
   而表里没有任何一格是这些值）。现在两处都改成按行解析，并加了结构校验
   （**行数 == arm 数、每行长度 == 档位数**）——这类错误只有结构校验拦得住。
   附带发现：`gradient` 与 `constant` 的误差同量级（最大都是 0.05%~0.15%），在六数量级 log 轴上
   两条曲线相差 < 3 px，逐列测量无法分开（会把**每一列**都判成"曲线交叉"而整体丢弃），
   所以图上低频只留 `constant` 作代表，`gradient` 的数字仍在判定表里。

### 版式与工具的两处配套改动

- **图例可以画 4 条**：85 px 的上留白只装得下"1 行说明 + 3 行常规行距"，所以 4 条图例时把行距
  收紧到 `cell*8`（= 16 px，1 + 4 × 16 = 80 < 85）。曾试过"≥5 条改两列"，但 mode 7 最终只画
  4 条（见上一条），那条分支没有被任何 mode 走过，已删除——**不留没被渲染验证过的布局路径**。
- 字体表新增 `*`（`no G (D*F/4)`）与 `u`（`sun`）两个字形，并补了工具
  `tool/validation/check_plot_font.py`：它做结构校验（数量 / 行数 / code 升序唯一 / 无空字形 /
  无重复字形）并把每个字形打成 ASCII art 供人眼扫。**字形互换这类错误回读工具抓不到**
  （编码解码共用一张错表，见 `D-06` 归因②），所以形状必须由人看一眼。

### 复现方式

```powershell
# 1) 生成两张表（离线；D-09 那次含 23MB HDRI 解码，约 2-3 分钟）
py -3 tool\validation\verify_brdf_integrals.py --emit-glsl shader\glsl\validationAlbedoTable.glsl
py -3 tool\validation\verify_split_sum.py --emit-glsl shader\glsl\validationSplitSumTable.glsl

# 2) 采图（solo 场景；面板精确铺满）
.\tool\validation\run_paper_case.ps1 -Name D-05 -PanelMode curve `
  -Scene 'Maps/SC_paper_case_plot_albedo/SC_paper_case_plot_albedo.json' -Shots d05_mode6_albedo
.\tool\validation\run_paper_case.ps1 -Name D-09 -PanelMode curve `
  -Scene 'Maps/SC_paper_case_plot_split_sum/SC_paper_case_plot_split_sum.json' -Shots d09_mode7_split_sum

# 3) 逐列像素（mode 7 需要把交叉门限收到 8 px：cosine 与 sun 最近只差 10~13 px，
#    而描边只有 3 px 厚，20 px 的默认门限会把所有相关列整列丢掉）
py -3 tool\validation\measure_plot_curve.py curve <shot> --mode mode6 --roughness 0.1414 --curves direct ibl smith nog
py -3 tool\validation\measure_plot_curve.py curve <shot> --mode mode7 --crossing-margin 8 --curves constant cosine sun bistro4k

# 4) 图头文字（图例落在运行时 UI 包围盒里，需要临时关 ui.enabled 采一张，读完立刻还原）
py -3 tool\validation\read_plot_text.py <ui-off shot> --row caption --legend-count 4 --no-ui-overlay --expect "..."
py -3 tool\validation\check_plot_font.py            # 字体表结构 + 字形 art
```

现场证据：`artifacts/D-05/20260914_111822/`（mode 6 采图与面板检查）、
`artifacts/D-05/20260914_112039/`（关 UI 的 mode 6 图）、`artifacts/D-09/20260914_113636/`（mode 7 采图）、
`artifacts/D-09/20260914_113732/`（关 UI 的 mode 7 图）、`artifacts/regression2/` 与
`artifacts/D-06-final/`（逐像素回归对比的两侧）。

---

## 场景自述契约与运行期测试基线（2026-09-13）

> 两条都与"验证链路本身是否可信"有关，不是 shading model 的 case，所以单独记在这里。

### 1. `SC_*.json` 必填 `description`（新数据契约）

- **内容**：16 个场景全部补上 `description`，写清这个场景干什么用、以及它验证哪篇文章 / 书本的
  哪个知识点（探针场景写到节号 / 图号，纯学习场景写明"不参与论文 case"）。探针场景的描述直接引用
  本计划的 case 号（如 `SC_paper_case_plot_g1_variants` → `D-03` / `D-20` / `D-21`）。
- **契约位置**：`source/scene/validation/sceneAssetValidator.cpp`——字段缺失或为空即拒绝加载，
  错误信息带上场景完整路径；契约文本写在 `AGENTS.md` 的 "Scene JSON top-level fields"。
- **验证**：① 正例 `SC_sphere_array` + `SC_paper_case_plot_g1_variants` 同进程加载、切场景、截图全通过
  （`artifacts/scene-description/20260913_204352/`，`PASS: 2 shot(s) captured`）；
  ② 负例：把探针场景复制一份去掉 `description` 后启动，直接失败并打印
  `Scene.LoadFailed: Scene is missing non-empty string field "description" (state what the scene is for, and which paper/book knowledge point it validates)`；
  ③ 运行期测试自己造的场景也补了字段（`runtimeTestFixtures.cpp`），否则测试会因数据契约而失败。
- **归因**：缺失时**拒绝加载**而不是只警告，是刻意选择——这条约定一旦不强制，新场景就会继续少写。
  代价是任何手写的新场景都必须带一行说明；这是可接受的，因为描述本来就该跟着场景一起写。

### 2. 顺带修复：三条受支持的运行期测试此前必然失败

排查"新契约会不会破坏运行期测试"时发现测试本来就是红的，两条根因都在 fixture 与数据契约之间脱节：

| 现象 | 根因 | 处理 | 结果 |
| --- | --- | --- | --- |
| `--shader-reload-test` / `--world-graph-transaction-test`：绑定 fixture World 时报 `Texture.LoadFailed: Failed to find file: textures/T_UV_Checker.json` | 资源库提交 `236d213 资源整理` 把纹理从顶层 `textures/` 移到 `Common/Textures/`，而 `shader/glsl/runtimeTest/M_shaderReloadTest.json` 的三个默认纹理路径没有跟着改 | 三个路径改为 `Common/Textures/T_*.json`（资产与它们的 source 图都已确认存在） | 通过（exit 0） |
| `--world-graph-transaction-test`：`Material parameter requires non-empty usage description: u_worldGraphTransactionCandidate` | 材质参数元数据契约（提交 `3325f1c`）要求每个参数带非空 `description`，而测试在运行时注入该参数时只写了 `type` / `default` | 注入时补上 `description`（`worldTransactionRuntimeTests.cpp`） | 通过（exit 0） |

修复后三条受支持命令全部退出 0（日志在 `artifacts/scene-description/`）：
`--shader-reload-test`、`--shader-compute-reload-test`（含"ABI 变了要拒绝并保留旧管线"的用例）、
`--world-graph-transaction-test`（含 World/Graph 回滚、M_ 提交、retirement drain）。
**结论**：`description` 契约没有破坏运行期测试；反而是在确认这件事的过程中把两条陈旧 fixture 修好了。



