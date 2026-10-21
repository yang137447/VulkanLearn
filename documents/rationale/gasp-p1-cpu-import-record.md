# GASP P1 CPU 导入验收记录

日期：2026-10-18（本地）。这是执行证据，不是实现契约。
接续 [P0 资产基线](gasp-p0-asset-baseline-record.md) 与
[GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 结果

**P1 的固定骨架/绑定/完整权重/片段 CPU 导入与数值验收通过。**
在隔离导出集上读取 91 根物理骨、12 段动画，构建 ozz 骨架和压缩片段，
保留三根源缺轨道具骨的绑定回退，保留侧车曲线、事件等值。
可查看原生导入后的层级、长度、姿态及根轨道报告与投影图。

这不是 VL 实窗蒙皮出图；没有新增 World 动画组件、GPU palette、动画材质变体、
C# 动画 API 或 Motion Matching。P2～P9 均未验收。
P0 的许可/使用范围与 UE 操作录像仍待核对；资产未提升到正式资源仓库，
因此不把这份技术数据验收写成“正式 GASP 场景已可加载”。

## 输入与实现

引擎基准 HEAD 仍为 `14a6cad4769c3b8dde4446fe7c701cc2997051a7`，
本轮改动未提交。输入沿用已隔离并导出的
`artifacts/gasp-p0/ue-audit-06/`，没有启动或保存用户原项目。

- `source/animation/animationAssets.*`：CPU 骨架/权重/片段数据与显式时间采样。
- `source/mesh/loader/animation/assimpSkeletalImporter.*`：固定 FBX 策略的导入与校验。
- `tool/animation/gaspImportReport.*`：真实导出集的导入报告。
- `tool/animation/compare_gasp_import.py`：独立 UE→VL 数值对照及可选图。
- `tool/animation-tests/skeletalImportTests.cpp`：20 项聚焦模块回归。

库由已有默认关闭选项构建。`main` 排除动画目录的递归收集，
没有引入 ozz 链接；既有静态 Assimp 导入器、Vertex stride 和静态材质不变。
当前契约与支持限制见 [动画 CPU 导入](../contracts/animation-asset-import.md)。

## 空间与权重

固定 `ue-fbx-cm-yup-v1`：
`VL = (FBX.X, FBX.Z, -FBX.Y) = (UE.X, UE.Z, UE.Y)`，
厘米、Y-up，源 Walk_F 前向成为模型 +Z。
模型、关节局部/模型矩阵、inverse bind、动画 TRS 使用同一基底。
该政策只确定模型空间，Actor/组件朝向未在场景中验收；
既有 C# 示例移动前向 -Z，在接角色时必须显式决定模型组件朝向。

真实 mannequin 导入结果：

| 项目 | 数值 |
| --- | --- |
| 物理骨 | 91，未丢无权重父链/IK/道具骨 |
| 虚拟骨 | 2，单列，不混入物理骨架 |
| section | 1，材质槽 `M_UEFN_Mannequin` |
| MikkTSpace 后顶点/索引 | 27972 / 27972 |
| 完整正权重条目 | 55098 |
| 最大影响数 | 9，未截断或归一化 |
| 最大权重和误差 | `4.284083843231201e-08` |
| bind 平移残差 | `4.549736331682652e-05 cm` |
| bind 线性矩阵残差 | `2.980232238769531e-07` |
| 展开后绑定姿态实际 CPU 蒙皮最大残差 | `4.6024666517041624e-05 cm` |

手算夹具使用有共享索引的四顶点两三角形，MikkTSpace 展开为六个角点；
逐项核对每个输出顶点的源身份及九个原始权重，并检查最终 CW 绕序。
真实网格的 27972 个顶点原本已按面展开，不能仅靠它证明重排正确。

## UE 与 VL 对照

最终报告 `artifacts/gasp-p1/comparison-03.json` 的 `passed=true`。
参考仍为 P0 固定 UE RAW 帧；没有拟合基底或修改验收阈值。

| 对照项 | 最大误差/数量 | 门槛 |
| --- | --- | --- |
| 网格参考位置 | `0.000018421642214740123 cm` | 0.1 cm |
| 网格参考旋转 | `0.000013673439557845415°` | 0.1° |
| 片段骨骼姿态 | 5460 = 12 × 5 × 91 | 所有物理骨参与 |
| 片段位置 | `0.05506014916345889 cm` | 0.1 cm |
| 片段旋转 | `0.05135985674054262°` | 0.1° |
| 根采样 | 2888 | 按源约 60 Hz 的时刻逐项核对 |
| 根位置 | `0.00030104443430900574 cm` | 0.1 cm |
| 根旋转 | `0.00004253379665977166°` | 0.1° |
| 片段时长 | `0 s` | 0.00001 s |

12 段均有 88 条作者轨道，`props_root`、`prop_01`、`poi` 缺轨回退，
不是导入时丢轨。根保留真实位移；Walk_F 四秒到达模型 +Z 约 800 cm。
曲线时间/值、notifies、同步标记、settings、rateScale 与限制文字逐值保留。
曲线存在时长外 key，未裁掉；曲线插值/切线、事件消费和 UE 行为等价均未验收。
完整 `.copy` 文件留在输入并记录摘要，不把文本引用视为 VL 游戏事件实现。

输入清单身份：
`9ec06d55dac00fa0b6bd96ce2d9e6df33072abaf672a8af35d49f3fb065b1389`。
这是固定审计输入与导入政策的报告身份，不是完整数据库/运行时资产缓存协议。

## 失败与修正

1. 首次真实导入因 UnitScaleFactor metadata 的类型拒绝：
   当前 Assimp FBX 发布为 float，而初版只读 double。改为接受两种显式数值类型，
   仍要求精确等于厘米单位 1，不添加缺失值猜测。
2. 手算 100 cm 根轨道端点原先得到 `99.9755859375 cm`。
   阅读 ozz SamplingJob 源码确认其插值系数使用 RcpEst。
   身体保留压缩采样，根改由同库原始 SampleTrack 求值后覆盖首个 SoA lane，
   再执行 LocalToModelJob；原 0.001 cm 手算断言通过，未放宽阈值。
3. 测试首次编译因 Eigen 模板逗号被 GTest 宏分割失败，补上表达式括号后构建通过。

额外边界检查拒绝 perspective inverse bind、非单位动画四元数、
虚拟骨作为物理输入和不能覆盖完整区间的多 key 通道。
最后一项避免把 Assimp DEFAULT 区间外语义静默改为 ozz 的端点保持。

## 可重复验收

从引擎根目录执行，输出路径每次使用新名字：

```powershell
cmake --build build --target vl_animation_probe animation_preflight_tests main -j8
ctest --test-dir build -R "^(AnimationSampling|AnimationDependency|UnrealProjectInventory|AnimationFbxInspection|SkeletalImport)\." --output-on-failure
.\build\bin\vl_animation_probe.exe --import-gasp artifacts/gasp-p0/ue-audit-06 --output artifacts/gasp-p1/cpu-import-new.json
& "C:/Program Files/Python314/python.exe" tool/animation/compare_gasp_import.py `
  --audit artifacts/gasp-p0/ue-audit-06 --imported artifacts/gasp-p1/cpu-import-new.json `
  --output artifacts/gasp-p1/comparison-new.json --figure artifacts/gasp-p1/comparison-new.png
```

前提：当前 build 已开启动画工具、测试和 C#，MinGW Debug；审计输入保持只读且无写入方。
Python 对照需要 numpy/scipy，图需要 matplotlib；本机使用常规 Python314，
`py -3` 默认的 free-threaded Python 不用于该科学计算入口。

预期：91 joints / 12 clips；38 项模块回归全通过；
数值报告 `passed=true`。图左侧为 Walk 根位移，右侧为导入姿态的根相对 YZ 投影。
图中文字与布局已检查；它明确标注 CPU 数据证据，不是 Vulkan 渲染。

本轮 `main`、工具与测试构建通过，38 项模块通过。
文档分层、65 条架构边界、Python 语法和 diff 空白检查通过。
没有运行完整根 CTest、Vulkan runtime test 或 VL 实窗；未改运行时绘制/同步路径。
没有提交、修改正式资源或恢复/覆盖不相关本地变更。

## 下一验收

P2 接入正式加载与角色绘制：冻结姿态/palette、独立蒙皮输入、主画面与阴影一致、
变形 bounds 和渲染历史。可见效果是 VL 中真实角色循环播放、阴影跟随，
暂停可核对姿势。正式资产推广仍需先关闭使用范围输入缺口；
角色组件朝向与胶囊位移归属在该集成路径显式落实。
