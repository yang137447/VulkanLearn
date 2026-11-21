# 布料展示关卡（Showcase Level）实现方案

状态：2026-11-17 第一版已实现（见文末“落地情况”），plan 文档。配套 UI 规范见
[studio-ui-design-system.md](studio-ui-design-system.md)。

## 定位

布料材质展示是**关卡级能力**，不是全局 UI 分支：

- 关卡声明自己是不是展示关卡、有哪些材质变体；
- 运行时只对声明过的关卡启展示 UI 与交互；
- 引擎不靠对象名前缀、路径子串之类隐式约定判断；
- 资产生成脚本是唯一写入方，场景 JSON 是唯一事实源。

## 数据所有权

场景 JSON 新增可选顶层 `showcase` 块（`SceneAssetValidator` 校验，
`WorldBuilder` 解析成 `WorldShowcase`）：

```json
"showcase": {
  "kind": "cloth",
  "defaultMaterial": "satin",
  "rotatePivot": [0, 0, -420],
  "cameraObject": "Camera",
  "materials": [
    {
      "id": "satin",
      "label": "Satin 缎面",
      "tag": "CLOTH · ASHIKHMIN 2000",
      "object": "ShowcaseModel_satin",
      "sheenHex": "#FFF8EB"
    }
  ]
}
```

约束：

- `materials[]` 至少 1 项、最多 8 项；`id` 唯一且匹配 `[A-Za-z0-9_-]+`
  （与现有 `SM_cloth_3d66Dress_slantedVelvet.json` 之类的变体命名保持一致，
  所以允许驼峰）；
- `object` 必须是当前场景里存在的 `mesh` 对象；
- `defaultMaterial` 必须命中某个 `materials[].id`；
- `rotatePivot` 是 cm 坐标，参与旋转的物体以它为轴；
- `cameraObject` 可选，缺省取场景里第一个 `camera`。

## 代码分层

| 层 | 位置 | 职责 |
| --- | --- | --- |
| 关卡数据 | `source/world/worldShowcase.h`、`worldBuilder.cpp`、`sceneAssetValidator.*` | 解析/校验/showcase 数据挂在 World 上 |
| 展示控制器 | `source/showcase/showcaseController.*` | 选中材质、可见性、拖动旋转、滚轮推拉、面板状态 |
| 视图绑定 | `source/ui/uiSubsystem.*` | 只做 `ShowcaseViewModel` → RmlUi 数据模型，不含关卡逻辑 |
| 组合根 | `source/engine/engineLoop.*` | 持有控制器；World 提交后把新 World 交给控制器 |
| 资产 | `tool/cloth-intake/build_cloth_showcase_stage.py` + `Maps/SC_cloth_showcase/**` | 生成场景、网格、材质和 `showcase` 块 |

`UiSubsystem` 不读 World、不判断场景类型；控制器不碰 RmlUi 细节。
两者之间只有 `ShowcaseViewModel`（纯值类型）。

## 运行时行为

- World 提交后，`EngineLoop` 把 `World::GetShowcase()` 交给控制器；
  控制器空则关闭展示 UI 并释放状态。
- 默认浏览态：面板关闭，只显示胶囊条与提示。
- 点击材质：切换 4 个 `ShowcaseModel_*` 的可见性（只留选中项），
  同时打开参数面板并更新标题/色值。
- 拖动（面板与胶囊条以外的区域）：绕 `rotatePivot` 转所有变体，
  水平 0–360°，垂直 ±10°。
- 滚轮：沿相机视线推拉，距离限制 300–1200 cm。
- 参数滑杆：写回材质实例的运行时覆盖（`f0`、`sheenColor`、
  `fiberRotation`），不改磁盘资产。

## 验收

1. 非展示关卡（任意现有场景）不出现胶囊条、不改变鼠标模式。
2. 展示关卡默认浏览态：单人台、面板关闭、胶囊条选中 `satin`。
3. 点 `Velvet`：只剩绒面人台可见，面板标题/副标/色值正确。
4. 拖动只转模特，不转相机；滚轮只改距离，不动朝向。
5. 关闭面板回到浏览态，材质保持。
6. `py -3 tool/validate-docs.py` 通过；场景加载无新增校验错误。

## 落地情况（2026-11-17）

已完成：

- 关卡数据：`showcase` 块解析/校验、`World::GetShowcase()`、4 个非法用例被拒。
- 控制器：`source/showcase/showcaseController.*`，材质可见性、绕 pivot 旋转、
  相机 300–1200 cm 推拉、面板开合。
- 视图：`ShowcaseViewModel` + `UiSubsystem::SetShowcaseModel`，RmlUi 不再读 World。
- 场景：主场景单模特 + 四个重合变体；四连排改为
  `SC_cloth_showcase_compare_3d66Dress.json`。
- 磨砂背板：`uiGlass.vert/frag` + 半分辨率 blit + 9-tap 模糊，矩形来自面板
  DOM 边界，面板关闭零开销（契约见 `documents/contracts/game-ui-stack.md`）。
- 非展示关卡零影响（截图比对：无胶囊条、无面板、鼠标模式不变）。

未完成（下一阶段）：

- 滑杆只显示、不写回材质实例；需要 MI 运行时覆盖通道。
- “与 PBR 对比”开关已按需求移除（对比直接点 `标准 PBR` 胶囊）。
- 键盘 1–4 切换。
- 关卡逻辑已可交给 C#：`OnUiEvent` + `Blueprint.Showcase` +
  `showcaseCommands`，见 `documents/contracts/csharp-blueprint-runtime.md`
  的“运行时 UI 事件”。主场景保持无蓝图（C# OFF 可跑），C# 版本是
  `SC_cloth_showcase_3d66Dress_csharp.json`。
