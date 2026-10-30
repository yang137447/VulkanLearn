# 契约层

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

这一层回答"现在的系统**是什么**"。它的可信度来自三条可执行的约定，而不是写作风格。

## 三条约定

1. **每个契约在开头 40 行内必须有一行 `> 状态：...`。** 这行是机读标记，让"这是契约"与
   "这是记录"在文件层面可区分。由 `tool/validate-docs.py` 强制。
2. **契约不得断言未来行为。** 校验器会拦下 `将(会)实现/支持/改为/引入/迁移`、
   `计划(实现|支持|...)`、`下一步(实现|计划)` 这类句式。<!-- validate-docs: allow-future-assertion -->
   注意：陈述**当前缺口**是允许的（"X 尚未实现"是在描述现状，不是承诺），
   引用计划文档来说明某个决定的出处也是允许的。
   需要引用被禁句式来解释规则本身时，在该行加 `validate-docs: allow-future-assertion` 标记 ——
   这是逐行显式豁免，不是整体放宽。
3. **契约必须能被路由到。** `documents/routing.md` 要么给它一条源码路径，要么在主题型契约表里
   列出它。未被任何路由覆盖的契约会被报成孤儿 —— 这是缺陷，不是提示。

## 与源码矛盾时怎么办

**以源码为准，并立刻修正契约。** 这是缺陷，不是"文档略旧"。修完之后用
`py -3 tool/validate-docs.py` 与 `pwsh -File tool/architecture-boundary-audit.ps1` 复验。

## 门禁规则怎么引用

`tool/architecture-boundary-audit.ps1`（64 条命名规则）是**可执行的架构契约**。
在契约或路由里引用规则时，写脚本里 `-RuleName` 的**字面值**，不要写概括或别名 ——
`tool/validate-docs.py` 会逐字核对，概括会失败。规则正文不要抄进文档，那是会漂移的东西。

注意该脚本整批扫描 `documents/`：**文档里不要写出被判为禁用的标识符字面量**，
否则描述这条规则的文档自己会被判失败。见 `documents/rationale/pitfalls.md` 的
`[boundary-audit-scope]` 条。

## 目录内容

| 文件 | 覆盖 |
| --- | --- |
| `vulkanlearn-architecture.html` | 正式架构总览（注意：它同时被门禁脚本按用词规则扫描） |
| `coding-guidelines.md` | 通用 C++ 约定与注释规则 |
| `testing.md` | 验证入口与"改动 → 验证"映射 |
| `game-ui-stack.md`、`ui-component-system.md` | RmlUi UI 栈与组件体系 |
| `material-instance-editor.md`、`material-param-authoring-and-reflection.md` | 材质实例编辑命令契约与参数 schema/反射 |
| `descriptor-imageinfo-management.md`、`gbuffer-channel-ownership.md` | descriptor 生命周期与 GBuffer 通道所有权 |
| `shader-build-cache.md`、`shader-hot-reload.md`、`shader-structure-and-material-function.md` | shader 缓存、热重载、结构与材质函数 |
| `material-mesh-pass-composition.md`、`csm-shadow-map-m1.md` | 材质求值/pass 组合与 CSM |
| `environment-update-scheduler.md`、`texture-asset-json-v1.md`、`asset-organization.md` | 环境更新调度、贴图资产格式、资源组织 |
| `vl-offline-rendering.md` | 离线渲染核心的所有权与边界 |
| `fracture-core.md` | 纯 CPU 平面/球面破碎、橡皮膜软壳、碎片挤出与体积校验 |
| `fracture-budget.md` | 全场景碎片预算账本、回收优先级与玻璃/气球降级策略 |
| `softbody-runtime.md` | `balloon` 场景对象、GT 橡皮软体、动态网格发布与爆裂软片 |
| `glass-fracture-runtime.md` | `glassPane` 场景对象、命中点局部 Voronoi 碎片、Jolt 凸包与落地二次碎裂 |
| `projectile-shooting-runtime.md` | 共享冲击世界、可见箭矢、发射输入与 `SC_fracture_playground` 试玩关卡 |
| `audio-runtime.md` | 32 voice 全局预算、块混音、等功率 pan、距离衰减与距离低通；WAV 资产加载、重采样与程序化回退 |
| `debug-runtime-commands.md` | 控制台调试命令 |
| `diagrams/` | 架构图的 IR 源与产物，见其 `README.md` |
