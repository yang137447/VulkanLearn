# P4 World bindings 独立中审

状态：**PENDING（CPU 局部证据通过，未进行真实 Editor/GPU 验收）**。
日期：2026-11-21，Asia/Shanghai。
本轮仅写本文件，不修改实现、测试、预算或其它 owner 文档；
未用 GPU，未启动 Game/Editor/Cook，未使用 `build-vt-release`。
不重复已经独立关闭的 leaf identity/R2 CPU 审计。

## 本轮结论

**所审范围未发现新的可证死锁、首次 false 部分发布或旧 descriptor owner 提前释放缺陷。**
独立重新编译执行既有四组 CPU 注入，另加双 pass 的部分成功后失败测试，全部通过。
实际代码已把 terrain World texture 槽走 cache snapshot/Set3 更新，
不再需要向 feedback PassMI 写未声明的 layerIds。
这支持根修方向，不代表 E003 问题已由新实际运行关闭；
**E004 真 Paint/Undo、跨 surface、source-update fault 尚未执行，整 P4 不得 PASS。**

Service 整合仍在变化；本轮仅核查已接入的 joint switch 路径，
不以编译中间态判最终 FAIL，也不宣称该 Service 事务已动态测试。

## 冻结身份与独立 CPU 方法

读取 [API 交接](../../artifacts/terrain-rvt-production/p4-world-bindings-api-handoff.md)
和 `artifacts/terrain-rvt-production/p4-world-bindings-native/tests.log`。
交接测试是自带 main/Check 的原始 CPU harness，**不是 GoogleTest 注册套件**，
本轮按四组报告，不将其错误计为四项 GTest。

复制 WorldBindings h/cpp、cache h、graph h 及测试至系统 TEMP：
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-world-bindings-independent/`。
从当前 cache.cpp/graph.cpp 按交接相同函数签名重新提取未改写的方法体至
TEMP `selected-cache.cpp` / `selected-graph.cpp`；
实际编译 WorldBindings.cpp、Budget.cpp、tressfxGeometryCache.cpp，
以及上述方法体和测试，不执行交接 exe。
未运行交接 native.py（该脚本会读取 build-vt-release）。

| 源文件 | 本轮 SHA256 |
| --- | --- |
| terrainCompositedWorldBindings.h | `b65c27cde52db36edec4a2cf21e559149c89c6858bc8a6a00c525edae6a96c74` |
| terrainCompositedWorldBindings.cpp | `b1cc2e981973c3ffcf794575e3ef83ae010d651b667d441d726b23ca6be0c5ef` |
| rendererResourceCache.h | `71f3b4c1280179156caf27fbb0f34c3487d403142cd58cd4dbdb7d65eae628aa` |
| rendererResourceCache.cpp | `5cdcbb646e7f3fe45ecee8141866aaa198f401627276d337f7974c849a7eeb4d` |
| renderGraph.h | `7cdc9fc519a4543ab81e39944103fddb6e0850b2f6626bc8d358a3d2a21c92f2` |
| renderGraph.cpp | `032354a5b1664bbb4dd0ac50bf2e71ba6b26b013f3f8fd96aa86085446037b5f` |
| renderGraphPassResources.cpp | `02d0f0cf63f3344f5a1c379910342b8caa6295516f866edc38d07ba8b8749b66` |

G++ C++17/O0、pthread、function/data sections/gc-sections，
include 为冻结 source、项目 source、Eigen3/Json/VulkanSDK。
Texture 构造/描述符与 descriptor writer 为 CPU spy；
这些测试没有 Vulkan device 调用。
本轮没有独立重编全部五个生产 TU，交接的五 TU native compile 属于实现者证据。

本轮输出：

```text
PASS prepare/cancel/second-invalid/metadata-return
PASS lock-failure-zero-effect/runtime-false-cancel/stale-revision
PASS new-multislot/real-graph-spy/per-image-hold/last-fence-leaf-release
PASS descriptor-failure-retry/package-replacement/no-world-owner-chain
PASS two-pass-partial-writer-failure/strong-owners/other-images-old/retry-only-failed-pass
```

两个独立 exe 位于 TEMP 下 `independent-tests.exe`、
`partial-pass-probe.exe`。第一项测试 map/snapshot prepare、无效第二 replacement、
metadata 归还；第二项持真实 cache recursive_mutex 的另一线程制造竞争，
验证 final try-lock false、Runtime false 模拟取消、stale revision 拒绝。
“Runtime false”是 harness 的显式 false 分支，不是本轮重新链接 Runtime 事务测试。

## 多槽准备、身份与锁

PrepareWorldTextureBindings 持 cache try-lock：
核对 package generation、当前 snapshot 身份、revision、
预算 owner/GPU reserve，拒绝无效 replacement。
复制 map 与 descriptor snapshot、metadata 准入均在准备阶段完成；
无效第二 replacement 不修改 active map/snapshot，收费回滚测试通过。

Prepared ticket 持 expectedWorld 强引用直到 commit/cancel；
final try-lock 核对当前 package、snapshot、revision 仍相同。
因此包已替换或 revision 变化会拒绝，未把过期 lookup 当作当前绑定。
这里 expectedWorld 是短期 prepared ticket 的预期包，
已发布 descriptor snapshot 不包含 package 强引用。
标准包替换测试中，旧 package weak_ptr 在释放 retired handle 后过期，
旧图像仍由其它 image snapshot 保活，没有 whole World 链。

读取到的 Service 路径是：

1. 先 Capture/Prepare World bindings，再准备 Material/PageSource；
2. final 获取 Material batch，再 World cache，再 leaf，再统一 Budget batch；
3. Runtime false 时取消 prepared runtime/leaf/world/material；
4. Runtime true 后 Finalize leaf、PageSource commit、
   World map commit、Material commit，再切 current inputs。

World map CommitNoThrow 仅对已分配 map/snapshot swap 和 scalar revision 发布，
不调用 Vulkan，也不写 PassMI 未声明槽。
该路径在所审四组测试中没有 phase/leaf 锁动态组合覆盖，
不能据局部 cache 测试宣称完整 Material→cache→leaf→budget 死锁证明。
Cancel 的 map/snapshot析构可能返还metadata租约；
调用方仍须在退出 leaf/budget 锁后执行，不能从noexcept推断绝不等待任何锁。

## acquired image fence 与 Set3 刷新

源码确认 BeginFrame 在 AcquireNextSwapchainImage 后读取
该 **swapchainImageIndex** 的 image-in-flight fence并等待，
再登记新 fence、重置该image command buffer并开始录制。
不是只等待 frameIndex 同步槽后就更新另一 image。

FrameRecorder 的 hook 在 Service RecordUploads 之后、object MI refresh及pass循环之前，
传入同一个 `ctx.swapChainImageIndex`。
SafeUpdateCurrentImageWorldBindings 每次只capture一次 immutable snapshot，
该image所有worldTexture消费者都由它生成writes；
其它image的owner和descriptor image info不改。
CPU spy验证dstSet只属于选中image，
最后一个旧image更新前旧leaf不释放，最后更新后可释放。

Graph retirement将worldBindingImageOwners和registry lease
与descriptor pool一起移入RetiredRenderGraphPassResource，
经既有ResourceRetireQueue的last submitted epoch退休。
retired payload析构体先销毁descriptor pool等原生资源，
成员owner随后析构；未见把descriptor owner单独从退休包提前丢弃。
本轮只是源码顺序检查，没有运行真实旧帧/descriptor pool退休验证。

## 部分pass写入成功后失败：独立新增注入

构造两个真实RenderGraph CPU pass，各3个World输入、3个image。
发布新map后，让writer在第二个pass调用时抛bad_alloc：

- 选中image中，一个pass已安装new snapshot，另一个仍old；
- 两个pass的其它image都仍old；
- retry后仅失败pass的3个binding重写，选中image两个pass均收敛到new。

这证明**跨pass更新并非回滚为全old**，而是保留每个pass与其实际descriptor一致的强owner，
然后弃掉本次scene帧并在下次重试。
成功pass不能释放其new owner，失败pass不能释放其old owner；
本轮新增注入确认两侧各自保活。

实际backend `UpdateDescriptorSets` 先把输入转换为RHI writes，
RHI再完整构造Vulkan writes，最后才一次调用void `updateDescriptorSets`。
因此目前可抛的vector/转换操作在该pass设备写入之前；
不能把“任意writer先设备部分写再抛”假设为此实际backend的现状。
后续若writer引入设备写后抛异常，这个owner安装协议必须重新审阅。

FrameRecorder catch WorldBindingDescriptorRetry 执行DiscardFrameCommands、
AbortFrame及FrameAbandonAfterAcquire恢复acquire/submit/present，
没有继续提交带部分pass更新的scene draw，也没有把它登记为显式注入fault成功。
**GPU原子scope是“不提交本次scene命令”而非descriptor写入回滚。**
在本image fence确已完成的前提下，部分更新不能影响其它仍在飞行image；
真实Vulkan该前提和恢复路径仍需实际GPU trace/validation确认。

## metadata与未验边界

prepared transaction、descriptor snapshot、registry state/lease均有CPU准入；
metadata声明早于其所收费map/vector/texture owner，逆序析构使payload先释放。
registry GPU quote从同一已收费GPU reserve中原子占用，
失败在result gpuReserveBytes仍为0时析构，不误减其它registry占用。
本轮未发现新增4 MiB重复reserve或last-image前返费的可证问题。

GPU descriptor metadata用每binding显式256 B预留，不是Vulkan公开实际分配测量；
CPU 256/512 B节点预留也不是所有STL实现精确allocator上界证明。
这些口径不能升级为实际WDDM/进程峰值通过。

仍待实际Service整合、false后的MI/input/current一致性、descriptor写入失败后的
真实恢复帧、跨generation多image退休和E004真实Paint/Undo；
当前没有这些证据，保持PENDING。未因新API存在或五TU编译就判整P4 PASS。
