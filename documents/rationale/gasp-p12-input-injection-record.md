# GASP P12 输入验收记录

状态：执行记录，非当前契约。记录 `injectinput` 合成 action，以及
`--focus-window` 下的真实 OS `F` 键路径；两者不能混为一谈。

## 参照 UE

UE 有 `UPlayerInput::InjectInputForAction`，用于自动化/测试向玩家输入注入
action，不依赖 Slate 当前的键盘焦点。VL 采用同样的边界：

- `InputSubsystem::InjectAction(action, pressed)` 维护合成 action 集合；
- `UpdateActionState` 在 SDL sample 之后把合成 action 合并到
  `actionState.gameplay.actions`，产生 held/pressed/released 边沿；
- 不伪造 `SDL_GetKeyboardState`，不改鼠标状态；
- 真实 OS 键盘路径仍要求 `SDL_GetKeyboardFocus() == window`。

## 命令

```text
injectinput <action> <down|up>
```

- 队列到 `RuntimeCommandType::InjectInput`；
- 执行端调用 `InputSubsystem::InjectAction`，日志
  `Input injected: <action> down/up`；
- 不写资产、不改变 World generation。

## 验收

场景 `SC_gasp_traversal_input.json` 关闭 `TraversalProbe`，由合成
`Interact` 触发 traversal：

```text
--initial-scene Maps/SC_gasp/SC_gasp_traversal_input.json
--console-command "injectinput Interact down"
--console-command-delay 120
```

日志：

```text
Input injected: Interact down
P8_TRAVERSAL_REQUEST grounded=True pos=0,90,-80
P8_TRAVERSAL_HIT distance=40 point=0,60,-120
P8_TRAVERSAL_START height=100 action=M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot
P8_TRAVERSAL_END pos=0,190,-202 grounded=True
```

证明 `Interact` action → C# `Input.WasPressed` → `RequestTraversal` →
动作完成的路径可被自动化驱动。

## 真实 OS 键盘

新增 `--focus-window`：显式调用 `SDL_SetWindowFocusable(true)` +
`SDL_RaiseWindow`，让自动化启动的窗口取得键盘焦点；不改变默认启动行为。

仓库与独立包都用真实 `keybd_event(VK_F)` 发送 F：

```text
仓库: P8_TRAVERSAL_REQUEST → HIT → START → END pos=0,190,-202
包:   P8_TRAVERSAL_REQUEST → HIT → START → END pos=0,190,-202
```

包 `artifacts/gasp-p12-input-package-01`（1723 文件）在
`--focus-window` 下收到真实 F，后续重复 F 因已到顶而 `REJECT no-hit`，
符合预期。

## 限制

- 合成 action 不是 OS 事件；真实证据来自 `--focus-window` + 真实 `F`。
- 只验证 `Interact`；`Jump`/`MoveForward` 轴、鼠标视角未测。
- `--focus-window` 是显式自动化 flag，不代表正常启动会抢焦点。
- 真实输入矩阵仍缺 WASD/鼠标/手柄和长时交互。

## 证据

- `artifacts/gasp-input2.log/.err`
- `artifacts/gasp-realinput.log/.err`
- `artifacts/gasp-p12-pkg-realinput2.log/.err`
- `artifacts/gasp-p12-input-package-01.log`
- 场景 `Maps/SC_gasp/SC_gasp_traversal_input.json`
