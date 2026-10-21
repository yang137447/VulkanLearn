# GASP P4 轨迹与特征执行记录

日期：2026-10-18。本文是证据记录，不替代
[当前契约](../contracts/animation-motion-features.md)。

## 本轮范围

按用户批准继续 P4；C# 保持高层状态机，原生负责求值、历史和特征。
没有切换到 UE 动画蓝图，没有修改原 UE 项目，也没有把 P5 搜索混入本轮。
正式描述位于资源仓库 `Maps/SC_gasp/MotionMatching/PS_gasp.json` 与 `DB_gasp.json`，
派生数据进入 Generated。P3 正式角色控制器及其源贴图/关卡保持现有行为。

落地 60 Hz 实际根/姿态历史、固定窗口向后差分、当前移动规则的未来预测；
版本化 42 维 schema、稳定成员/帧顺序、总体归一化统计与 BLAKE3 缓存。
新增 Motion 开关、青色历史/橙色预测/绿色关节点、骨骼预览 raw 特征表，
以及片段/帧浏览 HTML（raw/normalized/权重和俯视图）。

## 数据验收

正式源有 12 个显式成员。Idle、Walk、CrouchIdle、Run、Sprint、Fall 使用循环延拓；
左右 Turn、WalkStart/Stop、JumpStart/Land 使用非循环完整窗口排除。
30 Hz 采样产出 **1261 帧 × 42 维**。

- identity：`6c80a0fd4d83bd3a70a49ac7d490d0b2d8a81b21fb174753ef3da5b321d7a0e1`
- payloadBLAKE3：`86b42db98794411e1b8af96f3caebb54fe8d170eaa6d2e9be43cf0d88da02766`
- 正式缓存：`Generated/MotionMatching/<identity>.json`
- 初次/命中报告：`artifacts/gasp-p4-db-first.json`、`gasp-p4-db-hit.json`
- 隔离验证：`artifacts/gasp-p4-db-verification/verification.json`

隔离副本冷构建与正式库身份、payload 一致；后续 verified-hit 通过。
篡改 payload 后正确重建并恢复摘要；源版本 2 被拒绝；
schema 权重变化、成员倒序分别改变身份，成员输出保持新顺序。
隔离验证不修改正式输入。模块另覆盖 FBX 字节变化、路径逃逸和显式 loop 要求。

各成员均有完整窗口：Idle 300、Walk 120、CrouchIdle 300、TurnL/R 各 36、
WalkStart 75、WalkStop 60、Run 75、Sprint 60、JumpStart 46、Fall 100、Land 53。
合计 1261；没有以空成员冒充已构建的动作帧。

## 渲染与浏览验收

`capture_gasp_p4.py` 创建独立圆周输入 C# 夹具，通过正式
Character.Move → Jolt → 原生 in-place 求值 → 历史 → UI 叠层链路，
在单/双线程分别采集。不是手工按键或 P3 状态机转向录像。
输出 `artifacts/gasp-p4-trajectory-fixed/circle-t1.bmp`、`circle-t2.bmp`，
完整 BMP、空 stderr、config 原字节恢复均通过。

实图显示曲线青色实际历史、橙色当前意图预测、绿色关节及 Query ready。
首次实图发现 Motion 标签匿名文字丢失、右下状态栏收缩成逐字竖排；
改为显式 span 与固定响应宽度，重采集确认可读。
RmlUi 在 960×540、1280×720、1920×1080 检查标签、状态宽高及视口内边界。
同批 console 命令开启叠层并截图时，checkbox 可能仍是前一帧状态；
不以这张图证明 checkbox 勾选回显。值请求、静默刷新由模块测试覆盖。

`artifacts/gasp-p4-database.html` 为最终自包含浏览器。
Playwright 实际选择 Walk、frame 15，显示 t=0.5 s、120 行、42 通道，
raw/normalized 值和画布同步更新。1280×850 画布 596.5²，
检测到 2706 个橙色和 1085 个绿色像素；390×844 画布 351²。
两个尺寸均无横向溢出，console 为零错误/警告。
最终截图为 `output/playwright/gasp-p4-final-desktop.png`、`gasp-p4-final-mobile.png`。
临时验收 HTTP 服务不属于交付依赖，HTML 可直接打开。

所有截图进程只由工具终止，不构成正常 teardown 证据。
原生编辑器控件没有真实鼠标操作验收；浏览 HTML 的选择/输入属于真实浏览器验收。

## 测试与构建

动画套件完整 61 项连续两遍通过，包括根平移计入关节速度、
转向 SE(3) 正负循环、非循环边界、均值/方差、缓存身份、历史帧率重采样与切换失效。
脚本专项新增真实 CLR/Jolt 的暂停/Stop/冻结快照及碰墙实际历史测试。
全量脚本首次出现一项提示词大小写断言失败；界面文案由 query 改为 Query 后，
同步断言，运动值检查不变。
新增 UI 布局用例首次误用默认 MI 页（关卡视口隐藏），改为显式回到关卡页，
保留原布局边界断言。

Debug 全目标与 Development main 构建通过。
视口与叠层选择集 43 项通过；World/Graph 事务 runtime gate 退出码 0，
日志确认 rollback/resize/M_ 交错/退休及正常 teardown。
40 个正式场景 schema 零失败；65 条架构边界审计通过。
其余最终全量回归与 C# OFF 结果记录于本文后续验收段。

构建时曾因旧 script_tests 在链接前启动而锁住可执行文件；
等旧进程完成后重新链接通过，没有为此修改业务代码。
MinGW UI 大源文件超过 COFF section 限额，沿用仓库已有 `-Wa,-mbig-obj`，
仅应用于该翻译单元及其测试编译。

## 明确限制与后续

历史是物理后 GT 帧观察插值，并非 Jolt 每子步原始采样。
预测保持当前 heading/水平意图，空中积分重力；不预测未来碰撞、台阶或移动平台，
碰墙预测线允许穿墙。硬切附近的姿态速度窗口无效，等待有效窗口后再发布 query。
没有性能规模承诺，当前固定库和至多八个活动组件用于首版开发验收。

数据库归一化 API 与浏览器已使用冻结统计；运行时目前发布 raw query，
尚无正式数据库绑定、搜索、连续性偏置、混合、IK、曲线/notify 执行。
不将技术数据/渲染验收扩大为 UE 行为或内容使用范围确认。

下一阶段 P5 先接入数据库与查询的同版本绑定、精确加权距离基线和稳定决胜，
再做候选筛选/重选节流/连续性，并以转向、起停和切换调试证据验收。
当前仍由 P3 C# 状态机请求固定片段。

## 补充验收

原 UE 输入清单重新只读捕获于 `artifacts/gasp-p4-original-inventory.json`；
3902 个输入的清单与 P0 完全相同，inputBLAKE3 仍为
`8f2fba8d398635082bd06428d6d971c828573f9613c4cd905a8b9fc204e40afe`。
这是限定盘点范围内的字节一致证据，不扩大为未盘点目录的完整磁盘审计。

Development（RelWithDebInfo、C# ON、Validation OFF）另行通过同一生产链路
单/双线程轨迹采集：`artifacts/gasp-p4-development-trajectory/capture.json`，
config 恢复通过、stderr 为空。C# OFF Release main 构建也通过。
最终检查另修复关闭/离线时空状态栏的背景残留，显式隐藏并补充状态断言。
该小修不改变历史、数据库或预测数学。
修正后关闭状态的原生实图为 `artifacts/gasp-p4-motion-disabled/disabled.png`，
确认 Motion 标签可见且没有空状态背景残留；stderr 为空。

最终全部构建完成后，动画 61 项、脚本 316 项连续两遍通过；
最终 UI 修正后的完整 653 项也连续两遍零失败，每遍 648 通过、
5 项符号链接环境跳过。机器报告分别为
`artifacts/gasp-p4-animation-full-{1,2}.json`、
`gasp-p4-script-verified-{1,2}.json`、`gasp-p4-ui-verified-{1,2}.json`。
Debug main/UI、Development main 与 C# OFF Release main 均完成最终链接，
文档校验、65 条边界规则和空白检查通过。
P4 技术数据及渲染验收通过；原生编辑器的手工操作矩阵仍保留为未验收项。
