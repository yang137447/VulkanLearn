# RVT P6 独立验收

日期：2026-11-21。验收者未参与 P6 实现；仅运行独立 CPU 实验，
没有修改实现、冻结配置、原报告或原实验，没有启动 GPU。

**签署：通过 P6「本轮明确不采用 BC5/BC7」决策的独立验收。**
本结论只关闭可选压缩评估，不表示压缩生产链通过，不放行 P1–P5。
依据为[生产计划](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)
允许明确不采用，以及[执行报告](terrain-rvt-p6-compression-evaluation.md)限定的候选和证据范围。

## 独立复跑与结果

独立程序 `D:/Temp/vl-p1-audit-20261121/p6_independent.py`，
结果 `D:/Temp/vl-p1-audit-20261121/p6-independent.json`，
编译和复跑命令在同目录 `REPRODUCE.md`。Python 3.14.0、Pillow 13.0.0、
texture2ddecoder 1.0.6；没有安装新工具。自写 BC4 palette/index decoder
组合成 BC5 双通道，另与 texture2ddecoder 的 BGRA 输出交换回 RG 后比较。
三个常量控制块 `(0,255)`、`(128,64)`、`(255,0)` 均精确复原，排除 RG 顺序错误。

只 seek 读取 `../fr/VT/2c41caf5a359c87a/manifest.json` 中
`terrain|Landscape_00` 的 mip0/4/13、x=y=0；每页 221952 B，
normal 位于第二个 73984 B 通道。每页 BLAKE3 与 manifest 页摘要一致，
SHA256 与执行报告一致。BLAKE3 使用仓库实现构建的 stdin helper，
先通过空输入已知摘要检查。没有校验整个多 GB 包，也不声称完成安装验证。

| mip | offset | XZ 最大 byte 误差 | XZ MAE | 最近秩 p99 | normalize 后 XYZ 最大 byte 等价值误差 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 | 16 | 19 | 1.4553687284 | 10 | 22.1661707511 |
| 4 | 1207418896 | 27 | 2.7369971886 | 13 | 30.2084794255 |
| 13 | 1213411600 | 9 | 0.2970642301 | 9 | 9.0032561259 |

三种 decoder 的 RG 字节全部一致，最大相互差异为 0。
重编码所得每页 block 为 18496 B，DDS 为 18644 B。
独立程序未复测编码耗时；原耗时只代表串行 CPU save，不代表 GPU 编码或整条生产性能。

样本 SHA256 按 mip0/4/13：

```text
c42d731aca5c8c703027c17fee445588aca523d8bd7c714a69841e58afbe5b39
df554b17f8a0e2aa9a45a42ba415cdb85809383ca9491023d8d8bb0a7aee28c0
801505119c4ff80d11c205f8de0ab80fb73300b33774d43581e0b5ab61a81dcd
```

## 通道语义及编码配置边界

当前 shader `TerrainCompositedDecodeNormal` 对 BC5 使用 RG→XZ，
Y 为正平方根，最后 normalize。独立实验确实抽取原 RGBA normal 的 XZ，
没有将 XY、sRGB 转换或 BGR 当成目标。三页源 Y 均非负。
原报告的额外 Y 误差明确是 normalize 前值；本次另按 shader 两条路径
各自 normalize 比较，误差仍明显超过冻结最大 1 byte，结论不靠省略 normalize 成立。

发现一个必须保留的容器限制：Pillow 生成的 DX10 header 为
`(dxgiFormat=82 BC5_TYPELESS, dimension=3, misc=0, arraySize=0, misc2=1)`。
这不是已验证可交付的 BC5_UNORM DDS（arraySize=0 也不能据此宣称合法）。
因此通过的是原始 16-byte BC5 block 的 UNORM 误差实验，不能把该 DDS
声称为合法运行时输入。独立 decoder 直接解 block，三 decoder 完全一致，
证明 19/27/9 不是 DDS header 的类型选择或 decoder 通道配置造成的假误差。
没有测试其他编码 profile；不声称当前误差是 BC5 的最优值或下界。

Intel 本机源码的 MIT 许可与 `CompressBlocksBC5/BC7` 入口已独立读取；
确有合法候选，未编译或部署。BC7 质量、GPU 编码吞吐、实际 BC allocation、
目标设备 copy/sample/filter、scratch、整场视觉与包体均未验收。
这些采用成本尚未验证，加上本次 Pillow 候选未达到质量门槛，足以支持本轮不采用。
不能推导“所有 BC 不可能”“硬件不支持”或“所有编码器失败”。

## 身份和冻结

P0 配置 SHA256 仍为
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`，
base normal 最大 1 byte 门槛未改。
执行脚本、结果和历史 texture 日志的尺寸、SHA256 均与报告所列一致。
独立脚本 SHA256：
`916732683dfbd558e054b29a50f4c305c28b98848f94bc9fd7346779bf69972c`。
独立结果文件 SHA256：
`ea39c4ff84e01086876a478ac7249b25b618bf7416f0e12dcc5ee2bee0a55d88`。

格式算术复核：每通道 34×34×16=18496 B；
512 槽三通道为 28409856 B；
448 粗页为 24858624 B，较 RGBA8 少 74575872 B；
16×5468 页三通道为 4854534144 B。均只为 payload 算术，
不冒充实际显存、总包或性能收益。
