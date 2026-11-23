# 地形 RVT P6 可选压缩评估

状态：2026-11-21，**本轮明确不采用 BC5/BC7 压缩**。
执行侧评估已完成，独立验收待另一名未参与执行的验收者签署。
本文件不是运行时契约，不阻塞 P1–P5，也不引入压缩与未压缩自动回退。
本次未修改 GPU 路径、未启动 GPU/Vulkan/renderer、未构建或安装编码工具，
未复制大包。只读取现有证据、源码、本机工具文件，并做小型 CPU 实验。

依据 [生产方案](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)
P6，“明确不采用记录”是允许的交付。此结论不表示 BC 技术不可行，
也不表示不存在合法编码器；它表示当前没有满足冻结误差、GPU直拷生产、
吞吐与预算要求的已验证压缩生产链。

## 不变的冻结依据

[P0配置](../../config/terrainRvtP0Frozen.json) 实际 SHA256 仍为：

```text
43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd
```

本次未改其预算、画质、性能门槛或证据。
主要约束为：512物理槽、每帧1生产页、4在途任务、生产≤4 ms、
copy≤4 ms、整页/每帧生产复制合计≤6 ms、每机位10 s收敛、
base三通道各最大1 byte误差、独立Game≤1536 MiB、
fine cache≤256 MiB、最多448粗页及16必备根页。
压缩不能通过放宽这些约束获得采用资格。

## 已有实际成本与可节省范围

读取 P0 `allocation-requirements-final.json`：

| 项目 | RGBA8 已测/已知 B | BC5/BC7 相关结论 |
| --- | ---: | --- |
| 512槽三通道池 payload | 113,639,424 | 全BC三通道的格式算术为28,409,856，非实际分配 |
| 512槽三通道池 memoryRequirements | 226,492,416 | BC实际requirements本轮未测，不能按四分之一宣称 |
| 完整生产/动态输入 requirements 包络 | 759,801,856 | 仅压缩输出池不会消除此项 |
| 每页RGBA8 payload | 221,952 | 三个BC通道合计55,488，仅block算术 |
| 允许448粗页 payload | 99,434,496 | 三个BC通道为24,858,624，差74,575,872 |

输入包络包含源数组、共享coverage、frame、权重、height及映射。
R32逐层冻结权重、RGBA32F frame和独立R16位移高度不能未经语义/误差验证
改成BC。输出页压缩没有证明能改善这759,801,856 B输入包络。
CPU/GPU峰值、descriptor、页表、producer输出和scratch也不会因最终块大小自动同比缩减。

16×5468×55,488 = **4,854,534,144 B（约4.521 GiB）**，
只是所有旧全页改为三个BC通道的payload算术。没有生成这个包，
没有把它当成实际总包、V2发布闭包或1536 MiB目标的依据。
P1–P5采用稀疏发布，不能先生成这份全页包代替缺页生产。
实际独立Game还包含生产源输入、几何、程序、shader、索引等；
448粗页约71.12 MiB的理论节省不能证明总包达标。

P0两种权重共10936页的原始JSONL已在独立P0审计中复算：
生产最大2.507070 ms，output→host复制最大3.285090 ms，
同页合计最大3.319266 ms。它们均是RGBA8参考，
没有BC编码耗时、压缩误差或压缩GPU copy结果。
不得从这些值减出固定“免费编码时间”：最大值对应的页可能不同，
未来邻接生产与atlas复制也不同。

## 仓库和本机编码实现调查

检查仓库 `source/`、`tool/`、`extern/` 的编码相关文件及符号，
区分预编码块读取与编码。

| 位置/实现 | 本次实际观察 | 可用性与限制 |
| --- | --- | --- |
| 仓库DDS/KTX2 | `compressedTextureDecoder.h`明确只读预编码BC7/BC5块；HostImage有block布局与格式映射 | 是容器/上传支持，没有RGBA→BC编码器 |
| 仓库RVT | Runtime允许albedo BC7、normal BC5，但ORM校验仍要求RGBA8 | `BC7+BC5+BC7`尚未完整接线；枚举存在不等于生产可用 |
| PATH | `Get-Command texconv,compressonatorcli,nvcompress,basisu,toktx,magick`无结果 | 仅证明这些命令不在本次PATH，不声称整机不存在 |
| Python3.14 Pillow13.0.0 | 实际`Image.save(...,pixel_format="BC5")`成功；BC7抛`cannot write pixel format BC7` | 可做CPU BC5实验，不能完成三通道BC7链 |
| Python其他模块 | `imagecodecs`、`ispc_texcomp`不存在；`texture2ddecoder`存在 | 后者是decoder，不能当encoder |
| Intel ISPCTextureCompressor | 本机UE5.4.4 ThirdParty中有14d998c源码、ISPC编译器、Release/Debug `.lib` | 有MIT许可及`CompressBlocksBC5/BC7`，合法候选；未编译、未链接进MinGW项目、未测吞吐/误差 |
| RTX Remix附带NVTT | 有`nvtt_export.exe`、`nvcompress.exe`、`nvtt30205.dll` | 是真实本机文件，非PATH命令；本次不运行，避免默认GPU初始化 |

Intel具体根目录：
`D:/YYBWorkSpace/UnrealEngine-5.4.4/Engine/Source/ThirdParty/Intel/ISPCTexComp/ISPCTextureCompressor-14d998c/`。
检查 `license.txt` 的Intel 2017 MIT授权，允许复制/修改/分发并要求保留声明；
`ispc_texcomp/ispc_texcomp.h`有BC5/BC7入口、BC7 fast/basic/slow等profile，
要求BC5输入RG8、BC7输入RGBA8，16 bytes/block。
因此可以明确说有可调查的合法CPU源码，不可说“没有encoder所以结束”。
本次没有复制UE工程代码或MSVC库进入仓库，也没有假定该`.lib`已适配MinGW。

NVTT具体目录：
`C:/Program Files/NVIDIA Corporation/RTX Remix/exts/lightspeed.trex.app.resources/deps/tools/nvtt/`。
其相邻 `PACKAGE-LICENSES/dependencies/nvtt3-LICENSE.md`列MIT文本，
但 `nvtt-exporter-standalone-LICENSE.TXT`另有SDK使用/分发条件。
实际工具目录自己的 `LICENSE.TXT`也为NVIDIA SDK协议；
`README.TXT`标明Texture Tools 3.2.4 / Exporter 2023.4.0。
不能仅据一个MIT通知把整个附带可执行文件/DLL判为可任意嵌入Game。
本次未分发、未调用这些二进制；也未验证其CPU禁用GPU参数。
其他扫描范围包括本机Tools、VulkanLearn-deps、VulkanSDK Bin和UE ThirdParty；
不是全磁盘无遗漏的软件清单。

## 硬件与GPU copy证据边界

目标GPU身份来自P0：RTX3080、驱动621.01。
不启动GPU的约束下，本次没有查询该设备format properties、
textureCompressionBC、BC image requirements或执行copy。
VulkanSDK头文件中的BC枚举/profile支持要求不是目标设备能力回执。
`vulkaninfoSDK.exe`虽然存在，本次没有启动。

已有历史 `artifacts/texture_tests_merge.log` 记载
`CompressedTextureGpu.UploadsEveryBc7MipAndLayerByteExact`通过。
读取其测试源码：实际查询BC7 UNORM transfer src/dst，
上传16/8/4 mip、两array layer并读回比较。
这支持“仓库曾有BC7 buffer↔image逐字节copy通过”的有限结论；
日志没有目标硬件/驱动身份绑定，不能替代本次RTX3080的BC5/BC7 SRGB/UNORM
完整sample/filter/copy能力验收，也没有验证解码画质。

当前RVT代码通过 `RecordCopyBufferToImageRegions` 上传，
image usage为sampled|transfer-dst，并在shader-read/transfer-dst之间转换。
136×136对应34×34个4×4块；三通道16-byte block，
每通道18,496 B，offset必须满足块大小及相关copy对齐。
buffer中必须已经是合法编码块；copy本身不把RGBA像素压成BC。
现有producer输出 `packUnorm4x8` 的RGBA三通道，
直接把这些word复制到BC image只会产生错误块数据。

合法未来GPU方案是compute写storage buffer里的BC block，
完成compute→transfer屏障后buffer→BC sampled image copy，
再做transfer→消费屏障和提交/身份/status检查。
这不要求BC image支持storage image写，但**仍需要真实GPU编码器**。
本次未发现RVT GPU BC encoder，也没有假定BC storage写支持。
CPU编码若读回整页再编码/上传，会重建方案禁止的完整页CPU往返路径，
故本次CPU试验不接入运行时。

## 小型离线CPU实测

可复现脚本：
`artifacts/terrain-rvt-production/p6/cpu_bc5_probe.py`。
结果：
`artifacts/terrain-rvt-production/p6/cpu-bc5-result.json`。
执行命令为：

```powershell
py -3 artifacts/terrain-rvt-production/p6/cpu_bc5_probe.py
```

从冻结旧Full16私有资源 `../fr/VT/2c41caf5a359c87a/manifest.json`
定位 Landscape_00，分别seek读取mip0/4/13的(x=0,y=0)一页，
每页221,952 B，总读取665,856 B；不复制`.vlcvt`。
抽取每页73,984 B法线，按当前shader BC5语义将XYZ中的XZ变成RG。
输出仅在BytesIO中产生，结果JSON没有保存整页。
每页1次warmup、20次串行CPU `Image.save`，不含GPU、输入读取、swizzle与解码时间。
误差用Pillow解码后的RG与原始XZ字节比较；
Y按shader正平方根重建，额外报告未normalize的重建误差。

| mip | BC block B | CPU save中位 ms | CPU save最大 ms | XZ最大byte误差 | XZ MAE | XZ p99最近秩 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 18,496 | 0.551000 | 0.643700 | 19 | 1.455369 | 10 |
| 4 | 18,496 | 0.642550 | 0.680400 | 27 | 2.736997 | 13 |
| 13 | 18,496 | 0.163300 | 0.174100 | 9 | 0.297064 | 9 |

DDS文件18,644 B含148 B header；不能拿容器大小当block payload。
三页XZ已分别超过P0最大1 byte门槛，**该Pillow BC5方案不符合采用要求**。
该反例足以拒绝此候选，不证明所有BC5 profile或BC7都必然失败。
CPU encode时间不等于GPU任务耗时，也不能加到P0 GPU query数字上声称满足6 ms。
BC7误差、三通道编码吞吐、额外GPU scratch、实际BC分配和最终视觉尚未测量。
未做无依据的压缩率、速度或内存改善承诺。

样本校验局限：manifest页hash为BLAKE3；初次脚本误用SHA256比对而停止，
没有把该失败判成资产损坏。修正后验证读取长度，记录样本实际SHA256；
本机Python没有blake3模块，本次未重新验证manifest的BLAKE3，
结果明确记录 `manifestBlake3NotReverified`，不伪称完整包校验。

本次保留的复核摘要：

| 文件 | bytes | SHA256 |
| --- | ---: | --- |
| cpu_bc5_probe.py | 2843 | 968d5ba9ee25e729e47c03ab1f106ed7cf6cc1fcbb4ddc725c4131979d19914d |
| cpu-bc5-result.json | 2168 | 792c20fdb0661ccf4b1d3796bbcc9da821d526aba766109273a22d541494f5d6 |
| artifacts/texture_tests_merge.log | 9407 | ac9af58f13ce53487bc99e371096bd56d77b16475cb7768e942b0cbeedafc61e |

## 决策及后续验收交接

**不采用**。主线继续RGBA8；不新增CPU readback编码、BC storage写、
双格式运行时回退或全页压缩发布。
理由是已有RGBA8池/输入实测能支持原冻结预算评估，
压缩输出不能解决主要输入和生命周期问题；
已试CPU BC5不满足冻结误差，BC7虽有合法源码候选，
却没有已接线的GPU编码与满足冻结要求的误差/吞吐/分配/copy证据。
把这一支整合成生产路径将增加P1–P5之外的实现和GPU验证，
本轮可选优化不优先于完整飞马岛与独立Game交付。

独立验收者应复核本文件数值、源码/许可证路径、脚本和结果身份，
确认“不采用”的边界没有被扩大成“硬件不支持”或“所有编码器都失败”。
若以后重新提出采用，必须补齐合法编码实现、BC5 XZ/正Y语义、
BC7 albedo/ORM格式接线、完整冻结误差、目标设备能力、实际allocation、
GPU编码+copy每页/每帧与在途峰值、独立包闭包及场景视觉验收；
不得改P0冻结门槛。执行者不担任本P6结论的独立验收者。

文档验证：`py -3 tool/validate-docs.py`，exit0，`documents layering OK`。
结束时再次读取P0冻结文件SHA256，与本记录开头一致。
