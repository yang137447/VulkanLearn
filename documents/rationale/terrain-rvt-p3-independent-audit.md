# P3 独立中审：World producer、消费端支持域与待验收证据

> 2026-11-21；独立审阅，未参与实现。结论：**发现阻断正确性的问题，P3 pending**。
> 本文是中审记录，不是当前能力契约或最终阶段通过记录。

## 范围与证据身份

指定工作树为 `D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`，
HEAD 为 `5df9fbc0af122581260d614b3b9f35d8a4d8bacd`；审阅对象包含该目录未提交修改。
已读取 AGENTS、routing、文档分层、production plan、P0Frozen 及其 review receipt、
producer-padding rationale，并核对相关材质/descriptor/测试合同。
P0Frozen 实际 SHA256 为
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`，
与 receipt 一致；P0 receipt 不代表 P3 通过。

首轮只提交本文；按用户后续要求另附独立反例脚本。没有修改实现、项目测试、资产或冻结参数，没有使用 `build-vt-release`，
没有创建 Vulkan device、启动 GPU 测试或运行窗口。
小编译输出使用系统临时目录并在命令完成后清理。
本轮 CPU 证据是独立连续域端点推导和具体 footprint 数值复算；
Cicero 的 CPU 18 pass 是输入背景，本轮没有重新运行该 GTest 集合。

## F1 [P1] Repeat 的合法锚定 tap 会读取 canonical padding

定位：

- `shader/glsl/terrain/terrainCompositedBakeWorld.comp`：`main` 无条件把评价域缩为 `valid+8`。
- `shader/glsl/terrain/terrainCompositedBakeWorldPadding.comp`：把域外像素复制为 `min(pixel, evaluationExtent-1)`。
- `shader/glsl/materialFunction/mf_terrainCompositedVT.glsl:68`：Repeat 的锚定坐标由 centerTexel 加未包装的短程 delta 得到。
- `shader/glsl/materialFunction/terrainCompositedFootprint.glsl:32`：分数 targetMip 和 8 tap 允许低 mip 上超过 4 texel 的偏移。

独立反例直接使用现有消费函数的公式，不调用修改后的 CPU Page oracle：

| 参数 | 值 |
| --- | --- |
| 合法孤立 Repeat surface 的虚拟尺寸 | 129×129，检查 mip0 |
| centerUv | `(128.75/129, 64.5/129)` |
| uvDx / uvDy | `(12/129,0)` / `(0,1/129)` |
| major / minor / filteredMinor | 12 / 1 / 1.5 |
| targetMip / tapCount | `log2(1.5)=0.5849625007211562` / 8 |
| 第 7 tap 的横向偏移 | `12*((7.5/8)-0.5)=5.25` texel |
| centerPage.x / valid.x | 1 / 1 |
| 该 tap 的 local.x | `128.75+5.25-128=6` |
| 硬件双线性横坐标 | `local+4-0.5=9.5` |
| 非零权重 stored texel | 9 和 10，各 0.5 |
| 新 producer 的真实横向评价范围 | 0…8 |

该 tap 的 deltaUv 约为 0.04070，`round(deltaUv)=0`；local=6 满足
`[-3.5,131.5]`，因此已有中心页 Resident 时走锚定成功分支，不走普通页寻址。
纵向选择内区，不影响这个反例。低 mip 的虚拟 mip 混合权重为
`1-fract(targetMip)=0.4150374992788438`，不是零。
只需令两个目标虚拟 mip 均有有效页，就不会被 result.valid 丢弃。

stored 9/10 都被复制成 stored 8，然而 Repeat 的原始评价位置分别为
`fract(133.5/129)`、`fract(134.5/129)`，stored 8 为 `fract(132.5/129)`。
这些位置不同；合法变化的材质输入可使它们不同。故这里是可达材质数据被替换，
不是只改变不可见存储字节。本文没有把解析反例冒充 GPU 字节误差测量。

`ValidateAddressMode` 明确允许孤立 Repeat，`Validate` 允许 129 尺寸，
`RecordPage` 的孤立路径也转换进 World baker。不能以“连接世界拒绝 Repeat”
排除这个受支持输入。本例不声称 Full16 Clamp 相机已出现该伪影。

现有 `ConsumerBilinearSupportNeverReachesCanonicalPadding`
（`tool/terrain-tests/terrainCompositedAdjacencyTests.cpp:171`）
始终计算 Clamp 的 globalTexel，未运行 Repeat 分支、真实 footprint 或分数 mip 混合。
`composited_page_reference.h` 的 Page 又同样按 activeWidth/activeHeight 复制 padding；
仅与它逐字节对拍无法发现本问题。

需要实现者闭合 Repeat 的支持域或地址行为，并加入从消费端生成真实 footprint、
检查所有非零双线性 tap 的回归；参考值必须在未裁剪的 Repeat 评价位置取得。
这里只报告，不代改。

### 可直接运行的历史反例

用户要求保留的独立脚本：
[exact_repeat_counterexample.py](terrain-rvt-p3-audit-artifacts/exact_repeat_counterexample.py)。
从指定工作树根目录执行：

```powershell
py -3 documents/rationale/terrain-rvt-p3-audit-artifacts/exact_repeat_counterexample.py
```

仅使用 Python 标准库，不加载当前 producer 或 CPU Page oracle，不启 GPU。
输入 UV 与地址计算用 Fraction 保留有理数，targetMip 及最终混合权重使用浮点。
输出 JSON 含全部 8 个 tap 的 low/high mip 地址、非零双线性支持、
每个支持的精确权重与乘上虚拟 mip 混合和 1/8 平均后的最终权重。
第 7 tap 的 mip0 支持为 `(9,68)` 和 `(10,68)`，
各自最终权重约为 `0.02593984370492774`，合计约 `0.05187968740985548`。
脚本断言这些支持位于历史 `valid+8` 评价域外，并给出被复制位置和原评价位置的 UV。

脚本是保留首轮问题的可执行数值证据，成功状态明确为
`historical-counterexample-reproduced`；不会随实现整改自动变为修复回归 PASS。
整改后的消费/生产真实组合仍由后续独立复验验证，P3 继续 pending。
附录脚本已实际执行：全部断言成立，totalConsumerWeight=1，
两处 witness 最终权重均为 `0.02593984370492774`。
本次补充后重跑 `py -3 tool/validate-docs.py` 返回 0（documents layering OK）；
未修改 Pauli 负责的缺链文档。

## Clamp 的独立连续域证明及其边界

以下证明只针对 Clamp。它不依赖 producer/CPU oracle 同构，也不只取有限 UV 网格。
每一轴分别考虑，设当前 mip extent 为 E，锚定页为 p，
`valid=min(128,E-128p)`，真实生产存储域为 `[0,valid+8)`。

消费端先把每个 tap 的全局 texel 限制为 `[0.5,E-0.5]`。
锚定读取还要求 `local=texel-128p` 位于 `[-3.5,131.5]`。
硬件线性采样位置为 `s=local+3.5`，非零支持为 floor(s)，以及仅当
s 非整数时的 ceil(s)。两轴笛卡尔积就是全部非零双线性支持。

完整页 valid=128 时 `0<=s<=135`，支持恰好落在 0…135；
右端 s=135 时没有权重非零的 texel136。
不足整页的最后一页满足 `local<=valid-0.5`，所以
`s<=valid+3`，其最大支持至多 valid+3，严格小于 valid+8。
下界仍是 s>=0。锚定失败的普通寻址按 tap 重新选页：
`local=texel-floor(texel/128)*128`，同样满足上述支持包含关系。

这个论证逐 mip 成立，所以覆盖 low/high mip、缺页祖先回退、任意 anisotropic tap，
并覆盖不规则/矩形尺寸；二维无需额外假定。独立 Python 端点复算还核对了
E=1、2、7、127、128、129、259、8192 的所有页，全部满足该不等式。
1×1 根页保留的 9×9 域包含消费者支持；其真实 border 不能据此全部删除。
是否正确评价保留 border 的邻块内容是另一项数值验收，不能由地址域证明推出。

Repeat 没有全局 `[0.5,E-0.5]` 上界，以上最后一页证明不适用；
padding rationale 中“孤立 Repeat 保持原 evaluator 路径”不足以覆盖 World 新裁剪。

## F2 [P2] 176 sampler 上限检查目前没有接入生产调用

`TerrainCompositedBakeGpu::ValidateDeviceLimits`
（`source/render/resource/terrainCompositedBakeGpu.cpp:473`）
检查 176 combined samplers、176 sampled images、179 stage resources、
3 storage buffers 和 35872-byte 参数范围。
全 `source` 搜索只有这个定义与头文件声明，没有调用者。
构造函数先直接创建 World pipeline、descriptor pool 和 descriptor sets。

所以不能把“已有 ValidateDeviceLimits”解释为实际设备准入检查已生效。
固定 16 槽在只有一个 surface 时也仍声明并填满 176 descriptors；
设备 sampler 限制低于 176 时，这条路径无法通过合法布局创建。
这是源码可证的资源能力检查缺口，不是 RTX 3080 实测超限结论。
缺少实际调用前的拒绝测试与目标设备 limits 对照；
构造失败捕获不等于创建前检查 Vulkan 限制。
本轮未发现足够证据宣称当前目标设备的冻结预算已被该实现突破。

## 已核对的静态与小编译事实

实际执行两条 CPU 编译链：

1. SDK `glslangValidator -V --target-env vulkan1.2` 编译两个 active World shader，
   `spirv-val --target-env vulkan1.2` 均成功；反汇编检查成员布局。
2. 在临时目录运行现有 `check_composited_world_spirv.py`：
   glslc `-O --target-env=vulkan1.4` 及额外 `-g` 两种配置，
   World/WorldPadding 四个产物均经 spirv-val。
   脚本报告的 passed 只限编译、ABI 和指令检查，不是 P3 PASS。

World 的三个编译产物都只含 Shader/ImageQuery capabilities。
11 个 sampler binding 各固定 16 项，176 条 sampler descriptor AccessChain
索引均为常量，未发现 NonUniform 标记/能力或 opaque image 的 Phi/Select。
这证明本次产物的 literal switch 避开动态 sampler 索引；
不证明 GPU 吞吐、驱动执行或 Vulkan lifetime 正确。

独立读取 glslang 反汇编的 std430 装饰：

| 结构 | 实际布局 |
| --- | --- |
| World 前缀 | page=0，control=16，surfaces=32 |
| Surface 前八字段 | 0、16、32、48、64、80、96、112 |
| normalToSource | offset128，column-major，MatrixStride16 |
| globalToSlice / localToGlobal | offset192 / 1216，ArrayStride4 |
| Surface[] | stride2240，长度16 |
| World 参数总长 | 35872 |
| Chart | offsets0/16/32/48，stride64 |

与 C++ 的字段顺序、array 容量及 sizeof static_assert 一致。
padding shader 只消费 offset0 的 page 前缀，与同一参数 buffer 相容。
当前实质结构是一次 World 评价 dispatch，加不足整页时一次 padding dispatch；
不是逐 surface 分 dispatch，也不是所有页面总共只有一次 dispatch。
二者之间有 compute-write→compute-read/write 屏障，输出最后转 transfer-read。

SourceLod 使用各通道自身实际 extent 与经过 chart Jacobian、surface UV scale、
layer UV rotation/scale 的子样本 footprint；不是拿输出 mip 直接当源 mip。
两种权重路径保留 CoverageBilinear 身份合并，以及 LocalLayerArray 的
local→global→slice 映射。非零 height blend 继续用 height mip3。
新 evaluator 与保留单 surface evaluator 的四个函数体经脚本规范化比较一致。
这些是公式/重构核对，不是对源 evaluator 本身的独立数值正确性认证。

局部法线转换为 `normalize(S_source^T*S_target/scale_source²*n)`；
在验证器允许的正 determinant、均匀缩放且无 shear 条件下，
归一化后等价于把 target inverse-transpose 法线变到 source 局部方向。
平移不参与，跨样本平均前先换到 source 基；本轮未证出这个公式的问题。
各输出像素仍作 N×N 子样本面积积分、线性 albedo 平均后 sRGB 编码、
归一化法线均值、roughness RMS。未 clamp 原始 query 后再做邻接；
只有得到目标 chart 后 clamp domainUv，因此保留 border 仍会读取邻居。
邻接路由在不规则图/浮点边界的 GPU 等价性仍待测。

生成规则身份已升级到 generator3 / adjacency-padding-v3，
`terrainCompositedIdentity.h`、生产闭包校验和 ContentHash 已有相关接线。
本轮没有证出旧身份被直接当成新页使用；真实部署缓存淘汰仍需运行证据。

## 缺测试与阶段结论

- F1 的 Repeat 末页真实消费回归，以及 Clamp/Repeat 两类全部 tap 支持检查。
- 当前单 World dispatch 与 padding 的实际 GPU 字节比较；保留 border 独立按原始 UV、
  真实邻接与源过滤计算。覆盖两种权重、非零 height blend、粗 mip/1×1 根、
  四角、不等 Jacobian、旋转 placement、帧基和通道不同源尺寸；
  不能只比较同步修改过的 CPU canonical-padding oracle。
- 当前 shader 的 production/copy GPU timestamp、冻结 4ms/4ms/6ms 门槛与每帧预算。
  旧分 dispatch 性能 failed 保留为失败证据，不能转移成新实现通过。
- Full16 删除全部可选细页缓存后的真实 Game 缺页生产，
  新位置/瞬移 10s 收敛、实际页身份与可见画质；prepared 不算执行证据。
- 实际 memoryRequirements 全账、176 sampler 设备准入、Candidate/Active/Retiring
  重叠预算、status/fence/迟到身份拒绝、旧槽退休与失败回退。
- workerThreadCount=1/2 的实际运行及零 Vulkan validation 错误；
  当前不启 GPU，交由排窗后的执行者提供带源码身份的证据。

因此中审结论为：**F1 阻断正确性，F2 为资源能力准入缺口；
当前 P3 pending，不能批准最终通过。**

文档检查：执行 `py -3 tool/validate-docs.py`，返回 1。
三处既有文档（terrain-composited-vt 合同、testing 合同、final-evidence rationale）
引用的 `documents/reference/rendering/terrain-composited-vt-acceptance.md`
在检查时缺失；检查未报告本文问题。按只写本文的权限未修改这些文件。

## 本轮源文件 SHA256

用于区分共享工作树后续并行修改；本结论只绑定本次读取的版本：

| 文件 | SHA256 |
| --- | --- |
| mf_terrainCompositedVT.glsl | `1e49deb5df3d97d66ebd70af8c025c387dada99de5988ef2068ad563b3dff9c5` |
| terrainCompositedFootprint.glsl | `09e8e721fd116b4d0f5a969352a93fd44cea404763cbdc00b9e5423a073f047e` |
| terrainCompositedBakeWorld.comp | `69667437b8d9b36dfe448a7b5ec055e7f14b792bb93ff190c13562dfc8330832` |
| terrainCompositedBakeWorldPadding.comp | `ec5bf22a29ef27fc7f7ee010c534c94f75a6d1790dfe0303a0b21ad62f95730b` |
| terrainCompositedBakeWorldTypes.glsl | `6dd7d9e6381825daec2cf35146945b5602b2abb8b0455f6943e39fd027af3fcf` |
| terrainCompositedBakeWorldEvaluator.glsl | `d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b` |
| terrainCompositedBakeGpu.cpp | `1d7c2d344a8191a825cbc1ed2eefa025e86919f4401b06c39640ad07a0b46cb6` |

## 补充独立裁决：基础页参考语义与冻结门槛

2026-11-21，按用户要求独立核对原 plan、P0Frozen 和历史 S1 参考语义。
本节仅追加裁决，不改写前文中审事实、冻结文件或任何原始 failed report；
未修改实现，未启 GPU，也未独立复核本次对话提及的候选 GPU 测量数值。

**裁决：基础页三通道各最大误差 ≤1 byte 的比较权威，应是独立锁定、
未随候选修改的原实际 GPU 基础 evaluator。原冻结文本没有另行要求理想
double bilinear 材质参考也须 ≤1 byte。**
这是澄清主线程后加的“双参考均 ≤1”条件不属于原冻结要求，
不是放宽冻结的 1-byte 门槛，也不是把任何失败报告改判通过。

依据：

- `config/terrainRvtP0Frozen.json:203` 至其后三个 basePageMax 字段，
  分别固定 albedo/normal/ORM 最大误差为 1。
  同文件 `qualityTargets.visualComparison`（审阅时第 215 行）明确：
  “Compare baked base channels to independent current evaluator”。
  本次核对冻结文件 SHA256 仍为
  `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`。
- `documents/plan/rendering/terrain-runtime-virtual-texture-production-plan.md`
  的最终验收段（审阅时第 257 行）要求“与现有基础 evaluator 对比基础页”，
  没有建立另一个理想 double bilinear 基础材质精度合同。
- `tool/validation/terrain/S1_source_comparison.md` 的 Bounded source test
  段（审阅时第 166 行起）说明材质参考调用 GPU `EvaluateBase`，
  对比 direct GPU source evaluation，同时明确不独立证明共享材质方程。
  `tool/validation/terrain/validate_composited_bounded_source.py` 开头说明：
  “Material evaluation stays on the GPU; height additionally has an independent
  double-precision CPU bilinear sampling oracle.”
  因此 canonical height 的独立 double 验收不能转化为基础三通道的隐藏门槛；
  历史 S1 的 2-byte 材质阈值同样不能替代当前冻结的 1 byte。

执行与解释边界：

- hardwareparallel 可以继续完整 independent actual-GPU per-page comparison，
  不因 double 参考单独超 1 byte 判定其违反上述冻结基础页比较要求。
  这只是允许继续验证，不是候选通过裁决。
- 原 double 失败仍是诊断证据，保留其输入、误差、状态和原始报告；
  不能声称候选匹配理想过滤，也不能静默移除该偏差。
  若后续发现独立于理想过滤差异的身份、寻址或公式错误，仍须单独判错。
- manual candidate 若相对原 actual GPU evaluator 超过 1 byte，
  仍不满足冻结的基础页等价要求，不能因其更贴近 double 而替换验收权威。
  本裁决不推广该候选，不追认其性能或质量通过。
- actual GPU oracle 必须在比较前锁定原 evaluator 的源码 hash、传递 include、
  编译与 sampler/输入配置，并与候选产物分别记录身份。
  不能随候选改 reference、从候选输出生成 expected，或以同一候选实现自比替代独立性。
  若原单 surface evaluator 不具备新邻接能力，仍需独立解析邻接、原始 UV、
  footprint 和法线换基后调用锁定的原基础 evaluator；不能因此省略 border 证据。
- 缺失或 unknown 的 actual page comparison 一律未通过，保持 pending；
  小样本 prototype、prepared、有限计时 pass 都不代表全页比较或 P3 PASS。
  可达内区/border、Repeat 反例、两权重、粗 mip、四角及源资源配置须纳入真实证据。

**截至本裁决，P3 仍 pending。** 完整实际逐页质量、10936 页性能、
Full16 清细页缓存生产、可见质量与新位置收敛、冻结预算、validation、
两线程及真实运行闭环均不因参考语义澄清而豁免。
用户已授权 Cicero 按锁定原 actual-GPU oracle 推进 hardwareparallel；
本审阅者等待实际 GPU artifact 后再独立复验，不在此追加实现或运行。

## 有限候选复验：hardware / adaptive8 / coverage-static-top4

2026-11-21，独立读取
`artifacts/terrain-rvt-production/p3-hardware-coverage-top4-independent-full/`
的 `runs/run-001`、`runs/run-002`。本轮只追加本文，执行 Python CPU 原始字节/
计时/hash 复算和离线 spirv-val/反汇编；没有运行测试 executable、启动 GPU、
修改候选、reference、Frozen 或原始失败报告。

**有限裁决：该候选可以进入受控 production promotion 与真实范围验证；
已通过的只是本 artifact 绑定的有限数值范围及 validation-off 有限性能子项，
不是 P3 整体通过，不是 10936 页通过，也不是稳健性能保证。**
本节对前文 F1/F2 给出新增修复证据，不覆盖首轮发现的历史事实。

### 原始数据独立复算

没有只取 report 的 numeric/pressure 顶层字段。逐个枚举 candidate 下全部
29 个 `*/partial.json`：每次运行共 423 条 dispatch 计时记录，其中
70 numeric、352 pressure，另有 1 条预期失败的 status-injection。
70 页由 numeric-0…3 共16页、Repeat129两页、singleton四页、
spatial-mode0…2/group0…3 共48页构成。
pressure 为原 pressure 64条，加三 mode×三 mip×32条共288条。
因此 singleton、Repeat、spatial 和 status-injection 均纳入计时复算。

| 原始复算项 | run-001：validation on | run-002：validation off |
| --- | --- | --- |
| 正常 status=0 / 注入 status=4 | 422 / 1 | 422 / 1 |
| actual GPU 数值页 | 70 | 70 |
| 每轮比较三通道 bytes | 15,536,640 | 15,536,640 |
| candidate vs independent original GPU 最大通道误差 | `[0,1,0]` | `[0,1,0]` |
| 对 actual GPU 超1 byte数量 | 0 | 0 |
| candidate vs CPU double 最大通道误差（诊断） | `[2,1,1]` | `[2,1,1]` |
| production 最大 ms | 4.745120 | 3.998976 |
| atlas copy 最大 ms | 0.054464 | 0.811168 |
| production+copy 最大 ms | 4.779520 | 4.034592 |
| production >4ms 条数 | 2 | 0 |
| copy >4ms / combined >6ms 条数 | 0 / 0 | 0 / 0 |

逐页直接读取 candidate/original `atlas-*.bin` 的三段 73984-byte 通道，
另读尾部4-byte status；数值70页两侧 status 全0。未把 `reference-*.bin`
的 double bytes 当成 actual GPU authority。两次运行各280个原始 bin 文件的
`fileSha256` 全部重算匹配。每条 combinedMs 均等于 productionMs+atlasCopyMs。
pressure 未保存完整页 bytes且 numericCompared=false，故其352条只证明有限吞吐/
状态，不能冒充另外352页独立数值通过。

run-001 的两条 production 超限为 pressure-mode0-mip6 iteration9
4.054880ms、iteration14 4.745120ms；其 failed 性能记录仍有效。
run-002 最慢 production 和 combined 均在 pressure-mode0-mip6、
tile5、mip6、iteration29；copy 最大值在 spatial-mode2-group0 的 tile1/mip0，
并非同一页，不能把不同页 maxima 相加声称实测同页 combined。
3.998976ms 距4ms只有 **0.001024ms=1.024µs，约0.0256%**：
这是一次有限观测的达标，不能推导稳定余量、WCET或其他真实输入不会超限。
combined 4.034592ms 虽大于4，冻结 combined 门槛为6ms，不能错用单项4ms。

P0Frozen 正式条件 validation=false，因此不以 on 运行的生产超限否决
off 运行这一有限性能子项；on/off 不混合统计，也不把诊断小窗口/合成输入
称为同条件 Full16 正式 benchmark。两侧日志分别明确输出 validation layer on/off；
run-001 candidate/original 的 stdout+stderr 均无 `Validation Error:`，
stderr 均为空。它是本 harness 的零错误证据，不是 Game 集成的零错误证据。

### 候选与 oracle 独立性

已重算两次运行的 executable、私有 shader 快照及 build 所列三个 C++ 源 hash，
均与 manifest 一致。以下身份绑定本次结论：

| 对象 | SHA256 |
| --- | --- |
| candidate executable | `5e41a8724f7a4ab30232018825b8965bad2bcaa723471cbea2504c5f996f3cfc` |
| original oracle executable | `501412f82a319af3c4d106de27a078da9d2479f6271ff21748936768bf9440c4` |
| candidate World SPV | `e3352f34149306d7e843ff11e8997e945a22b526b940492d7aebd84ba4c9b855` |
| candidate 展开 GLSL | `45a360166e49e45ce5dc13d0763beb1a0ae1bee3350516abdad7f44d9f854d04` |
| independent oracle 展开 GLSL | `fd663e656145ee2566c8f549a89d0b1c184467faf31998063f4bf8f4382cceff` |
| independent oracle SPV | `1d7f420d09ba55ec0f2a980ffbc7844332a2f50779dd9256ea3d49108745438c` |
| run-001 dual-reference-report.json | `364a8235f0db9fa3a2bfdb52583bf379391ffbde232071e0a0b4c719a9f7d8e0` |
| run-002 dual-reference-report.json | `215c9175d7299cb323fb4efaec5cb5d19931446761fc57a7482fe38026682c61` |

检查 `prepare_independent_gpu_oracle.py`、保存的
`independent-oracle/independentOriginalBaker.cpp` 和展开 shader：
reference 不调用 candidate GPU 路由；binding14 每个积分子样本提供CPU double
`graph.Resolve` 的目标、domainUv、Jacobian及法线矩阵。
`composited_cpu_address_oracle.h` 按原始像素/子样本计算query，Repeat显式fract，
Clamp调用CPU图解析；法线以target cofactor inverse-transpose再乘source transpose
独立计算，没有调用 candidate 的 NormalToSource。
shader 读取这些记录后调用原 hardware textureLod evaluator。
该展开 shader 虽仍包含未使用的 GPU route 函数，其 main 的地址来自CPU记录。

预锁 evaluator 的 `d977382c…3038b` 来源为
`p3-baker-world-repeat-fix/runs/run-003/sources/shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl`。
独立比较其 FiniteValue/SourceLod/ResolveLayers/EvaluateBase 四个函数体与
oracle 展开 shader，规范化后逐一一致；未换成 manual filtering。
参考仍共享基础材质公式，证明的是保持原实际 evaluator 的等价范围，
不独立证明原材质方程物理正确，也不声称理想 double filtering 匹配。

身份留档仍有应补强处：build.sourceHashes只列三个C++源，
没有逐项记录外部 `composited_cpu_address_oracle.h` 的编译时hash。
本次检查其当前源码SHA256为
`35a2c8aa023ec1f49ee30f2900c6397fd99ae99f7207ba15eeac63f7f74f3d6f`；
这不是追溯证明编译时header未变化。已锁exe/SPV及原始bytes足以绑定本次
有限运行对象；promotion前应完整保存oracle与candidate编译闭包，
包括CPU地址header、图对象源码/对象、共享include、sampler/输入配置，
避免以后只能凭可变工作树解释reference。不得修改原run补造历史身份。

### 单改动、padding 与能力准入

把保存的 adaptive8 展开shader施加四槽替换，所得文本与本候选展开shader
**完全相同**；差异限于 Coverage 最终top4 insertion loop展开，
候选合并/原始权重累加顺序、sample数/输入、height/source采样与归约规则未随之改动。
实际重新运行纯CPU `coverage_static_top4_prototype.cpu_check()`：
4352 cases通过，包括相等权重、signed zero、稀疏打乱global ID。
检查四槽移位与原循环等价，tie为权重降序、global ID升序；
raw候选累加代码保持原顺序。4352案例是有限bitpattern验证，不是所有浮点输入穷举。

adaptive8每组64 lane，area4用8 lane/texel分两阶段；各阶段按原sample顺序串行累加，
共享暂存3072 bytes。无效像素lane仍到达barrier，不提前退出后再要求其参与同步。
候选World release/debug、Padding release/debug以及oracle两个SPV均离线spirv-val通过。
release/debug World重新做指令检查：11×16 sampler、176常量AccessChain，
仅Shader/ImageQuery capability，无NonUniform，std430 stride2240/offset32/总35872一致。

F1 的可达域根因在本候选已闭合：Repeat producer/dispatch都评价完整136²，
padding shader读control并在Repeat直接return；Clamp仍按valid+8评价并规范化域外。
本次70页比较包含singleton Clamp/Repeat与Repeat129反例，
后者原stored9/10不同于复制8，未裁剪direct evaluator误差最大1，
且与独立actual GPU全页对比通过。先前Clamp连续域证明仍适用。
这里没有实际运行完整fragment footprint消费，但原独立证明已给出9/10的非零权重；
已检查这些可达texel的数据修复，而非只靠同步修改的padding CPU oracle自洽。

F2 的遗漏调用在当前构造路径已修复：Baker缺selectedDeviceLimits即拒绝，
ValidateDeviceLimits在pipeline/descriptor/buffer创建前调用；
Service从backend.GetSelectedDeviceLimits取得limits传入。
两次harness均记录missing拒绝及8类低限拒绝，surfaceCapacity=1仍按176检查，
资源identity计数不变。harness本身查询独立instance的physical device0，
不能推广为多GPU选择正确的实测；生产Service使用实际selected-device accessor，
其Game接线仍随集成验收。

### promotion 条件与剩余风险

允许把**这个身份明确的候选**送入production接线和后续实测，
不等于允许以本报告替代promotion后的重新验证：

- 提升须同时匹配shader调度与C++ dispatch布局、shared/barrier、padding/参数ABI，
  完整冻结源码闭包和产物身份；artifact分支测过不能自动证明runtime文件已是同一实现。
- 保持原hardware evaluator/采样、两权重、sample规则和冻结门槛；
  actual GPU oracle继续独立，不推广manual，不改double/旧性能failed报告。
- 必须跑冻结真实输入范围10936页及完整逐页质量/身份检查；
  本次352条synthetic pressure和70页不能代表该realScope。
- Full16 Game删除细页缓存后的冷生产、初始/新位置/瞬移收敛、可见图像与TES、
  worker1/2、validation、真实production/copy每帧预算与全资源准入/退休账仍须完成。
  多任务batch仅证明slot隔离，不证明每帧一页的真实scheduler满足预算。
- off最慢页只有1.024µs余量；真实输入或负载变化可能再超4ms，
  不新增门槛，但也不据此提供稳健保证。真实范围一旦超限应如实失败，不调阈值。

**有限数值与off性能子项接受；受控候选promotion可继续；P3整体仍pending。**

## 真实输入10936页性能独立复验

2026-11-21，读取 `artifacts/terrain-rvt-production/p3-real10936/runs/run-001/`
的report、build、两mode原始JSONL、launch、clock CSV及source-and-leaf-charge-audit，
并按实际编译的`.ii`核对关键路径。本轮仅CPU复算与追加本文，
不启GPU、不改实现/候选/reference/Frozen，不重复此前4352与423有限审计。

**接受P0规定的单tile00、双权重、共10936页有限真实输入性能子项；
关闭前文“10936性能未完成”的这一明确范围待项。**
不是全岛所有页/混合邻块最坏输入、逐页数值质量或完整P3通过。

### 全量原始键与timestamp复算

独立从8192尺寸、内页128、floor-half-to-one构造预期
`(mip,x,y)`全集，未用report声明的page count代替预期。
每mode恰5468行/5468唯一键，与预期集合相等，无重复、缺失或越界。
mip0…13逐级数量为
`4096,1024,256,64,16,4,1,1,1,1,1,1,1,1`。
sample恰0…5467，slot恰sample%4；全部surfaceId为
`terrain|Landscape_00`，weightSource与mode一致，heightBlend=3；
mip0每轴1 sample，其余每轴4 samples。两mode全部status=0。

对10936行逐一验证timestamp为严格正duration，production结束不晚于copy开始，
用原始整数差×1ns换算ms，和记录production/copy相符；
combined逐行等于两段duration之和。

| mode | 最大production ms | 最大atlas-copy ms | 最大同页combined ms | 超4/4/6条数 |
| --- | --- | --- | --- | --- |
| CoverageAtlas | 1.246176 | 0.508160 | 1.261088 | 0 / 0 / 0 |
| LocalLayerArray | 1.946176 | 0.494016 | 1.959712 | 0 / 0 / 0 |

Coverage最慢production/combined为mip0,x6,y42,sample2694；
Local为mip3,x5,y2,sample5397。copy最大分别在Coverage mip0,x57,y59、
Local mip0,x52,y50，不把不同页max相加。
少量production与copy timestamp区间之间存在间隙，最大分别0.001504/
0.001536ms；额外按从production起到copy末的完整包络重算，
两mode最大分别1.261088/1.960416ms，亦低于6ms。
因此该统计细节不影响本子项结论，仍保留原始timestamp。

两mode均为validation-off、immediate日志，符合冻结的该性能条件。
82条clock CSV全保留，覆盖12:49:14.016…12:49:30.525，
包含P8/405MHz至P0，不排除升频前样本。
collectorExitCode=1由收集器结束时terminate路径解释；
它不使已记录82条失效，也不证明每页精确对应某时钟或时钟受控。
本次不声称锁频、WCET或稳态帧性能。

### 输入真实性、编译身份与资源scope

build绑定的CoverageAtlas006-final manifest SHA256重算匹配，
其66个真实生产对象共390,809,921 bytes逐文件SHA256重算全部匹配；
生产闭包含16个surface，经Restore与MakeBakeInput实际恢复输入，
没有换成synthetic fixture图片。
Local快照保留原66对象，新增515×515×7层R32_FLOAT、7,426,300-byte权重对象；
build绑定的本地快照/receipt/payload、四个候选SPV共7文件hash全部匹配。
Local来自真实tile00 TR、Paint及导入层权重，经当次编译的
BuildTerrainCompositedWeightOverlay执行margin1映射后冻结。
16个surface的差异限于weightSource及对应inputs；此诊断快照给所有surface
绑定tile00生成的local权重，不代表每邻块各自真实Local输入的最坏工作负载。

真实harness executable、candidate Baker object、runtime-support archive及
candidate build hash均重算匹配。World release SPV仍为前节已审
`e3352f34…c9b855`，不据此重做同一候选的有限oracle。
10个新编译closure的`.ii`与object共20个hash均匹配。
依赖记录并集914文件；现工作树的terrainRvtBudget.h/.cpp已与编译记录发生漂移，
故本节按保存`.ii`解释该run，不把当前头文件当成旧binary证据。
新Cook/currentSource必须另锁身份，本结论不自动转移到漂移后的源码。

特别核实保存的
`terrainCompositedProductionGpu.cpp.ii`（SHA256
`ae919fd0f85239d8cee35390c862a71a2a83706190d977a760e2612fe29d723f`）：
MetadataBytes为每object/source-map预计`sizeof(Use)+256`及字符串，
先TryReserve元数据、把lease交给result，再admit输入；
Leaf的实际image quote加`sizeof(Leaf)+128`等metadata也在make_shared之前
TryReserve，随后make_unique Use使用已预留metadata。
这是当次编译内容的证据，不以事后修订的头文件或注释推断。

harness在循环外创建并持续保活：输入owner/snapshot、Baker参数与descriptor、
四套output/status task、三通道512层atlas及query pool；每页一次提交并等待完成，
四task轮换不是四页并行或真实Game scheduler验收。
生产timestamp涵盖实际Baker Record，copy涵盖三通道buffer→atlas及对应屏障/
status复制；未以CPU提交时间充当GPU生产时间。

| harness账本（bytes） | Coverage | Local |
| --- | --- | --- |
| 三通道pool GPU | 226,492,416 | 226,492,416 |
| input GPU | 407,358,464 | 416,820,224 |
| workspace GPU | 5,266,720 | 5,266,720 |
| GPU总计 | 639,117,600 | 648,579,360 |
| 账本当前CPU | 17,581,893 | 17,757,466 |
| 账本峰值CPU | 85,618,397 | 85,793,970 |

分别约216MiB pool、388.49/397.51MiB input、5.023MiB workspace，
class相加与总计一致，task=4、rejected=0；按Frozen同口径子cap均未超。
pool按实际image memoryRequirements quote，参数与task buffer按实际buffer quote；
workspace含4MiB GPU/16MiB CPU显式metadata reserve。
这些是harness已接入的资源账本，不是Game Candidate/Active/Retiring总账证明，
也不是整进程CPU/private、sampler/驱动opaque峰值或WDDM实测。
输入已恢复且atlas/四task常驻的这个有限运行范围可以确认；
完整Game的feedback/page tables/geometry/退休重叠等不能从此账本外推。

### 新oracle编译闭包校准的有限接受

另读 `p3-oracle-compiled-closure-calibration/runs/run-001`。
两页identity原始atlas（各221,956 bytes含status）两侧逐字节一致、status=0；
8个原始bin hash及两侧exe hash均匹配，validation-on日志无参考错误。
这两页是原serial evaluator对独立CPU地址oracle的校准，
不是adaptive/top4新性能或完整邻接回归。

新编译记录中independentOriginalBaker闭包559文件明确包含CPU地址header
`35a2c8aa…f74f3d6f`，`.ii`及object hash匹配；
另两编译单元558/624文件的`.ii`/object也均匹配。
该新运行补足“以后reference须有完整编译闭包”的条件，
不追补旧run的历史身份。其依赖并集630文件，当前VulkanManager.h、
presentTarget.h、terrainRvtBudget.h已有漂移，仍以保存编译输入为准。
不因闭包补齐重新要求既有70页或4352/423有限测试再做一遍。

### 证据锚点与未关闭事项

| 文件 | SHA256 |
| --- | --- |
| real10936 run-001/report.json | `62af5b742cef650a96474fcfaa1e8fad63260a0be8b68d5482efc4cac3bde5f0` |
| real10936 run-001/build.json | `e4d255cc5eafc9c97f4b3995c82189cc7c0e60fed43168d03a3ab6d4e0472251` |
| mode-0/production-raw.jsonl | `76264c60e25293aff591e2c599279d15d5b249c450d0831a886a46592dcf193c` |
| mode-1/production-raw.jsonl | `763f0152d0d077ef8cbec819e6d4634d0e1027556a87f2600c41750d07daa113` |
| 新闭包calibration dual-reference-report.json | `f82313defe824e64ecde7d3695b93bbfc36699931d30aa4636346f59c2dd61d9` |

**10936单tile00双权重有限真实输入性能接受，不再标此子项未完成。**
本轮无真实输入全页通道readback比较；Full16 Game冷生产、全部邻块真实输入/
混合边界质量、可见画质/TES、收敛、steady frame、worker1/2、
故障/身份/退休、全Game内存和WDDM仍pending。实际当前源码的新Cook/包闭合
可继续，不必等待重复finite oracle；不得将本子项接受写成P3 complete。

## C008 Coverage：192页有限真实输入数值独立复验

2026-11-21，读取 `artifacts/terrain-rvt-production/p3-real-numeric-c008/runs/run-001`，
只追加本文；执行CPU原始字节/hash/坐标复算、离线shader验证，
未启动GPU或修改候选、reference、Frozen。未重做已接受的10936性能。

**接受C008 Coverage有限真实192页基础通道数值子项，
关闭此前该新增真实Coverage数值缺口。Local192、最终screen及完整Full16 Game/P3
仍pending；本节不把Coverage结果转移为Local通过。**

### 独立原始坐标与全部字节复算

预期集合独立构造为16个`terrain|Landscape_00…15`，每个surface：
mip0的x/y各取0、31、63，形成中心/四边/四角共9页；
加mip3(3,3)、mip6(0,0)、mip13(0,0)，合计12页、全World192键。
这里的mip3“center”指该有限矩阵指定的(3,3)，不另声称其为几何中心唯一页。
candidate与reference两侧均恰192行、192唯一键，与该集合完全相等，
sample为0…191、slot=sample%4；全部mode0、heightBlend3、
mip0每轴1sample/其余每轴4samples。

384个原始page bin各221,956 bytes：前三通道各73,984 bytes，
末4-byte status逐个解码均0，与各自report一致。
独立逐byte比较42,614,784个通道bytes，最大误差 **[0,0,1]**，
超过冻结1-byte数量0；仅以下三byte非零，其余全部一致：

| surface / 页 | stored XY | 通道/分量 | original GPU → candidate |
| --- | --- | --- | --- |
| Landscape_00 / mip6(0,0) | (70,72) | ORM.g roughness | 205 → 204 |
| Landscape_01 / mip6(0,0) | (102,118) | ORM.g roughness | 210 → 209 |
| Landscape_04 / mip6(0,0) | (55,81) | ORM.g roughness | 178 → 177 |

重新生成的所有非零差异与保存的`all-nonzero-byte-differences.json`完全一致。
`numeric-completeness-crossborder-audit.json`所列384个原始bin SHA256全部匹配，
没有只核summary或仅检查其例举的跨边界页。
其中Landscape_00右边mip0(63,31)、右上角(63,63)、Landscape_05左下角、
Landscape_05 mip6整域、Landscape_15 root等也包含在全集比较中。
两侧validation-on日志均无`Validation Error:`或`VUID-`，
report无error且完成192页；不能用候选完成数替代reference完成数。

### 真实输入及实际kernel身份

不以双方打印相同production hash为唯一依据：

- 独立重算actual-input inventory的manifest及67个C008 raw objects，
  共68文件SHA256全部匹配；布局是16个8192 Clamp surface，
  生产权重模式Coverage，实际输入来自C008发布包。
- 检查实际编译的numeric harness `.ii`：Open该manifest，Restore实际object，
  逐surface调用MakeBakeInput，然后实际Baker Record，保存完整三通道及status，
  没有走fixture输入或读取候选输出生成reference。
- 两侧launch均传入同一个C008 manifest和mode0，分别启动不同exe。
  两侧实际输出production BLAKE3均为
  `5a0265a470baca4eeeb7008e639636af69cade40bc42e5c4ac3967bcf184ab3f`，
  manifest BLAKE3均为
  `16c76701ab0f86244def67a82960ec9ee0be5deb6ba19b6b501ffef92103be5a`。
  本轮独立文件完整性复算用SHA256，不把打印BLAKE3本身称为独立重算。
- 发布generator声明的四个compiled artifacts均能定位到实际raw object，
  与candidate私有project中World/Padding release/debug逐一hash匹配。
  实际candidate World release为
  `fc19b407f5b4e99ab1525c297e84b2c17ec37905180b74ab8bc5652790ee478c`，
  **不是**前次10936 artifact的`e3352f34…c9b855`；本节结论绑定本次C008产物。
- generator的computeSource+includeSources共7个实际发布源码payload，
  其hash与build记录和检查时当前7个shader源码全部匹配。
  candidateBaker已保存编译输入中是adaptive8 dispatch，
  与64-lane World布局一致。

检查运行脚本按candidate-project/original-project分别设cwd；
harness通过backend创建无附加ShaderCompiler的PipelineFactory，
ComputePipeline按名字读取`terrain/terrainCompositedBakeWorld_comp.spv`。
CommonFunction先查资源根、再查project/shader/spv；
检查资源根同名World runtime/debug和Padding runtime均不存在覆盖文件。
因此私有project产物与launch/加载路径相互支持，未发现跑旧fixture kernel的证据。
没有把production声明的compiledArtifacts当成已经执行的充分证据。
这仍是保存的binary、启动记录、加载代码和输出的证据链，
不是本审阅者新增GPU trace。

候选与参考四个SPV分别离线spirv-val通过；
两侧World重新检查11×16 samplers/176常量descriptor访问、
Shader/ImageQuery capability、无NonUniform、stride2240/offset32/总35872一致。
额外当前SDK glslc独立小编译所得release SHA仍为`e3352f34…c9b855`，
不与发布shaderc产物字节相同；该结果不冒充C008产物重现。
本接受依据是C008实际发布/实际私有加载文件的hash绑定与该产物的192页raw结果，
不依赖不同编译入口输出必须相同的假设。

### reference独立性与新编译闭包

candidate exe SHA256为
`e82253af07b38c169fae06d408c5c7db24a149d4935aadf5a0a64a9d878d4b25`，
original exe为
`afb286de3dd81ad0f8bba6c0ba791050d755b00ee65487cb4372c854b8753243`，
两者实物重算匹配build。runtime-support archive也匹配。
11个本轮编译closure的`.ii`与object全部匹配；
记录中各依赖文件重算未发现当前源码漂移。
这包含candidateBaker 558文件、numeric harness 636文件的编译闭包，
可核其输入和dispatch并未只留事后源码。

originalBaker使用已校准的独立对象
`6a721873f6315f94e00a3d8b5d4bfa16d9f59ad12c3cc1b854388cb1284e0616`，
复制object与559文件编译closure的object hash一致，`.ii`也匹配；
CPU地址header hash仍为`35a2c8aa…f74f3d6f`。
在该保存`.ii`核实每子样本调用CPU double graph.Resolve，
独立target cofactor inverse-transpose与source transpose，
绑定诊断地址SSBO；不是candidate GPU route/normal函数自比。
original World SPV为已审
`1d7f420d09ba55ec0f2a980ffbc7844332a2f50779dd9256ea3d49108745438c`，
锁定原实际GPU evaluator `d977382c…3038b`，
新两页byte-exact校准report hash也与本节build引用一致。
因此reference192输出有独立地址/法线与锁定原hardware evaluator的足够有限证据；
不重新把CPU double bilinear设成材质验收门槛。

双方Restore同一发布闭包符合“固定输入比现有基础evaluator”的裁决。
原evaluator未消费candidate新实现的GPU route；基础材质方程和底层上传仍有共享，
故这证明候选对该原actual evaluator的有限等价，
不独立证明原公式、上传器或整套输入的艺术正确性。
oracle诊断SSBO不计入Runtime预算，这次numeric harness也没有物理atlas完整常驻/
游戏发布事务，不能把其budget表当成最终Game内存通过。

### 证据身份与阶段结论

| 本run文件 | SHA256 |
| --- | --- |
| report.json | `58e9cda6dddfa1d4e1d6aed329270440f4515ab70480d863e798dd0ea900a6ac` |
| build.json | `499cea9721abe4846c6a34c57195e7fb4b30c669412ea155a59d79cbcb5794aa` |

**C008 Coverage真实192页数值子项通过，冻结三通道各≤1 byte不变。**
接受范围包括本矩阵的内区、边/角border及粗页/padding实际输出。
不声称穷尽16surface全部虚拟页、全部混合邻接footprint或最终屏幕质量。
Local192尚未运行/独立验收，保持pending，等待真正L009发布及服务绑定证据；
L008 tracker时序CPU修复不能替代Local GPU数值通过。
完整Game冷生产/收敛、screens/TES、steady frame、全预算/WDDM及故障闭环
仍pending；本节不改10936已接受性能结论，也不宣告整个P3通过。

## L009 Local：192页有限真实输入数值独立复验

2026-11-21，读取 `artifacts/terrain-rvt-production/p3-real-numeric-c008/runs/run-002`，
仅补充Local子项；不重算已接受的Coverage/10936压力，不启GPU、不改源或Frozen。
**接受L009 Local有限192页数值子项，关闭前节Local192待项。**
与C008 Coverage合计两个明确发布身份、384个有限真实页；
不合并宣称全岛screen/TES/transients或整个P3通过。

独立构造16个Landscape surface各12页的集合：
mip0 x/y各0、31、63的9页，加mip3(3,3)、mip6(0,0)、root13(0,0)。
candidate/reference各192行/192唯一键与该集合完全一致，
无缺失、重复、额外键；sample0…191、slot=sample%4、
weightSource=1、heightBlend=3、mip0每轴1sample/其余每轴4sample逐行核对。
384个raw bin各221,956 bytes，尾部4-byte status逐个解码全0。

完整比较42,614,784通道bytes，最大误差 **[0,0,1]**，超1-byte数量0。
仅Landscape_04 mip3页(3,3)，stored(108,91)的ORM.g roughness
original215→candidate214这一byte不同，其余191页全部byte-exact。
独立生成的全量非零差异与`local-numeric-completeness-audit.json`完全一致，
其384个rawSha256逐文件重算全部匹配。双方validation-on，
日志无`Validation Error:`/`VUID-`，均完成produced-real-numeric-pages。
launch PID原25700/candidate67072在本次检查时均不再存在；
不是以仍运行的partial输出给出完整通过。

### L009实际输入与冻结运行身份

从 `actual-input-inventories.json` 定位实际L009 manifest，
其自身及75个raw对象共76文件SHA256重算全部匹配。
实际对象中45个image、16个R32_FLOAT local权重；
16surface各绑定一个不同objectId且digest也各不相同，
每个权重对象7,426,300 bytes。不是先前tile00权重向全部surface复用的诊断快照，
也没有在本run为缺失Local输入临时合成权重。

双方launch显式传入
`p5r/L009/VT/17e02619cb09e6a1/manifest.json`和mode1，
恢复后的production BLAKE3都为
`3ebe53215905f82ab21c63a99011556820294697f80ee915fe11dc9ca02242c4`，
manifest BLAKE3都为
`99a01eb86ef395d3f6f3576dec75a80b9c190bee5ae45fcc98fdd717dc537354`。
不只凭上述相同声明：实际75个对象和manifest已独立SHA核对，
本轮用前节已审的同一native Restore/MakeBakeInput/Record/readback路径，
各surface绑定其本次真实发布输入。

build的derivedRunnerEvidence正确绑定run-001父build hash，
说明本轮显式Local-only、不重跑或隐式复用Coverage结果。
其中遗留localModePolicy文本仍写L008，这不是实际输入权威：
本轮inventory、launch和双方manifest身份均一致指向L009；
保留这一元数据文字差异，不将其解释为实际消费L008。

candidate/original executable、双方shader、original object及
independentOracle编译记录与前节已审身份全部一致，实物hash匹配。
重新确认11个native closure及3个oracle校准closure的`.ii`/object共28个hash匹配，
reference仍是559文件闭包的CPU逐样本double邻接、独立协变法线变换、
锁定原hardware evaluator `d977382c…3038b`，没有为Local换成候选路由自比。
新输入不改变原reference权威或冻结三通道各≤1-byte门槛。

L009发布的World/Padding release/debug四个实际raw SPV均hash匹配其记录，
也与双方相应私有project产物保持前节身份关系。
candidate World release仍为
`fc19b407f5b4e99ab1525c297e84b2c17ec37905180b74ab8bc5652790ee478c`；
reference World仍是锁定独立oracle。
因此本节接受的是同一已审kernel在L009实际Local输入上的新增有限数值证据，
不要求重复旧Coverage数值或10936性能矩阵。

### 本run证据锚点及结论范围

| 文件 | SHA256 |
| --- | --- |
| report.json | `e9fb00e103a462cd725e225ff261167538cba90aeeed059ffa6a8bcb85545d1b` |
| build.json | `92edf7f4c5d0b39d09e80c0ffaf3d9abd3eb39c78e8529095f6a4b3038823db7` |
| local-numeric-completeness-audit.json | `7ec6fdc67ba6ccc064eeafa64a9c9bfdf807514389b3ab92c04278bc85b8c18b` |
| actual-input-inventories.json | `e56c2c8d1b535a623f0687052540c933e610102884f774f43b18db35883213c6` |

**Local192有限真实页数值接受；Coverage192此前已接受，合计384页。**
这是同输入下相对锁定原actual-GPU evaluator的基础页比较，
不独立证明输入作者数据本身、共享上传器或材质物理公式的正确性，
不声称所有虚拟页已作真实通道比较。
最终Full16 Game画面/screens、TES、流式瞬态与收敛、两线程、
全Game预算/WDDM、故障与退休闭环仍pending；P3整体不标complete。

## TES黑线单行修复：有限GPU效果独立复验

2026-11-21，独立审阅Avicenna实现的
`artifacts/terrain-rvt-production/geometry-seam-fix/`，
重点为source-freeze/receipt及ab-02-quality。
本审阅者未参与修复；仅CPU读文件/复算像素、离线SPIR-V验证、view_image目视，
未启动GPU、未改源码/Frozen/原包。

**接受该单行根因修复在junction_0_0的有限黑线消除效果，
可关闭这个有限effect子项。不能据此签全部24边/9交点或P3 screen all-pass；
正式新包边角验收仍pending。**

### 源码delta、编译与运行身份

直接比较gb1/ga1的`shader/glsl/materialFunction/mf_terrainLayerBlend.glsl`，
唯一源码行差为第79行Coverage坐标从`context.texCoord`改为`context.texCoord1`。
当前源、source-freeze副本、ga1副本SHA256均为
`1b0f8c28e4946693176ac8d2e9139fdac03134a5e930e96bad9e141c6de0aaf4`；
before为`78888634704dfe4ca02155072bd02c4ad5f281be365751afd81f23b64ef992bf`。
检查LocalLayerArray分支仍从局部`context.texCoord`构造带margin的weightUv，
没有把Local权重错误换成全局UV。

native ShaderCompiler记录8个变体：Base TES0/1、ShadowDepth0/1、
LocalShadow0/1和两个terrainFeedback图形变体。
逐项核对committed、sourceFingerprint/artifactGenerationKey、
完整cacheRecord在测试stage cache中存在；36个runtime/debug输出SHA256
及148项可定位依赖文件SHA256匹配receipt。
独立对18个runtime SPV和8个linked material stage产物再跑spirv-val，均通过。
这是原生编译/生成记录与离线验证，不把prepared状态当成GPU通过。

运行用同一main_game.exe，SHA256
`ce9c1814d4813a94473193dc365fb235eeec318d6665e9b0a062ad35fee332d3`。
对完整stage前后inventory重算，626→634文件、23项delta与approvedDelta集合相等；
两实际shot目录中168个受保护的resources/config/DLL/exe文件逐一SHA匹配旧inventory，
未出现资源、曝光配置或binary偷换。
图形shader/cache按修复重建；两World/WorldPadding generator cacheRecord
在gb1/ga1逐字相同。8变体的完整测试编译与实际ga1的base-only刷新应区分：
实际运行cache中4个record变化，不声称运行了所有8种绘制路径。
两次启动日志均显示hits60、misses0、compiled0、shaderc0、failed0，
支持运行消费已生成cache，而非临场重编译未绑定源。

### 同机位、正常TES及质量条件

两张原始BMP均1280×720、24-bit top-down，实际/声明长度均2,764,854 bytes，
像素起点54；SHA256分别为
before `f6496b0fd81d3058a7e400ca68e6a6535e98d14a240240192643d84ee078a4bc`、
after `08613d3d2609f2e6240f2e78afb4ff1c70ba5f6e277b76461f505f296886584b`。
receipt的11个evidence文件hash全部复核一致。

实际日志camera ack两侧一致：
position=(-100800,9723.415039,-99600)，forward=(0,-0.554700,-0.832050)，
pose=junction_0_0、poseId=2；同样的serialized schema/runtime MI参数、
toneMapping MI、renderGraph、scene、HDRI环境及其强度。
u_layerDisplacementScale=1，detail材质VL_TERRAIN_TESSELLATION=1，
四个目标实际chunk均LOD0：
Landscape_00|3|3、Landscape_01|0|3、Landscape_04|3|0、Landscape_05|0|0。
不是关闭位移的规避对照，也没有只用全局LOD初始统计替代目标chunk日志。
参数依据是相同serialized输入与服务冻结绑定，不是GPU UBO读回；
相同tone/exposure配置也不能替代实际曝光buffer实测。

两侧稳定姿态quality均visible=16384、validAtRawTargetPlusOne=16384、
unresolved/incorrectIdentity/rootOnly=0、interactive=true、convergenceFailed=false，
分别completed/submission epoch151/150，worldGeneration=1。
两侧runtime.log无Validation Error/VUID，result记录exit0并有正常teardown；
before因保存路径斜杠匹配问题恢复已有BMP，未追加第二次before渲染。
本次认可的是同静态姿态稳定后证据，**没有同一frame/同一epoch的screenshot-quality原子绑定**。

### 原始BMP像素复算与目视

独立按BMP header/row stride/BGR顺序读取源像素，未使用缩略图计算。
`dark`定义是像素max(R,G,B)严格低于阈值，不是luma阈值：

| 原始像素区域（右/下边界不含） | maxRGB<80 before→after | maxRGB<100 before→after |
| --- | --- | --- |
| seamStrip x[0,1280), y[340,380) | 20→0 | 472→0 |
| central x[320,960), y[320,400) | 20→0 | 234→5 |
| full 1280×720 | 20→0 | 752→361 |

40px接缝条带changedPixels=49,872，RGB MAE=2.6740234375 bytes，
最大单分量差133，均与归档comparison一致。
较宽central并非<100全部为0，全图也仍有361个暗像素；
不能把条带统计外推为画面所有暗点/全部误差消失。
view_image分别查看两BMP并查看`ab-02-quality/side-by-side.png`：
左侧贯穿接缝附近的细黑线分段清楚可见，右侧相同地貌位置黑线不再可见；
地表纹理总体相符，保留亮度/细节差异。目视支持有限黑线effect，不是像素完全相等。

### 几何与数值残差，不能宣称零缝隙

从四个归档edge数组各1025对数据独立复算，
宏几何worldLeft/worldRight差为0，LOD0法线欧氏差约
4.9e-6…1.3e-5，接近零但不是全零。
保留TES输入/位移CPU残差：四对最大displacement差分别
0.004945018、0.005797101、0.008154735、0.038637736 cm，
约0.005…0.039cm，不写成零误差。
这些数组/后续UV图推导是CPU诊断，不是GPU TES顶点输出真值；
即使理想共同coverage坐标推导一致，也不等于实际浮点链、量化、采样误差消失。

**未提供同帧patch/depth/world-position/ID readback。**
RGB黑线消失与单行UV根因、几何诊断、实际正常TES/LOD0运行相互支持，
足以接受这一机位有限效果，但不足证明所有patch边界没有几何裂隙，
也不能区分每个像素的深度/材质/coverage贡献。

### 发布身份边界与后续要求

counterfactual中16个baseSurfaceHash逐一before=after，
两generator cacheRecord不变；但递归作者依赖包含修改后的mf源码，
完整production身份从
`5a0265a470baca4eeeb7008e639636af69cade40bc42e5c4ac3967bcf184ab3f`
变为
`67b8d60217305afedf1267e110675a83d0c738112efba2614ebb924362235a70`。
该counterfactual只计算身份，不是正式发布的新production闭包。
gb1/ga1保留旧raw作者发布输入/完整hash作为明确的私有诊断对照，
因此**旧pub不是修复后新正式发布包**；不能只因base页身份不变就忽略作者失效。

支持下一步走正常refresh/Cook/export生成新的作者、shader/cache与发布闭包，
由正式新包再做正常TES/位移下全部24内部边与9交点验收。
不手改manifest、不绕stale校验，不把本诊断包宣告已验证的新包。
本节有限effect接受不改变Frozen，也不取消上述正式边角与完整P3待项。

本次证据SHA256：
source-freeze/receipt.json为
`21337449608af35fcffc267a15a7b6be3a5e33c5904c0026498b76c94b191cb3`；
ab-02-quality/independent-comparison.json为
`6c2ece6ea635ad202e26f037160f490fd7ac5373e084c150e504092ea8e1d888`。

## R8 C010：Game质量失败的有限CPU raw调查

2026-11-21，独立读取R8冻结证据
`artifacts/terrain-rvt-production/p5/C010r8-tour-w2-b04/`；
只读Quality ABI、capture-108 raw、runtime-events、R8 frozen source copy和已记录的
package/binary身份。没有启动GPU、没有改production/source/shared build，
没有重复Ptolemy的fresh单点或remaining53任务。
失败报告保留，不改变Frozen门槛。

**调查结论：失败是可证的“可见样本在target+1以内未解析”的实际页解析失败；
raw足以证明实际解析到了过粗mip，但不足以证明请求未发出、请求被丢弃、
预算/池满或authorMaterialSnapshot bug中的任一项是唯一因果。**
因此这是对main/Euclid的有界定位，不是猜测pool满，也不是把R9修复归因倒灌到R8。

### capture-108 的独立解码

`capture-108.bin` 的TVQ1 ABI为version2、128×128、16 surfaces、
frame329、pose6、worldGeneration1、submissionEpoch330；
header visible/resolved=16384，duplicate/invalid/uninitialized/dropped/overflow均0。
capture JSON对应`completedCaptures=108`且文件完整。
R8 frozen quality decoder按实际owner dictionary、surface dictionary、每像素
8 taps×2 branches重新解码：所有owner identity、surface identity、surface/mip/page
匹配，unresolved=0、incorrectIdentity=0、rootOnly=0；唯一失败计数是
validAtTargetPlusBias=14095/16384，缺口2289（86.028% valid）。

2289个不合格sample全部来自`terrain|Landscape_08`，不是跨surface身份错误。
独立输出的逐sample结果保存于：
[r8-quality-failure-raw-investigation.json](../../artifacts/terrain-rvt-production/p5/C010r8-tour-w2-b04/r8-quality-failure-raw-investigation.json)
（SHA256 `ef1e885914d10344815bd41ead3076ba2fd47dd5e5d4ee48f1de519c937f4c2f`）。
实际过粗解析的分布为：

| requested mip → actual mip | 失败tap/branch计数 |
| --- | ---: |
| 0 → 4 | 1767 |
| 0 → 5 | 1521 |
| 1 → 3 | 71 |
| 1 → 4 | 2887 |
| 1 → 5 | 1521 |
| 2 → 4 | 32 |
| 2 → 5 | 5 |

这些是tap/branch计数，不能相加当成sample数；对应2289个唯一可见sample。
大多数sample targetMip为0（16369个），14个为1，1个为3；
最终raw实际tapCount分布为1:12984、2:1360、3:958、4:504、
5:394、6:121、7:45、8:18。实际失败分支保留了anchor标志，
因此“跨页”不能仅从最终失败mip推出。

按UV/footprint公式复算tap页坐标后，2289个失败sample中31个至少有一个tap
跨center page边界，2258个没有跨边界。因而跨边界是少数关联维度，
不能作为2289缺口的统一解释。失败UV范围（raw存储的material UV）为
`u=0.3391205…0.4999491`、`v=0.2473512…0.5054214`；
majorUv范围为`x=8.2055e-6…0.00109068`、
`y=1.0039e-5…0.00180551`。这些数值支持失败集中在该pose的
Landscape_08可见区域，但不提供世界空间坐标或请求队列因果。

实际owner dictionary有289个valid owner，raw能证明失败tap的最终entry被写出并
通过identity校验；它不能证明该entry是当时所有可用页的唯一候选，也不能看到
feedback pass每个fragment提交的请求列表。Quality capture ABI只保存：
最终可见winner的UV/footprint/target/bias、最终解析entry/mip/page、owner和expected
identity。它**没有逐tap feedback request、request reason/priority、队列入队、
拒绝原因、pool free水位、eviction/retirement时间、producer task→page的因果链**。
`runtime-events.jsonl`同样只给quality状态/capture和生产状态，不补这些字段。
所以当前raw不足以回答“这2289页是否曾被请求”或“请求后为何没有驻留”。

### R8边界、原因候选与禁止外推

R8包/二进制为C010诊断包，`main_game.exe` SHA256为
`c6cb2334ad4c63093d1d9ebc5da439a540915d165f005c838b947ca5dad4a4ea`；
R8 frozen checkpoint保留source manifest与C010资源身份，
`mf_terrainLayerBlend.glsl`冻结修复源SHA256为
`1b0f8c28e4946693176ac8d2e9139fdac03134a5e930e96bad9e141c6de0aaf4`。
本节没有读取或混入R9当前source/build变化。R8 runtime source确实含
feedback的clamp/floor寻址和质量capture的`TerrainCompositedAddress` walker；
但这只能确认公式/ABI，不是请求生成或调度日志。

R8的authorMaterialSnapshot长期initial94-slot bug已有独立E005Undo198/216 CPU复现，
而R9 Service已修复；本节不把它宣称为本次C010 Game失败原因。
当前唯一实际结论是：在pose6 deadline前，R8已完成的capture观察中，
实际解析页大量停在target+3/4/5，导致冻结99%质量阈值失败。
为什么没有更细页可供QualityAddress选择，必须由Ptolemy的新鲜实验或新增
request/queue/residency因果字段判定。不能以“pool满”“feedback floor”
或“author snapshot”任何单一标签替代证据。

本调查也不声称failure是单纯地址错：failure大多数不跨page，且identity/owner
全部正确；不声称是纯缓存错：raw没有request/cache-miss lineage；不声称是纯生产
慢：runtime有production status=0，但Quality capture没有逐页等待关系。
R8报告的complete=true只代表capture ABI本身完整，不等于99% quality通过。

**有限裁决：R8 C010 Game质量子项保持失败并pending，已给出2289缺口的实际
解析mip/page证据和raw字段缺口；不改门槛，不替Ptolemy归因，不把R9修复预判为
本次R8根因。** Full16 Game、fresh currentSource/package、steady frame、
内存/WDDM、故障/退休与完整P3仍pending。

## R10 fresh失败：冻结source与8个需求页的数学交叉核对

2026-11-21，按用户限定只读
`core-native-r10-local-author`及两个fresh失败的末次raw，
没有启动GPU、运行probe或shared build，没有改production，
没有重做R8整体raw计数。R11 probe/实际queue-slot调查留给主线程执行者。
本节不把R10失败归因于已经修复的R8 initial94-slot问题。

可执行CPU复算与结果：

- [independent-source-math.py](../../artifacts/terrain-rvt-production/p5/R10-twofail-fresh-cpu-audit/independent-source-math.py)
- [independent-source-math.json](../../artifacts/terrain-rvt-production/p5/R10-twofail-fresh-cpu-audit/independent-source-math.json)
  SHA256 `54ddbb687c9e273fc281a3c60897399aae8305cb99e720d22bcc6bc60b25730e`。

**结论：这两张raw的winner→期望feedback页编码→CPU选择公式间未证出一个
可解释本次全部失败的数学错误。已给8个明确缺失高分支页的期望bytes和实际祖先，
可直接用于R11逐环节对照。缺的是实际feedback attachment/ring/selector证据，
不能把下列“期望bytes”说成已经观察到GPU实际发出。**

### 冻结源与实际cache身份

checkpoint与包的main_game SHA均为
`add49468f40b417fb040ec6a88afa7bf492f603f3a1bd79198fe90b2ee07be29`。
冻结source、包source及checkpoint记录核对一致：

| 文件 | SHA256 |
| --- | --- |
| pass/terrainFeedback.frag | `276f25fd2320005460897fa4cdf36d3dd5bb73a13b1fd964602d59d20c28487f` |
| pass/terrainFeedback.vert | `43c846bf07f9a231e46913e3bf6459fefe29dfa8ff6bc5261f6965c773a6ba99` |
| materialFunction/terrainCompositedFeedback.glsl | `b329a4e75f02aed64da709546bd9cb1d14d1cc45cb2d51c5cfb411d44aa1e2b2` |
| materialFunction/terrainCompositedFootprint.glsl | `09e8e721fd116b4d0f5a969352a93fd44cea404763cbdc00b9e5423a073f047e` |

包内实际feedback有UNLIT带hash路径和DEFAULT_LIT兼容路径两个cache record：
sourceFingerprint分别`1d004d7b…35a20c3`、`4a121be6…7aa47008`。
两套runtime fragment实物SHA相同
`41f42babc7cfa8369452d425a39eda8b0e0c43a6f13458ed34ceaef067e43df3`，
vertex同为`90b68ff231a1ca61810ccc5c6a3f569ed1e25b3c09d3a2080e2de339bcb2976b`；
两fragment均离线spirv-val通过。
两套debugFragment反汇编的嵌入原始源码均出现
`min(int(floor(compositedMip))+1,maxMip)`，不存在旧`ceil(compositedMip)`；
record列出对应primarySource及Feedback/Footprint依赖。
没有发现其中一个variant遗留ceil的证据。
这里绑定的是保存的源码/cache/产物与启动包，不新增GPU调用追踪。

### 最小8页：期望编码与实际解析对照

直接读取center_0_2末capture27（frame70/epoch71）和center_1_2末capture41
（frame122/epoch123），先核原文件SHA：
`c146ca95…a2936388`、`ba1a5bf3…ec4faa31`。
每个pose选择四个最常出现且owner dictionary未驻留的高分支中心页，
所有示例targetMip=0，高分支应请求mip1；不是对floor/ceil语义的模糊推断。

| pose / surface index | mip1 XY | 期望address RGBA8 | 需求sample数 | 示例实际high mip/页 |
| --- | --- | --- | ---: | --- |
| center_0_2 / 8 | 16,16 | [8,16,16,129] | 6154 | 4 / (2,2) |
| center_0_2 / 8 | 15,15 | [8,15,15,129] | 3403 | 5 / (0,0) |
| center_0_2 / 8 | 16,15 | [8,16,15,129] | 2038 | 5 / (1,0) |
| center_0_2 / 8 | 15,16 | [8,15,16,129] | 1521 | 5 / (0,1) |
| center_1_2 / 9 | 16,16 | [9,16,16,129] | 5591 | 5 / (1,1) |
| center_1_2 / 9 | 15,16 | [9,15,16,129] | 4396 | 5 / (0,1) |
| center_1_2 / 9 | 15,15 | [9,15,15,129] | 2531 | 4 / (1,1) |
| center_1_2 / 9 | 16,15 | [9,16,15,129] | 1117 | 4 / (2,1) |

surface8/9分别为Landscape_08/09。完整JSON包含witness像素位置、原UV/majorUv、
每tap actual entry/anchor/mip/page、期望bytes。
例center_0_2 pixel(64,0)：
UV=(0.5000695586204529,0.49750709533691406)，target=0、tapCount1；
mip1 extent4096，clamp texel后除128得page(16,15)，
shader高地址应输出[8,16,15,129]/255，CPU解码还原surface8/mip1/x16/y15。
实际quality记录entry85、anchor=true、mip5/page(1,0)，其owner存在且identity正确。
该例不涉及anisotap、多页需求或高分支混合权重为0导致请求可省的假设：
R10原消费/quality合同仍要求low/high两branch存在。

### selector/cap与各向异性核对

按两raw所有winner重建去重中心request集合：

| 推导项 | center_0_2 | center_1_2 |
| --- | ---: | ---: |
| 中心low/high唯一request keys | 114 | 230 |
| 按mip键数 | m0=81,m1=31,m2=2 | m0=85,m1=97,m2=34,m3=9,m4=4,m5=1 |
| owner字典不存在的中心键数 | m0=50,m1=31,m2=2 | m0=43,m1=86,m2=34,m3=9 |
| 最多单surface选页数（含root） | 115 | 227 |
| 有tap跨虚拟页的sample数 | 97 | 81 |
| tap离开surface [0,1]的sample数 | 0 | 0 |

R10 CPU `DecodeTerrainCompositedFeedback`扫描2组完整address/metadata attachment，
解码后sort/unique，未设置256/512条截断。
`Service::ConsumeFeedback`仅做surface/layout合法性检查并转token，
与surface-map登记顺序相同。
`Runtime::ConsumeFeedback`的全World容量为512减16root减4free reserve=492
（single active World、无candidate requiredPages前提），只在去重request超容量时
整体取一次Parent并标queue bias1；
`Residency::ConsumeFeedback`另在单surface含root选择数>511时循环取父。
对于这里推导的114/230键，两级均不会触发coarsen。
这证明“如果这些winner键作为实际feedback输入”，selector不会主动降到mip4/5；
不证明实际ring输入只含这些键或当时无candidate额外reserve。
R10另一条coverage/pageManager的`maxPageRequestsPerFrame`不是composited请求截断；
256 duplicate-witness capacity亦不是此request cap。

将每tap在requested和requested+1的可接受anchor/direct页集合，与这两张raw全部
推导中心request集合比较，未发现任何tap/branch完全没有可接受页被覆盖。
统计使用float32分步计算tapUv，并保留边界例子。结果是0个不可覆盖branch，
**不要求额外anisotap request才能解释这两张raw的target+1失败**。
这不是任意footprint/场景对center-only feedback的普遍完备性证明；
也不把“请求集合里有可用候选”解释成页已实际被请求/生产/发布。

### 梯度、坐标/翻转与winner转置

冻结vertex显式`outPageUv=inTexCoord1`（全局coverage坐标）、
`outMaterialUv=inTexCoord`（局部材质UV）。
fragment用前者查32²surfaceMap，后者同时写quality winner和encode RVT页请求；
没有把全局UV直接编码成surface-local页。
surfaceMap CPU布局`((originY+y)*32+originX+x)*4`与shader `texelFetch(x,y)`一致，
Upload flipY=false。feedback ring复制image到buffer的flip参数false；
CPU解码顺序是RGBA的surface/x/y/mip，未交换XY。
Quality shader读`ivec2(pixel%128,pixel/128)`，winner `.xy`读UV、
`.zw`读major，selector只匹配surface低16位，没有发现源码层winner转置。

footprint由两屏幕梯度的对称平方和/叉积构造major/minor；
单独把dFdy整体反号、或交换dx/dy，不改变这套椭圆axis矩阵与mip/tap数。
major方向反号只逆序对称tap集合；center encode本身不读取major方向。
因此不能仅凭图像Y轴约定推断请求错页。实际UV V翻转/XY交换则会改变非对称例子
如(16,15)；上述8页与witness给R11足够明确的原坐标对照。
raw没有第二份“实际feedback address”值，故无法只靠quality raw证伪实际GPU
attachment绑定、copy offset、surfaceMap运行时绑定、frameTag/fence消费错误。

### 精确待观测字段与交付结论

要把剩余不确定性缩到R11 probe，最少应在同一World/pose/capture或明确相邻epoch
对这8键提供：

1. 实际feedback四个RGBA8 attachments的address/metadata bytes，
   含frameTag、copiedEpoch及消费期待tag；尤其高slot2是否真是上述129。
2. CPU解码后、Runtime selector前后这8键是否存在及实际targetCapacity、
   selected size、candidate reserve、effective bias。
3. 每键Request/queue/production/status/slot/pageTable提交的state和identity，
   不能只记录全局pool满/总体queue长度。

本任务未从两fresh raw和冻结公式证明新的production source bug，
不提出未经证明的坐标翻转或增池修复。若R11实际bytes与上述计算不符，
应修复第一个发生差异的绑定/encode/copy/decode环节；
若bytes相符但selector丢弃，应按本节容量公式给出实际选择原因；
若选择正确，应追踪页级queue/slot/publication，不能再改feedback数学来“过验收”。
**这是一份具体8键/bytes与公式的排除性证据，不是R10失败cause已确认，
不取代主线程R11实际probe，也不改变原失败和99%/10s门槛。**

## R11r4：首单实际LOD0 TES patch有限GPU子项独立验收

2026-11-21，独立读取
`artifacts/terrain-rvt-production/r11-first-tes-stage-r4/retry-02/`。
审阅者未参与actual capture实现；本轮只读冻结core、raw/metadata/日志，
在私有临时目录作CPU阴性检查并重新调用原compare_tes，
不启动GPU、不运行native renderer、不改source/shared build/Frozen。

**接受首单实际正常LOD0 TES patch的有限捕获与R16高度数值子项。**
这是一个patch、一次capture的接受，不是table/border、scene-lit全53或P3整体通过。
首attempt初始化失败仍保留，不能计作GPU捕获成功。

### 实际执行、原始数据及完成绑定

重算retry-02原始文件身份：

| 对象 | SHA256 |
| --- | --- |
| raw/tes-1-1-206.bin | `dbeff327b5d06ae3088f6d17413ab47704eb72fb2962dc0f35430e1c8f67086e` |
| raw/tes-1-1-206.native.json | `d50a4916167ec41b1be9cc01e9491576b542681d00d6dde1fdf601a73ec19304` |
| result.json | `ff78439c22b922cc589abbe8cb7f43f422c0a06178e7ea186c299638bc207dd4` |
| tes-bound.json | `19196132f57eda3207917894433cef038ecacafded572e3723d77bbc28ba952d` |
| tes-oracle-private-jsonfix.json | `887fac978df08df3a235f6becfa717497222d6a3af213c200c90fe0a0ea63fad` |

raw长度98,368 bytes，容量512、record stride192 bytes、header64 bytes。
独立decode header：
`[116,0,16383,1] / [2,0,3,0] / [1,0,1,0] / [206,0,0,10]`，
分别绑定records116/overflow0/primitive16383/count1、frame2/submission3、
World1/pose1、drawOrdinal206/surface0/LOD0/VK_PATCH_LIST。
116条有效records全部finite且record primitive ID均16383；
不是依赖metadata自己声明“TES已运行”。

native PID63696的保存退出收据为exit0；
native.log中唯一RVT_TES_CAPTURE与raw metadata一致，
completedEpoch=3等于submissionEpoch3。
检查冻结实现：TES shader storage write→transfer read屏障后复制host buffer，
再transfer-write→host-read屏障；host coherent内存，
Submitted绑定actual epoch，TryConsume仅在actualCompletedEpoch≥submittedEpoch时读取，
header其余字段须与expected完全一致，storage/immutable draw owners保活。
Frame bridge用实际frame fence完成状态消费。没有以异步未完成的host字节作证据。

runtime-file trace末尾traceEnd/incomplete=false，15,763条terminal前记录、
9,207,524 bytes独立复算吻合；
runtime-evidence末尾sequence384、前383条/159,729 bytes吻合，
incomplete=false。trace是observedAPIOnly，不称OS全量文件审计。
原attempt错误为`VULKANLEARN_RUNTIME_EVIDENCE=1`而非绝对输出路径，
captureCount=0、nativeExitCode未持久化；其失败文档不改判。
retry-02使用新raw output且保持同publication。

### 真实Draw VB/IB、material descriptor及capture variant

coherent core冻结checkpoint SHA256为
`71752867c800de317c31efc2974f3d70b48abc21828e9d1e123993cd9f751400`，
其中注册native_terrain_capture_game.exe与stage实物均为
`6e7ca82677c8329314ad8b875134fbdbe52ae61472159b780e8d40cc5b3a45c6`。
stage-receipt列864个bin/shader/resource文件重算全部匹配，
case的72个sourceInputs也全部匹配。
publication SHA仍为
`1f92ee6690d1c026bc03aad135cbf196e8263929ba0f8388e7db3d9db6ec2e9e`。

读取该冻结core的rendererDrawExecutor、terrainCaptureHooks、
nativeTerrainCaptureAdapter及terrainTesCapture/Policy/Pipeline：
选中实际resolved draw的Landscape_00|3|3、LOD0；
mesh key为512 grid、128 chunkQuads、chunk3,3、lodStep1。
实际patch IB indexCount65536、firstIndex0、vertexOffset0、instanceCount1；
65536/4=16384 patches，所选最后primitive16383合法。
VB/IB由renderable.DescribeDrawBindings取得，
SameDraw逐一比较VB、offset、indexBuffer/count/type、instance、frameSlot及
regularPipeline；renderer随后仍调用原renderable.Draw，
不是以合成patch代替真实绘制。

set0…3复用实际object/pass descriptor sets，MaterialStateSnapshot及draw owners强持有；
仅选中这一draw换成诊断pipeline并绑定set4 TES-only SSBO，
draw后恢复正常pipeline/sets。记录的实际detail MI为
`…/RVTv2_Full4x4_R8192_Default_CoverageAtlas/MI_composited_tile_00_detail.json`。
pipeline要求实际normal opaque terrain Base TES1 compile request，
加入VL_TERRAIN_TEST_CAPTURE，并用真实instrumented TES source生成candidate；
device必须实际启用vertexPipelineStoresAndAtomics+tessellation及set4能力，
不能用普通未启特性的pipeline冒充storage capture。
正常其他draw保留原pipeline。动态CompileCandidate不向普通cache提交，
所以不要求普通cache出现已提交capture variant。

instrumented source SHA256为
`f1f4aca54aaec769624e1f244395ca274d4da493a6f24c4df77529f4d6f385b4`，
raw metadata绑定sourceFingerprint
`fcbd71849110265199f85331d8a27bb0e5ca9b1b740c95599d7e7b13dc2e644a`、
artifactGenerationKey
`d08ab6635fce2dfc53ce2313990cc1c6d244f34a04bd2526b7019adde22f8cb7`。
TES使用gl_TessCoord/实际插值、实际source height sample后写record；
不是compute-only source评价伪装TES输出。记录包含macro world、post-displacement
world、fade/normal、local/global UV、层ID/权重、采样height与UV、amplitude等。
不把逻辑draw metadata等同于额外GPU buffer handle/descriptor内容dump；
本接受基于冻结的实际绑定检查路径、身份、真实stage records共同成立。

### 原compare_tes重跑与阴性检查

直接import并调用现存原`rvt_visual_oracle.compare_tes`，其源码SHA256仍为
`45451ea64cbc759c4c8c99775bf90404d220ec33c035aa9b62783d3efbe94a80`。
没有运行会覆盖原report的finish_saved_oracle脚本。
私有JSON adapter只对np.generic调用item()用于序列化，
没有更换计算、输入、gate或rounding。独立重跑得到完全相同结果：

- records116、正权重source-height samples232；
- 原发布R16源纹理按layer-major/mip-major取固定source mip2，
  payload SHA256为`756d3cd59a63142a85433a291d01932abb2b3b10e11ba95fb90104c9d4e81f94`；
- 独立double source bilinear最大高度误差
  **0.003735423089022971 R16单位 ≤ 冻结2单位**；
- 116条均实际非零位移，最大2.0203586558345443cm，
  displacementScale1、TES radius6000cm，存在实际半径内的active invocation；
- double预测world最大绝对分量差0.00387360506283585cm仅为诊断，
  不另造world-coordinate门槛。

runtime-file trace包含该R16 payload的RootedFileReader/finalHandle readerValidation，
与case实际输入hash/源slice映射一致；
source_mip2 oracle从publication选择layerHeight对象，不拿已捕获GPU heights生成expected。
不过层ID、权重、UV及amplitude使用记录值，因此此项独立验证的是给定实际采样输入的
源height读取精度，不是对全部coverage/权重公式另作独立证明。

另在临时副本做10项阴性检查：compute-only、synthetic、LOD1、
fence未完成、publication hash错误、overflow、zero count、nonfinite、
实际displacement off均正确拒绝；源height注入+3 R16单位后
原比较函数判heightGateFailed，实测误差2.998602867单位。
现有rvt_visual_oracle_test.py的16个CPU测试也全部通过。
所有阴性只改临时副本，原始raw/metadata/report不动。

**首单实际TES捕获+R16数值有限子项接受。**
不能从一个末角patch推广成table/border、所有邻块、所有patch、
53个scene-lit机位或完整P3/P5接受；
完整Game质量、画面/TES空间连续性、流式瞬态及预算/性能等仍按各自待项验收。

## R13组合：两原失败点实际收敛及短run CPU改善独立验收

2026-11-21，独立核对
`p5/R13-probe-center_0_2-w2`、`p5/R13-probe-center_1_2-w2`，
以原始quality bin/JSON、dedicated runtime events、CPU_RAW/GPU_RAW及冻结
`core-native-r13-priority-staging-r1`为准；Hume的combined receipt作为可比对汇总，
不取代raw。没有启动GPU、运行probe/shared build、修改production，
未读取R14当前源码归因，也未修改R8/R10/R11原失败或Frozen。

**有限接受：R13组合在这两个原失败pose实际达到原10s/99%质量条件，
并显示本组validation-on短run的CPU interval/quality消费路径墙钟改善。**
不宣告全53、正式1000帧性能、validation-off基准或完整P3/P5通过。
priority和cached CPU copy同时变化，本证据接受组合效果，不分离宣称各自因果份额。

### 到达/ready时钟与真实质量raw

对两run全部26/46份quality raw（共72份）独立调用CPU decoder：
逐sample claim、low/high所有tap的实际mip/page/entry、owner generation/revision/
完整identity、expected surface layout/contentHash、header frame/pose/World/epoch，
再与同submissionEpoch的native QualityState逐项比对，全部匹配。
不以readbackSequence、capture计数或“读取完文件时刻”替代GPU renderFrame。

| pose | 实际到达monotonic ms | 首ready延迟ms | 首ready实际frame/epoch | 首ready有效/可见 | 最终有效/可见 |
| --- | --- | --- | --- | --- | --- |
| center_0_2 | 639581813.8481 | 1776.2929000854492 | 117 / 118 | 16384/16384 | 16384/16384 |
| center_1_2 | 639695015.9145 | 6502.141299962997 | 258 / 259 | 16304/16384 | 16384/16384 |

到达时间取实际metadata.completedMonotonicMs减同epoch Service.elapsedMs，
与原生deadline减10000ms一致；不是observer sleep、process结束或native finish时间。
首ready分别绑定capture17/capture35，completedEpoch均不小于submission；
center_1_2首ready为99.51171875%，不是100%，之后才到全有效。
首ready native sink打印较completedMonotonicMs约晚11.6365/11.6076ms；
这不把ready延迟改成后处理完成时间，也不使捕获字节“过晚”。
最终分别capture26 frame188/epoch189、capture46 frame343/epoch344，
poseId均2、worldGeneration1。

两点首ready和最终raw均effective shader bias0、unresolved0、incorrectIdentity0、
rootOnly0、uninitialized0，header/resolved/counter完整；实际按floor(target)+branch
与该branch原target+1检查，无放宽目标mip。
两run无native质量failure，正常exit0。
dedicated events分别561/1018条，顺序、terminal前字节583679/1071963及record数
560/1017独立核对，incomplete=false，无未决artifact或abort。
其clean footer证明本run闭合，不追认旧failure的incomplete尾记录为成功。

作为对照，保存的R11r4两个失败raw独立核对最终分别7555/16384、
11/16384，均没有native ready；原始10s质量失败保持原判。
R11 center_1_2有一份capture13没有匹配QualityState，未拿它补造时序或ready；
其余39份及最终失败计数可绑定。R13没有这种unbound capture。

### 真实frame ID与短run CPU比较

所有GPU_RAW先按`worldGeneration/renderFrame/submitEpoch`核唯一性，
frame=renderFrame、fenceCompletedEpoch≥submitEpoch；
与存在的同renderFrame CPU_RAW submitEpoch相符。
R13两run分别194/349个completed GPU记录，真实renderFrame范围0…193/0…348；
CPU分别195/350条。GPU首帧0没有相应record-begin CPU0属于记录起点边界，
未把readbackSequence0当成frame0的充分依据。

CPU统计窗口是“实际pose到达至process停止前记录或10s，以先到为准”，
以sampleMonotonicNs筛选原始record-begin interval；未使用滚动均值：

| pose / 版本 | interval数 | mean ms | p50 ms | p95 ms | max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| center_0_2 R13 | 130 | 35.132539 | 33.0201 | 44.4781 | 237.8301 |
| center_0_2 原R11 | 63 | 159.238708 | 48.5130 | 305.8448 | 587.6885 |
| center_1_2 R13 | 283 | 34.439412 | 33.0629 | 49.8363 | 234.9730 |
| center_1_2 原R11 | 60 | 167.032617 | 47.2784 | 339.2907 | 383.3460 |

实际可用终点相对arrival约4522.6721/9697.2106ms，
所以R13尤其第一点更短；不把130对63等帧数差称为等时长throughput实验。
quality完成观测→native state输出的路径墙钟平均独立复算为
11.925831/14.346489ms，旧R11约249/279ms。
这包含导出/Observe/日志路径，不是新增的memcpy-only计时或GPU timestamp；
median分别11.614700/12.126150ms，最大15.469100/75.078900ms。

这些是worker2、validation-on、30Hz、1280×720、MSAA2、immediate、
warm immutable shader cache、fine cache disabled的有限实测。
不能用44.4781/49.8363去签正式validation-off/≥1000帧的49.025ms p95门槛，
不扣除instrumentation、不外推稳态GPU或全岛性能。

### 冻结source/bin/cache、原门槛和新增CPU账

R13 checkpoint SHA256
`24c96cf967648c8483f1497ab645e6924d689ce83cac3e15c8c3f5c87572adcb`，
1114个source-before/after记录相等，冻结实物逐文件hash匹配；
120 build inputs前后稳定，checkpoint记录普通9 targets成功。
实际两包Game均为
`0b9f2cba466cc82684ee10ce3e01a1373345d84a91e747157e192ff92dccda34`。
两stage分别150个resource文件及452个warm shader/source/cache文件重算hash全部匹配，
package-before/after字节相同。C010 manifest与原两点同一SHA
`1f92ee6690d1c026bc03aad135cbf196e8263929ba0f8388e7db3d9db6ec2e9e`；
P0 SHA仍`43fba0dd…88e0dd`。位置/朝向沿用原pose，阈值仍10000ms/99%。

checkpoint中实际保留的binary/library/shader/objects记录重算无hash冲突；
另有71项辅助native/test的build-record路径未在该冻结目录归档，
故不把本次检查称为9 targets所有object逐个可重放的完整编译存档。
本子项实际普通Game、1114冻结source、关键库/运行资源/cache身份已核实，
不需要从R14共享build追取缺失object来替代历史证据。

冻结Service确实调用source-owned TerrainCompositedProductionPriority：
root优先、namespace round-robin，non-root coarse/fine有界quota及周期sweep，
仅成功Commit推进游标；已移除旧whole-list rotate作为选序策略。
本节不重做Darwin/Euler先前policy/parity CPU验收。

QualityGpu冻结代码先按预算TryReserve再resize cpuReadback，
quote CPU显式增加Abi::Bytes=1,619,200；
完成fence/query后只一次memcpy(mapped→cache)，后续Recompute/WriteReadback用cache，
outstanding强/弱持有阻止下一次capture覆盖，公开TVQ1 ABI仍1619200 bytes不变。
原admitted CPU 1,881,344→R13 3,500,544，差恰1,619,200；
GPU admission仍4,942,592，captureGPU allocations3,238,656、
host1,619,200、两run observed Workspace GPU峰值均27,396,128不变。
最新capture metadata记录CPU预算峰146,666,910 bytes，
原R11对应145,042,668；其中含priority等metadata，不能把整峰差
全归于单次copy或声称整进程private实测。
该copy与cache字节来自同一完整fence后payload，
raw独立解码/原生计数全匹配；不是用cached估算计数替换实际GPU结果。

### 一页/四task上限与接受范围

冻结Service每次成功record后Commit并立即return，仍一页/帧、221956 bytes/page，
四个task资源及全局上限不变。两run stdout的119/252条实际production完成记录
逐submissionEpoch最多一条，status/effectiveStatus全0，
符合不暗增并发吞吐的证据；不是用产页次数冒充渲染frame。
validation-on全run（含到达前）production最大5.902080/5.674528ms，
combined最大5.955968/5.730688ms，保留这些观测，
**不宣称本run正式单页4ms性能通过**；本节接受的是两点质量收敛与CPU路径改善。

Hume总receipt
`artifacts/terrain-rvt-production/r13-combined-two-point-actual-receipt.json`
SHA256 `7a393d4c98170b04124b8f979631135d330f57d37e6941b0a30023b23e2dd721`
与各run implementation-combined receipt的结论在本次raw复算范围内相符。
两点原生result仍为collected-independent-review-required/acceptancePassed=false；
本节是独立有限接受收据，不改写原report。

**R13组合两原失败点实际10s/99%收敛接受，validation-on短run CPU改善接受。**
原失败不撤销，其他pose/worker1/Local/全53、
正式1000帧validation-off性能、完整Game资源/故障/退休及整体P3/P5继续pending。
