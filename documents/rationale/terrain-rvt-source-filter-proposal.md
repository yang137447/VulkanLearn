# RVT 源过滤双参考诊断与确定性规范提案

> 状态：提案及有限实验解释，2026-11-21。没有以本提案替换验收条件，
> 没有宣称 P3 正确性或性能通过。正式 evaluator 在本次分析期间保持稳定。

## 保留的两个参考

第一参考是原 actual-GPU evaluator：相同源 image view、sampler、
textureLod 和各通道 footprint，不能用理想化 CPU 参考替代它。
第二参考是独立 double：按输入 payload 的格式解释、sRGB 解码后过滤、
同 layer 参数和积分位置评价。验收继续分别报告相对两个参考的误差。

`artifacts/terrain-rvt-production/p3-sampling-experiment/run-001` 实际读取了
原 sampler 的 textureLod、同 image view 的四个 texelFetch、手工双线性和
采样差值；另用关闭 anisotropy 的 sampler 作对照。关闭 anisotropy 没有改变
这些取值，不能据此建议仅关闭 anisotropy。

四个 Repeat 接缝点，按实际读取的 GPU 层采样值和独立 coverage 权重合成：

| stored | 原 textureLod 输出 byte | 实测 fetch 后手工过滤输出 byte | 独立 double byte |
| --- | --- | --- | --- |
| 7,7 | 40 | 42 | 42 |
| 10,19 | 51 | 53 | 53 |
| 17,46 | 36 | 38 | 38 |
| 29,93 | 42 | 44 | 44 |

手工值与 double 相符，但它与旧 GPU 输出相差 2 byte。把容差仍写成 1 byte
并仅报告 double 通过会隐瞒兼容性失败。整数中点 41/52/37/43 虽然各自距双方
都为 1，却没有统一物理过滤规则推出这些值；不能为通过四点而选择定向舍入、
旧值/新值混合或接缝特判。该表是已有 GPU float 的 CPU 合成，不冒充完整
manual candidate 的 GPU 整页验证。

## 提出的统一源过滤取值规范

以下是拟实现、拟验证的规范，不是已通过的新合同：

1. 源 mip LOD 继续使用当前 SourceLod：每个通道自己的 mip0 实际 extent，
   chart Jacobian、surface UV scale、layer rotation/scale 和子样本 footprint
   组成 deltaU/deltaV；rho 为两个轴长度的最大值，LOD=max(0,log2(rho))。
   height blend 继续固定读取原 mip3，不把它换成输出页 mip。
2. LOD 限制在实际 view 可见的 `[0,mipLevels-1]`，`low=floor(LOD)`，
   `high=min(low+1,mipLevels-1)`，`lambda=LOD-low`。high==low 或 lambda==0
   时可避免重复读取，但结果必须与同一个公式相同。
3. 每级采用 `position=uv*textureSize(level)-0.5`。对 floor(position) 的四个
   整数 texel 应用声明的 Repeat 地址；用双线性连续系数重建，最后在 low/high
   两级线性插值。不得把输出页 mip 直接当源 mip，也不得漏掉奇数尺寸的真实 extent。
4. albedo 从 sRGB view texelFetch 得到已解码的线性值，再执行过滤；不能在
   encoded sRGB byte 上过滤后解码。BC7/BC5 同样走 image view 的硬件块解码
   texelFetch，不在 shader 中重新实现 BC decoder。
5. normal/ORM/height 保持 linear view；normalTS 解码、frame 变换、
   per-layer 归一化、height score、层权重、roughness RMS、最终 sRGB encode
   等 evaluator 公式均不变。本轮不凭四个 albedo 点调整色彩或 roughness。
6. 为保证过滤是可复算的，系数与累加顺序必须明确，测试覆盖整数/fractional LOD、
   正负 UV、边缘 wrap、非方形/奇数 mip、只有一级的 view、BC view 和不同通道尺寸。
   GPU texelFetch 的格式转换精度仍需与 double 区分记录，不能宣称无限精度。

目前源数组约定为 repeat/linear mipmap；若某生产闭包声明不同 sampler/address
语义，必须在入口明确验证或实现对应规则，不能由 manual filtering 静默覆盖。
frame、Local 权重和 coverage 不是可以顺便任意替换的层源过滤；任何改动均需
各自参考及内容身份。

## 兼容性判定与消费者

全源 manual filtering 会改变旧硬件 textureLod 的可观察结果。本次有限数据已证明
不能假定新/旧 evaluator 全部输出天然相差不超过 1 byte。若完整对照再次确认
超过门槛，则即使 double 通过，也应保留“与原 actual-GPU evaluator 不兼容”的
失败，交主线程及独立复核裁决，不能自行更换参考或放宽阈值。

若后续采纳统一过滤规范，应让基础 evaluator 的相关参考/作者生产使用同一规范，
并由 source closure hash 与生成规则身份阻止旧页混用。这不等于恢复旧基础材质路径。
是否同步动态 detail、TES、frame 等消费者须按其真实过滤合同分别判断，
不借本次 producer 修改顺带改写所有 textureLod。

## 并行性能方案的边界

子样本并行 prototype 已有小矩阵实际时间：同公式、同输入、同 sample 顺序归约，
root 从四个 workgroup 增至 81 个；代表 mip0/6/13 未超 4ms。
它仍带原 textureLod 的四字节失败，不是最终通过。
manual trilinear 每源样本最多八次 fetch，可能消耗原 mip6 3.252ms 的剩余预算；
必须用新实现验证全部代表 mip，再跑冻结 10936 实际输入范围，不能以 root 的加速替代。
