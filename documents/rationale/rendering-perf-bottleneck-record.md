# 渲染性能瓶颈与优化记录（2026-10-23）

状态：**证据归档**（rationale 层，不是契约）。记录本轮实测数字、已落地的优化、
以及尚未实施但已评估的方案。任何结论都能在文中对应的提交或 `artifacts/` 日志里复核。

## 1. 测量手段

- GPU：引擎内 timestamp 查询（`documents/contracts/gpu-pass-timing.md`），
  `--gpu-stats` 周期打表、console `gpustats`、`--perf-jsonl <path>` 逐秒 JSONL。
- CPU：进程 `TotalProcessorTime` 采样 + 帧时间（`perf json` 的 `cpuMs`）。
- 画面：固定帧 `screenshot` + 像素差；冻结姿态用 `animation seek` + `animation pause`。

## 2. 本轮定位到的三个问题与结果

| 问题 | 根因 | 修复 | 效果 |
| --- | --- | --- | --- |
| 动画 CPU 成本 ~12.4 ms/帧 | `BuildSkinPose` 每帧对所有顶点做 CPU LBS 只为求包围盒 | 预计算逐骨骼绑定空间 AABB，姿势时只变换 8 角点（`skeletalPlayback.cpp`） | 12.4 ms → ~0.5 ms；skinned 场景 54 → ~83 FPS |
| 4 级阴影各画一遍全部 caster | `DrawShadowScene` 没有逐级联过滤，draw list 原样复用 | `shadowCasterCull`：级联 light view-proj 阴影盒 vs 当前世界 AABB，加方向光 receiver-XY 预筛 | GPU 12.09 → 4.71 ms；79 → 187 FPS；只切剔除时冻结帧逐像素相同 |
| 每级联重扫全部 draw packet 收紧 Z 范围 | 投影只依赖光源朝向，四级完全相同 | 每帧缓存一次光源空间 AABB，逐级联只做 XY 过滤 + Z 合并 | 微秒级，当前规模测不出；移除随级联数增长的 N× 循环 |

另外一个已删除的历史坑：`IntersectsSplitFrustumFast`（相机 slice 剔除）曾被
`#if VULKANLEARN_SHADOW_TIGHT_Z_BOUNDS`（默认 0、全仓未定义）挡在编译外，
判据体积也不对（相机 slice ≠ 光源阴影盒），跨相机平面时还会退化为零宽。
`7cf2274` 修 CSM Z 界时删掉了调用，于是退化成"完全不剔除"。该函数与
`ComputeViewAabbFromWorldCorners` 已在 `b7f6d68` 删除。

## 3. 当前剩余预算（200 m 阴影距离、剔除开启、skinned 场景）

GPU 总计约 4.7–5.2 ms：

- `shadowCascade1` 2.3–2.4 ms（角色蒙皮绘制；c0/c2/c3 已降到 ~0.006 ms）
- `geometry` 2.2–2.6 ms（同一次角色绘制）
- 其余所有 pass 合计 < 0.1 ms

CPU 帧时间波动很大：空载 ~6.5 ms（约 153 FPS），有其他进程时 ~11.8 ms（约 85 FPS）。
**多数时候 CPU 先到瓶颈**，此时削减 GPU 时间不会提高帧率。

## 4. 已评估、未实施的方案：每帧一次预蒙皮（pre-skinning）

现状：同一批 23.5 万顶点在一帧内被蒙皮两次——`geometry` 与 `shadowCascade1`
各跑一遍骨骼混合的顶点着色器。判据：同一网格静态版每级阴影 0.05 ms，蒙皮版
2.4 ms，差 40 倍来自 vertex fetch + 混合，而不是光栅化。

做法：加一个 compute pass，每帧把顶点蒙皮一次写入世界空间缓冲（位置，必要时
法线/切线），`geometry` 与 shadow pass 直接读该缓冲。

- 收益：一次蒙皮替代两次，预期省 ~2 ms GPU；角色跨多个级联时不再随级联数线性增长；
  同一结果可复用于运动矢量与逐帧精确包围盒。
- 代价：显存 3–12 MB；需要"已蒙皮"顶点布局与对应 pipeline 变体（阴影只需位置）；
  compute→vertex barrier 与帧内顺序；若将来接入 blend shape / SpeedTree 风，
  必须在同一 pass 内计算，否则出现"预蒙皮没算风、几何里算了"的不一致。
- 结论：**只有在 GPU 受限时才有意义**；当前多数帧是 CPU 受限，优先级低于定位 CPU 瓶颈。

## 5. 下一步

1. 增加 CPU 分阶段计时（与 GPU pass 表对称）：输入/脚本、动画、头发 GT 模拟、
   世界快照发布、UI 视图模型、渲染录制与提交、present/等待。
2. 用该表定位 CPU 瓶颈，再决定优化项（预蒙皮、提交合并、UI 绑定频率等）。
3. 若之后要做 200 m 大世界演示，还需要把相机 `far_clip`（当前 1000–2000 cm）
   与阴影距离对齐，否则级联 2/3 覆盖范围内没有可见接收者。
