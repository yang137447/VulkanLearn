# 架构图

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/ue-lite-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

这里的 `.json` 是**图源**，`.html` 是它的产物。**图源是契约，产物不是。**
改架构请改 JSON 再重新生成 HTML；直接编辑 HTML 会在下次生成时被覆盖。

## 图与门禁的分工

archify 的 IR 拥有**拓扑事实**：组件身份、类型、owner、有向连接（仅正向）、
逐节点源码证据（钉 40 位 revision 的文件与行号）。它**不治理**禁止词、内容约束、
文件存在性与调用方向 —— 那些留在 `tool/ue-lite-boundary-audit.ps1`。
完整边界见 `documents/README.md`。

## 现有图

| 图源 | 类型 | 说明什么 |
| --- | --- | --- |
| `editor-frame-sequence.json` | sequence | 一帧内从平台事件到上屏的顺序：UI 输入独占、GT 稳定帧边界、快照发布、可见性在快照构建处收口 |
| `world-graph-transaction-lifecycle.json` | lifecycle | World/RenderGraph 事务：隔离准备、提交前身份校验、无抛提交、GPU epoch 退休 |

## 重新生成

需要第一次 `finalize` 时使用 `--repo-root`：archify 按钉死的 revision 校验每个
`sources` 的文件是否存在、行号是否在范围内。仓库只存在于本地时（commit 未推送），
证据按本地 git 对象校验，这是可接受的。

```powershell
$cli = "$env:USERPROFILE\.dsh\profiles\web\node_modules\@tt-a1i\archify-dsh\skills\archify\bin\archify.mjs"
node $cli finalize sequence  documents\contracts\diagrams\editor-frame-sequence.json `
     documents\contracts\diagrams\editor-frame-sequence.html --repo-root . --quality showcase --json
node $cli finalize lifecycle documents\contracts\diagrams\world-graph-transaction-lifecycle.json `
     documents\contracts\diagrams\world-graph-transaction-lifecycle.html --repo-root . --quality showcase --json
```

**非零退出永远不算成功。** 失败时读收据里的 `diagnostics[]`，按 subject 局部修复后重跑同一个
`finalize`；不要替换整个候选、不要把失败当通过。四个 gate 全部 `pass` 才算交付：
`validate` / `deliver` / `check` / `browser-check`。

## 漂移检测

`py -3 tool/validate-docs.py` 会检查：

- 每张图都能从 `documents/routing.md` 到达（否则报孤儿图）；
- 图的 `meta.repository.revision` 与当前 `HEAD` 一致。

**后者是这套机制的重点**：图钉在某个 revision 上，代码前进后校验失败，提示你重新核对图是否
仍然成立 —— 而不是让人以为图还是对的。重新生成后 revision 会更新到新的 `HEAD`。

## 入库边界

- **入库**：`.json`（图源）、`.html`（产物）、`.finalize-summary.json`（精简回执）、
  `.delivery.json`（规范/产物哈希收据与回归追踪）。
- **不入库**：`.finalize.json` 与 `.browser-check.json`（见 `.gitignore`）——
  体积大、可重新生成，且 `finalize.json` 内嵌绝对路径。

## 已知限制

- 图只覆盖它声明的 slice。未画的模块不代表不重要，只代表这张图不主张它。
- 图源里的 `sources` 只证明"该位置可见的事实"，不证明运行时行为。
- `col` 只有 0..4 五列，lane 最多 4 行（其中第一行 id 必须是 `main`，最后一行 `terminal`）——
  复杂状态机需要拆成多张图，而不是压缩到一张。
