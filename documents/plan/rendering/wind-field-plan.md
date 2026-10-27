# 自研风场（Wind）计划

> 状态：**计划**（非契约，2026-10 定稿草案）。本文只记录决策与拆分，不描述当前实现。
> 当前行为以 `documents/contracts/` 与源码为准。

## 1. 背景与目标

现状是两套割裂的风：

- 头发：`simulationDefaults.wind` 是资产级、位置无关的单个向量，写入 TressFX 风锥参数。
- 植被：`.stsdk` 里带 SpeedTree 的风曲线（分支、阵风、物种级参数），挂在每个物种的
  `SpeedTreeWindProfile` 上，由资产决定。

问题：SpeedTree 的风参数实际是**用户调的响应系数**，而风本身只有一套。风场不应该跟物种绑定。

目标：

1. 风是场景里的**全局对象** `wind`，像灯一样摆放，全场景唯一。
2. 风场与物种、材质无关。树、草、头发只读同一个风场，各自用**响应系数**决定自己怎么动。
3. 命名自主，不沿用 SpeedTree 的术语（`profile`、`branch1/2`、`shared`、`gust` 的 SDK 含义）。
4. 分两阶段：先标量全局风（P1），再升级为空间体素风场（P2）。

## 2. 核心决策

| 决策 | 内容 |
| --- | --- |
| 命名 | 场景对象 `type: "wind"`；World 成员 `Wind`；风场数据 `WindField`；阵风 `Gust` |
| 解耦 | 风场不进入任何材质或资产。资产只保存**响应系数**（`windResponse`），不保存风 |
| 时间 | 风时间按渲染帧固定步长推进（每帧 1/60 s），不用墙钟。其他动画按帧率取数据，两者解耦 |
| 单一来源 | 每个场景只有一个风对象；场景没有放 `wind` 时，使用引擎默认值（无风） |
| 植被迁移 | 植被不再读 `.stsdk` 的风曲线作为风场，只读 `Wind` 与自身的响应系数 |
| 编辑器 | TF 资产 Tab 可放置一个本地 `wind` 组件用于调试，不写回场景 |
| 单位 | 存储统一为 **cm/s**（与项目厘米制一致）。显示层只提供两种单位：cm/s 与蒲福风级（0–12）。换算只发生在显示与输入时，不改变存储值 |
| 风级输入 | 选择风级时存入该风级区间的**中值**（如 3 级存 440 cm/s），显示时由存储值反查风级，保证往返不跳档。风级 12 的中值取 3270 cm/s 的下界 |

## 3. 数据结构

### 3.1 场景对象（P1）

```json
{
  "type": "wind",
  "name": "MainWind",
  "direction": [1, 0, 0],
  "strength": 200.0,
  "gust": { "amplitude": 0.3, "frequency": 0.5, "turbulence": 0.2 }
}
```

- `direction`：作者向量，归一化后使用；长度不进入方向，只用 `strength`。
- `strength`：厘米/秒，表示风的基准幅值。
- `gust`：可选。`amplitude` 为相对强度 `[0,1]`，`frequency` 为赫兹量级，`turbulence` 为噪声扰动。
- 未声明 `gust` 时阵风关闭。

### 3.2 响应（资产或放置）

每个受风对象（植被、头发）持有一组**响应曲线**，**不包含风本身**。风强度由新风场按位置与时间计算，
响应曲线把这个强度映射为该对象的响应系数：

- 响应曲线：`strength → response`，横轴为风场计算出的风强度，纵轴为响应系数。采样点数与
  SpeedTree 一样取 20 点，便于从现有数据迁移。
- 响应曲线按**响应项**组织，而不是按物种/分支：`bend`（弯曲）、`oscillation`（摆动）、
  `speed`（摆动速度）、`flexibility`（整体柔性）、`turbulence`（扰动）。
- 物种差异通过**各自的曲线数值**表达，风场本身不带物种信息。
- `phaseOffset`：可选相位偏移，用于同一风场下的错相。

头发的 `simulationDefaults.wind` 从资产中移除，改由场景 `wind` 提供。

### 3.3 GPU 侧（P1）

- 一个 UBO 结构 `WindFieldGPU`：方向（vec4）、强度与阵风参数（vec4）、帧时间（float，固定 tick 累加值）。
- 植被与头发的 draw 共享同一个 UBO，不再每个物种一份 `SpeedTreeWindStateGPU`。
- 具体偏移与 std140 布局在实施阶段由 `BaseStructs.h` 对照确定，必须有 `static_assert`。

### 3.4 空间风场（P2）

- 体素网格 `WindVolume`：世界包围盒内的三维向量网格，分辨率与范围作者指定。
- 数据来源两类：离线烘焙（由场景或 DCC 生成）和运行时平流（沿 `WindField` 的标量风推进）。
- 采样：顶点着色器在构件位置三线性采样，得到局部风向量，替代 P1 的全局常量。
- 参考结构：VLM（体素网格）。P2 在 P1 验收后单独立项，不在本轮实施。

## 4. 阶段拆分

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| P0 | 固定风 tick + `SC_speedtree` 基线截图 | 同一帧两次截图逐字节一致 |
| P1a | 场景 `wind` 对象：schema、校验、World 成员、固定 tick 推进 | 场景无 `wind` 时行为与 P0 基线一致 |
| P1b | 植被改读场景 `wind` + 物种响应系数；移除物种风曲线的风驱动部分（当前仍沿用每物种 208 字节 GPU 状态，未合并为单一 `WindFieldGPU`） | 零风门 + 视觉门（见 §6.2） |
| P1c | 头发改读场景 `wind`；移除 `simulationDefaults.wind` 并在加载期拒绝旧字段 | hair 测试通过，零风/有风截图符合预期 |
| P1d | 编辑器：场景 `wind` 只读 actor（已完成）；TF Tab 本地 `wind` 组件调试与旧 SpeedTree 控件清理（未完成） | Tab 调节即时生效 |
| P2 | 体素风场 `WindVolume` | 单独立项 |

进度（2026-10-27）：P0、P1a、P1b、P1c 已完成；P1c 同时移除了资产 wind 并在四个女性发丝场景
补了场景 wind。P1b 的验收按 §6.2 执行（零风 `zero*` 截图逐字节一致，风场 `windy*` 与零风对比
约 13.3% 像素差异，P0 基线 run1/run2 逐字节一致）。P1d 只完成编辑器只读 Scene Wind actor；
TF Tab 本地风、`windstrength`/`windgust` 与 SpeedTree UI 控件清理仍待做。

P1b 的"前后效果不变"只能在**同一固定 tick**下比较。因为改了风的来源，像素级一致不一定成立，需要先定误差阈值（待你确认）。

## 5. 风险

- **时间源**：P0 已把墙钟改为固定 tick。如果 P1 之前未提交，需要保证 P0 的改动是单独可回退的。
- **共享状态**：`speedTreeWindProfiles` 目前是 RenderSystem 成员，并参与世界切换和 prune（`world.cpp:74-101`）。迁移到场景 `wind` 时，必须跟随 World 的 stage/swap 生命周期。
- **GPU 布局**：`UBOModel.speedTreeWind` 固定 208 字节（`BaseStructs.h`）。改布局会影响所有材质，需要统一的 static_assert 和 descriptor 检查。
- **双重风源**：迁移期间若资产风与场景风同时生效会叠加。P1 需要明确优先级，建议直接切换，不做兼容叠加。

## 6. 已定决策与参考

### 6.1 SpeedTree 风曲线的实际形状（参考，不再沿用）

源数据（`source/mesh/meshAssetTypes.h:147-206`）是**按风强度查表**，不是时间曲线：

- 每条曲线是 20 个采样点（`std::array<float, 20>`），横轴为作者风强度，纵轴为该响应的系数。
- 每个物种有 `shared`、`branch1`、`branch2` 三层分支，每层含 `bend`、`oscillation`、`speed`、
  `turbulence`、`flexibility` 五条曲线，以及 `independence`。
- `ripple` 层含 `planar`、`directional`、`speed`、`flexibility` 四条曲线和 `shimmer`。
- 阵风（gust）是 `common` 里的标量：`gustFrequency`、强度与持续时间的上下限、上升/下降系数。
- 风向由 `strengthResponse`、`directionResponse` 两个响应标量决定。

结论：这些曲线混合了"风强度 → 响应"与"物种/分支 → 响应"两个维度，所以风和物种绑在了一起。
新系统保留**风强度 → 响应**曲线这一维（§3.2），但横轴的风强度改由新风场计算，不再是作者填写的值；
物种与分支的差异只体现在各自曲线的数值上，风场不带物种信息。

### 6.2 决策

1. **P1b 验收（两道门，取代原"逐字节一致"要求）**：
   - 零风门：场景风强度为 0 时植被静止，无 NaN、无穿插。可严格验证。
   - 视觉门：风强度非零时植被按风场摆动，方向与强度一致。人工判断加截图，不做逐像素比较。
   - 原因：基线（P0）是 SpeedTree 风生效时截的，零风画面必然与基线不同；两套算法本身也不逐像素相同。
   - 前后差异量化：P1b 完成后，用同一场景截图与基线做差异统计，作为是否需要等效换算的依据，不预先定阈值。
2. **旧数据迁移**：不做兼容叠加。`simulationDefaults.wind` 与 SpeedTree 风曲线在 P1 中直接移除，
   资产若仍带这些字段，加载期报错并指向场景 `wind` 对象。
3. **阵风进入 P1**：P1a 的 `gust` 参数要实现，不只做静态风向量。阵风用 `frequency`、`amplitude`、
   `turbulence` 三个参数，取代 SpeedTree 的 `gustFrequency`、强度与持续时间上下限、上升/下降系数。
   持续时间与上升/下降系数不沿用，改为由 `turbulence` 决定的连续噪声，避免离散阵风事件。
