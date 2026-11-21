# RVT producer 的有效域、真实 border 与不可达 padding

> 状态：P3 实施中的推导与待验证变更，2026-11-21；不是阶段通过记录。

依据是 `TerrainCompositedVTLayout::ValidExtent` 与消费端
`mf_terrainCompositedVT.glsl` 的 `TerrainCompositedTexel` /
`TerrainCompositedAddressAnchored`。物理页仍固定为 136×136，内区起点仍为
stored texel 4；不足 128 的末页不改变该起点。

若某轴的有效内区长度为 `valid`，producer 完整评价该轴的
`[0, valid + 8)` stored texel：4 个左 border、有效内区、4 个右 border。
每个真实 border texel 的全部积分子样本仍使用未做 surface clamp 的原始 UV，
逐样本经过显式邻接；只有连通世界外缘才 clamp。面积积分 footprint、
Jacobian、局部法线转换以及粗 mip 的 RMS roughness 规则不变。
因此 1×1 根 mip 的真实评价域为 9×9，不能只生产中心点，也不能删去真实 border。

不可达 padding 的判断来自消费端，而不是来自与 producer 同构的参考结果：

- Clamp 的 `TerrainCompositedTexel` 把全局 texel 限制到 `[0.5, extent-0.5]`。
- 锚定页读取只接受局部连续 texel 坐标 `[-3.5,131.5]`。
- atlas UV 为 `(local + 4) / 136`，因此硬件双线性坐标是 `local+3.5`。
  完整页的非零支持不超过 stored texel 135。
- 对不足 128 的末页，全局上界进一步给出 `local <= valid-0.5`，
  因此非零双线性支持上界至多是 stored texel `valid+3`。
  这严格落在保留的 `[0, valid+8)` 内。
- 未通过锚定范围检查的 tap 走普通地址解析；该路径重新选页并具有相同上界。

`ConsumerBilinearSupportNeverReachesCanonicalPadding` 从上述连续采样地址和
非零双线性支持独立检查 1、2、7、127、128、129、259、8192 等尺寸及全部页。
它不比较两个同构的产页实现。该证明仅适用于 Clamp。
显式连接世界不支持 Repeat；孤立 Repeat 必须另行覆盖实际 wrap anchored 支持域。

新 producer 的最终 encode 把 `pixel >= valid+8` 的不可达部分规范为
`min(pixel, valid+7)` 对应真实 border texel 的最终编码值。这保证全部三通道
物理存储确定、有初始化且不依赖旧槽内容，且不会减少任何可达样本或真实 border。

早期 P3 harness 将所有 stored136 都映射成新的虚拟区域，包括根 mip 中一直到
约 132 个 surface 宽度的不可达 padding。该定义导致无意义的远块评价，也不应
成为“必须与旧包每个字节一致”的新合同。run-001…004 保留该旧定义的图像、
参考 bytes、源码身份和失败计时，不能拿它们证明新定义已通过。新 oracle 仍须
对真实内区/border 的独立 evaluator，以及完整 canonical padding 字节逐项核对。
这是一项明确的生成规则变化，发布侧必须改变生成器/滤波身份并重新生成页面，
不能把旧缓存 payload 解释为新规则下的相同内容。

本文件只说明安全裁剪的依据。bbox/indirect 的 GPU 正确性、完整页字节误差和
4ms/6ms 硬门槛仍须实际测试及独立复核，不因推导成立而视为通过。

## Repeat 中审阻断及修正

Hypatia 的独立中审指出：129×129 孤立 Repeat，centerUv.x=128.75/129、
major=12、minor=1 的 8 tap footprint，在 targetMip=log2(1.5) 时，
低 mip 的最后一个 tap 有非零权重读取 stored9/10。把不足页的 Repeat
也限制为 valid+8 并复制末端 texel 是错误的；此前“保持原 evaluator”
不能证明支持域安全。

修正方案是孤立 Repeat 在同一 World evaluator 中完整评价 136×136，
不运行 Clamp canonical padding；所有位置按原始 query 做 fract 后评价。
Clamp 继续使用上文证明的 valid+8。CPU 回归从真实 footprint 和分数 mip
混合推导 stored9/10 的非零支持；GPU harness 另从未裁剪的直接 evaluator
计算这两个 texel 的参考值，而不是只比较同步改过的 Page padding oracle。
尚未运行的 GPU 验证不因这个修正被视为通过。
