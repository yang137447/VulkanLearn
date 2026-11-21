# Cloth Ashikhmin Runtime

> 状态：**当前契约**（实现已落地）。本文件描述 VulkanLearn 现在的 forward-only
> Ashikhmin 2000 布料运行时；每条都能核对到源码、材质资产或场景资产。

## 范围

`Cloth` / `SHADING_MODEL_CLOTH`（ID 8）当前实现 Ashikhmin、Premože 与 Shirley 2000
的三个闭包：

| Material | Closure | Runtime distribution | Table |
| --- | --- | --- | --- |
| `M_clothAshikhminSatin` | Satin | 两段圆柱高斯按面积混合（70% σ=0.1、30% σ=0.3） | 2D `(mu, phi)` |
| `M_clothAshikhminVelvet` | Velvet | 各向同性倒高斯（σ=0.5） | 1D `mu` |
| `M_clothAshikhminSlantedVelvet` | Slanted Velvet | 绕侧法线倾斜的圆柱高斯（σ=0.5、40°） | 2D `(mu, phi)` |

运行时保留解析 `p(h)`；`g(k)`、`R0(k)`、`R1(k)` 与 `eta/c0/c1` 来自离线同口径的
LUT。直接光按闭包求 `p`、查 `g/P/R`、算一次 Schlick Fresnel；Velvet 与 Slanted
Velvet 使用 15° 相关遮蔽。间接光只做常值/低频环境近似：
`Lbar = EvaluateIrradianceSH(N) / PI`，镜面项为 `Lbar * R(N)`，漫反射剩余项为
`Lbar * rhoD * (1 - R(N))`；没有套 GGX split-sum 环境预滤波。

## Forward-only 路由

`Cloth` 不进入 `geometry` / deferred GBuffer。材质校验拒绝 `Opaque`、`OpaqueClip`
与 `ThinTranslucent`，只允许 `ForwardOpaque` 或连续透明 renderMode。`ForwardOpaque`
材质由 `forwardOpaque` pass 绘制；透明材质由 `forwardTransparent` pass 绘制。两者
共用 `ShadeClothAshikhminForwardSurface`。没有 Cloth GBuffer encode/decode 消费者。

## 材质贴图输入

三个母材质的 surface 入口统一走 `EvaluateMFPbrInputs(context)`，再套用布料闭包
参数。`M_clothAshikhmin*.json` 公开以下可选贴图和对应宏：

| Texture | Macro | Color space | Cloth consumer |
| --- | --- | --- | --- |
| `albedoMap` | `USE_ALBEDO_MAP` | sRGB | `u_tintColor` 相乘后作为 `rhoD` |
| `normalMap` | `USE_NORMAL_MAP` | linear | MikkTSpace 切线空间的表面法线 |
| `pbrParamMap` | `USE_PBR_MAP` | linear | R/G 是闭包不消费的元数据，B 是 AO |
| `emissionMap` | `USE_EMISSION_MAP` | sRGB | 加到 forward 最终颜色 |
| `alphaMap` | `USE_ALPHA_MAP` | linear | 与 `u_tintColor.a` 相乘覆盖 opacity |

`u_baseColor` 保留论文预览的最终 diffuse tint，默认仍是深色；带 albedo 贴图的
material instance 把它设为白色，让贴图与 `u_tintColor` 决定反照率。贴图宏由
material instance 稀疏覆盖，未启用贴图的现有论文场景保持原常值路径。

`pbrParamMap` 的 roughness/metallic 不驱动 Ashikhmin 闭包：Satin 的
`σ=0.1/0.3`、Velvet 的 `σ=0.5` 和 Slanted Velvet 的 `40°/σ=0.5` 来自 LUT 的
论文默认参数。AO 是唯一被光照消费的通道。normal-map green 不在 shader 内翻转；
DirectX Y- 源必须在导入阶段转换。

## 文件与资产

- 着色器：
  - `shader/glsl/common/ashikhminCloth.glsl`
  - `shader/glsl/common/clothAshikhminConstants.glsl`
  - `shader/glsl/M_clothAshikhmin.surface.glsl`
  - `shader/glsl/M_clothAshikhmin*.vertex.glsl` / `*.surface.glsl`
  - `shader/glsl/engine/forwardLighting.glsl`
- 材质：
  - `shader/glsl/M_clothAshikhminSatin.json`
  - `shader/glsl/M_clothAshikhminVelvet.json`
  - `shader/glsl/M_clothAshikhminSlantedVelvet.json`
- LUT 纹理：
  - `Common/Textures/T_clothAshikhminLut.json`
  - `Common/Source/Textures/T_clothAshikhminLut.exr`
- 运行时预览场景：
  - `Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_satin_runtime.json`
  - `Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_velvet_runtime.json`
  - `Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_slanted_velvet_runtime.json`
  - `Maps/SC_cloth_showcase/SC_cloth_3d66_dress.json`
    - 3d66 group017 的宽檐帽长裙；裙子 `Material #119` 与帽体
      `Material #121` 使用 Satin 闭包，其余身体/鞋/配饰使用 `M_pbr`
    - 源网格：`Maps/SC_cloth_showcase/Source/Models/SM_cloth_3d66Dress.fbx`
  - `Maps/SC_cloth_showcase/SC_cloth_showcase_3d66Dress.json`
    - **关卡级布料展示**：单人台，场景里同时存在
      `ShowcaseModel_satin/velvet/slantedVelvet/pbr` 四个重合变体，
      运行时只显示其中一个；顶层 `showcase` 块声明材质列表、默认材质、
      旋转轴 `rotatePivot` 和相机对象，`SceneAssetValidator` 校验、
      `WorldBuilder` 解析成 `WorldShowcase` 挂在 World 上
    - 交互由 `source/showcase/showcaseController.*` 负责：底部胶囊切材质、
      拖动旋转、滚轮推拉、右侧参数面板；`UiSubsystem` 只消费
      `ShowcaseViewModel`，不读 World
    - UI 资产：`ui/runtime-hud.rml`（`#sc-root` 展示层）+
      `ui/cloth-showcase.rcss`；视觉规范见
      `documents/plan/ui/studio-ui-design-system.md`
  - `Maps/SC_cloth_showcase/SC_cloth_showcase_compare_3d66Dress.json`
    - 静态四连排材质对比：同一套服装从左到右 Satin、Velvet、
      Slanted Velvet、DefaultLit PBR，间距 95 cm，不带 `showcase` 块
    - 模特清单：`Maps/SC_cloth_showcase/showcase_models.json`
      （`models[]` 的 `id`/`displayName`/`mesh`，生成时回填 `scene`）
- 服装导入：
  - `tool/cloth-intake/export_3d66_groups.ms`
  - `tool/cloth-intake/postprocess_3d66_exports.py`
  - `tool/cloth-intake/import_3d66_betterfbx.py`、`split_3d66_blender.py`、
    `organize_blender_collections.py`、`flatten_blender_outfit_hierarchy.py`、
    `scale_blender_outfits.py`
  - `Maps/SC_cloth_showcase/` 保存 3d66 导出后的 `SM_`/`MI_`/`T_`/`SC_` 资产

## 标准展示舞台

论文布面 CPU/GPU 同条件验证由
`tool/validation/offline/compare_ashikhmin_runtime.py` 生成四组控制场景：
Satin、Velvet、斜绒黑、斜绒红。两侧固定 EV100 9、补偿 0、ACES + sRGB、
5000 lux 单方向光、无环境/贴图，离线 0 bounces，实时关闭 Bloom。
离线手动曝光倍率为 1，纤维基显式取导入切线。
原 Velvet 实时预览也已移除额外点光并将 `rho_d` 从 0.006 统一为 0.002。
此验证覆盖默认论文参数下的主要响应，不证明全部角度、纹理或 GI 等价。

离线验证入口为同一生成脚本输出的
`SC_cloth_showcase_<satin|velvet|slantedVelvet>_offline.json`，运行需 `--offline`。
模型、舞台几何、SunPatch 和浏览态相机复用；布料使用独立离线论文求值器，
显式选择导入切线，复用颜色/法线贴图。环境关闭，墙地和非布料槽显式为
Lambert，因此它是布料诊断场景，不是完整实时画面的逐像素等价路径。
支持范围与导出工具见 `vl-offline-rendering.md`。

`tool/cloth-intake/build_cloth_showcase_stage.py` 生成舞台资产和每个模特的场景，
重跑会覆盖 `SC_cloth_showcase_<id>.json`，因此场景里的手动改动要回写到脚本。
交互主场景及其 C# 版本包含 `ShowcaseReference`，直接复用文章 lookdev 的
`SM_ue_color_calibrator.json`（色卡与参考球整体），位置 `(95,30,-150)`、
旋转 `(-90,0,0)`、scale `(90,90,90)`，为原 lookdev 实例 scale 300 的 30%。
模特 TRS、镜头和舞台灯光不因添加参考物而改变。
离线派生场景不包含该实时 PBR 组件，对比四连排也不添加；
参考物用于当前实际照明下的外观审阅，不构成认证色卡标定。

### 呈现态（active）与编辑态

展示控制器 `ShowcaseController` 有显式 `SetActive(bool)`，判定式是
`!editorFlavor || scriptRuntime.IsPlaying()`：

- **游戏档**：始终 active，关卡打开即呈现；有 C# 宿主时自动进 Play。
  游戏档未挂载 `IEditorUiShell` 时不启用编辑器离屏视口，
  即使配置中开发 UI 可见，也直接向窗口输出场景。
- **编辑器档**：只有 Play 中 active。编辑态下：
  - 不应用旋转/相机缩放，保持作者 TRS 和机位；
  - 展示 UI 隐藏（`ShowcaseViewModel::visible = false`）；
  - 拖动/滚轮不拦截，交给编辑器相机和选择；
  - C# 蓝图不 Tick、不接收 UI 事件（事件只在 Play 中路由）。
- **Stop**：`SetActive(false)` 恢复 World 提交时缓存的 TRS、相机和可见性。

例外：可见性在编辑态也应用一次——只显示 `defaultMaterial` 那个变体，
避免四个重合网格互相 z-fighting；这是唯一的编辑态世界修改。

- 舞台就是**两块互相垂直的平面**，不做出弧过渡（cyclorama）：
  - 地面：`Meshes/SM_cloth_showcase_floor.json` + `Source/Models/showcase_floor.obj`，
    水平面 `y=0`，`z=1500..-500`；材质 `MI_cloth_showcase_floor.json` 走 `M_pbr`，
    tint `0.178/0.154/0.131`、roughness `0.16`
  - 墙面：`Meshes/SM_cloth_showcase_wall.json` + `Source/Models/showcase_wall.obj`，
    垂直面 `z=-500`，`y=-50..700`；材质 `MI_cloth_showcase_wall.json` 走
    `M_pbr`，tint `0.182/0.158/0.134`、roughness `0.88`（粗糙）
- 墙地使用暖米灰材质；当前单灯轮廓检查中，挡光区域接近黑色。
  相机曝光固定为 EV100 9，当前截图不代表参考图的完整明暗还原。
- 两块平面的 90° 折线就是保留的墙脚缝，它由法线方向突变直接产生，不做焊接、
  不做倒角。地面边缘正好落在墙面平面，墙面往下多出 50 cm 藏在楼层以下，
  所以缝是可见折线而不是能看穿的黑洞
- 舞台半宽 1200 cm，地面延伸到相机后方 `z=1500`，墙面顶到 700 cm；这是为了让
  编辑器里换机位时不至于在画面边缘看到黑背景，正常展示机位下不可见
- 展示关卡和对比场景的模特都放在 `z=-150`（离墙 350 cm）；对比场景横向
  间距 95 cm（`x = ±142.5 / ±47.5`），展示关卡四个变体全部重合在 `x=0`，
  靠运行时可见性只留一个，避免复制几何
- 服装槽位：只有 `m04`（裙）和 `m07`（帽体）跟随闭合；`m01/m02/m03/m05/m06`
  在四个变体里保持同一份 PBR，保证对比只反映布料闭包
- `m05`（源 `Material #120`）是裙子开口露出来的贴身内衬，单独给黑色面料
  （tint `0.03/0.03/0.035`、roughness `0.55`、specular `0.45`）；`m03`
  （源 `Material #116`）才是灰色人台本体/头/腿，保持 `0.35` 灰
- 变体资产：`MI_cloth_3d66Dress_<satin|velvet|slantedVelvet|pbr>_m04/_m07`，
  以及引用同一份 `SM_cloth_3d66Dress.fbx` 的
  `SM_cloth_3d66Dress_<variant>.json`。PBR 对照使用同一套 albedo/normal，
  `u_pbrFactors.x` 为 `0.55`（裙）/`0.65`（帽）
- 四个变体共用布料 albedo 系数 `0.62`：原始贴图直出时裙子接近死白
  （215–230），sheen 差异全被压在色调曲线肩部；压到中间调后同一像素上
  Satin/Velvet/SlantedVelvet/PBR 约为 `177/190/203/206`
- 墙与地使用同一层漆：墙面 tint `0.182/0.158/0.134`、地面 `0.178/0.154/0.131`，
  亮度差只来自朝向、粗糙度和反射；地面 albedo 明显更亮时会在墙脚形成硬亮带
- 展示关卡当前**只保留阳光**：`SunPatch` 一盏 spot 同时负责照明和投影。
  位置 `(-1500, 1100, 1800)`、`rotation (-20, -33.11, 0)`，
  外/内半角 `35°/32°`、8000 cm 衰减、850000 cd、
  暖色 `1.0/0.94/0.84`，接收面阴影 bias 为 8 cm。
  地面人台投影延伸向右后方；主体头部投影落在墙脚以下，
  不再在墙面右侧形成完整人形剪影。
- 其余灯（`KeySoft`/`KeySpot`/`FloorWash`/`SheenLight`/`FrontFill`）保留在
  场景里但强度为 0。
- `ShowcaseSunFlag` 引用 `SM_cloth_showcase_gobo`，一个网格内含两个封闭的
  4 cm 厚遮光实体：上方斜边挡板与左侧竖边挡板。
  两者位于 `z=800`，在当前展示和对比机位的相机后方、阳光前方。
  墙面亮区左边界为 `x=-166`，上边界连接墙面点
  `(-166,243,-500)` 与墙脚收口点 `(347,0,-500)`；
  生成脚本通过阳光位置将墙面目标边界反投影到挡板中面，
  实际边缘受厚度、阴影采样与 bias 影响略有偏移。
  这些是墙面世界坐标，非屏幕坐标。
- 墙面斜边与左侧暗区来自遮光实体的阴影，不依赖聚光灯锥边。
  删除 `ShowcaseSunFlag` 的对照场景用于检查这两个边界的来源。
- 当前是单灯阴影路径，未接入面积光源半影；边缘的采样过滤
  不代表参考图的物理软阴影。
- 模特 `MODEL_Z = -150`（离墙 350 cm）；相机 fov 35°（水平），
  展示机位为 `(54,72,620)` 看向 `(54,74,-150)`（纵深距离 770 cm），
  人台占画面高度约 63%；打开参数面板时使用作者机位，
  水平中心约为画面宽度的 38%。关闭面板时，控制器沿相机 right
  方向平移镜头，使 `rotatePivot` 水平居中；切换面板保持缩放状态，
  原生回调和 C# 外部状态请求共用该构图规则。
  模特 TRS 保持原资产值，通过相机平移、距离与俯仰匹配构图。环境仍是
  `proceduralSky` intensity 1.0
- `SunPatch` 使用现有 spot 局部阴影路径；在其余灯强度为零的当前场景里，
  阴影区域仍接近黑色。

## LUT 图集

`T_clothAshikhminLut.exr` 是 256×256 RGBA half EXR，按材质纹理合同加载为线性、
无 mip、clamp、双线性采样：

```text
(0,0)    64x64  Velvet 1D table replicated on every row
(64,0)   64x64  Satin 2D table (x=phi, y=mu)
(0,64)   64x64  Slanted Velvet 2D table (x=phi, y=mu)
(192,0)  3x1    (eta, c0, c1) constants for Velvet/Satin/Slanted
```

RGB 通道是 `g, R0, R1`。运行时 `R = R0 + f0 * (R1 - R0)`；漫反射剩余能量用
`mix(c0, c1, f0)`。Slanted Velvet 的 `R` 在 `f0=1` 时可以超过 1，shader 不夹
镜面 `R`，只把 `(1-R)` 的漫反射剩余项夹到非负。

## 生成与验证

- `tool/cloth-runtime-lut/generate_cloth_ashikhmin_lut.py`
  - 生成 `shader/glsl/common/clothAshikhminConstants.glsl`
- `tool/cloth-runtime-lut/write_cloth_lut_exr.py`
  - 生成 `Common/Source/Textures/T_clothAshikhminLut.exr`
- 启动冒烟：

```powershell
build\bin\main_editor.exe --initial-scene Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_satin_runtime.json --exit-after-frames 2
build\bin\main_editor.exe --initial-scene Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_velvet_runtime.json --exit-after-frames 2
build\bin\main_editor.exe --initial-scene Maps/SC_cloth_ashikhmin_runtime/SC_cloth_ashikhmin_slanted_velvet_runtime.json --exit-after-frames 2
```

## 当前边界

- LUT 只烘论文默认参数（Satin σ=0.1/0.3、Velvet σ=0.5、Slanted 40°/σ=0.5）；
  修改 `u_sigma`/`u_slantDegrees` 不会重烘表。
- 3d66 合集原始的 596 MB `.max` 不能直接进引擎。当前链路是：在 Blender 4.5 里
  用 BetterFBX 6.1.0 导入整套 FBX 导出件，每套一个 collection、层级压成
  `<name>_ROOT` 空物体 + 网格、整体 ×2.54 烘成米制且部件 scale 归一，
  再单独导出要展示的那一套。`artifacts/incoming/**` 里的中间 `.blend` 不入库。
- 舞台地面是光滑 PBR，但 SSR 还没接；地面反光目前只有 IBL 和直接光高光。
- 方向光的阴影是一张全局遮罩：被遮挡像素拿不到任何直接光，环境项也几乎为 0，
  所以方向光主光会打出纯黑剪影。舞台因此改用 spot 主光（逐灯阴影只衰减自己
  那一份贡献），阴影是灰的；半影质量、接触阴影和环境补光仍属于后续阴影改进。
- `p(h)=0` 只归零镜面项，漫反射继续按剩余方向能量计算，与
  `vl_offline_core` 的 `satinBrdf`/`velvetBrdf`/`slantedVelvetBrdf` 一致。
  运行时曾经在半程向量落出 lobe 时把整条结果返回 0，导致 Slanted Velvet 的
  裙褶和帽檐出现纯黑条纹；2026-11-17 已按离线实现修正。改这一处时不要顺手把
  `AshikhminClothDensity` 的截断也去掉：那是论文的分布支撑，不是漫反射开关。
- 服装材质默认使用 `ForwardOpaque`。连续透明只开放了 renderMode 校验，尚无
  专门的透明服装母材质、折射/透射或排序参数。
- `Cloth` 没有 deferred/GBuffer 路径，也没有 Cloth 专用 debug view 数据供给。
- 环境项是低频/常值近似，不是按闭包 `p(h)` 预滤波的环境。
- `Cloth` 的双面法线翻转与 `fiberRotationDegrees` 已接线；没有切线时
  `BuildAshikhminClothFrame` 使用稳定的回退轴，失败不静默改变闭包。
- 没有 Cloth LUT 数值单元测试；当前验证是离线表生成 + 三个运行场景冒烟/截图。
