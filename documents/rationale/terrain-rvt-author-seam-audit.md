# Full16 中央交点作者输入离线审计

日期：2026-11-21。状态：**CPU 定点证据完成；旧图硬边根因未证；P3/P5 新 Game 视觉验收仍 pending。**
这是取证记录，不是运行契约，也不是作者资产修复建议。没有启动 GPU、Cook 或 Game，
没有改作者资产、生产实现、其他文档或当前 config。

## 1. 输入与复算

本次代码 checkout 是
`D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`，
资源是兄弟 `VukanLearnResources`。先读了
[production plan §8](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)、
[原 Full16 审计 §10](terrain-full-island-cutover-audit.md)，并实际查看旧截图
`artifacts/terrain-composited-vt/full16-standard/reader-final/runtime/cross_tile_corner.png`。

读取三个现存 namespace 的 scene、中央 05/06/09/10 的 TR、基础/detail MI、PNG、完整 VLP1、
源数组 descriptor/源图；两套 V2 还读取显式 world author adjacency、Paint 收据和 capture plan：

- `Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default`
- `Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas`
- `Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_LocalLayerArray`

全部 16 TR 的层定义按 scene 次序重建 registry 身份；读取 16 块的基础/detail MI 参数。
数值边界取证只限中央四块，不推广成全岛 24 边/9 交点验收。

新增脚本：[rvt_author_seam_audit.py](../../tool/validation/terrain/rvt_author_seam_audit.py)。
用带 numpy/Pillow 的 Python 执行 `python -B tool/validation/terrain/rvt_author_seam_audit.py`，
缺省资源路径即上述兄弟资源目录。桌面 bundled Python 已实际执行，输出仅在
`artifacts/terrain-rvt-production/seam-author-audit/`：

- `audit.json`：输入 SHA256/大小、完整定点、TR/MI 差分、拓扑、层映射与每边最坏点。
- `edge-{family}-{first}-{second}.json`：每边 2049 个配对点，双方 UV/高度/frame/七层权重、
  源 albedo/height 的 mip0 CPU 定点及 ideal/mesh coverage UV。
- `paired-weight-deltas.png`：四边配对权重差值曲线；不是 Game 图。
- `legacy-points.png`：旧图上标出复算像素；标注不是 GPU world-position 回读。
- `run.log`、`verification.json`：执行与独立复核收据。

计算使用 float64，保留采样公式，不称 native float32/GPU 逐 bit 等价。
脚本内测了双线性仿射场、repeat 半 texel、top4 相等权重排序及 round-half-up。
PNG 16-bit height 走现有 CRC/scanline parser，VLP1 校验完整 layer-major float32 + opacity8。
独立复核 11 项全通过：包括旧 513 点边界最大值零误差重现、旧屏幕宏交点重现、
用独立 struct/scalar 插值重算 Local 的 tile09 corner、8 份新旧副本字节相同；
245 项输入/源码摘要的运行末尾复核没有变化。`py -3 tool/validate-docs.py` 通过。

## 2. 真实放置与版本差异

四块尺寸都是 `100800 × 100800 cm`，grid=512、heightmapMargin=1，
高度 PNG 为 515²；中央共享顶点高度 `21779.764050625083 cm`。

| placement | 中心 X/Z cm | pageOrigin | 中央 corner surfaceUV |
| --- | --- | --- | --- |
| 05 | −50400 / −50400 | 8 / 8 | 1 / 1 |
| 06 | 50400 / −50400 | 16 / 8 | 0 / 1 |
| 09 | −50400 / 50400 | 8 / 16 | 1 / 0 |
| 10 | 50400 / 50400 | 16 / 16 | 0 / 0 |

两套 V2 与旧 namespace 的四块 height/Paint **文件字节相同**；TRS、size、grid、margin、
LOD 配置与 layers 相同。独立核对原 §10 收据绑定的 12 个旧 TR/height/Paint hash，
仍全部一致。因此不能把新旧差别解释成地块旋转、倒置、缩放或作者高度被改。

旧版每块选择不同 `VT/<id>/manifest.json`，另有 residentPages/heightBudgetBytes；
两套 V2 每个 World 改选一个各自的 `VT/worldV2/<namespace>/manifest.json`，并显式给出
`world.author.json`。V2 作者图有全岛 48 条有向内部连接；本次四条中央边均有往返连接：
maxU→minU 是 `(u−1,v)`，maxV→minV 是 `(u,v−1)`，反向分别加 1，区间均为 [0,1]。
这是真实作者拓扑，而非凭截图名称/坐标自动推断邻接。

本次快照中**两套 V2 指向的 manifest 均不存在**。已生成 scene/TR/MI/作者图不等于
已生成 production input、根页或可运行新 Game 包，更不能据此给阶段 PASS。

## 3. 高度与 frame：连续证据

四条边按同世界点严格配对，X 边 `(1,t) ↔ (0,t)`，Z 边 `(t,1) ↔ (t,0)`；
`t=i/2048`。双方 world X/Z、高度、frame normal 最大分量差均为 **0**。
frame 使用当前 `terrainCompositedService.cpp::Frames` 的中心梯度：读取高度 margin 两侧，
分母为 `2 × 100800/512 = 393.75 cm`；tangent 都是 `[1,0,0,1]`。
双归一化 acos 的约 0.0000017° 是浮点读角噪声，不是可见法线裂缝。

这覆盖 513 个共有宏顶点及其间 CPU frame 双线性口径。它证明这四边的作者宏高度/当前
LOD0 frame 构造一致；不证明正常粗 LOD 过渡、TES、动态位移或 Game fragment 插值一致。

## 4. 权重必须区分三种 UV 口径

实际 Paint 是 512²×7、imported weight array 是 515²×7、opacity 全部 255，无 `.meta.json`。
**不能虚构 featherTexels 或直接把 raw 边框当成同世界点的有效作者边。**

1. 原宏网格/coverage builder 的 Paint UV：
   `paintUV = (1 + 512 × surfaceUV)/514`，
   然后按 `paintUV × (Paint.width−1)` 双线性读 Paint。
2. 当前 Local overlay helper 在 imported 数组像素中心反推 surfaceUV：
   `((pixel+0.5)/W × (W−1)−1)/(W−3)`，直接调用 `Paint.Blend(surfaceUV, base)`。
   shader/reference 再以 `(1+surfaceUV×(W−3))/(W−1)` 的 normalized UV 线性读 overlay。
   该复算完整包含两次插值，**没有误把 Paint 当 textureLod 或仅比较 raw 外圈**。
   本例 W=515，因此 reader weightUV 数值恰与口径 1 的 marginUV 相同；
   不同的是 overlay 生成时传给 Paint.Blend 的坐标，而非 reader 的 UV 字面公式。
3. coverage CPU 先在 1024²覆盖 texel 中心按口径 1 取作者权重，top4、round255；
   再合并 ID 后线性采样/选 top4/归一化。这里单块 clamp 是诊断口径，非真实 World 边界。

| 配对边 | 高度/frame 差 | 网格 margin Paint 最大差 | 当前 Local overlay 最大差 | 单块 clamp coverage 最大差 |
| --- | --- | ---: | ---: | ---: |
| 05→06，X | 0 / 0 | 0.009176362 | 0.663667792 | 0.227450980 |
| 09→10，X | 0 / 0 | 0.010608586 | 0.726030547 | 0.253370098 |
| 05→09，Z | 0 / 0 | 0.007210601 | 0.630462728 | 0.227450980 |
| 06→10，Z | 0 / 0 | 0.010275165 | 0.700661048 | 0.241516134 |

原审计仅 513 点；本次 2049 点在后两条边找到了更大的 margin Paint 差，
不是输入漂移。这些是采样集合上的最大值，不冒充任意连续位置的解析上界。
四条作者 Paint 在有效 margin UV 下**都不是完全等权连续**，但最大差约 0.0072–0.0106；
不能把下游 Local 0.63–0.73 的差全部归咎于该约 1% 作者不一致。
两套 V2 及旧字节在**当前同一 CPU helper**下得到相同结果，这不是旧 Game 使用 Local 的证明。

四块中央 corner 的 `[layer0, layer3]`：

| 块 | 网格 margin Paint | 当前 Local overlay |
| --- | --- | --- |
| 05 | [0.720031165, 0.279968835] | [0.664145699, 0.335854301] |
| 06 | [0.722644800, 0.277355201] | [0.574463018, 0.425536976] |
| 09 | [0.717214492, 0.282785537] | [0.804533109, 0.195466901] |
| 10 | [0.719869727, 0.280130274] | [0.645646567, 0.354353444] |

Local 最坏点可供 P3/P5 定点复测（全部 surfaceUV/七层向量在 JSON）：
05/06 的 t=0.0322265625；09/10 的 t=0.75537109375；
05/09 的 t=0.4140625；06/10 的 t=0.529296875。
例如 09/10 最坏点 layer0 是 **0.219829217 / 0.945859765**，不是 top4 排序后才产生。

### coverage 地址不能因上述作者差异而豁免

ideal domain→World coverage UV 在四边配对差为 0；中央 corner 都为 `[0.5,0.5]`。
内部边界按全局覆盖图应合并双方 half-texel taps，再选 top4；
脚本给出这种共享 tap 的 CPU 参考，不把单块 clamp 的差叫作 Game 寻址缝。
全边两个端点还涉及中央四块以外输入，所以共享 half-tap 数组明确仅覆盖内部 t∈(0,1)。

另实读当前 `terrainMeshBuilder.cpp`：mesh texCoord1 的 max 行/列仍向内缩
`0.5/(128×32)=0.0001220703125` 的全局 UV，即半个 coverage texel。
相同边界世界点的 **mesh texCoord1 并不与 ideal domain UV 完全等价**，
双方最大 UV 差即此值。报告分开记录 ideal 和插值 mesh UV；
production bake 另用 domain→globalUV，不能拿 mesh 诊断替代 producer 证据。
哪条消费者在新 Game 最终生效、是否跨邻块/角点/粗 mip 正确取样，仍须新运行证明。

## 5. 层身份、catalog、源纹理与静态材质

16 块、三套 namespace 的七层 key/定义和去重顺序一致：
Gravel Ground、Rock Face、River Water、Scree Debris、River Silt、Sea、Beach Sand。
按 registry 和 `terrainSourceSets.cpp` 的局部 slice 规则得到
`local→global = global→sourceSlice = [0,1,2,3,4,5,6]`，sourceSet=0。
同 key 没有参数冲突；四块源 albedo/normal/PBR/height/weight 数组 descriptor 及源路径相同。
旧实际 Cook dependencies 的 worldLayerMapping 亦是该恒等顺序。
**未发现作者 layer/catalog/sourceSet/slice 串层，但没有读取不存在的新 GPU source map。**
`terrainLayerSourceMap` 作者 MI 是 placeholder；JSON 中的 sourceLayerMapping 是明确标注的
CPU 推导，而非伪造 production 纹理或已生成包收据。

基础/detail MI 对每条配对边的 effective 参数一致。
CoverageAtlas 与旧版无静态参数差；LocalLayerArray 只将 `u_terrainWeightMapMode` 从 0 改为 1。
重要有效值：u_uvScale=4、u_heightBlendStrength=3、u_detailUvScale=12，
global origin=[−201600,−201600] cm、global size=403200 cm；其他值完整存于 JSON。
不能只看 MI 显式列出 u_uvScale 就认定 height blend=0，必须读 M schema 默认值。
共用 tile00 imported weights 也不能解释本次差异：四块 opacity 全 1，helper 的 base 被完全覆盖。

**确有另一个作者 UV 不连续项：** layer1 Rock Face 的 uvScale=0.875，乘 u_uvScale=4 是
3.5 repeats/tile；u/v 重置过边时相位差 0.5。其余六层乘积为 4、4、5、6、2、2，均为整数，
在当前零旋转 + repeat 源采样口径下，跨边相位匹配。
脚本实际解码源图并按 reference 做 mip0 线性定点：X 边 Rock Face 源 albedo 最大通道差约
0.282102，源高度最大差约 0.351919；Z 边也有非零差。
Z 边源 albedo 最大通道差约 0.344424、源高度最大差约 0.266644。
这些是**未加权的源层**证据，不是最终 height-blended/global/detail/lighted 像素。
指定旧图硬边三点没有 layer1 权重，不能借这个真实 UV 相位差给旧图定根因。

## 6. 旧图具体像素：症状与交线分离

用旧 capture plan 的完整相机精度、HFOV65、1280×720、像素中心投射，
按固定 LOD0 三角宏面求 first hit；没有假定 TES 位移为零后的结果就是 GPU fragment world 值。

| 旧图像素 | tile | macro X/Z cm | 原 margin Paint | 当前 Local 的 layer0 / layer3 |
| --- | --- | --- | --- | --- |
| 499,549 | 09 | −664.305 / 1152.262 | 纯 layer3 | 0.101486 / 0.898514 |
| 500,550 | 09 | −658.952 / 1158.021 | 纯 layer3 | 0.099231 / 0.900769 |
| 501,551 | 09 | −653.609 / 1163.769 | 纯 layer3 | 0.096816 / 0.903184 |
| 639,550 | 09 | −2.423 / 1059.107 | 纯 layer3 | 0.049763 / 0.950237 |
| 640,550 | 10 | 2.423 / 1058.513 | 纯 layer3 | 0.225535 / 0.774465 |

旧图 x500/y550 在 tile09 内部，距 X=0 约 659 cm、距 Z=0 约 1158 cm，
不能因文件名叫 cross_tile_corner 把这里称为 tile 交线。
旧实际 coverage 包在前三点仍是 texel(1017,11)、page(7,0)、mapping(7,0,0,255)，
ID=(3,0,0,0)、weight=(255,0,0,0)，surfaceId=terrain|Landscape_09；
真正左右跨 X 的两点分别为 09(1023,10) / 10(0,10)，也都是主 layer3。
这支持原审计的有界结论，不证明新 RVT 任意 mip/fragment 的寻址正确。

## 7. P3/P5 下一步不可删减的视觉证据

旧图硬边是否来自作者输入不连续：**目前不能归因。**
已证明有效作者 Paint 小幅不一致、Rock 源 UV 相位不一致，以及当前 Local 权重 UV 解释
与 margin coverage 口径不同且导致显著配对差；尚未建立它们到旧图最终像素的因果链。
旧截图既不作为新 RVT 失败证据，也不作为通过证据。

新 Game 应绑定最终 scene/TR/MI/production/source map/shader/二进制身份，对双模式及双 worker：

- 复拍中央四块 corner、四条真实边与 Local 最坏 t，正常 LOD/TES，不用 fixed-LOD CPU图代替。
- 同相机复拍上述内部旧症状点，分清 tile 内部问题和 X/Z 边界问题；
  同时回读 surfaceId、world/UV/frame、有效 mip、coverage/Local ID 权重及实际源 slice。
- border、四角二跳、粗 mip 跨界 footprint 另验；将 CPU margin 作者差和 Local helper 差列为
  诊断基线，**不能提前豁免任何寻址缝**，也不能只查 mip0 clamp。
- 新 V2 production 输入/页表/根页与候选/active/retiring身份、readiness/quality 成立后，
  再做有最终 Game 图的 P3/P5 判定。本侧任务不修改资产以掩盖差异，不预先关闭任何门禁。

## 8. Local Paint UV 整改的独立复核附录

2026-11-21 10:53（Asia/Shanghai）。结论：**坐标缺失的 CPU 整改确认；515 freeze→linear
重采样限制保留；作者输入未改；整 P3/P5、视觉、TES 与寻址门禁不据此 PASS。**
本审计者没有实现整改，也没有改实现、测试、作者资产或 GPU 路径；本轮仅追加此附录和独立收据。
上文 §4/§6 的 Local 数值属于整改前源码快照，原始 JSON/曲线/Python 脚本均保留，不覆盖历史失败。
该旧 Python 脚本仍是缺失 Paint UV transform 的历史模型，不应重跑后冒充整改版模型。

### 8.1 独立构建、实 C++ 执行与输入身份

读了整改方的 [CPU checkpoint README](../../tool/validation/terrain/weight-overlay-cpu/README.md)、
`audit.cpp`、CMakeLists、20 项测试、实际 WeightOverlay/Heightmap/Paint 源码、
当前 GLSL Local reader 和 `weight-overlay-cpu/{before,after,final-native,source-hashes}.json`、
`regression-tests.xml`。没有仅采信整改方的“边差变小”结论。

使用新的独立目录 **`artifacts/terrain-rvt-production/seam-author-audit/b`** 配置 Release
MinGW module，重新编译 `overlay_cpu`、`weight_overlay_tests`、`weight_overlay_audit`。
库链接实际生产 C++ overlay、Paint Blend/文件 reader、heightmap transform 与 HostImage，
不是 Python overlay 替身。没有使用整改方已有 build、Euclid 的 `build-vt-release`，
没有配置/构建根 runtime，没有启动 GPU。
首选长目录遇到 MinGW `.obj.d` 路径过长，保留失败日志；仅缩短输出目录后全新构建成功，
没有为避错修改源码、工具链或 CMake。

实际复跑：

```powershell
cmake -S tool/validation/terrain/weight-overlay-cpu `
  -B artifacts/terrain-rvt-production/seam-author-audit/b `
  -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build artifacts/terrain-rvt-production/seam-author-audit/b `
  --target weight_overlay_tests weight_overlay_audit -j 4
artifacts/terrain-rvt-production/seam-author-audit/b/weight_overlay_tests.exe `
  --gtest_output=xml:artifacts/terrain-rvt-production/seam-author-audit/localuv-remediation-independent/regression-tests.xml
artifacts/terrain-rvt-production/seam-author-audit/b/weight_overlay_audit.exe `
  ../VukanLearnResources `
  artifacts/terrain-rvt-production/seam-author-audit/localuv-remediation-independent/native.json
```

**20/20 tests PASS、0 failures、0 disabled**，测试名与整改方最终 XML 一致。
9 层 raw/nonsequential mapping、无归一化/不压平 top4、opacity hole 原始 base 恢复、
不可变 revision 与 undo/redo、filtered raw 后 top4/正权重 local tie 次序、
非法 UV/缺层及不同 imported/heightmap 尺寸均有实际执行。
这些测试覆盖 CPU fixture，不宣称测试执行了 shader、完整 GT/RT 编辑链或 GPU 归一化。

重跑前后 251 项输入/源码摘要无变化；与原作者审计绑定的 **230 项资源收据全部相同**。
中央四块旧/新 height/Paint、TR/MI 和引用源图没有因整改被抹平或替换。
当前 overlay cpp SHA256：
`8f2659bb632425235844ce5088cc6a720611fdd1c27acc0fd9c39e4fd0d42708`，
与整改方最终身份一致。独立库/两个 exe、源码/测试/reader 和数值收据 hash
均绑定在 `localuv-remediation-independent/identity-bindings.json`；不以他人 exe 代替本轮执行。

### 8.2 确认修的是坐标，而非邻边平滑

实际代码仍先用 imported array 的 W=515 反推 surfaceUV，
再应用 **`MeshUvTransformFor(plan.desc, *plan.heightmap)`** 的
`offset=margin/(heightWidth−1)`、`scale=grid/(heightWidth−1)`，
最后将 normalized Paint UV 传给 `Paint.Blend`。
没有把 transform 误写成依赖 Paint 分辨率或 imported 数组尺寸的另一套 margin，
也没有按边邻居修改权重。

本例 grid=512、heightmapWidth=515、margin=1。对任意冻结 texel p：

```text
surface(p) = (((p+0.5)/515)*514 - 1)/512
paintUV(p) = (1 + 512*surface(p))/514 = (p+0.5)/515
```

审计另用这个**独立化简后的 texel-center 公式**读取真实 512² Paint，再按保留的 normalized
Local reader 线性读取冻结的 515² overlay；没有调用整改 C++ 函数作为数学期望值。
515 个坐标的化简残差最大 `5.55e−17`；16 个 native corner/内部定点加 8 个 native 最坏边点，
全七层向量与独立 float64 参考的最大差：frozen `9.404065e−6`、direct Paint `4.668582e−6`。
跨 float32/float64 检查使用 `1e−4` 数值容差，**不是提高视觉允许差或放宽门禁**。
20 项 native tests 中的仿射 Paint texel 值 0.1/0.3/0.5/0.7/0.9、异尺寸 import=9/height=5
检查也验证 normalized mesh transform，而不只是对称/变小的 seam 指标。

### 8.3 实际定点结果与重采样限制

每边仍取同世界位置 2049 点（含端点）。本轮独立 native 的四条 edge 和 16 个内部/corner
点 JSON **与整改方 final-native 数值完全相同**；整改前四边 float64 差也用独立公式精确重现
原作者 audit。下表 before-native 是只读整改前收据，本轮没有重建或运行旧实现：

| 边 | before native overlay 差 | 本轮 native margin author 差 | 本轮 corrected overlay 差 | frozen 对 direct Paint 最大差 |
| --- | ---: | ---: | ---: | ---: |
| 05/06 | 0.663672209 | 0.009178847 | 0.004123807 | 0.250111997 |
| 09/10 | 0.726037383 | 0.010613918 | 0.005338788 | 0.253848433 |
| 05/09 | 0.630467474 | 0.007211149 | 0.005118936 | 0.333791435 |
| 06/10 | 0.700665057 | 0.010280907 | 0.005032092 | 0.274788857 |

独立化简公式给出 corrected 四边差
`0.004122604 / 0.005335258 / 0.005110334 / 0.005025714`；
其与 native 的约 `1.2e−6..8.6e−6` 差是本轮明确量出的 float32/float64 计算差异，
不隐去或改写成 bit-equivalent。
corrected 最坏 t 为
`0.0185546875 / 0.7607421875 / 0.12744140625 / 0.0341796875`，
旧最坏点不再自动代表整改后的最坏位置。

corner layer0 的 direct/frozen 仍分别为：
05 `0.720031738 / 0.681448400`，06 `0.722645938 / 0.684262097`，
09 `0.717213929 / 0.678427458`，10 `0.719869733 / 0.681282640`。
tile09 内部 surfaceUV `(0.371,0.629)` 的 layer0 是
`0.394163162 / 0.394343883`。不是强行让 frozen 等于 direct 才给修正通过。

**resample limitation is not proven equivalence：** 正确坐标下仍先将非仿射 512² Paint 采到
515²冻结数组，再线性读取；这不等价于在任意 query 点直接读 512² Paint。
上述 0.25–0.33 是两种参考之间的差，不是对“消费同一 frozen raw overlay 的 GPU reader”
的误差，更不是被认可的 Game 视觉误差。native 非仿射 fixture 仍实测 direct=0.875、
frozen linear=0.75，明确保留该反例。边差小于作者差也不证明作者被修复或画面无缝。

### 8.4 收据与仍未关闭的验收

新增收据只放在 `artifacts/terrain-rvt-production/seam-author-audit/localuv-remediation-independent/`：
`configure-short.log`、`build-short.log`、`regression-tests.log/xml`、`native.log/json`、
`formula-check.json`、`inputs-before.json`、`identity-bindings.json`；长路径失败日志另保留。
独立 formula-check 8 项全通过。原 12 条边数组和旧图取证未覆盖或替换。

本轮仅关闭“Local overlay 生成缺少 normalized mesh→Paint 坐标变换”的有界 CPU 整改复核。
真实作者 margin Paint 小幅不一致、Rock Face 的 UV 相位差、515重采样后的单层值变化仍另报。
原旧图硬边到最终 fragment 的因果链仍未证明；不将 corrected seam 指标倒推成旧图根因证明。
新 Game 的 frozen 输入、shader consumer、local top4、mapping、正常 LOD/TES、
跨 surface border/角点/粗 mip footprint 和最终视觉仍须独立 GPU/实图证据。
**整 P3/visual 不 PASS，P5 不 PASS，任何寻址缝不预先豁免。**
