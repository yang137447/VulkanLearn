# 头发资产与发丝物理选项记录

状态：2026-10-19 资产路线选定 TressFX 5.0 for UE 5.4（粉发女性）；
已实现 CPU `.tfx` 数据层与 Female 保存曲线/Orig 网格的正式静态关卡，完整绑定/贴图与头发渲染尚未完成。
当前实现边界见 [TressFX 资产导入](../../contracts/tressfx-asset-import.md)。这是 `plan/` 层，不是实现契约；
选定路线后另起实现计划，不能把本文当作当前行为。

## 背景

目标：先把 UE 写实头部肖像搬进 VulkanLearn，再决定是否做发丝物理。
参考效果是 RE4R 的 `Hair Strands`：guide + 插值发丝、硬件/软件混合光栅、
顶点色单散射 + 多重散射、deep shadow。

以下为选型时的前提；静态 guide 的后续落地见
[实施记录](tressfx5-hair-port-plan.md#第二批落地正式网格路径的静态-guide)：

- Hair 着色模型已于 2026-09-12 删除；`common/hairPathScattering.glsl` 与
  `SC_paper_case_plot_hair_solo` 探针仍在，重建口径见
  [shading-model-alignment-plan.md](shading-model-alignment-plan.md) §2.4 / `H-20`。
- GBuffer 是 deferred，逐像素 coverage 的发丝应走 forward，并单独接阴影与 AA。
- 角色侧已有 GPU 蒙皮与 GASP 骨架，发根可以绑到头骨/头皮骨骼；
  契约见 [animation-runtime-skinning.md](../../contracts/animation-runtime-skinning.md)。
- 还没有 groom 资产导入、发丝几何、发丝阴影或发丝模拟。

## 资产路线

| 路线 | 资产 | 运行时形态 | 效果上限 | 一次性成本 | 备注 |
| --- | --- | --- | --- | --- | --- |
| A | MetaHuman + UE Hair Card Generator | cards mesh + 贴图 | 高于手摆 cards，低于发丝 | 低 | 最快进引擎；仍不是发丝级 |
| B | MetaHuman / Fab groom → 离线烘 ribbon | 普通 mesh（保留 guide/权重） | 发丝级静态 | 中 | 需要转换器；物理要保留 guide |
| C | AMD TressFX 4.1 | strand + 自带 Vulkan 管线 | 上一代发丝 | 中 | MIT；适合管线/模拟参考 |
| D | Fab / ArtStation 现成 cards 角色 | cards mesh | 中 | 低 | 最快，但画质封顶 |

资产路线的共同前提：发丝级资产都来自 groom；如果只烘成纯静态 ribbon
并丢掉 guide/插值权重，后续做物理会很难补回来。

### Tomb Raider / TressFX 补充

- 头发观感排序：`Shadow of the Tomb Raider`（2018）> `Rise of the Tomb Raider`（2015）
  > `Tomb Raider`（2013）。三代都是 TressFX / PureHair 一系；2013 是初代，
  发丝细、易飘，观感最老。
- 官方没有 Lara 的模型/头发资产。TressFX SDK 4.1（MIT）带示例发丝、Maya exporter、
  模拟与渲染代码，是唯一可合法使用的“古墓丽影头发技术”资产；示例不是 Lara 本人。
- 粉丝提取的 Lara 模型与 RE4R 同理：侵权、cooked 数据、不建议进仓库。

### TressFX 5.0 for UE 5.4（当前选择）

2026-10-19 决定优先 **TressFX 5.0 for UE 5.4** 的粉发女性示例；UE4.27 版不是目标。
它不是一个独立库，而是 UE 5.4.4 的 TressFX patch：

- 资产：`Asset/Maya` 里有 Maya 2018 源文件（female avatar + collision mesh），
  以及 `.tfx` / `.tfxbone` / `.tfxmesh` 导出链路。
- 运行时：TressFX Plugin + UE Renderer hooks，深度绑定 UE deferred 管线
  （BasePass、DeepShadowMap、Marschner、Composition）。
- 访问：patch 在 `GPUOpenSoftware/UnrealEngine` 的 `TressFX5-5.4.4` 分支，
  需要 Epic UE GitHub 权限；未登录访问是 404。

搬运拆成两件事：

| 部分 | 做什么 | 依赖 |
| --- | --- | --- |
| 资产 | 从 Maya/UE 导出 female avatar、body/head、hair `.tfx`/`.tfxmesh`、贴图、骨骼 | UE5.4.4 patch + Maya 2018 |
| 运行时 | VulkanLearn 的 strand 渲染 + guide/follow 模拟 | TressFX 4.1 standalone 作基座 |

运行时以**拆 5.0 工程**为主基座：抽 TressFX 核心/插件/shader，把 UE glue
重写到 VulkanLearn；4.1 standalone 只作算法对照与 fallback。
直接编译 5.0 的 UE 插件不可行，但 5.0 的 BasePass/DeepShadow/Marschner/
Simulation 是目标源，不再以 4.1 为主。

实现计划见 [tressfx5-hair-port-plan.md](tressfx5-hair-port-plan.md)。

垂直切片：

1. V0：静态 female avatar + hair，forward strand pass，和 UE5.4 截图对照。
2. V1：guide/follow 插值 + 骨骼 skinning；转头不炸。
3. V2：guide 模拟 + SDF 碰撞 + VSP；deep shadow。
4. V3：continuous LOD、TAA、材质/光照对齐。

阻塞：UE5.4.4 patch 访问、`Asset/Maya` 导出、资产 license 确认。

拉取路径：`GPUOpenSoftware/UnrealEngine` 分支 `TressFX5-5.4.4`；只需要资产时用
sparse checkout 取 `Asset`，`Asset/Maya` 是 Maya 2018 源文件；如果仓库里已有
`.tfx`/`.tfxmesh`/FBX，可跳过 Maya 导出。完整 UE 源码构建仅用于跑 UE 参考画面。

## 物理路线

| 路线 | 做法 | 碰撞 | 适用 | 粗估工作量 |
| --- | --- | --- | --- | --- |
| P0 | 不模拟 | 无 | 静态肖像、纯展示 | 0 |
| P1 | 顶点着色器程序化摆动 | 无 | 呼吸/微风/转头时的轻微跟随 | 1–3 天 |
| P2 | guide 链 PBD/XPBD | 球/胶囊（头、肩） | 可动的肖像/半身，推荐起点 | 1–2 周 |
| P3 | TressFX 式 guide 模拟 | SDF + 胶囊 + LOD | 全身角色、接近 UE/TressFX | 1–3 个月 |
| P4 | 逐发丝模拟 | SDF/自碰撞 | Pragmata 级，20k 发丝 | 3 个月以上 |

P2 的最小闭环：

1. 只模拟 guide（约 512–2048 根，每根 16–32 点），render strands 由 guide 插值。
2. 约束：距离（拉伸）、弯曲、rest shape（保造型）、碰撞、摩擦；固定步长 + 2–4 substep。
3. 外力：重力 + 程序化风；阻尼分根/中/梢三段调。
4. 碰撞：头骨球/胶囊；肩部胶囊；先不做自碰撞。
5. 发根随头骨蒙皮；模拟在 GPU compute，回读为零；双缓冲写位置/切线。
6. LOD：远处减少 guide 数/迭代次数，或冻结。

## 难度判断

- “头发会动”不难：程序化摆动或简单弹簧链，1–3 天能看到效果。
- “头发像头发”难在四件事，而不是积分器：
  1. 保造型：没有 rest shape / 局部形状约束，PBD 会把发型拉塌。
  2. 碰撞：脸、耳、肩的穿插最毁画面；球/胶囊只能算第一版。
  3. guide → render strand 插值：滑移、夹紧、发根漂移都在这里暴露。
  4. 调参：头发很硬，stiffness/damping/substep 不稳会抖或变橡皮。
- P3 的生产级还多出 SDF 碰撞、LOD、motion vector、发丝阴影同步和工具链；
  这些是“月”级，不是“周”级。
- 对本仓库，物理的前置不是 solver，而是 groom 导入 + 发丝 forward pass +
  发丝阴影；没有这三样，物理没有可看的载体。

## 建议顺序

1. 静态：选定资产路线，先做 groom/ribbon 导入与 forward 渲染（P0）。
2. 轻量动效：P1 程序化摆动，验证发根绑定、风/重力方向和材质响应。
3. 物理 MVP：P2 guide PBD + 球/胶囊碰撞，先只做头部肖像。
4. 生产化：P3 的 SDF/LOD/阴影/运动矢量，按角色需求再上。
5. 逐发丝（P4）不作为近期目标。

## 待定

- 资产路线：已选 TressFX 5.0（待确认 UE5.4.4 patch 访问与资产 license）。
- 运行时基座：TressFX 4.1 standalone（建议）还是直接 port 5.0 UE plugin。
- 第一阶段是否只做静态肖像（V0），还是 V0+V1 一起。
- 物理范围：只做肖像微动（P1）还是角色级（P2/P3）。
- guide 数量、每根点数、sim rate（60/120 Hz）与 substep 数。
- 碰撞代理：球/胶囊起步，何时上 SDF。
- 是否保留 guide 数据；如果先烘 cards，是否同时留一份 guide 侧车。

## 参考

- [Capcom RE:2023《BIOHAZARD RE:4 の髪の毛解説》](https://docswell.com/s/CAPCOM_RandD/K6Y844-RE2023)
- [CEDEC2026《Pragmata》头发/表情开发事例](https://area.autodesk.jp/case/game/pragmata/)
- [UE Groom Strands](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-strands-in-unreal-engine)
- [UE Hair Card Generator](https://dev.epicgames.com/documentation/en-us/unreal-engine/hair-card-generator-for-grooms-in-unreal-engine)
- [AMD TressFX 4.1（MIT，Vulkan/DX12 standalone）](https://github.com/GPUOpen-Effects/TressFX)
- [AMD TressFX 5.0 for UE5 更新说明](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/)
- [TressFX 5.0 Developer Guide（PDF）](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf)
- [MetaHuman 许可](https://www.metahuman.com/license)
