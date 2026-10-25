# C# Blueprint Runtime

状态：当前契约，2026-10-18。对应 `source/script/`、`script/`、
`source/editor/scene/scriptBlueprintAuthoring.cpp` 与 GoogleTest `script_tests`。
覆盖蓝图作者、游戏逻辑、物理、编译重载、动态资源和导出；完整目标及历史见
[开发计划](../plan/architecture/csharp-actor-runtime-plan.md)。

作者功能统一称为蓝图（Blueprint），不包含可视化脚本节点图。
资产为 `Blueprints/BP_*.json`，C# 源码为 `*.blueprint.cs`，基类为 `Blueprint`，
运行时句柄与作者引用分别为 `BlueprintHandle`、`BlueprintReference`。
编辑器只注册 `blueprint.*` 命令；旧 Actor 资产类型、场景类型、引用字段和命令不兼容。

骨骼 MeshComponent 的片段请求、原生反馈、Play/Pause/Stop 与重载限制见
[动画播放与 GPU 蒙皮](animation-runtime-skinning.md#c-片段控制)。
`Animation.Match` 提交数据库与允许片段集合、动作锁及可选 `matchSpeed`、
`footPlacement`、`inertialization`、`rootOffset` 开关；原生拥有搜索、帧/时间选择、
稳态步频、交叉混合、双参考点接地与视觉根修正。
足部与根修正只作用于渲染姿态，MM 历史读取 pre-IK/无根修正姿态，Actor/Jolt 继续拥有位移。
单数据库、空筛选、同版本资产绑定、查询/代价冻结及首次同步加载限制见
[动画轨迹与特征数据库](animation-motion-features.md#运行时绑定与搜索)。
`CharacterController.LinearVelocity` 为每帧角色实际位移除以帧 dt 得到的实际速度，
不再表示撞墙时仍可能非零的期望走路速度。
`StepPhysics(0)` 的零时步指令同步帧保留最近一帧的实际速度；胶囊纯 Y 轴转向不清速，
也不视为位置传送。显式位置传送清零该反馈并刷新支撑查询。
正式 GASP 控制器用该实际水平速度决定地面状态：小于 `2 cm/s` 的 Idle
只允许 Idle 与 WalkStop 两个片段；持续向墙输入也遵守这个动作族。
Walk/Run 保留包含循环、起步、停止与左右转身的六/七成员候选集合，
由原生比较具体帧，C# 不指定片段时间或目标帧。

资源侧 GASP 控制器的 `TurnSpeed` 属性默认 `540°/s`，用 scaled delta
限制 Actor 每帧 yaw 变化，并取 `(-180°, 180°]` 内的最短角差；
恰好 `180°` 固定向正角度旋转。每帧从当前 Transform 计算，不维护第二份旋转时钟。
无输入保留朝向，Move 意图仍立即按相机方向提交；相机局部 yaw 抵消 Actor 转向。
`TimeScale=0` 不消耗转向预算，但 Tick 内鼠标观察仍按控制器既有输入语义处理。
该属性没有加入正式 BP 资产的 `instanceEditable` 清单，正式 BP 实例 Details 不提供其编辑。
正式角色的 `MatchSpeed` 默认关闭，作为实验对照项加入 BP 的实例编辑清单；
修改后按需重新提交 Match。HUD 的速率与速度残差来自原生反馈，
不由 C# 计算动画时钟。API 默认 `matchSpeed=false`，旧调用保持原速。
它匹配源根行程与实际速度，不保证足底锁定或消除脚滑。
正式角色的 `FootPlacement`、`Inertialization` 与 `RootOffset` 同样默认关闭并允许 BP 实例编辑；
`FootLock` 默认 `Auto`，可显式切 `Ball`/`Ankle` 与 UE `PivotAroundBall/PivotAroundAnkle` 对照。
`PackageProbe` 默认关闭，仅用于独立包固定动作矩阵的测试 BP：开启时用固定时序替代
Move/Sprint/Jump 输入并记录 MM 状态，不改变正式默认输入路径。
HUD 显示原生接地原因、骨盆偏移与根偏移状态，`Animation.FootPlacement` 提供双足只读诊断。
接地使用 ball 与踝下 heel 双绑定参考点、显式/自动支点和两骨 IK，
不能作为真实鞋底网格、完整台阶质量或移动平台保证。
原生局部姿态混合、未来朝向近似及脚滑限制见
[动画播放与 GPU 蒙皮](animation-runtime-skinning.md#c-片段控制)。

Blueprint/Component 的 `DebugDraw` 服务提供点、线、折线、箭头、圆、球、
定向盒、胶囊、坐标轴和世界文字。调用限制、发布者句柄隔离、
单帧/计时/持久生命周期、异常回滚与原生预算见 [公共 DrawDebug](debug-draw.md)。
脚本只生产世界空间值，不持有 UI/渲染对象；自动清理在同批新几何拒绝时仍生效。

## 构建与配置

Windows x64、MinGW 和 .NET 8 SDK：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=ON
cmake --build build -j8
.\build\bin\main.exe --dev-ui
```

SDK 未进入 PATH 时使用 `-DVULKANLEARN_DOTNET_EXECUTABLE=D:/path/to/dotnet.exe`。
开关默认 OFF；关闭时不查找 SDK，普通场景照常构建，含 Blueprint 的场景加载被拒绝。
构建输出在 `build/bin/managed`：桥接、Gameplay DLL/PDB/runtimeconfig 与 hostfxr。
托管配置跟随原生构建：Debug 使用 Debug，其余配置使用 Release。
缺省路径优先取当前可执行文件旁的 `managed`，开发环境才回退到构建期输出路径。
`cmake --build build --target vl_script_package` 额外复制 SDK 随附的 .NET 8
`shared/Microsoft.NETCore.App` 与 `host/fxr` 到 `managed/dotnet`。
宿主优先显式使用该私有根；没有该子目录时使用配置期 SDK 根。
程序和完整 managed 目录可一起搬移，C# 宿主不要求目标机器安装 SDK。
完整游戏导出使用下文 `vl_export_game`，同时收集配置、资源、shader 与原生依赖 DLL。
开启时同时下载并构建锁定的 Jolt 5.6.0 CPU 库（提交
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`，归档 SHA256 在
`script/CMakeLists.txt`）。不构建 samples、GPU compute、毛发模拟、debug renderer
或 profiler；x64 使用 SSE2 基线，不要求 AVX2。第三方编译选项隔离在子目录，
不覆盖工程的 Debug/Release 配置。关闭时不获取 Jolt，不依赖 Bullet。

`config/config.json` 的可选 `scripts` 对象包含 `enabled`、
`runtimeDirectory`、`assembly`、`autoPlay`，两个路径相对项目根解析。
`autoPlay` 缺省 false；true 时每个正式实时关卡 generation 提交后自动走同一 Play
入口一次，Stop 不会自行重启，资产预览与离线模式不启动。
编译启用时缺省启用宿主，并使用该构建目录的 managed 输出和 Gameplay.dll。
初始化失败报告错误。桥接/API 常驻，Gameplay 及托管依赖位于可回收的分代加载上下文，
通过共享桥接 Blueprint 类型保持 API 身份；DLL/PDB 读入内存后关闭文件。
不卸载 CLR；Gameplay 可手动重载或通过编辑器编译后重载，桥接/API 改动仍需重启。
Stop 只重建实例，Gameplay 静态字段在程序集换代后重置。

### 引擎与用户源码归属

引擎仓库的 `script/VulkanLearn.Runtime/` 拥有常驻桥接、Blueprint/Component 基类与公共 API；
`script/Gameplay/Gameplay.csproj.in` 只提供构建模板，不存放用户游戏逻辑。
用户源码属于 `resourcePath` 指向的资源仓库：

```text
Common/Blueprints/
  DoorBlueprint.blueprint.cs
  BP_wooden_door.json
Maps/SC_example/Blueprints/
  BP_iron_door.json
Generated/Runtime/CSharp/
  Gameplay.csproj
```

只服务一个关卡的源码随地图目录管理，多关卡共享代码归 Common。
所有 Common/Maps 子目录的 `.cs` 都进入同一个 Gameplay.dll，
普通用户数据类不要求 `.blueprint.cs` 后缀；不是每个地图单独一个程序集。
类型由完整命名空间身份区分，移动源码目录不改变 BP 的 `class`。
上例两个门资产可引用同一个 DoorBlueprint 类，源码与配置文件不按文件名配对。
初始构建与编辑器编译共用同一源码收集器及显式 MSBuild 清单，
扩展名和 bin/obj/.git/Generated 排除目录按 ASCII 大小写不敏感匹配；
资源编译服务拒绝源码符号链接。
编译请求冻结明确源码清单，监视新增、删除、改名和字节变化；BP JSON 变化不触发 C# 编译。
资源编译不再从旧引擎 Gameplay 源码目录收集用户代码。

CMake 根据 config.json 的 resourcePath 生成供 IDE 使用的 Gameplay.csproj；
`VULKANLEARN_SCRIPT_RESOURCE_ROOT` 可显式覆盖其资源根。项目引用引擎 Runtime，
Generated 中的项目可重建，不作为作者源码维护；初始构建的中间产物隔离在构建目录。
运行中编辑器编译按当前 RuntimeConfig 的资源根收集源码，候选仍进入
Generated/Runtime/ScriptBuilds，不覆盖活动程序集。

## 场景与编辑器

现有 Scene 的 `objects` 可添加：

```json
{
  "type": "blueprintInstance",
  "id": "rotator-01",
  "name": "Rotator",
  "class": "Gameplay.RotatorBlueprint",
  "enabled": true,
  "position": [0, 0, 0],
  "rotation": [0, 0, 0],
  "scale": [1, 1, 1],
  "properties": {"DegreesPerSecond": 45},
  "components": [
    {"type": "mesh", "modelPath": "Common/Meshes/SM_material_preview_ball.json"}
  ]
}
```

`id` 在同一场景唯一，禁止 `/`、`\`、`|`、NUL；显示名称不用于脚本身份。
TRS 必填，位置厘米、旋转角度，缩放非零。`enabled` 缺省 true，
`properties` 必填，接受下文列出的命名类型值。`components` 可省略或为空，最多 128 个
`scene`/`mesh`/`tressfx`/`camera`/`springArm`/`pointLight`/`spotLight`/`directionalLight` 组件。多个组件必须给唯一稳定 `id`；可选 `parent` 指向同 Blueprint
组件，空串表示挂在 Blueprint 根。拒绝重复 ID、缺失父级与环。局部 position/rotation/scale
可省略，默认零/零/一；mesh 另带 modelPath 与可选 materialOverride。
tressfx 组件另带 `modelPath`（必须是 `type: "tressfx"` 的 `SM_*.json`）、可选
`rootBone` 与 `weights`（`auto`/`rootBone`/`mesh`），并且必须有 `mesh` 祖先组件。
它展开成场景 `mesh` 对象，把放置级绑定写在生成对象上
（`__tressfxSkinningSource`/`__tressfxParentModelPath`/`__tressfxRootBone`/`__tressfxWeights`），
不写作者 JSON、不生成临时描述符；网格模板键包含父组件身份，跨 actor 不共享绑定。
tressfx 组件的局部 TRS 必须是单位阵（与父 mesh 同一对象空间），否则加载期拒绝。

蓝图资产页的组件添加入口额外提供 `TressFX`；Scene 侧通用组件面板不提供，因为它没有
“挂在哪个角色上”的上下文。添加时父级自动取当前选中组件向上最近的 `mesh` 组件，
父级下拉只列 mesh；没有 mesh 祖先或 `modelPath` 为空时拒绝添加并给出提示，不生成
加载期必然失败的组件。Details 只暴露 `Hair Asset`、`Root Bone`、`Root Weights`
（`auto`/`rootBone`/`mesh`，缺省 `auto`）；局部 TRS 与图元碰撞控件不显示，写入也被
忽略；tressfx 组件不进入 `componentInstanceEditable`，所以 Scene 实例不能覆写它的字段。
Scene/关卡 Details 里这三个字段以只读值呈现，并提示“在蓝图资产里编辑”；事件处理层
同样拒绝来自关卡侧的 `modelPath`/`rootBone`/`weights` 写入，不存在“看起来能改、实际被
丢弃”的假编辑。

tressfx 组件的展开在蓝图资产预览、关卡放置和正式加载中是**同一条链**（预览把组件塞进
一个内联 `blueprintInstance` 后走同一个 World 加载事务），不存在“仅供预览的静态特例”。
毛发以 rest 中心线即可绘制，不要求模拟推进：`simulation.enabled`/backend 只门控 compute
模拟，不门控 draw。

运行时 `Spawn` 的蓝图演员同样成立：克隆时重写放置级绑定身份并建立该演员的组件网格模板，
渲染侧为本帧未知的 placement 键创建独立的 renderable（模拟状态、params UBO 与绑定各一份），
销毁或资产释放时按 placement 作用域退休。具体日志与计数见
[tressfx-asset-import.md](tressfx-asset-import.md) 的「运行时实例化」。
Console `script <play|pause|stop>` 调用与编辑器 Play 按钮相同的 `ScriptRuntime::Control`，
供无 GUI 烟测/自动化驱动蓝图生命周期；它不是 runtime test flag。
父链完整矩阵参与合成，保留非均匀缩放导致的 shear，不做 TRS 分解。
显式 ID 的 mesh 身份为 `__blueprint/<actor-id>/<component-id>`；
旧无 ID 单根 mesh 保留 `__blueprint/<id>` 身份。由正式 mesh loader 导入资源和材质；
作者 Scene 不保存派生对象，普通对象名称不得占用此前缀。

Content Browser 的 C# Classes 虚拟目录与 C# Blueprint Classes 筛选展示当前程序集注册的根蓝图类，
按完整命名空间区分；父类及源码位置显示在条目提示中，类目录随成功程序集重载更新。
此目录使用 `csharp:/完整类名` 的 UI 内部地址，不进入 JSON 资产库或资产加载路径。
双击及右键 Open in VS Code 只提交类身份，GT 从当前类目录重新解析资源源码，
通过 `vscode://file/` 打开；C# 类没有引擎编辑 Tab。
源码位置来自匹配程序集的 Portable PDB 类型文档与方法序列点，支持空类、文件名与类名不同及 partial；
PDB 缺失时类仍可配置，打开源码报告无可用位置。
只允许资源根 Common/Maps 下的现存 `.cs`，排除生成目录；路径经规范化、根边界校验及 URI 编码。
本机 VS Code URI 处理程序不可用时由平台打开操作报告错误。

类条目右键 Create Blueprint Asset 打开创建表单。
Outliner 底部加号的 Mode 也提供 Place instance 与 Create Blueprint Asset：
前者选择类、名称、可选 mesh 路径向关卡放置实例；
后者选择当前已编译根蓝图类、名称和 Common/Blueprints 或 Maps/SC_*/Blueprints 下的 BP_*.json 路径
（支持子目录），GT 校验类身份后只创建 JSON 并刷新目录。
创建不要求选中实例或当前关卡，不执行 OnCreate，不生成 C# 源码或发起编译。
同一类可创建多个资产，`class` 指向 C# 类型，`properties`/`components` 初始为空，
未写入的属性从 C# 类默认值继承；没有 JSON 对 JSON 的继承链。
移除类、逻辑组件类及无效路径拒绝作为创建目标；已有 JSON 拒绝覆盖，已有相邻源码不阻止创建。
文件沿用完整验证后的 create-new 发布，不提供文件操作 Undo。
新建 inline Blueprint 的 `properties` 为空，不把类默认值复制成实例覆写。
Details 显示根 TRS、类、ID 和 Expose 属性。属性支持 public
bool/int/uint/float/double/string、System.Numerics.Vector2/3/4/Quaternion、非 Flags enum、
AssetId、BlueprintReference 可读写属性上的 `[Expose]`，以及下节描述的内联数据类型。
根 Expose 仍使用属性；自定义数据内部可使用公开可写字段。
向量存为长度精确匹配的有限 float 数组；枚举存为大小写精确的声明名称，
拒绝数字、未声明值和 Flags。枚举使用下拉框，向量使用分量输入；
连续分量请求从最新权威草稿合成，不用旧快照整向量覆盖。
Quaternion 存为 `[x,y,z,w]` 四个有限 float，长度平方与 1 的差不超过 `1e-4`；
不自动归一化，非法完整候选被拒绝。它是 Expose 属性类型，根/空间组件 rotation
仍使用角度制 YXZ Euler。BP/Scene 的 Blueprint 与逻辑组件属性均用一个完整 XYZW
数组文本输入，整组有效后才提交；非法或未完成文本留在输入框，不逐轴发布非单位候选。
普通向量继续使用分量控件。
AssetId 存规范资源根相对路径字符串或空串；不解析文件存在性、不导入资产。
BlueprintReference 存不含 `/`、`\`、`|`、NUL 的稳定 Blueprint ID 或空串；
`Resolve(World)` 在有效所属回调中返回可空句柄，不序列化 Play 上下文句柄。
`[Expose]` 只允许配置 Blueprint 默认值，不自动允许场景覆写。
inline Blueprint 的类使用 `[Expose(InstanceEditable = true)]` 显式开放实例属性；
BP 实例使用资产的 `instanceEditable` 名称白名单，缺省为空。
未开放项在场景 Details 只读，GT 编辑请求和正式加载同时校验权限，
手写 Scene JSON 不能绕过。根 TRS/名称/启用状态仍由场景实例独立拥有。
未覆写的属性采用程序集构造默认值。
类必须 public、非 abstract、有无参构造。示例包括 RotatorBlueprint、PlayerBlueprint 和
ThirdPersonCharacterBlueprint。Blueprint 与逻辑组件的 `class` 接受完整类型名
（`Gameplay.RotatorBlueprint`）、简单程序集限定名
（`Gameplay.RotatorBlueprint, Gameplay`），以及类目录发布的真实完整
`assemblyQualifiedName`；逗号周围空白可省。完整身份的程序集/版本不符或附加未知字段被拒绝。
类目录同时携带 `assemblyName`；解析只查询当前 Gameplay 代的已注册类型，
不会触发任意程序集加载。默认值、实例权限、作者和运行时解析共享同一身份规则，
作者 JSON 保留原始 class 文本。
Details 可以改名、复制、删除 C# Blueprint；改名保留 ID，
复制生成新 ID 并保留稀疏引用/覆写。Details 按父链显示组件树、局部 TRS 和 Parent。
inline 组件可新增上述六类、修改局部 TRS/父级/mesh 路径/相机投影/灯光参数、
删除整棵组件子树。添加第二个组件时为旧单组件补上 `root` ID。
GT 验证整个候选后暂存 Scene 并正式重载。继承组件只开放 BP 明确授权的字段，
物理配置仍由资产拥有；这些空间组件没有独立 Tick，C# 逻辑组件见下节。

Physics Details 编辑根 motion/shape/trigger、尺寸、质量、摩擦、恢复系数及角色参数；
切换 Character 自动采用非 Trigger 胶囊。物理表单另提供碰撞层/掩码、
Compound 与静态 Mesh、组件 primitive 碰撞，以及 Fixed/Hinge 约束的目标 ID、
世界 anchor、单位轴和角度限制。Compound 切换同时建立首个组件碰撞；
切换回 primitive 或 None 会清理组件碰撞。选中物理 Blueprint 时显示 box/sphere/capsule
碰撞轮廓，应用根旋转和统一缩放；这是无深度遮挡的 UI 辅助线。

UI 回调只投递值和 World generation。GT 验证脚本候选后调用
`SceneDocumentService::StageScriptBlueprint`，复用同一个 Scene 草稿、
BLAKE3 冲突检测和显式 Save Content 确认；接着排队既有 LoadWorld。
新增和脚本属性编辑需要正式 World 事务成功后才反映在活动场景。
TRS 沿用 SetActorProperty/场景补丁入口。Preview World 和离线模式拒绝脚本操作。
标准资产 JSON facade 的 Scene 能力表没有对象创建或实例属性编辑操作；
这些窄操作由编辑器 GT 适配器提供。Edit 菜单的 Scene Undo/Redo 对这些作者操作及
现有场景属性补丁保留最多 32 步、16 MiB 的作者 JSON 历史；新编辑清空 redo。
历史只在当前草稿与记录值相符时可用，标准 API 或外部重读导致不符时拒绝回放。
Undo/Redo 也走正式 World 重载；即使候选资源准备失败，仍能撤销未提交草稿。
保存后仍可撤销为脏草稿，磁盘仅经显式 Save Content 改变。
离线、资产预览页、Play/Pause 禁用 Scene 作者编辑及 Scene 历史操作；没有跨资产通用 Undo。

## 内联自定义数据

Expose 支持公开的普通数据类/结构体、一维零基数组、`List<T>` 和
`Dictionary<string,T>`，元素与成员可递归组合已有标量/向量/枚举/稳定引用类型。
数据类要求公开无参构造；成员包括 public 可读写实例属性和 public 非 readonly 实例字段，
按名称稳定排列。`[NonSerialized]` 字段及非 Never 的 `[JsonIgnore]` 成员不进入数据描述。
不支持 object/接口/抽象类型、委托、任意引擎对象、未列出的 System 类型、
递归类型、多态、循环引用或 null 作者值。普通未标记 C# 状态不受该配置类型集合限制。

常驻 Runtime 用弱类型键缓存元数据；描述不把旧 Gameplay Type 固定在加载上下文外。
类目录沿用根属性 name/type/value/options/instanceEditable，递归扩展：

| type | 描述 | JSON 值 |
| --- | --- | --- |
| object | typeName、fields（带名称的成员描述） | 字段对象 |
| array | typeName、element（数组/List 的元素描述） | 数组 |
| dictionary | typeName、element（值描述，键为字符串） | 键值对象 |

一个属性最多 16 层、4096 个类型化值节点，每个容器最多 1024 项；
向量/Quaternion 按一个类型化叶节点计。整体仍受宿主 4 MiB 与类目录 3 MiB 预算约束。
未知对象字段、非有限值、类型错误和超限值在绑定/作者边界拒绝，
托管错误带属性路径。构造默认的 null 数据对象/容器在配置描述与绑定时物化为
声明类型的默认对象/空容器，字符串为空；这不允许作者 JSON 显式写 null。

合成采用 C# 构造默认 → BP 配置 → Scene 获准覆写。
object 逐成员递归合成，array/dictionary 整体替换；新集合元素内部的缺字段继承元素类型默认。
绑定与迁移按值深拷贝，不保留配置中的共享对象引用；实例运行后自行赋值的引用归用户逻辑拥有。
Scene 仅保存实际覆写的对象子字段，重置子字段后恢复继承；集合整体覆写及 Reset。
父级根属性必须在 Instance Editable 权限内，嵌套字段不能绕过根权限。

蓝图根/逻辑组件及 Scene 对应 Details 共用递归控件：
对象展开成员，集合提供分页、增删和字典键输入，向量与 Quaternion 沿用各自原子提交规则。
Scene 子路径采用独立字符串/下标 token，由 GT 对最新权威草稿合成，不用点号拆字典键。
标准 `blueprint.modify` 仍提交完整 properties 对象，不新增通用 JSON Pointer 接口。
普通游戏状态留在托管侧；自定义配置不是自动存档标记，也不自动注册逐帧同步。
运行中的复合逻辑组件属性在原生状态中保留作者配置视图，不每阶段遍历实时对象图；
显式 `[Migrate]` 才读取当前复合值。根 Blueprint 的实时自定义属性同样不逐帧导出。

资源根 `Common/Blueprints/WeaponConfiguration.blueprint.cs` 提供
`Gameplay.WeaponConfigurationComponent`：Stats 为 WeaponStats 结构体，
BurstDelays 为 List、StartingAmmo 为字典；CurrentAmmo 是独立托管实例状态。
这些是内联配置，没有独立数据资产类型或存档 API。

## Blueprint 资产与实例

`Blueprints/BP_*.json` 是独立、单层的可复用配置，不继承其它 Blueprint 资产：

```json
{
  "type": "blueprint",
  "name": "Rotator",
  "class": "Gameplay.RotatorBlueprint",
  "properties": {"DegreesPerSecond": 45},
  "instanceEditable": ["DegreesPerSecond"],
  "rotation": [0, 0, 0],
  "scale": [1, 1, 1],
  "components": [
    {"type": "mesh", "modelPath": "Common/Meshes/SM_material_preview_ball.json"}
  ]
}
```

Scene 实例将 inline 的 `class`、`components` 换成
`"blueprintPath":"Maps/SC_example/Blueprints/BP_rotator.json"`；其它身份、TRS 和
`properties` 字段保持不变。`blueprintPath` 与 inline class/components/physics 互斥。
属性合成顺序为 C# 构造默认值 → 资产 properties → 获准的实例 properties。
`instanceEditable` 最多 128 个不重复的属性名，名称必须来自类的 Expose 属性；
不要求资产 properties 已写入该键，因此可以暴露仍继承 C# 默认值的属性。
资产白名单由 BP 编辑器拥有，不由 Scene 实例声明。取消暴露后，仍带该键覆写的 Scene
在下次正式加载时被拒绝；编辑器不会静默删改已有 Scene 文件。
组件使用独立白名单 `componentInstanceEditable:{"visual":["position","materialOverride"]}`，
Scene 只保存 `componentOverrides:{"visual":{"position":[0,20,0]}}`。
字段权限由稳定组件 ID 定位，TRS 以完整向量授权；ID/type/parent、结构增删和碰撞不开放覆写。
模型、相机、灯光的可授权字段来自共用表；UI、作者请求和两条正式解析路径都核对权限。
未授权项只读，黄色框/Reset 表示显式覆写，Reset 删除稀疏键。删除 BP 组件子树同时清理其白名单。
资产 rotation/scale 可省略，只在放置时复制为新实例的初始值；
已有实例的根 TRS 由 Scene 独立拥有，不随资产改动。

WorldLoader 在隔离准备中解析一次引用，网格计划和 CLR 消费同一份有效声明。
作者 Scene 与运行时合成数据分开保存；Scene 保存始终保留引用和稀疏覆写，
不会写入资产或补齐其它默认属性。inline Blueprint 同样要求类显式开放实例属性。
资产输入限制为 4 MiB、JSON 深度 64，拒绝重复键、递归资产、越界路径及链接逃逸。
正式内容不能引用 Generated，Common 不能引用 Maps；组件路径相对资源根。

Content Browser 识别 Blueprint 类型、绿色图标和筛选；双击打开独立中央 Blueprint 编辑标签。
编辑页左侧常驻 Components 父链树及新增入口，中间为真实 3D Viewport，
右侧为独立滚动的 Details。
选择 Blueprint (Self) 显示 Class Defaults/实例白名单/根物理，选择组件只显示该组件配置。
选择按稳定组件 ID 保留，重排不改变目标，删除所选组件后返回 Blueprint 默认配置；
选择仅属于页面会话，不修改 BP 或 Scene，也不产生 World 请求。
该页配置类、名称、说明、默认 rotation/scale、Expose 默认值及逐项 Instance Editable、
组件父链/局部 TRS、mesh/materialOverride、camera 投影/激活、灯光参数和根物理参数。
组件支持新增上述六类和删除整棵子树；不含图节点或资产继承。
类切换清空旧类的默认属性与实例白名单，组件/物理仍经完整候选验证。
修改走中央 `blueprint.*` 命令与独立 BP baseline/draft/revision，失败保留原草稿。
同帧多个字段合成一个修改请求，聚焦的未完成输入不会被快照刷新覆盖。
脏标记、Save Content、Ctrl+S、关闭保存/丢弃/取消、外部摘要冲突沿用资产文档流程。
Edit 菜单在蓝图标签中只操作该 BP 的历史，使用 `blueprint.undo` / `blueprint.redo` 和当前
`expectedDocumentRevision`。每个 BP 的 undo/redo 两栈合计最多 32 步、16 MiB 序列化作者 JSON；
一次 modify 为一笔，无变化或失败不消耗历史，新修改清空 redo。Save 保留历史，
撤销已保存操作会产生脏草稿；Revert/Reload 清空历史。历史候选重新验证当前类目录与资源引用，
失败保留文档和两栈；没有文件操作撤销。`canUndo` / `canRedo` 来自 GT 文档快照。
修改或历史命令等待 GT 终态反馈后恢复交互，拒绝也会确认，不以 revision 推进作为成功的唯一判据。
关闭时保存/丢弃都等待权威草稿确认干净；失败或更新的脏版本保留标签与弹窗。
修改只暂存，显式保存后由内容观察器通过正式 World 加载应用到关卡。
Blueprint 预览独立使用 `SC_model_preview`，与模型/MI 一样占用单个活动 World，
返回关卡重放已有 Scene 草稿。UI 仅提交 BP 路径和文档 revision，GT 从文档 owner 的
已接受草稿提取组件与默认根 rotation/scale，revision 不符拒绝；不读取 UI 候选或写临时资产。
组件/根变换输入稳定 350 ms 后刷新，标量属性和实例白名单修改不重复加载几何。
预览 inline 声明的 properties 为空，不将资产默认属性误作实例覆写；
使用完整父链矩阵和正式 mesh/material 资源加载事务，准备失败保留活动 World 与 BP 草稿。
预览 World 不创建托管上下文，不执行 OnCreate/播放回调或物理模拟，并拒绝 Play。
图像只在新 World 发布且 Blueprint 身份匹配路径和请求 revision 后显示，避免旧画面串入标签。
相机合并所有组件网格的变换后包围盒；无网格时使用原点附近的厘米级观察范围。
左键按正式导入 section 的局部包围盒及完整模型矩阵拾取最近 mesh 组件，同步树与 Details；
这是包围盒近似，不是三角形或深度拾取。相机/灯光/Scene 组件有辅助图示和拾取；
box/sphere/capsule 与 compound 子形状显示碰撞轮廓。
右键拖动环绕，中键平移，滚轮缩放，双击或 Frame 按钮重新取景；
Refresh 按钮可重试资源准备。该页提供三轴 gizmo：平移沿父坐标轴，旋转编辑局部 YXZ Euler 分量，
缩放沿组件局部轴并保留缩放符号。完整父矩阵保留 shear，不反解世界 TRS；近视线退化轴/侧向旋转环隐藏。
拖动向 GT 发送冻结值请求，直接更新当前预览的模型/灯光，真实网格连续跟随；
不每帧重载 World 或写草稿，释放提交一笔修改。Self 有根旋转/缩放手柄，
物理根统一缩放，Character 仅 Y 轴旋转；支持移动、角度、缩放吸附。
Esc、失焦、切页、模态、播放、
文档/World 换代或视口矩形变化取消手势，恢复预览基线且不写草稿。
旧预览与当前组件/根变换不符时禁止交互。
gizmo 是无深度 UI 叠层，不提供世界坐标模式或关卡通用 gizmo。
运行中编辑要求已启用 C# 宿主；Play/Pause 禁用该页作者控件。
拖动 Blueprint tile/list row 到关卡视口，通过 RmlUi 原生 clone/dragdrop 发出
值请求（路径、World generation、视口归一化坐标）。GT 用当前观察相机反投影，
与 `y=0` 地面相交放置，生成新 ID 并复用 Scene 草稿与正式 LoadWorld。
不吸附模型表面，不提供拖动中的 3D 预览；水平/朝上无交点的射线明确拒绝。
视口外释放、Esc、失焦、旧 World、模态、播放/暂停、离线及预览页不创建实例。

Details 中黄色边框表示显式实例属性覆写，回转箭头删除该属性键并重新继承默认值。
属性编辑从原始 Scene 草稿开始，不从补齐默认值后的 World 复制。
New Asset 路径框与保存图标从当前作者实例创建新资产，
只允许 `Common/Blueprints/BP_*.json` 或 `Maps/SC_*/Blueprints/BP_*.json`。
此按钮明确创建文件，完整暂存校验后以硬链接 create-new 语义发布，已有文件始终保留；
不改变源实例引用。创建成功刷新目录，失败显示反馈。
BP 文件在外部修改或编辑器保存后由下述内容观察器触发正式重载。
修改 C# 类需要编译，可使用编辑器 Compile C#。没有嵌套 Blueprint 资产、资产继承或 Blueprint 节点图。

本地资源仓库示例：
`Maps/SC_actor_authoring/SC_actor_authoring.json` 的两个实例引用同一个
`Blueprints/BP_rotator_demo.json`，分别继承 45 与覆写 90 度/秒。
`Maps/SC_actor_gameplay/SC_actor_gameplay.json` 展示组件树、静态地面、定时 Spawn、
刚体落地与碰撞日志；它是学习场景，不是论文验证。
同内容域的 `Maps/SC_actor_gameplay/SC_actor_playground.json` 提供胶囊角色、
跟随相机、WASD/Space 移动跳跃、Trigger 内 F 交互生成及左键前方生成。
LookX/LookY 只在 Esc 开启指针捕获后有效。此场景同样不是论文验证。

## Camera 组件

camera 可设置 `fov`（水平角，默认 60，0<值<179）、`near_clip`（默认 1 cm）、
`far_clip`（默认 10000 cm），要求有限且 0<near<far。
`autoActivate` 默认 false；Play/Spawn 在没有活动镜头时选取显式启用且 `priority` 最高的镜头，
同优先级按声明顺序取首个。`priority` 默认为 0，范围为 -100000..100000 的整数；
它只参与初始选择，运行时改优先级不会抢占已激活镜头。
C# 使用 `GetComponent<CameraComponent>(id)`，通过 `HorizontalFov`、`NearClip`、
`FarClip` 修改投影，并在所属 GT 回调内 Activate/Deactivate 或查询 IsActive。
位置合成完整父挂接矩阵，镜头基向量只合成父挂接旋转，缩放/shear 不扭曲正交镜头。
World 独立拥有脚本相机，物理同步后更新位置，再由 WorldSnapshot 冻结；
宽高比随视口调整，曝光沿用作者相机。作者相机不会被脚本镜头覆盖。
活动 owner 被禁用、故障或销毁时回到作者镜头；Stop 清除脚本镜头并恢复编辑观察视角。
GASP 正式角色与翻越探针使用下述 SpringArm 缩短遮挡时的镜头距离；
没有通用镜头混合或自动选择后备组件。

## SpringArm 组件

所有空间组件的局部 TRS 都相对父挂接空间；无 parent 时相对 Blueprint 根。
`SceneComponent.WorldMatrix` 表示组件空间到世界空间，`WorldRotation` 为独立合成的正交四元数；
`GetSocketTransform()` 返回供子组件使用的世界挂接矩阵。普通组件的挂接矩阵就是自身世界矩阵，
`springArm` 的组件原点在臂根，挂接矩阵在求值后的臂端。
Scene、mesh、camera、灯光及嵌套 SpringArm 均使用相同挂接路径，
不按组件类型特判相机。子组件始终保留作者局部位置/旋转/缩放，
中间 Scene 的局部偏移继续合成；不再把所有相机局部位置反写成同一世界落点。
相机激活仍使用 Camera 的既有入口。
VL 的后向为局部 +Z（对应 UE 的 -X），世界为 Y-up、厘米。

原生使用列向量：`子组件世界矩阵 = 父挂接世界矩阵 × 子局部 TRS`；
C# `System.Numerics` 使用行向量，乘法顺序相反，空间含义相同。
Mesh 组件空间位于导入 section/model 空间外层；骨骼 pose/skin 矩阵仍属于动画模块，
不把 Blueprint 父组件、mesh component space 与 bone local space 混成一个坐标系。
重挂父级沿用现有 KeepRelative 语义，只换 parent，保留局部 TRS；没有 KeepWorld/Snap 选项。

| JSON 字段 | 默认值与语义 |
| --- | --- |
| `targetArmLength` | 300 cm，0..100000；0 时跳过碰撞查询 |
| `targetOffset` | 零向量，臂根的世界空间偏移 |
| `socketOffset` | 零向量，按目标旋转变换的臂端偏移 |
| `probeRadius` / `doCollisionTest` / `collisionMask` | 12 cm（0..100000）/ true / 65535（16 位整数）；半径 0 使用射线 |
| `usePawnControlRotation` | false；启用时读取 `Blueprint.ControlRotation`（X/Y/Z 为 Pitch/Yaw/Roll 度），初值为 owner 初始朝向 |
| `inheritPitch` / `inheritYaw` / `inheritRoll` | 均 true；关闭的轴保留 arm 局部作者角，适用于普通父链与 ControlRotation |
| `absoluteRotation` | false；启用时忽略父旋转及逐轴继承，使用 arm 局部角；显式 ControlRotation 仍可覆盖 |
| `rotationOffset` | 零向量，VL 扩展；叠加在目标臂旋转上，同时影响臂端和镜头朝向 |
| `enableCameraLag` / `cameraLagSpeed` | false / 10（0..1000）；平滑臂根，单步系数 `clamp(speed*dt,0,1)`，速度 0 直接到目标 |
| `cameraLagMaxDistance` | 0（0..100000 cm）；0 不限臂根滞后距离 |
| `enableCameraRotationLag` / `cameraRotationLagSpeed` | false / 10（0..1000）；最短弧四元数插值，同时驱动臂端和朝向，速度 0 直接到目标 |
| `useCameraLagSubstepping` / `cameraLagMaxTimeStep` | true / 1/60 s（0.005..0.5）；在相邻原始目标之间插值，分别推进位置与旋转阻尼 |
| `drawDebug` | false；通过公共 DrawDebug 画单帧臂线与命中球，多臂按独立 key 发布 |

参数经原生组件 schema 和 C# 属性 setter 校验；加载、运行时 `ComponentDescription`、
状态导出、BP Details 与获准的 Scene 实例覆写均使用同名字段。长度与 SocketOffset 不乘父缩放，
臂根位置仍经过完整父挂接矩阵；旋转只组合父链四元数，避免非均匀缩放/shear 扭曲镜头。
臂端的子组件局部偏移仍继承完整缩放/shear，不把“不缩放臂长”误解成“不缩放子空间”。

每帧先完成 Tick → `StepPhysics(dt)` → 查询/接触回调 → LateTick →
`StepPhysics(0)` → 动画，再执行仅引擎内部的 `camera_prepare` / 球扫 / `camera_finish`：
零时步先同步 LateTick 改动的根/碰撞体，使动画 Foot IK 查询本帧最终 Jolt 支撑状态；
先在本帧最终 owner 位姿上计算 lag，再从未滞后的臂根（含 TargetOffset）扫向完整臂端
（含 SocketOffset），最后发布裁剪后的相机。相机、刚体及障碍物使用同一物理状态，不额外推进时钟；
用户 LateTick 查询仍在下一帧领取。静止臂每帧查询，移动障碍物不会复用旧命中。
Jolt 球扫返回接触时的球心，直接作为相机位置，不重复减半径；碰撞修正在 lag 之后执行。
忽略 owner 自身、排除 Trigger，按 Jolt mask 过滤；无命中/关闭碰撞/超预算使用未裁剪臂端。
嵌套臂按父依赖求值，父臂碰撞完成后才派发子臂查询；同一帧可有多轮内部查询，
均不推进物理或游戏时钟，组件数组的声明顺序不决定结果。
CLR 导出臂节点内部 `__springArmSocket`（世界位置/旋转）供原生矩阵路径消费，
不写子局部 TRS；作者 schema 拒绝此内部字段，不能保存进 BP 或 Scene。

C# 的 `GetTargetRotation()`、`GetUnfixedCameraPosition()`、`IsCollisionFixApplied()` 提供只读诊断；
目标旋转即时计算，未裁剪位置与碰撞标志来自最近一次求值，因此 LateTick 中读取后两者是上一帧结果。
`ResetCameraLag()` 使下一次求值直接使用目标，适用于显式传送。臂只更新自己的挂接空间，
直接修改子相机局部 TRS 会作为新的臂端相对变换保留。
碰撞查询覆盖臂根到 SocketOffset 后的臂端；子组件额外局部位置不扩展这条查询，
需要参与扫球的偏移应写在 SocketOffset。
参数与 ControlRotation 参与回调异常回滚，
播放中程序集重载保留参数、lag 历史、挂接落点及子组件局部变换，Stop 后重建。

这是 UE 风格行为子集：碰撞使用本项目 Jolt mask，没有 UE TraceChannel/Profile、虚函数 BlendLocations、
Pawn/PlayerCameraManager 栈、镜头混合或物理最大 dt 联动。
没有命名 socket/骨骼挂点表，当前每个组件只提供一个默认挂接空间。
VL 保留完整矩阵中的非均匀缩放/shear，不宣称逐位复刻 UE FTransform 的组合数值。
Lag 子步沿原始目标插值，属于 VL 明确采用的帧率一致性口径，不宣称复刻特定 UE 版本的历史数值缺陷。
UE 接口依据与空间边界见 [组件挂接记录](../rationale/springarm-component-space-record.md)。

资源侧 `BP_gasp_character.json` 与 `BP_gasp_traversal_probe.json` 的父链为
`springArm → camera`：挂点位于胶囊中心、局部位置 `(0,0,0)`，初始俯仰 `-12°`、
臂长 `450 cm`、探测半径 `12 cm`，启用碰撞与 `usePawnControlRotation`，位置/旋转 lag 默认关闭。
`Gasp.CharacterBlueprint` 从臂的作者俯仰和 Actor 初始 yaw 初始化 `ControlRotation`，
鼠标输入只更新该世界朝向；移动和翻越根旋转不带偏玩家镜头。
子相机作者局部位置/旋转为零，控制器不再另行写相机或提交镜头扫球。
Blueprint 编辑预览使用同一原生组件空间求值，显示静态臂线、末端相机及子组件局部偏移，
图示/拾取/gizmo/真实 mesh 使用一致的父挂接矩阵。
预览没有托管上下文，不执行 OnCreate、ControlRotation、碰撞或 lag，
以作者父链、旋转继承开关和 rotationOffset 显示静态设计姿态。

## 灯光组件

pointLight/spotLight 要求 `intensityUnit:"cd"`，directionalLight 要求 `"lux"`，
不支持 lm 或单位切换。可选 `intensity` 默认 1000，有限且在 0..1e12；
`color` 为线性 RGB、各分量在 0..1，默认白色；`visible` 默认 true。
spotLight 使用角度半角 `coneAngleInner`/`coneAngleOuter`，默认 30/45，
要求 0≤inner<outer<90。位置厘米，朝向为旋转后的局部 -Z。
BP 和 inline Details 均可配置；继承 BP 的 Scene 实例仅能覆写授权字段。

C# 用 `GetComponent<PointLightComponent/SpotLightComponent/DirectionalLightComponent>(id)`，
设置 Color、Intensity、Visible；聚光另有 InnerConeAngle/OuterConeAngle（角度）。
位置合成完整父矩阵，方向只合成旋转；父缩放不改强度或锥角。
World 拥有派生灯值，WorldSnapshot 复制并给稳定 ID；RT 不读取组件或 CLR。
点光/聚光仍只在现有 GPU 上传边界乘 10000，没有额外 shader 换算。
这些组件不进入作者灯表，不替换 CSM 主方向光，不提供局部光阴影。
隐藏、禁用、故障或销毁 owner 的灯不发光；Spawn 模板不发光，Stop 移除生成灯并恢复作者灯值。
无效回调先恢复该次灯光值，再隔离故障 owner。预览发布作者灯值，不执行脚本。
没有面积光组件或衰减半径控件。

## Spawn 与刚体

Scene 可选 `spawnAssets` 数组声明最多 128 条 BP 路径；场景实例引用的资产也自动可用，
合计最多 128 个模板。模板随候选 World 通过正式 mesh/resource 计划预加载并隐藏。
`World.Spawn(path, position, rotation?, scale?)` 返回本上下文 BlueprintHandle；
省略 rotation/scale 使用资产初始值。回调里不导入新资源、不调用 Vulkan。
RT 消费新快照后、录制 draw 之前，由既有逐对象 registry 为新身份补齐模型 UBO 与
descriptor 包；既有包保持不变，不重建环境或 RenderGraph。包由 World-local cache
持有直到 World/GPU epoch 退休，因此大量 Spawn 仍有首次逐对象分配成本。
`World.Destroy(handle)` 与 `World.IsAlive(handle)` 校验上下文；
服务只在所属 Blueprint 的 GT 播放回调中可用，OnCreate 不可结构性修改 World。
结构请求在该批回调结束后处理，Spawn 构造/OnCreate/BeginPlay，失败隔离并报告诊断。
一次队列最多 256 条、一个 Play 上下文累计最多 4096 实例（含销毁槽位）。
Spawn 与 Destroy 不写作者 Scene，Stop 移除生成网格，Play 重建上下文。
Destroyed 实例保留墓碑到 Stop；新增资源使用下述异步加载入口。

inline Blueprint 或 BP 可选根 `physics`：

```json
"physics": {
  "motion": "dynamic",
  "shape": "box",
  "halfExtents": [50, 50, 50],
  "mass": 1,
  "friction": 0.5,
  "restitution": 0.2
}
```

motion 为 static（默认）/dynamic/kinematic/character，
shape 为 box（默认）/sphere/capsule/compound/mesh；box 要求正 halfExtents，
sphere 要求正 radius；capsule 要求正 radius 和 halfHeight，
后者是竖直直段的一半，总高度为 `2 * (halfHeight + radius)`。
可选布尔 `trigger` 缺省 false，character 拒绝 true。
质量 kg 且为正，friction 非负，
restitution 在 0..1。Blueprint 根缩放必须正且统一，播放中固定；
视觉组件可独立缩放，但不会改变根碰撞形状。
世界输入为 cm/kg/s，Jolt 内部转换为 m/kg/s；重力为 -981 cm/s²。
`ScriptPhysicsWorld::Step(states, declarations, dt)` 是唯一物理入口，每帧在 GT 同步执行：
角色走 `MoveCharacter(dt)`，按帧 dt 推进，内部再按 UE CMC 的
`MaxSimulationTimeStep=0.05` / `MaxSimulationIterations=8` 分段；
刚体在 `1/60` 秒子步中推进，单帧最多 16 步，未达上限时保留不足一步的余量，
超过上限时丢弃整步多余量；`dt=0` 不推进任何实体，只同步指令与接触/查询。
Pause 冻结，Stop/World 换代释放物理世界。
Jolt 注册表/Factory 属于进程，多个物理世界共享注册但分别拥有 body、allocator 和刚体子步余量。
任务使用 `JobSystemSingleThreaded` 同步执行在 GT，不启动物理 worker。
动态体与 kinematic 回写 position/rotation；脚本改根位姿表示 teleport。
Character 根位置变更同样表示 teleport；其合法纯 Y 轴转向只更新朝向，
保持最近完成物理帧的实际速度与既有支撑查询，后续物理帧再产生新反馈。
`Body.LinearVelocity` 为 cm/s，`Body.AddImpulse(Vector3)` 为 kg·cm/s，指令仅消费一次。
每帧全部 Tick 后先同步一次性指令，再执行该帧 `Step(dt)`：角色按帧 dt 移动、
刚体按内部子步推进。全部子步后发 OnCollisionEnter/OnCollisionExit，再执行 LateTick；
LateTick 的指令由帧末 `Step(0)` 只领取、不推进时间，留给下一物理帧求解。
接触按稳定 Blueprint ID 去重，不按 mesh section 重复发事件。
接触快照通过 Jolt broadphase/窄相位形状查询获得，休眠不触发假 Exit。
实体接触使用 0.2 cm 分离容差；Trigger 只接受实际相交，不产生碰撞响应，
通过 OnTriggerEnter/OnTriggerExit 单独发事件。查询包括静态 sensor 与其它根体的相交。
快照按帧末状态交付，单帧穿过 Trigger 后已离开的瞬时相交不产生事件。
根 `collisionLayer` 为 0..15，`collisionMask` 为 0..65535，缺省 0/65535；
响应、接触、Trigger 与角色支撑都检查双方 mask。
compound 可写 `shapes:[{"component":"pivot","shape":"box","halfExtents":[50,50,50]}]`，
或自动收集组件 `collision`（只含 primitive shape/尺寸）。局部完整父链必须有限、
正统一缩放且无 shear；运行时组件变换/挂接变化重建形状。
`shape:"mesh"` 仅 static，`component` 指 mesh 组件；三角形来自正式模型导入器冻结的
CPU 索引几何，保留 section/组件变换，不另建模型导入路径。
`constraints` 最多 32 项，格式为 id/type/target/anchor；
type 为 fixed 或 hinge，hinge 另有单位 axis 与 minAngle/maxAngle（度，范围跨零、限于 ±180）。
anchor/axis 在世界空间；目标是同关卡有非 Character 刚体的稳定 Blueprint ID，
禁止 static/static。BP 预览允许保存外部目标，正式关卡准备解析引用；
Spawn 模板不接受外部约束。销毁目标移除约束并唤醒相关刚体。
仍没有独立组件级刚体。

`Physics.Raycast(from, to, callback, includeTriggers: false)` 是异步有界线段查询。
默认忽略所属 Blueprint 与 Trigger，返回最近命中的 BlueprintHandle、cm 点/距离和世界单位法线，
未命中传 null。凸体内部起点的距离为零，法线约定为逆射线方向。
Tick/BeginPlay 请求在物理之后、LateTick 之前交付；LateTick 请求在下一帧交付。
回调仍走所属 Blueprint 的故障隔离与变换回滚边界；失败、Destroy 或 Stop 清理未交付请求。
一个上下文最多 256 条未完成查询。
`Physics.SphereCast(from,to,radius,callback,includeTriggers:false,collisionMask:65535)`
沿线段扫球，半径/点/距离为 cm。Raycast 也支持 query mask；mask 过滤命中层。
球初始重叠报告距离零；角色相机示例用它探测遮挡并保留最小间距。
`SphereCastLocal` 的端点使用所属蓝图完成本次物理后的根矩阵转换，半径和命中距离仍为
世界厘米；移动中的角色镜头因此使用当前物理位姿，统一缩放只影响端点和局部镜头臂。

character 必须配 capsule、正统一固定缩放及仅 Y 轴旋转；根位置是胶囊中心。
`Character.Move(Vector3)` 设置持续的水平 cm/s 速度，Y 分量必须为零；
`Character.Jump()` 请求下一个有效物理步跳跃，仅已落地时生效。
`Character.IsGrounded` 在物理后同步，LateTick 看到该帧结果。
控制器使用 Jolt `CharacterVirtual::ExtendedUpdate`，支持滑墙、台阶和贴地。
可选 `stepHeight`（非负 cm，默认 30）、`jumpSpeed`（非负 cm/s，默认 500）、
`maxSlopeAngle`（0≤角度<90，默认 45）。mass 默认 70 kg。
角色有原生 capsule 查询代理，参与射线与 Trigger；重力只在角色的每帧物理推进中积分。
出生或传送在完整 body 集合同步后刷新支撑查询；移走/删除根体会唤醒旧位置附近的休眠体。

## 输入、计时器与 Blueprint 通信

`config/config.json` 可选 `gameplayInput` 配置由启动时 SDL 一次解析键名：

```json
"gameplayInput": {
  "actions": {"Jump": ["Space"], "Interact": ["F"], "Fire": ["MouseLeft"]},
  "axes": {
    "MoveForward": [{"key": "W", "scale": 1}, {"key": "S", "scale": -1}],
    "MoveRight": [{"key": "D", "scale": 1}, {"key": "A", "scale": -1}]
  }
}
```

省略 actions/axes 时分别使用默认集：上述按钮、W/S/D/A/E/Q 三个 Move 轴及 MouseX/MouseY
两个 Look 轴。鼠标按钮另支持 MouseRight/MouseMiddle。
每类最多 64 个名称、名称最长 128 字节、每名称 1..16 个绑定；轴 scale 有限且绝对值≤10000。
`Input.IsPressed` 为当前保持，WasPressed/WasReleased 为物理边沿，GetAxis 返回轴值；
Move Vector2 与旧 MoveForward/Right/Up 字段保留兼容。
首次采样只建立物理基线；UI/失焦屏蔽继续更新基线，恢复权限本身不生成边沿。
键鼠混合 action 的未屏蔽来源仍可产生边沿，相对鼠标轴只在已捕获指针时有值。
配置缺少的自定义轴/按钮在 C# 查询时明确报错。

`Timers.Set(seconds, callback, repeat: false)` 返回 TimerHandle，Cancel 仅接受本 owner/context。
延迟必须有限且非负；repeat 必须正延迟。每上下文最多 1024 个 timer。
播放时钟在 Tick 前推进，到期任务在 Blueprint Tick 前按到期时间和 ID 运行；
repeat 每帧至多一次，跳过积压；回调新建的零延迟任务留到下一帧。
Pause 不推进时钟；失败 owner、Destroy、EndPlay 和 Stop 清理注册。
Time Scale 为零时也不领取到期任务；再次恢复正倍率后按播放时钟继续。

`World.Find(id)` 返回可空句柄，FindAll(className?) 返回只读句柄列表，
GetBlueprint(handle) 返回 BlueprintInfo 值快照，不暴露其它 live Blueprint 引用。
`Events.Subscribe/Unsubscribe` 以字符串名称监听，Publish 广播，Send 向指定句柄投递；
GameEvent 包含发送者句柄、名称及字符串 payload。名称 1..128 字符，payload 最多 4096 字符；
每上下文最多 1024 个订阅、256 条待发消息。
LateTick 后按 FIFO 投递当批消息，接收者按订阅 ID 排序；
接收回调产生的新消息留到下一帧。回调失败撤销该次服务变更/消息并清理失败 owner。
服务只在所属 Blueprint 的 live GT 播放回调可用，OnCreate 不可调用；
Stop/World 换代释放委托，旧上下文句柄被拒绝。

## 运行时组件与动态资产

`AddComponent(ComponentDescription)` 返回带 Context/Blueprint/ID/Generation 的 ComponentHandle；
`RemoveComponent(handle)` 删除子树，`AttachComponent(handle,parent?)` 保持局部 TRS 重挂父级，
`ResolveComponent(handle)` 返回当前所属组件或 null。同一 ID 删除重建后旧 handle 失效。
支持 scene/mesh/camera/springArm/三类灯光；每个 Blueprint 最多 128 个组件，每批最多 256 个结构请求。
请求先在值节点上验证父链、投影、灯光及碰撞，再在回调批次结束后应用；
回调失败丢弃该回调的结构请求。移除活动相机回到作者镜头，Stop 恢复作者组件树。
Mesh/Material 组合需由当前 World 或已加载 BP 模板正式准备，回调本身不导入或上传。

### 自定义 C# 逻辑组件

`Component` 是用户可继承的逻辑基类，与 `SceneComponent` 等内置空间组件独立。
BP 或 inline 声明可带：

```json
"scriptComponents": [
  {
    "id": "health",
    "class": "Gameplay.HealthComponent",
    "enabled": true,
    "properties": {"Maximum": 100},
    "instanceEditable": ["Maximum"]
  }
]
```

每个 Blueprint 最多 128 个逻辑组件，按声明顺序执行；ID 在逻辑组件集合内唯一，
禁止 `/`、`\`、`|`、NUL。组件类须 public、非 abstract、有无参构造，类目录与
Blueprint 类目录分开。Expose 类型及默认值验证与 Blueprint 相同。
逻辑组件不占空间父链、不持有独立 TRS；`Transform` 指向 Owner 根，
`Owner`、Input/Time/World/Timers/Events/Physics/Assets/Hud 由所属 Blueprint 提供。
不包含独立逻辑组件资产或任意对象图序列化。

生产示例资源 `Common/Blueprints/Rotator.blueprint.cs` 中的
`Gameplay.RotatorComponent` 暴露 `DegreesPerSecond`（Vector3，Instance Editable），
在 Tick 中旋转所属 Blueprint 的根 Transform；独立 Gameplay 编译回归检查其组件类目录。

BP 中每个逻辑组件用自己的 `instanceEditable` 属性名白名单授权；
Scene 使用 `scriptComponentOverrides:{"health":{"Maximum":150}}` 保存稀疏覆写。
合成顺序为 C# 默认值 → BP 组件 properties → 获准实例覆写。
inline 的属性权限仍由 `[Expose(InstanceEditable = true)]` 决定，
不能通过手写白名单越权。BP 实例不开放组件结构、类或 enabled 覆写。
BP 标签左侧 C# Components 集合支持新增/选择，右侧配置类、enabled、Expose、
Reset 和逐项 Instance Editable；Scene Details 支持 inline 增删/启用及获准属性编辑，
继承属性的黄色框/Reset 删除稀疏键。修改、历史、revision 和保存复用既有作者事务。

组件具有 OnCreate、BeginPlay、Tick、LateTick、EndPlay、OnReload。
候选构造先绑定完整组件集合，再执行 OnCreate；此阶段可访问 Owner.Transform 和日志，
不能使用播放服务，失败拒绝候选。每阶段先执行 Blueprint，再按声明顺序执行其活动逻辑组件。
禁用组件或 owner、故障、Destroy/Stop 会对已开始的组件执行一次 EndPlay 并清理其服务；
重新启用的未故障组件再次 BeginPlay。组件故障只隔离该组件，其它组件继续运行；
owner 故障则结束所属组件并清理所有服务。EndPlay 异常不阻止其它组件清理。
timer/event/ray/asset 注册同时归属 Blueprint 与当前组件，组件不能取消兄弟组件的 token。
回调回滚内置 World 值、逻辑组件 enabled、物理/结构指令和本次服务修改，
不回滚任意用户 C# 字段或属性。

`ScriptComponents`、`GetScriptComponent<T>(id)` 查询所属逻辑组件。
`AddScriptComponent(ScriptComponentDescription)`（或泛型版本）返回
`ScriptComponentHandle(Context,BlueprintId,Id,Generation)`；
RemoveScriptComponent/ResolveScriptComponent 验证所属上下文和代际，
删除重建同 ID 后旧 handle 失效。结构请求每批最多 256 条，在回调批次边界领取，
生命周期回调产生的新请求留给下一边界；异常丢弃该回调排队的变更。
动态添加执行 OnCreate，首次进入活动阶段时 BeginPlay；移除先 EndPlay 再清服务。
运行时变更不写作者文件，Stop 恢复作者集合。

```csharp
Assets.LoadBlueprintAsync("Maps/SC_game/Blueprints/BP_enemy.json", result =>
{
    if (!result.Success) { Log.Info(result.Error); return; }
    var enemy = World.Spawn(result.Asset, Transform.Position);
    Assets.Unload(result.Lease!.Value);
    Timers.Set(2, () => World.Destroy(enemy));
});
```

`Assets.Cancel(AssetLoadHandle)` 取消未完成请求，`Unload(BlueprintAssetLease)` 释放成功租约。
每上下文最多 256 个请求/租约，请求 ID 在进程内不复用；旧 Play、外 owner 的 handle 被拒绝。
同路径请求共享资源候选，各自在所属 GT 回调交付一次成功或错误。
owner 故障/销毁、Stop、World 换代清理请求/委托/租约；取消不会调用完成委托。
成功回调中异常撤销该次 Spawn/服务修改并释放 owner 租约。
Spawn 不再局限于场景 `spawnAssets`：动态资产成功后可用 `World.Spawn(AssetId,...)`。
`Gameplay.AsyncSpawnerBlueprint` 是加载、生成、卸载租约和延迟销毁的生产示例。

Engine 的单活动加载会话在 worker 读取 BP、正式导入 mesh、冻结 MI/M_、
解码纹理并编译 shader 候选；worker 不接触 World、CLR 或 Vulkan。
GT 每次推进上传一个贴图或一个 mesh，在最终输入字节摘要复核后提交 shader 制品，
再无抛交换 pipeline/cache/World 模板扩展；不会替换 World、重建脚本上下文或重置播放时间。
新增 mesh 使用既有逐对象 descriptor 发布入口。失败、取消或陈旧结果保留活动资源。
Scene 常驻模板不因租约释放卸载；动态模板在所有租约、活 Spawn 和引用 mesh 的组件均释放后移除。
动态键台账处理资产间共享资源，旧 GPU package 按最后提交 epoch 退休。
取消和会话销毁不等待 CPU 导入，后台结果被丢弃；GT worker registry 回收已完成线程，
Engine Shutdown 在释放配置与 renderer 所有者前统一 join 剩余 CPU 任务。

## 编辑器编译

工具栏与 Edit 菜单提供 Compile C# 和可选 Auto Compile C#，输出区保留编译器文件/行/列诊断。
编译 worker 直接启动配置期 dotnet，在独立 candidate/obj 目录编译生成的 Gameplay 项目，
并引用当前加载的桥接副本；不覆盖正在运行的 DLL 或 CMake 的共享输出。
候选生成源码通过使用本机路径分隔符的 PathMap 映射至固定虚拟目录，
保留项目已有映射与真实作者源码位置；相同源码、配置和桥接输入的独立候选
产生逐字节一致的 Gameplay DLL/PDB。
配置时将 SDK、项目、候选输出和桥接路径固定为绝对路径，子进程切到项目目录不会重解释相对资源根。
源码监视每 0.75 s 采样 BLAKE3，连续两次相同改动才编译，跳过 bin/obj；
编译中继续变化的源码拒绝发布。重复编译请求被禁用，Shutdown 终止自有编译进程树并 join。
桥接源码变化要求完整构建并重启。SDK 或项目不在机器上时编译控件禁用。
成功候选只在同一 World generation 的实时关卡经既有程序集事务重载；
Editing 重建作者配置，Playing/Paused 走下述显式迁移，不等待 Stop。
World 换代丢弃结果。编译或重载失败保留当前程序集；迁移因在途资源/物理请求
被拒绝时，该编译结果已领取，需完成清理后再次编译或重载。

## 独立游戏导出

```powershell
cmake --build build --target main vl_export_game -j8
.\build\bin\vl_export_game.exe --project-root . --initial-scene Maps/SC_actor_gameplay/SC_actor_gameplay.json --output artifacts/game-export
```

输出必须是尚不存在的新目录。工具在独占 staging 收集依赖并写 BLAKE3 文件清单，
全部成功后 rename 发布；失败清理 staging，已有输出目录拒绝覆盖。
Scene/BP/SM/MI/T/环境引用递归收集；模型源目录保留 glTF buffer/贴图等 sidecar。
Common 和 Maps 都按引用闭包复制，另扫描 shader JSON 的默认贴图和 RenderGraph 材质引用；
运行时拼出的额外路径通过
`--dynamic-assets <JSON字符串数组文件>` 显式列入。没有静态分析任意 C# 字符串。
配置改为包内相对 resources/managed 路径，关闭开发 UI 与 UI 热重载；
启用脚本的包开启 `scripts.autoPlay`。
输出包含 shader 源码/缓存、UI、递归 PE imports/delay imports 的 x64 原生 DLL、
桥接/Gameplay 与私有 .NET 8 runtime/hostfxr；不要求目标机器安装 SDK。
`--binary-directory` 指定构建输出，`--dotnet-root` 可指定运行时根。
缺少依赖或所需私有运行时拒绝导出；引用闭包含 BP/实例却关闭 scripts 时同样拒绝，
不会发布不可执行的游戏配置。
`game-export-manifest.json` 位于 main.exe 同目录时，启动从该目录解析配置，因此任意 cwd 可运行。
仍要求 Windows x64、支持当前功能的 Vulkan 驱动；工具不生成安装器或平台签名。

## BP 内容热重载

候选读取时记录实际 BP 文件字节的 BLAKE3-256 摘要，活动 World 保存这份基线。
Editing 的实时关卡每 0.5 s 采样依赖，连续两次相同的新内容才排队既有 LoadWorld。
Play/Paused 延后到 Stop；离线与模型预览不自动重载。
Scene 草稿继续由原 owner 提供，实例稀疏覆写保留，不由观察器写盘。
坏 JSON、缺失文件、类/属性/组件/资源准备失败均保留活动 World；
同一失败内容不反复重试，文件修正后再尝试。成功提交后新 World 回到 Editing。
观察范围是已引用/预加载的 BP JSON，不包含程序集、Scene 文件、源模型、贴图或 MI 的通用观察器。

## 生命周期与边界

工具栏 Reload C# assembly 在实时关卡可用。Editing 时先编译 Gameplay，
点击后从同一路径读取新 DLL/PDB，构造隔离类目录，并用当前 World 已解析的作者声明
重新合成默认值、验证实例权限、全部关卡 Blueprint 和预加载模板的 OnCreate。
保留补齐类默认值之前的声明，因此未覆写项继承新默认值，显式覆写保持。
全部准备成功后在 GT 边界无抛交换脚本配置/上下文与类目录；World generation、
Scene 草稿、作者文件和 GPU 资源不改变。
坏 DLL、缺类、属性/权限变化、OnCreate 失败都丢弃新代，保留旧目录、上下文和配置。
离线和资产预览拒绝重载。Playing/Paused 使用下述独立候选迁移。
重载释放旧 timer/event/raycast/委托上下文，并请求旧加载上下文卸载；
正常帧不强制 GC。桥接的 `assembly_status` 诊断可显式收集并观测旧代弱引用。
脚本自行保留线程、静态跨代引用或未托管资源会阻止回收，这些副作用不具备回滚语义。

### 播放中显式迁移

`[Migrate]` 标记 public、可读写的非静态属性，支持与 Expose 相同的值类型；
可独立于 `[Expose]` 使用。迁移按属性名和完整类型名精确匹配，
被标记旧属性在新类中删除或改类型会拒绝候选；新增属性使用新代构造/作者默认值。
只复制值，不复制委托、任意对象图、静态字段或旧代实例引用。
内联自定义数据使用相同的按值编解码；递归核对旧成员 CLR 类型，
删除/改类型拒绝候选，新增成员保留新代默认。实际值中的 null、非法数值或超限
只使迁移候选失败，不把旧代普通游戏状态强制纳入每帧配置校验。

Playing/Paused 重载先根据新类目录验证作者 Blueprint、全部 Spawn 模板、
当前作者实例、已生成实例及运行时逻辑组件，并构造新上下文、执行 OnCreate。
随后恢复 `[Migrate]` 值及根/内置空间组件当前值、enabled/failed/destroyed、
逻辑组件状态和活动镜头；保留原生 World generation、Jolt 世界（含刚体 1/60 子步余量）、
位姿/速度、脚本播放时钟与帧号、Time Scale 及 Playing/Paused 状态。
接触/Trigger 已见边沿集合也保留，换代本身不重复产生 Enter。
OnCreate 之后不会重复 BeginPlay，已开始且未故障/销毁的 owner 与组件执行 OnReload。
旧上下文 handle/token 失效，应以 BlueprintReference/稳定 ID 重新查找；
旧 timer/event/委托不迁移，OnReload 可重新注册 timer/event。

OnReload 不得改变 World 根/空间组件值、启用/销毁状态、逻辑组件结构或状态，
也不得发 Spawn/Destroy、物理指令/查询或动态资产请求。
候选导出后逐值比对再无抛交换脚本 owner；OnReload 失败或比对不符保留旧上下文。
在途/待交付资产请求、租约、取消/卸载通知或射线请求/结果会直接拒绝迁移，
未领取结构指令或待交付事件同样拒绝且保留原队列，
要求先完成或取消并释放租约、让原上下文领取未处理批次。原生活动动态模板本身可保留。
成功后旧上下文 EndPlay/清理并请求卸载，新代回调累计耗时重新统计；
不保证任意脚本自身副作用能够回滚。
候选完整导出与原生运行状态在同一提交边界交换，保留 Jolt 速度/着地值；
Paused 下也立即显示新代逻辑组件的默认属性和句柄代际，不依赖下一次用户回调补发。

SubsystemCollection 拥有 ScriptRuntimeSubsystem。候选 World 保存自己的
作者声明与 RAII ScriptWorldContext。PrepareWorldLoad 中构造实例、
绑定属性、执行 OnCreate；类不存在、类型不匹配或 OnCreate 失败使候选加载失败，
原生活动 World/图/资源不变。脚本是可信进程内代码，没有沙箱；
静态字段、文件操作及脚本自行创建的线程不具备事务回滚语义。

默认 Editing 不 Tick。Play 重新构造实例、绑定作者 TRS/属性、OnCreate、BeginPlay；
每帧在 GT、输入更新后、渲染快照前执行
Tick(dt = frameDt × TimeScale) → `StepPhysics(dt)`（角色按帧 dt、刚体内部 1/60 子步）
→ 接触/查询回调 → LateTick(同一 dt) → `StepPhysics(0)`（只同步 LateTick 指令/接触，不推进时间）→ 动画求值
→ SpringArm 相机求值与同步查询（存在 arm 时）。
Pause 冻结脚本时钟与回调；Play 恢复。Stop 执行 EndPlay、
释放上下文、恢复作者 TRS 和可见性。成功替换 World 后停止旧上下文，
新 World 回到 Editing；失败候选不会停止原来的游戏。Shutdown 在 renderer teardown 前释放。
切到离线渲染后、该帧脚本 Tick 前自动 Stop，恢复作者 TRS。

运行时 TRS 与作者声明分开存储；无网格的逻辑 Blueprint 也能在 Details 回显。
网格使用 World 的 Blueprint/组件完整矩阵写入，保留多 section 局部矩阵。
RT 只读 WorldSnapshot，不访问托管对象。Play/Pause 期间阻止 Details 作者编辑，
观察相机不消费游戏移动输入，视图使用活动组件 Camera 或作者 Camera。
键盘焦点和模态继续由 SDL/RmlUi 输入仲裁决定。

当前 API：Transform.Position/Rotation/Scale、Translate/Rotate、Input 的 Move
Vector2 与 MoveForward/MoveRight/MoveUp 轴、下述 Time 值、
回调内 Log.Debug/Info/Warning/Error、Destroy，Components/GetComponent、SceneComponent.Parent/WorldMatrix、
BlueprintWorld 的 Spawn/Destroy/IsAlive 与可选 Body。Destroy 停止后续回调、
结束播放并隐藏组件网格；实例槽位保留到上下文释放，Stop 恢复作者实例。
计时器、跨 Blueprint 查找/消息、组件与动态资产见上节。

### 时间与性能快照

原生 GT 独占时间累积，脚本使用只读 Time 值：

| API | 语义 |
| --- | --- |
| DeltaTime / UnscaledDeltaTime | Tick/LateTick 为同一帧的倍率后/倍率前时长；没有独立固定步时钟 |
| TimeScale | 有限 0..100，只能由原生 SetTimeScale/工具栏 Time 命令修改 |
| TimeSinceStartup | 原生脚本 subsystem 初始化后随引擎帧推进，Editing/Pause 继续 |
| TimeSincePlay / UnscaledTimeSincePlay | Playing 期间累计倍率后/倍率前时长 |
| FrameIndex | Play 中每帧 +1；没有固定步帧号 |

每帧只回调一次 Tick 与一次 LateTick，二者使用同一个帧 dt；没有 accumulator、
固定步追赶或待补时间，因此不存在固定步数、丢步或丢时间字段。
游戏回调的帧 dt 是 `EngineLoop` 以 `min(rawDelta, 0.1 s)` 做 hitch 防护后的帧间隔；
FPS 与资产/脚本构建轮询仍使用原始 dt。
Pause 不回调、不推进 Play 时钟与帧号；
TimeScale=0 仍每帧 Tick/LateTick（dt=0），推进非倍率时钟和帧号，物理与 timer 冻结。
Stop/World 换代重置 Play 时钟、帧号及倍率至 1，保留 startup；Shutdown 全部清零。
工具栏 Time 输入走 World generation 校验的 GT 值请求，离线/资产预览拒绝，
未完成且聚焦的文本不被快照刷新覆盖。

`ScriptRuntimeView.performance` 是冻结 JSON 值，包含 time、physicsMs、
blueprintCount/truncated、blueprints、pending、assembly、gc。
每个 Blueprint 含 ID/名称/类、enabled/failed/destroyed、生命周期、最后异常文本，
及 callbacks 的 phase/calls/lastMs/totalMs/maxMs；逻辑组件有对应独立指标。
性能采样在回调执行边界计时，physicsMs 覆盖原生同步/查询/视觉发布及 Jolt，
不等同于求解器独占时间。快照最多 128 个 Blueprint，每个最多 32 个逻辑组件、
全快照合计最多 128 个逻辑组件，超额以 truncated 标识；组件指标台账每实例最多 128 项。
异常仅保留最多 4096 字符的文本，不持有旧 Exception/Type/委托。
pending 报 timer/subscription/event/ray/asset/结构请求数，
assembly 报当前代/已加载代/尚存退休弱引用/上下文数，
gc 报各代 CollectionCount 和不强制收集的托管堆字节。
普通帧不强制 GC，快照与 Log/性能面板不访问 live CLR 对象。
播放时摘要、时钟、队列、程序集和 GC 数据每帧刷新；Blueprint/组件回调明细按
非倍率时间每 0.1 秒采样，故障、生命周期、启用和组件结构变化立即采样。
`detailsFrame` 与 `detailsAgeSeconds` 表示保留明细的帧号与年龄，
`pipeline.detailSamples` 表示本帧实际明细采样次数。
Pause 保留明细和年龄，不触发采样；Stop/World 换代清空。
ABI 的显式 `runtime_snapshot` 缺省仍返回完整明细，`details:false` 允许仅请求摘要。

`gc.allocatedBytes` 是采样前当前 GT 托管线程的累计分配字节，可对同线程连续采样求差；
它不是存活堆大小或其它托管线程的分配量。
`performance.pipeline` 报本帧桥接 serializeMs/managedMs/parseMs、原生 applyMs、
snapshotMs、请求/响应字节、调用数及实际状态更新数。
managedMs 包含托管解析、回调和响应序列化；snapshotMs 覆盖整次诊断请求，
与桥接阶段重叠，不能将全部字段直接相加。

跨边界数据是 ABI v1 有界 UTF-8 JSON 批次（4 MiB），不含原生指针。
生产播放的 `tick`/`late` 使用显式 `delta:true` 请求与同名响应标志：
缺失身份表示该实例未变化，缺失字段保留既有值；新 Spawn 的首条记录完整。
默认请求、Start 和显式迁移导出仍返回完整状态集合，完整结果继续校验缺失身份。
GT 按稳定 ID 索引直接修改 World 运行状态槽，未变化对象不重新同步组件网格或发布视觉值。
只有物理实例进入 Jolt 同步，物理回传带根 TRS、速度与着地状态，并先推进托管增量基线，
保证下一帧 Tick 看见上一物理帧结果；普通自定义配置不参与物理往返。
冲量、速度指令、Jump、查询与资产请求/取消按一次性命令交付，同值新命令不被状态去重。
活动相机切换同时标记新旧 owner；回调之外产生的资源清理取消也标记 owner。
托管只检查执行过用户回调/结构指令的实例的字段差异。
未重写的基类 Tick/LateTick 不执行、也不产生回调 timing；
显式空 override、继承 override、timer/event/查询/资产委托和生命周期仍执行。
回调位掩码只有 Tick/LateTick 两位，用弱 Type 键缓存，不阻止旧 Gameplay 程序集回收。
此通道仍为 JSON，不是二进制 ABI；每帧仍采集有界性能摘要。
导出先比较值再分配增量字典/向量，组件结构数组未变化时复用作者节点；
完整导出及首次 Spawn 仍返回完整记录。
回调异常/无效 TRS、投影或灯光值恢复该回调之前的根/组件 TRS、镜头投影与活动镜头、灯光值、
销毁标记、物理指令，
丢弃该回调排队的结构请求，禁用故障实例后续 Tick/LateTick，
其他实例继续；诊断含类、ID、组件、阶段、上下文、帧号、exceptionType 和
DisableBlueprint/DisableComponent 处置。Spawn 拒绝使用 RejectSpawn；
桥接请求失败使用 RejectRequest，运行上下文致命失败使用 StopContext。
原生日志补充程序集路径/代际及场景路径/World generation，Log 页签保留完整结构化值。
超限结果不重放请求，宿主停止该上下文。释放后拒绝旧上下文 ID。
EndPlay 日志/诊断不经逐帧 Apply 发布；析构清理不传播异常。

### 游戏 HUD 文本

`Blueprint.Hud.SetText(key, text)` 与 `Remove(key)` 在所属 GT 播放回调内发布文本；
逻辑组件经 Owner 的 Hud 使用独立组件命名空间，同 key 不覆盖兄弟组件或根文本。
OnCreate、外 owner、旧上下文及工作线程调用被拒绝。key 为 1..64 个非控制字符，
text 最多 1024 字符且不含 NUL；每发布者最多 8 行、每 Blueprint 32 行、
每上下文 128 行。超限沿用回调故障隔离；本次服务修改参与回调回滚。
禁用/故障/移除组件只清理该组件文本；owner 禁用/故障/Destroy、Stop 和 World
换代清理其文本。Pause 保留最后文本。程序集迁移不复制旧 HUD 注册状态，
新代可在 OnReload 根据 `[Migrate]` 的游戏状态重新发布。

`ScriptRuntimeView.hudLines` 是按 owner 和组件/key 稳定排列的冻结文本值，
包含 blueprintId/componentId/key/text；World generation 不符时为空。
UI 将这些值合成 `game_ui.gameplay_text`，通过 `ui/runtime-hud.rml` 文本绑定显示，
遵循既有 HUD 可见性策略。文本插值不解释 RML markup，C# 没有 DOM、布局或 Vulkan 权限。
`Gameplay.HudStateBlueprint` 示例使用 Fire 输入修改弹药并发布 HUD，
不需要增加 C++ 游戏逻辑分支。

## 验证入口

```powershell
cmake --build build --target script_tests editor_ui_tests -j8
.\build\bin\script_tests.exe --gtest_repeat=2
.\build\bin\editor_ui_tests.exe --gtest_repeat=2
ctest --test-dir build -R "^(Script|GameplayInput|PreparedAssetSourceSnapshot)" --output-on-failure
```

script_tests 使用真实 hostfxr/CLR、独立测试程序集和生产 World，不创建 Vulkan 窗口。
关闭开关时仅运行场景/资产校验与放置几何用例；托管用例只在开启时注册。
编辑器回归使用真实 RmlUi 与假动作 sink，不等于 Vulkan 或桌面验收。
私有运行时部署检查先构建 `vl_script_package`，再将测试程序、所需原生 DLL 和
完整 managed 目录复制到独立目录，以新进程仅运行
`--gtest_filter=ScriptDeployment.ExecutableRelativePrivateRuntimeHostsGameplay`。
该用例核对实际加载的 CoreCLR 路径；缺少私有运行时时跳过，不构成部署通过。
