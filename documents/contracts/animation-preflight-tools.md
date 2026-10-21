# 动画预检工具

> 状态：当前契约。描述动画开发工具；运行时播放与 Motion Matching
> 分别见动画运行时、轨迹与特征契约。

## 构建与依赖

根 CMake 的 `VULKANLEARN_BUILD_ANIMATION_TOOLS` 默认关闭。
开启后构建 `vl_animation_preflight` 和 `vl_animation_probe`；
同时开启 `VULKANLEARN_BUILD_TESTS` 时通过根 `vulkanlearn_add_gtest(...)`
注册 `animation_preflight_tests`。
引擎与工具均需要 CMake 3.24+；`main` 常规链接 `vl_animation_assets`，
该选项仅控制开发工具与相应测试。CPU 导入库与 `--import-gasp` 入口见
[动画资产导入](animation-asset-import.md)；P0 的源空间审计入口保持原语义。

`extern/OzzAnimation/CMakeLists.txt` 以固定 commit 和 tarball SHA256
锁定 ozz-animation 0.16.0，只构建库，不引入 Autodesk FBX SDK、
上游样例/工具/测试。ozz 使用静态链接，局部 `BUILD_SHARED_LIBS` 设置
不改变相邻依赖；MIT 说明复制到构建输出的 `bin/licenses/ozz-animation.txt`。

## 检查入口

`vl_animation_probe --output <new-report.json>` 读取锁定依赖的
`pab_skeleton.ozz` 与 `pab_walk_raw.ozz`，本机构建压缩片段，
含起止端点采样 121 次，以原始关键帧参考比较关节模型空间位置。
报告含骨架层级、时长、轨道数、每帧位置、最大误差/位移与 BLAKE3 输入摘要。
位置保持源空间，未声明 VL 轴向/长度换算。

`SampleModelPositions` 在工具边界拒绝空骨架、轨道数不匹配、
无效时长及非有限/越界采样时刻。采样 context 只属于单次调用的片段，
支持非单调时刻；它不持有 World、GPU 资源或游戏时钟。
轨道数相等不证明骨骼语义一致，调用者负责映射。

ozz archive 本身不提供损坏/截断防御；工具仅读取依赖内可信夹具，
没有开放外部 `.ozz` 路径，不作为正式资产加载器。

`vl_animation_probe --project <file.uproject> --output <new-report.json>`
只读盘点 UE descriptor 及 Content/Config/Plugins 内指定扩展名的文件，
输出排序路径、大小和 BLAKE3 身份；排除项目根 Saved/Intermediate 等生成目录。
不加载 UE，拒绝解析到项目外的链接，拒绝向源项目内输出或覆盖已有报告。
输入捕获要求关闭编辑器和其他写入方；不承诺活跃写入期间的事务快照。
盘点仅覆盖清单指定输入，不是完整项目源码/导出依赖闭包。

文件名前缀只作提示，不确认 UE 资产类型或 GASP 身份。
只读清单本身不验证许可、版本库 revision、动画曲线/事件/schema、导出或视觉行为。

`audit_gasp.py` 是独立的 UE PythonScript commandlet 工具：读取实际 Asset Registry，
从隔离副本导出固定 mannequin、12 段片段和侧车数据，不调用资产/项目保存。
侧车含参考姿态、5 个 RAW 参考帧、60 Hz 根轨道、float 曲线时间/值、
notify 与同步标记，以及选定 schema/database/Chooser/AnimBlueprint 的 UE 文本。
RAW 求值禁用 retarget，保留根位移；此 API 的 WORLD 指 component/model 空间。
UE 文本不是 VL 格式；曲线切线/插值和动画蓝图行为不在已验证范围内。
输出目录必须新建且位于 UE 项目之外；异常与不可用属性显式记录。
`asset-export-complete` 只指固定列表导出成功，不代表 P0 整阶段完成。

`audit_gasp_traversal.py` 是独立的 UE PythonScript commandlet 工具：
只读盘点 Traversal 目录的 AnimSequence 并写 `traversal-audit.json`；
`VL_GASP_TRAVERSAL_CLIPS` 列出的名称另行导出 FBX，使用同一 no-bake、
无预览网格、root motion 保留策略。输出目录必须新建且位于 UE 项目之外；
它不保存/升级原项目，也不承诺全部 Traversal 动作已接入运行时。

`audit_gasp_catch.py` 是同一 commandlet 下的只读 Catch 盘点工具：
枚举 `AM_M_Neutral_Traversal_Catch_*`，用 UE `ObjectExporterT3D` 导出 Montage 文本，
并从 `SlotAnimTracks(... AnimReference=...)` 解析、导出被引用的 AnimSequence FBX。
输出路径由 `VL_GASP_CATCH_OUTPUT`、`VL_GASP_CATCH_T3D_DIR`、
`VL_GASP_CATCH_FBX_DIR` 给出，必须位于 UE 项目之外；报告已存在即拒绝。
它不保存/升级原项目。Catch 的 Montage id 不总是 `AM_` + clip id
（如 `AM_..._Catch_Cliff_high_standF` 播放 `M_..._Catch_Cliff_high_stand`），
转换器按 T3D 的 `AnimReference` 取 clip，不按文件名配对。

UE 5.8.2 的骨骼网格 FBX 导出依赖渲染资源，NullRHI 实测触发 MeshObject 断言。
工具使用 AllowCommandletRendering/RenderOffscreen，显式关闭材质烘焙；
首轮可能编译引擎 shader，材质没有转换为 VL。

`vl_animation_probe --fbx <file.fbx> --output <new-report.json>`
使用已有 Assimp，只读输出全部节点、局部/模型矩阵、权重统计与动画 TRS 关键帧。
同名网格/armature 用节点和父节点数值索引区分。
禁用 pivot 辅助节点保留、空骨删除及默认轴向校正，postprocess 为 0；
上游仅为含网格 FBX 校正根轴向，因此预检统一保留原基底。
无几何动画的 INCOMPLETE 标志是 Assimp 支持的输入类型，报告保留该标志；
不照搬静态网格导入器的拒绝条件。不启用 GlobalScale，不裁剪/归一化权重，
不执行正式骨架导入、顶点重排、VL 坐标转换或运行时发布。

`compare_gasp_export.py` 是外部数据对照入口，比较每片段 5 个 UE RAW 姿态、
时长及 60 Hz 根位置；固定候选基底为 `(UE.X, -UE.Y, UE.Z)`、源厘米单位。
阈值为 0.1 cm / 0.1 degree / 0.00001 s，失败保留在报告；
`VB ` 虚拟骨单列，不冒充物理 FBX 节点。
同时比较网格参考骨架和 bind 矩阵残差；必须保留 UE 作者轨道。
源片段本就没有轨道的骨骼使用绑定姿态，仍参与姿态误差比较。
图示为源数据的二维投影，不是 VL 动画渲染证据。

## 特征数据库

`vl_animation_probe --build-motion-db <资源相对源.json> --resource-root <资源根> --output <新报告.json>`
从正式模型、schema 和显式有序成员构建确定性特征库。
缓存位于 `Generated/MotionMatching/<BLAKE3>.json`；命中验证 payload 摘要及结构，
损坏重建，发布前重验全部输入；报告拒绝覆盖及源描述目录内输出。
格式、差分/循环边界、统计与预算见 [轨迹与特征](animation-motion-features.md)。
该离线构建入口不执行 UE Pose Search 二进制或运行时搜索；
正式运行时通过同版本数据库与统计进行原生精确搜索。

`browse_motion_database.py` 生成自包含 HTML，检查片段帧、raw/normalized/权重和俯视轨迹；
`verify_gasp_motion_database.py` 在隔离资源副本验证冷重建、命中、损坏恢复、
版本拒绝、schema/成员顺序变化失效。
`capture_gasp_p4.py` 在 Generated/Validation 中创建独立圆周输入 C# 夹具，
经过生产 Character/Jolt/动画/历史链路，串行采集单/双线程 Motion 叠层。
只临时替换 config 的 scripts 字段，并在 finally 恢复原字节。
需 .NET SDK、numpy/Pillow；仅终止自己创建的进程，不证明手工输入或正常 teardown。

`capture_gasp_p5.py` 使用独立 C# 输入夹具，通过生产 Match 与 Character/Jolt
执行八类动作、单/双线程及多窗口尺寸检查。夹具只传允许动作族和锁定意图，
不提交 query 或指定帧；跟随相机保持角色与候选表可见。
报告区分截图完成与原生动作类别通过，未选中对应动作时保留失败截图和 trace。
Walk 要求输入至少持续四游戏秒、原生循环片段连续保持至少 0.75 游戏秒，
六秒截止；不把偶尔出现过 Loop 当作稳态通过。夹具审阅地面与物理地板
独立扩大，覆盖该阶段的实际移动距离，不改正式关卡。
可选 `--stages blocked` 添加独立静态墙，持续 Move=200 cm/s；
四游戏秒后且真实受阻、原生 Idle 连续保持一秒才通过。
trace 的 `physicallyBlocked` 由夹具已提交意图与实际 Jolt 速度推导，
不是未跨 CLR 发布的原生查询标志。原生 blocked 和红色冻结查询仍由真实 UI
及对应模块快照检查；未来点重合时显示原地红标记，不画虚构的长轨迹。
托管反馈与 RT 截图不是同一帧，图中实际 Selected 行需要单独核对。
可选 `--pause-animation-before-capture` 使用既有 Console 暂停动画；
不暂停 Character/Jolt，也不提供精确反馈/图片配对保证。
仅写 Generated/Validation 与 artifacts，临时 config 精确恢复；
不执行正式控制器、不代表真实键鼠或正常退出。

同一工具可通过 `--match-speed` 开启原生稳态步频匹配，默认关闭。
选项经独立 Scene 的 `MatchSpeed` 属性进入 C# Match 请求；
trace 同时记录开关、原生速率与 `SpeedMatching` 反馈。
Run 在开关两组都要求循环连续保持至少 0.75 游戏秒，失败截止为六秒，
避免将混合中的关闭组与稳态开启组作对照。此选项复用既有输入与采集链路，
不注册新 runtime test，也不证明图片与反馈精确同帧。

`--foot-placement` 默认关闭，经独立 Scene 的同名属性进入生产 Match，
trace 保留原生双足参考点、探测、法线、锚点、锁定状态和骨盆偏移。
可选 `--stages slope` 使用 18° 静态横坡，碰撞盒顶面与可见平面共面；
输入静止至少四游戏秒，并等待 grounded/Idle 连续一秒，六秒截止。
这是绑定 ball 单参考点的图片对照，不替代独立几何误差或鞋底网格接触验收。

## 验证

`vl_animation_probe --skin-gasp <audit-directory> --time <seconds> --output <new.json>`
输出固定 Walk 片段的 CPU 蒙皮顶点、法线、UV 与索引。位置独立按全部影响累加，
不复用 GPU 的打包 CSR 解码。时间经过采样入口校验；输出禁止覆盖或写进审计输入目录。
`build_gasp_skin_reference.py` 将此报告变成 `Generated/Validation/<name>/` 下的
静态 OBJ/SM/MI/Scene，供外部像素对照，不是新的 runtime test 标志。

P2 的正式关卡 `Maps/SC_gasp/{SC_gasp.json,Meshes/,Materials/,Source/}` 现由资源侧
直接维护；一次性 `build_gasp_preview.py --formal` 生成器已于 2026-10-20 退役，
不再提供“把隔离导出提升为正式关卡”的入口。审计清单仍在
`Generated/Import/SC_gasp/`；材质是 VL PBR 审阅材质，不宣称 UE 原材质等价。

`capture_gasp_p2.py` 使用生产启动参数串行采集 Beauty、Normal、deferred velocity 和编辑器画面，
比较 CPU 静态参考、单/双线程和暂停重复；它验证 BMP 完整载荷，不是 CTest 或 runtime test 标志。
速度检查使用固定 Normal 参考的几何掩码，排除没有写入速度附件的 HDRI 天空。
采集结束只终止自己创建的进程，不将该动作计为正常 teardown。

`export_gasp_level.py` 从隔离 DefaultLevel 读取可见静态 LOD0 sections、
变换、材质身份、碰撞开关与出生点；`build_gasp_level.py` 创建独立
`Maps/SC_gasp/SC_gasp_playground.json` 和初始审阅色板，不覆盖 P2 场景。
`export_gasp_materials.py` 解析 MID/MIC 继承，导出实际源贴图、材质/函数文本、
primitive bounds、天空与 Logo 参数，以及公开 Landscape RG 高度回读。
首次回读等待源编辑器材质编译；导入器在正式写入前拒绝全黑高度图。

`import_gasp_materials.py` 使用独立 Textures/Level、Materials/LevelTextured、
Meshes/LevelTextured 资产，保留源颜色空间并离线抵消材质上传的一次翻行。
HDR 天空不翻行。源网格材质通过 `M_gaspGrid` 重建三层投影、顶部颜色和漆面细节；
Logo 是固定透明平面，源地表高度烘焙为普通静态网格和碰撞，不增加 terrain 类型。
新增对象、组件引用与环境字段不在标准 Scene 能力表内，由导入器直接作者；
描述字段仍经正式 get_document/get_capabilities/modify/validate/save。
工具拒绝已有专属输出，但不承诺多文件失败回滚。UE 交互/小部件/NPC、
Lumen 与曝光等价不属于该转换范围。

`capture_gasp_p3.py` 在 Generated/Validation 中作者固定相机场景，复用正式
P3 模型/材质/贴图；相机在正式加载前固定，避免移动相机与截图同帧的快照延迟。
串行采集总览、Logo、细节、BaseColor、Normal、Roughness，
沿用 P2 的完整 BMP 检查与进程清理；不是键鼠操作或正常 teardown 证据。

构建、操作命令与已知范围见
[工具入口](../../tool/animation/README.md)。
模块套件为 `AnimationSampling`、`AnimationDependency`、
`UnrealProjectInventory`、`AnimationFbxInspection`；包含手算双关节层级、端点、倒序采样、
入口拒绝、真实走路片段与原始参考、文件字节失效、生成目录排除与稳定清单。
还覆盖中文资产路径、项目内输出拒绝和 Windows 大小写路径别名。
不增加 Vulkan runtime test 标志。
FBX 模块回归使用已有 Assimp 骨架夹具，验证父链/轨道保留、原始未归一化权重可见，
以及缺失/错误扩展名输入拒绝；不将依赖夹具当作 GASP 骨架。
