# GASP P0 启动记录

日期：2026-10-18。本文是证据记录，不是实现契约。
执行目标见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)；
当前工具边界见 [动画预检工具](../contracts/animation-preflight-tools.md)。

## 本轮结果

**P0 部分完成，未通过整阶段验收。**
已交付依赖预检、只读项目清单及数值回归。
尚未取得并锁定 GASP 项目，因此没有 GASP 导出、UE/VL 参考帧对照，
也没有 VL 骨骼动画画面。P1～P9 没有声明完成。

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 依赖来源 | 已锁定 | ozz-animation 0.16.0，commit `6cbdc790123aa4731d82e255df187b3a8a808256`；tarball SHA256 `ef52ed59de92cfbe352417683a254a4fdcab2dffd92967df680f5194bdd8332a` |
| MinGW 兼容 | 已通过 | GNU 15.1.0 / C++17；独立 Release 的采样/离线库构建；根 Debug 的工具与测试构建 |
| 真实片段采样 | 已通过 | 依赖自带 `pab_walk_raw.ozz`，67 关节，时长 1.33333337 秒，含端点采样 121 次 |
| 原始关键帧对照 | 已通过 | 本机构建压缩片段后，模型空间关节位置最大误差 `0.0006899377913214266`，为未换算的源单位 |
| 数值/盘点模块 | 已通过 | 15 项根 GTest 连续两遍全部通过，无跳过 |
| 实际 UE 项目盘点 | 已通过工具验证 | `AsseEditorUITest` 的 273 项输入清单；这是非 GASP 项目，不作 GASP 基线 |
| CLI 边界 | 已通过 | 中文项目/报告路径，已有报告拒绝且字节保留，源项目内输出拒绝且未创建文件 |
| GASP 输入 | 未取得 | 限定范围调查中未定位到实际项目，不能以本地 ALS/Slay 替代 |
| GASP 资产/行为验收 | 未执行 | 版本、使用范围、角色/片段、数据库/schema/Chooser、曲线/事件与最小导出仍待实际项目 |

工具为默认关闭的可选目标；本轮本机 `build` 配置已开启
`VULKANLEARN_BUILD_ANIMATION_TOOLS=ON`，保留原有 Debug / C# ON 设置。
`main` 没有链接 ozz。没有修改引擎运行时、shader、正式场景或角色资产。
没有运行 VL 实窗、全量根 CTest 或三个 Vulkan runtime test。

文档分层、diff 空白检查与架构边界审计通过。
构建保留上游 GoogleTest 的 `<ciso646>` 弃用告警；未为消除告警改动第三方源码。

## 本地调查范围

- `D:/YYBWorkSpace/UnrealProject/` 的项目描述；
  其中 `SlayAnimationSample` 是另一示例，EngineAssociation 为 5.4。
- `D:/YYBWorkSpace/Project/`、`D:/YYBWorkSpace/Game/`、
  常用临时/下载/文档/桌面目录的有限深度项目查找。
- UE 各版本的最近打开项目记录，以及 Epic VaultCache/FabLibrary 的本地缓存；
  没有查询或输出账户凭据。
- 已核对安装的 UE 5.7.4 与 5.8.2 的 `Engine/Build/Build.version`。
  它们是可用工具链，不代表 GASP 所需版本已经确认。

这不是全磁盘不存在性证明。继续资产导出所需的最小输入是实际 GASP 的
`.uproject` 路径或可访问的项目包。Fab 页本轮直接访问返回 403，
未据此作任何资产许可结论，也未擅自以第三方项目源替代。

## 验收入口

从仓库根运行：

```powershell
cmake -S . -B build -DVULKANLEARN_BUILD_ANIMATION_TOOLS=ON
cmake --build build --target vl_animation_probe animation_preflight_tests -j8
ctest --test-dir build -R "^(AnimationSampling|AnimationDependency|UnrealProjectInventory)\." --output-on-failure
.\build\bin\vl_animation_probe.exe --output artifacts/gasp-p0/ozz-probe-review.json
```

最后一条使用尚不存在的新路径，得到骨架层级、121 帧关节位置、输入摘要及误差报告。
这是数据验收入口，不是 Vulkan 角色渲染入口。
UE 项目盘点命令见 [工具说明](../../tool/animation/README.md)。

本轮原始生成物位于引擎 gitignored 的 `artifacts/gasp-p0/`：

- `configure.log`、`build.log`、`build-final.log`：构建日志。
- `ctest-final-pass1.log`、`ctest-final-pass2.log`：最终两遍模块结果。
- `ozz-probe-final.json`：实际采样报告。
- `non-gasp-inventory.json`：实际非 GASP 项目的盘点结果。
- `中文清单.json`：CLI Unicode 路径检查。
- `boundary-audit.log`：架构边界结果。

上述生成物不入库；本记录保存可审查的数值和范围。
P0 的其余工作依次是实际 GASP 项目身份与使用范围核对、行为/资产矩阵、
最小角色与动画导出、坐标和骨架映射测量，以及 UE 对照片段。

## 后续：实际项目已取得

2026-10-18：用户提供 `D:/YYBWorkSpace/UnrealProject/gasp`，
并说明原项目不会改动。后续使用隔离副本完成固定 mannequin 和 12 段动画导出、
真实 UE Asset Registry 审计与源空间数值对照。
前文“未取得”是本记录初轮执行时的历史状态，保留而不覆盖。
最新范围、验收入口、失败记录和 P0 余项见
[P0 资产基线记录](gasp-p0-asset-baseline-record.md)。
