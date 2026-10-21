# GASP P3 控制与贴图关卡记录

日期：2026-10-18。这是执行证据，不是实现契约。
接续 [P2 蒙皮记录](gasp-p2-skinning-record.md) 与
[GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。
当前边界见 [动画播放与 GPU 蒙皮](../contracts/animation-runtime-skinning.md)。

## 结果与验收边界

C# 状态机、原生片段请求/反馈与 Jolt 实际运动已形成闭环；
走跑、跳落、碰墙及生命周期由真实 CLR/Jolt 模块回归验证。
正式 DefaultLevel 的静态场景、贴图和角色自动 Play 已出图。
**真实键鼠、Play/Pause/Stop 与相机操作仍待补验，不宣称 P3 完整操作验收通过。**
没有实现片段混合、Motion Matching、IK、曲线/notify 执行或障碍翻越。

角色动作包括 Idle/Walk/Run/Jump/Fall/Land。C# 选择动作与第三人称相机，
原生独占片段时钟、完成反馈和姿态；胶囊/Jolt 独占 Actor 位移与朝向。
Walk/Run 为 200/450 cm/s，jump 为 500 cm/s，胶囊半径 30 cm、
半高 60 cm；visual 偏移 -90 cm、yaw 180°。实际速度取物理步位移，
碰墙不再把输入意图当作走路速度。镜头使用局部扫球缩短相机臂。
当前换片段硬切；播放/暂停期间有受控骨骼时，程序集重载要求先 Stop。
同一导出边界内移除再添加完全相同组件 ID 的原生代际身份仍有限制，
不能保证该情况自动重启时钟。

## 正式资源与入口

资源根 `D:/YYBWorkSpace/GitHub/VukanLearnResources`：

- `Maps/SC_gasp/SC_gasp.json`：保留 P2 审阅场景。
- `Maps/SC_gasp/SC_gasp_playground.json`：独立可运行 DefaultLevel。
- `Blueprints/GaspCharacter.blueprint.cs`、`Blueprints/BP_gasp_character.json`：
  位于该 Maps 目录，保存高层状态和胶囊/模型/相机配置。
- `Source/Textures/Level`、`Textures/Level`：源图与正式纹理描述。
- `Materials/LevelTextured`、`Meshes/LevelTextured`、`Source/LevelTextured`：
  重建材质、模型描述、Landscape 与 Logo 静态几何。
- `Generated/Import/SC_gasp/material-import.json` 与 `MaterialGraphs`：
  审计记录和源图文本，不是正式资产的运行时依赖。

迁移 DefaultLevel 的 110 个可见静态实例、6 种 LOD0 网格与原有 64 个
启用碰撞的组件，另加入 Landscape 和 Logo；纹理阶段生成 78 个 MI、
85 个模型描述和 9 张源贴图。原始审阅色板仍留在资源目录但不被该场景使用。
静态实例由被动 C# 类承载；源 UE 交互、传送、小部件和 NPC 行为不执行。

```powershell
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_playground.json --dev-ui
```

编辑器点击 Play 后，WASD 移动、Left Shift 跑、Space 跳、鼠标观察。
这是操作入口说明，不能当作这些键已在本轮桌面操作的证据。
临时自动 Play 截图结束后已删除 config 的 scripts.autoPlay；
默认 initScene 仍为原 SC_sphere_array。

原 UE 目录仅作只读输入，commandlet 使用 P0 隔离副本，未保存源项目。
收尾盘点 `artifacts/gasp-p3-source-inventory.json` 为 3902 文件，
输入 BLAKE3 与 P0 一致：
`8f2fba8d398635082bd06428d6d971c828573f9613c4cd905a8b9fc204e40afe`。
技术验证不补齐 P0 使用范围确认与 UE 操作行为录像。

## 材质与贴图

成功导出为 `artifacts/gasp-p3-material-export06/materials.json`：
114 条材质记录、9 张纹理、11 个材质/函数文本、111 条组件记录。
导出在隔离 UE 5.8.2 中解析 MID/MIC 继承与静态开关，不调用保存。
9 张源图逐像素核对行转换和颜色空间通过，HDR 字节保持一致；
正式 JSON 中 703 次 Maps/Common 引用存在，未引用临时导出目录。

| 源纹理 | 尺寸 | 颜色空间与用途 |
| --- | --- | --- |
| T_Hologrid | 1024×1024 | sRGB，投影网格 |
| T_Grid_Crosses | 4096×4096 | sRGB，投影十字 |
| T_Paint_Normal | 2048×2048 | linear，漆面法线 |
| T_Paint_Diffuse | 2048×2048 | sRGB，漆面颜色 |
| T_Paint_Glossiness | 2048×2048 | sRGB，源图按 G 产生 roughness |
| T_Default_Material_Grid_N | 256×256 | linear，保留隐藏组件依赖 |
| T_Default_Material_Grid_M | 512×512 | linear，保留隐藏组件依赖 |
| T_sky_01_8k | 8192×4096 | linear HDR，天空与环境 |
| T_GameAnimationSample_Logo | 6000×6000 | sRGB，生成 Logo alpha |

PNG 离线翻行以抵消 VL 材质上传的一次翻行；HDR 环境路径不翻行。
两个 Engine 默认图来自隐藏组件，只保留，未强加给可见几何。
`M_gaspGrid` 重建三层投影合成、顶部颜色、漆面与法线；
世界空间重排回 UE XYZ，物体投影按 primitive bounds 中心与归一化物体轴。
专用顶点入口借未使用的 vertexColor varying 保留源局部法线插值，
不改变公共 varying ABI。投影使用源负 TextureSize、
CheapContrast 侧面混合后顶面混合，不改成归一化三轴权重。
源 Glossiness 按 sRGB 解码后的 G、强度、幂和范围混合生成 roughness，
没有按文件名擅自反转；法线保留源切线语义，没有追加绿色通道翻转。

透明材质使用普通 PBR alpha blend，常量自发光通过 linear 8-bit 1×1 图表示，
存在量化差异。Logo 使用 1200 cm 静态 alpha 接收平面替代 deferred decal；
由源 sRGB 解码 R 生成透明度，颜色/alpha 为 [1,0.5,0,0.4]，
接收平面沿法线偏移 0.05 cm，不提供通用 decal 投射。

按用户建议，正式 Scene 通过生产 get_document/get_capabilities/modify/
validate/save 把 sun 设为 **10 lux**，配套环境强度 **2**、固定 **EV100=3**。
这是本关卡审阅设置，不是通用日光物理建议，也不是 UE Lumen/曝光等价结论。

## Landscape 与失败修正

源 Landscape 有 64 个组件，公开 RT 回读为 249×249，
不是此前观察时误判的平面。必须预载引擎高度导出材质，
等待 `Editor.AsyncAssetCompilationFinishAll` 后再回读；
UE API 返回 true 仍可能得到全黑清屏图。导入器在正式写入前拒绝该结果。
最终从 export06 在隔离资源目录重建后提升，修复了先前 export05 的清屏高度。
正式导入器拒绝已有专属输出，但不承诺多文件事务回滚。

RG 解码 localZ=(R×256+G-32768)/128，源平移 [-3100,2100,-10]、
scale=25、单位旋转，转换成 123008 个普通静态三角形及 Jolt 碰撞。
法线来自高度有限差分，没有 terrain 运行时类型。
源 Z 范围约 [-7.0703,607.7734] cm，公开 Canvas 回读重采样后为
[-7.0703,616.9531] cm，最大高度边界差 **9.1797 cm**。
这是可见几何近似，不声称逐顶点或原始高度图身份一致。

## 验证证据

MinGW Debug、C# ON、动画工具 ON 的全部目标构建通过。
根 CTest 1377 项：1371 通过、6 项符号链接权限跳过、零失败，
479.74 秒（`artifacts/gasp-p3-ctest-all.log`）。
`script_tests` 314 项连续两遍通过；C# OFF Release 构建与 77 项连续两遍通过；
动画原生 46 项及 CLR 动画专项 13 项各连续两遍通过。
覆盖请求原子性、独立时钟、缩放时间/Pause、Stop 身份、
回调失败回滚、未知片段、移除组件、热重载拒绝、
真实 GASP 状态转换、Jolt 跳落与撞墙速度。
本轮最后增加贴图后 main/asset editor 构建通过；没有再次无差别重跑全部 CTest。

最终 `artifacts/gasp-p3-textures-verified` 有 overview/logo/detail/
baseColor/normal/roughness 六张完整 1280×720 BMP，每张 2764854 bytes，
stderr 均为空；逐图查看了网格、橙灰障碍、透明标记、Logo 与天空。
相机在加载前固定，以消除相机命令与截图同帧的 GT/RT 延迟。
`artifacts/gasp-p3-play-textured/play.bmp` 使用临时 autoPlay、
双线程和 240 帧等待，显示角色第三人称相机、Idle/原生时间反馈与正式贴图场景；
stderr 为空。以上截图进程由工具终止，不构成正常退出证据。

图形 reload 与 Compute reload 在正式 P3 场景、双线程、no-dev-ui 下串行通过，
退出码 0，均确认 `EngineLoop teardown complete`。
日志为 `artifacts/gasp-p3-shader-runtime.log` 和 `gasp-p3-compute-runtime.log`。
Compute stderr 仅含故意改变 workgroup size 的预期 ABI 拒绝，随后恢复通过。

World/graph transaction 同样从正式 P3 场景双线程启动，串行运行后退出码 0，
`artifacts/gasp-p3-world-runtime.log` 确认回滚、World/light 同步提交、
graph-only、resize、shader/M_ 交错与退休排空通过及正常 teardown，
stderr 为空。该检查包含生产夹具切换，不能替代用户的关卡切换操作验收。
65 条架构边界、40 个正式场景 schema、Python 工具语法、
文档分层/链接与 `git diff --check` 均通过。

Computer Use 的 Node 内核初始化报告 `failed to write kernel assets`、
`os error 3`，重置一次后仍失败，没有发生真实键鼠操作。
原生截图与模块测试分别作为渲染/逻辑证据，不替代实窗交互验收。
既有 runtime HUD 的窄场景名栏仍会纵向换行，本轮未修改 UI 布局。

## 用户实机反馈补记

2026-10-18：按用户要求重新配置并构建 Development，
`build-vscode/development/bin/main.exe` 使用 RelWithDebInfo、
C# ON、Vulkan Validation OFF，双线程启动正式 P3 关卡并开启编辑器。
构建日志为 `artifacts/gasp-development-build.log`，运行日志为
`artifacts/gasp-user-development.log`，stderr 为空。
用户操作后反馈“运行起来没啥问题”，记为 Development 实机运行冒烟通过。
运行日志另确认 `Quit event received` 与 `EngineLoop teardown complete`，
此次实机正常关闭通过。

用户反馈没有逐项列出走跑、跳落、镜头、Pause/Stop 或骨骼预览控件，
因此不把本条扩展为完整动作/控件矩阵验收，也不将其视为 Motion Matching 验收。
前文自动化交互受阻与待验收描述保留为当时的执行历史。
