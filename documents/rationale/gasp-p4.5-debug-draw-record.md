# GASP P4.5 公共 DrawDebug 执行记录

本文为 2026-10-18 的执行证据，不是接口契约。当前接口以
[DrawDebug](../contracts/debug-draw.md) 为准。P5 尚未开始。

## 范围与入口

用户同意在 P5 前完善公共 DrawDebug，并补充“深度绘制还是需要的，不过可以做成可选参数”。
本轮交付十种 C++/C# 形状、World 收集器、冻结快照、生命周期、发布者隔离、
类别/owner 筛选、Console 和视口弹窗，轨迹复用同一适配器。
默认 Overlay，可选 DepthTest；C# 使用 `DebugDrawStyle.Depth`，
C++ 使用 `Style.depth`。控制台 `debugdraw demo depth` / `debugdraw demo overlay`
替换同一组持久示例；`debugdraw clear owner Demo` 清除示例。

深度显示采样图已有的单采样 resolved sceneDepth，不创建第二套 World 或场景加载路径。
不回读 CPU、不写深度；逐像素裁剪线/点/文字，普通 UI 不参与深度比较。
RT 在 acquired-image fence 后更新该 slot 的 descriptor，并恢复图末布局。

## 生命周期修正

单帧项以 EndFrame 的 revision 水位清理，避免命令或 BeginPlay 在帧清理前生产的项被误删。
Pause 保留上一帧，Stop、Shutdown、换 World 清理；程序集迁移成功清脚本项，
迁移失败保留活动项。C# 回调失败撤销新图形，组件故障/禁用/移除的生命周期清理
不被同批新几何预算失败回滚。真实 CLR 回归发现 EndPlay 与外层重复清理会吞掉
尚未交付的 cleanup 消息，已保留 cleanup，覆盖到组件发布者隔离测试。

## 渲染证据

外部 `tool/debug/capture_draw_debug.py` 使用生产启动与 Console 路径；
没有添加 runtime test 标志。验证资源位于资源根 Generated/Validation，
配置临时改动在 finally 中逐字节恢复。原 UE 项目与正式资产未修改。

原 Overlay 采集位于 `artifacts/draw-debug-visual-20261018/`：
十种形状可见；单/双线程 ROI 字节相同，主开关、类别、owner 隐藏和清理后均与 baseline 相同。
原图形对照 ROI 改变 11622 个像素。Editor 和 Motion 共用显示已查看。

新增 Debug + Vulkan validation 深度/C# 采集位于
`artifacts/draw-debug-depth-visual-20261018/`，八个进程的 stderr 均为空：

| 对照项目 | Overlay 像素数 | DepthTest 像素数 |
| --- | ---: | ---: |
| 平面后方洋红线 | 2410 | 1330 |
| 穿过平面的青线 | 2620 | 2160 |
| 前方绿线 | 2950 | 2950 |
| 后方红点 | 256 | 0 |
| 前方蓝点 | 256 | 256 |
| 后方黄色 BEHIND 字形 | 139 | 0 |

后方线的两端保持，模型遮挡的中段消失；穿平面的线保留前半部和遮挡范围外的端部。
前方 FRONT 标签保持。单/双线程 ROI 最大字节误差 0。
已查看实际 Overlay、DepthTest、编辑器深度和 C# 十形状截图；
编辑器缩放视口的深度 UV 映射正确，`<C#> & DrawDebug` 文字按转义文本显示。
图中的盒位于遮挡平面外围，证明可选深度模式下的盒显示，不单独作为盒边遮挡量测。

截图进程由工具结束，只计渲染证据，不计正常 teardown 或真实鼠标交互验收。
沉浸 HUD 的场景名狭窄换行在 baseline 也存在，未在本任务扩大修改。

最终 Development 的十二张采集位于
`artifacts/draw-debug-development-final-20261018/`：
主开关、类别/owner 隐藏与清理全部恢复 baseline，owner Demo 恢复演示，
各对照 ROI 均为字节误差 0；十形状改变 11622 个像素。
`debugdraw demo depth` 与同图 `shaderreload all` 后的 ROI 同样字节相同。
后者日志为 `committed=true, pipelinesCreated=0`，是无内容变化的手动重载及显示检查，
不计为制造 UI shader 内容变化后的新管线替换验收。

## 验证状态

纯 CPU 的 `debug_draw_tests` 27 项连续两遍通过，涵盖十形状深度传播、
默认 Overlay、非法枚举、near/far 映射与屏幕裁剪后的 reciprocal-Z 插值。
UI 全量 659 项连续两遍均为 654 通过、5 项符号链接权限环境跳过、零失败；
新增显示/标签/弹窗选择集 8 项也连续两遍通过。标签实际字形顶点验证每个锚点
各自携带深度，普通 UI 不继承标签深度。owner 菜单使用真实 RmlUi SetSelection 路径，
消除测试 helper 的双重 change 事件，不通过减少命令断言掩盖问题。

C# ON 全量脚本 332 项连续两遍通过，包含真实 CLR 的可选深度跨桥及生命周期；
C# OFF 脚本 77 项、纯 CPU 27 项均连续两遍通过。
Debug 全目标、Development main、C# OFF Release main/script/core 构建通过。
初次全目标构建与 CLR 回归并发导致桥接 DLL 文件锁失败；回归结束后串行重建成功，
失败构建未计入通过结果。日志为 `artifacts/draw-debug-depth-*-build.log`、
`draw-debug-depth-ui-full.log`、`draw-debug-depth-script-full.log` 和 `draw-debug-depth-off-*.log`。

三项既有 Vulkan runtime gate 在双线程、`--no-dev-ui` 下串行退出码 0，
均有 `EngineLoop teardown complete`。Graphics/World stderr 为空；
Compute stderr 仅有用例故意更改 workgroup size 触发的 ABI 拒绝，随后恢复成功。
World/Graph 检查包含 minimize/resize 与资源退休，未以无编辑器结果覆盖既有
编辑器草稿源路径问题。没有声称完整根 CTest 或真实鼠标矩阵完成。
文档校验、65 条边界审计和 diff 空白检查通过。

## 边界

DepthTest 仅以场景深度为遮挡源；调试形状彼此不遮挡。
文字按各自世界锚点深度展开，每个字形像素接受遮挡。
透明物体不写场景深度时不遮挡，MSAA 采用现有 SampleZero resolve。
离线显示或缺少可采样且 STORE 的二维 sceneDepth 的图隐藏 DepthTest 项。
关闭整个 UI 子系统时没有可见调试叠层。
没有实现 MM 搜索、动作混合、IK 或 P5 角色效果。
