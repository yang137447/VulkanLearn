# GASP P14 Traversal Montage 运行时记录

状态：阶段 A 中间记录，非当前契约。Montage 的 section/notify **名称**来自
UE，时长来自 UE Montage/序列；notify **时间**目前是近似值，不是 UE 资产时间。

## UE 事实

UE Python 检查：

```text
AM_M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot
  sections = [Default], slot = DefaultSlot, length = 2.0
AM_M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot
  sections = [Default], slot = DefaultSlot, length = 2.2
AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot
  sections = [Default], slot = DefaultSlot, length = 1.667
```

`AnimSequence` notify track 名称（顺序来自 audit）：

```text
Mantle: Footstep Left, Footstep Right, Handstep
Vault:  Footstep Left, Handstep Left, Handstep Right
Hurdle: Footstep Left, Footstep Right, Handstep Left, Handstep Right
```

`AnimSequence.Notifies`/`AnimMontage.Notifies` 在 Python 中是 protected，
无法导出精确时间。

## VL 实现

`GaspCharacter` 增加：

- `traversalMontage`：`AM_` + clip id；
- `traversalNotifies`：按 clip 类型映射的 UE notify 名称；
- `P8_MONTAGE_START/END` 日志；
- `P8_MONTAGE_NOTIFY` 日志；
- notify 时间暂用 `duration * (index+1)/(n+1)` 均分。

运行证据（Mantle）：

```text
P8_MONTAGE_START id=AM_M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot section=Default notifies=3
P8_MONTAGE_NOTIFY name=Footstep Left t=0.50
P8_MONTAGE_NOTIFY name=Footstep Right t=1.00
P8_MONTAGE_NOTIFY name=Handstep t=1.50
P8_MONTAGE_END id=... section=Default
P8_TRAVERSAL_END pos=0,190,-202 grounded=True
```

## 限制

- 不是从 UE Montage 资产读取的 notify 时间；目前是均分近似。
- 只有 `Default` section；没有 section 跳转/链接。
- root motion 策略仍在 C# traversal 分支，不在 Montage 数据内。
- 没有 NotifyState 的起止窗口，MotionWarping window 还无法挂接。
- 这是阶段 A 的可运行脚手架，不是完整 AnimMontage 运行时。

## 下一步

1. 通过 UE C++/Reflection 读取 `AnimMontage.Notifies` 的精确时间；
2. 引入 Montage 资产 JSON（section/link/notify/root motion policy）；
3. 把 notifyState 起止接到 MotionWarping window。

## 证据

- `artifacts/montage-sections.json`
- `artifacts/montage-props.txt`
- `artifacts/gasp-montage-mantle.log/.err`
