# Hair shape-preservation engine A/B (2026-11-14)

## 目的

验证 M0-011 的 `δ1/L=0.01` 甜点在真实 2k-guide 资产上的观感，
并与当前配置、以及更软的 `k=0.02` 对照：

- 静态：无风时造型不塌。
- 风下：有可见、方向正确、不过冲的摆动。
- 回正：风停后回到静态造型附近。

## 资产与场景

- 资产：`Maps/SC_tressfx_female/Meshes/TF_female_hair.json`
  - `simulationGuideCount=2000`
  - `bendStiffness=0.892`
  - `damping=0.0375`
  - `iterations=8`
  - `rootFixedPoints=2`
  - `vspCoefficient=0`
  - `globalShapeEffectiveRange=1.0`
- 场景：`Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json`
  - `MainWind` 方向 `+X`，测试风强度 `2000`
  - 静态对照时临时把强度改为 `0`
- 客户端：`build/bin/main_editor.exe --initial-scene ... --dev-ui`

## 三档配置

| 档位 | `globalShapeStiffness` | 说明 |
| --- | ---: | --- |
| 当前基线 | 0.0308 | 已有资产配置，约 τ=0.53s |
| M0-011 甜点 | 0.043 | `k=1-exp(-dt/τ)`，τ≈0.38s |
| 更软 | 0.020 | 风感更强，约 τ=0.82s |

## 截图

- 基线风下：`artifacts/hair-ab-20261114/baseline-k00308-settled.png`
- `k=0.043` 风下：`artifacts/hair-ab-20261114/k0043.png`
- `k=0.020` 风下：`artifacts/hair-ab-20261114/k0020.png`
- `k=0.020` 静态（风强度 0）：`artifacts/hair-ab-20261114/k0020-static.png`

## 初步观察

- `k=0.020` 静态：造型完整，头发自然垂落，脸没有被压塌；说明
  authored rest + global shape 的静态保持成立。
- `k=0.020` 风下：摆幅最大，头发明显向 +X 偏移；需要继续确认
  风停后的回正是否在可接受时间内。
- `k=0.043` 风下：摆幅比 `0.020` 收，静态轮廓更稳；更接近
  M0-011 `c001` 的“静态稳、风感中等、回正 0.3–0.4s”。
- `k=0.0308`：介于两者之间，作为现有基线保留。

## 待确认

- 用户选择哪一档作为默认。
- 引擎内风停回正时间尚未逐帧测；M0-011 的 0.34–0.42s 是 CPU
  原型值，不能直接当客户端实测。
- 最终需恢复 `MainWind` 强度并写回选定 `k`。
