# UDS 复刻路线图

## 状态

- 类型：未来功能路线与执行计划
- 状态：初版范围评估，尚未进入实现阶段
- 更新时间：2026-08-07
- 目标对象：在 VulkanLearn 中复刻 UE Ultra Dynamic Sky / Ultra Dynamic Weather 的核心运行时体验
- 推荐目标：先完成 UDS + UDW 的视觉核心，不追求 Blueprint、编辑器和资产格式的一比一兼容

## 1. 目标与边界

### 1.1 复刻目标

目标不是移植 UE 的代码或资源，而是在 VulkanLearn 中建立等价的运行时能力：

- 可连续运行的 time-of-day 和太阳/月亮系统
- 稳定、可调的程序化天空和物理大气表现
- 2D 云层到体积云的可升级路线
- 雾、aerial perspective 和云遮挡
- clear、cloudy、foggy、rainy、storm 等天气状态及平滑过渡
- 降水、风、湿润材质、积水和基础地表反馈
- 低频或分帧收敛的 SH、环境 cubemap 和 prefilter IBL
- JSON 配置、运行时调试和质量档位

### 1.2 不在第一阶段复制的内容

- UE Actor、Component、Blueprint、Editor Details 面板和 Sequencer API
- UE/Niagara 资产格式以及 UDS 的付费代码、材质、纹理、声音和特效资源
- 多人同步、网络天气、开放世界分区和无缝旅行
- 复杂室内天气排除、局部天气体积和完整季节系统
- 完整的天空音频、雷声素材和后期镜头特效资产

这些能力可以在视觉核心稳定后作为独立扩展，不应混入第一条实现链路。

### 1.3 目标分档

| 档位 | 内容 | 估算 |
| --- | --- | ---: |
| 演示级 UDS Lite | 艺术化动态天空、昼夜、2D 云、简单高度雾、天气预设、基础雨、湿润和闪电 | 10–14 人周 |
| UDS 天空核心 | 物理大气、aerial perspective、单层体积云、云阴影、时域更新、动态 IBL | 24–34 人周 |
| 推荐目标：UDS + UDW 视觉核心 | 天空核心 + 天气状态、降水、风、湿润、积水、涟漪、雷暴和调试工具 | 38–55 人周 |
| 高完成度功能对标 | 多层云、局部天气、室内排除、积雪、尘土碎片、季节、存档、多人同步和工具链 | 80–120 人周 |

以上估算按一名熟悉本项目和 Vulkan 的高级图形程序员计算，包含基础集成、调试和性能验证，但不包含高质量内容资产制作。实际排期建议额外预留 25%–35% 缓冲。

## 2. 当前基线

### 2.1 已有能力

- `SkyParametersGPU` 已包含太阳、颜色渐变、地面颜色和太阳光晕参数
- 场景 JSON 已支持 `proceduralSky` 环境类型和程序化天空参数
- `skyToCubemap` 已能在 GPU 上把程序化天空写入 cubemap
- 已有 environment cubemap、SH 漫反射 IBL、prefiltered specular IBL 和 BRDF LUT
- Sky Pass 已使用 fullscreen triangle 和视线重建
- GBuffer 已有 `sceneDepth` 与 `gbufferVelocity`，可作为后续时域云雾的输入基础
- 当前工作区已有 SpeedTree 风状态和运行时风力控制入口
- Render Graph、材质反射、运行时控制台和 Tracy/NVTX 标记可以复用

### 2.2 主要缺口

- 当前天空模型是艺术化渐变加太阳盘，不是物理大气模型
- `cloudControls` 目前只是 GPU 数据预留，没有云层、云影或体积云 pass
- 当前没有独立的天气状态、天气预设和参数过渡系统
- 当前没有通用的 3D 噪声纹理或体积纹理资源链路
- 当前有运动矢量，但没有完整的 TAA、历史颜色、时域重投影和历史资源管理
- 当前没有通用 GPU 降水粒子系统
- 当前没有全局材质天气参数集合，wetness、puddle、snow 等不能统一驱动 PBR 材质
- 环境 IBL Phase 0 已完成 dirty、分帧预算、active/pending 提交和 GPU timestamp；后续天气系统直接复用该调度基础
- 当前没有面向天气调参的专用 UI，只能依赖 JSON、控制台和调试视图

### 2.3 必须优先处理的性能问题

旧程序化环境路径曾在单帧内全量录制 cubemap、SH 和 prefilter。Phase 0 已将该路径改为按代际和预算分帧更新，当前契约见 `documents/rendering/environment-update-scheduler.md`。

在接入 time-of-day 之前必须完成：

1. 背景天空和 IBL 派生资源解耦
2. Sky、SH、cubemap、prefilter 各自维护 dirty 状态
3. cubemap face、prefilter mip 和 SH 支持独立更新预算
4. 新环境资源完成前继续使用旧资源
5. 记录每个派生资源的完成进度和 GPU 时间

### 2.4 UDS 研究基准场景

第一阶段使用 `scenes/SC_uds_mountain_range.json` 作为大尺度户外背景板。

- 源资产：Gaea `mountain_range_01`
- 网格：513×513 顶点网格，524,288 个三角面
- 运行时尺度：500 米见方，约 87 米高差
- 纹理：4096×4096 Base Color、Normal、Roughness、AO 与原始 Height
- 用途：验证 Sky、IBL、Time of Day、height fog、aerial perspective、云影和天气能见度
- 约束：保留完整网格和原始纹理，不通过减面、裁剪或移除远景降低测试压力

该场景是环境系统集成基准，不替代后续用于材质 wetness、puddle 和
snow accumulation 的近景 PBR 校准场景。

资源验收状态（2026-08-09）：

- [x] 场景、模型、材质和纹理 JSON 通过运行时校验
- [x] Base Color 使用 sRGB；Normal 与 packed PBR 使用 linear
- [x] packed PBR 通道为 `R=roughness`、`G=metallic(0)`、`B=AO`
- [x] normal map R/G 方向与高度图梯度及当前 MikkTSpace 约定一致
- [x] 山地 OBJ 通过模型级 `importOptions.generateSmoothNormals=true` 生成缺失的平滑法线；Assimp 不再全局生成法线
- [x] OBJ、JPEG 和 PNG 资源由 Git LFS 管理
- [x] 派生 packed PBR 贴图可通过工具脚本确定性重建
- [x] 三次 World reload stress 和 300 帧 frame smoke 通过

## 3. 目标架构

### 3.1 数据流

```text
Scene JSON / Runtime Commands
            ↓
TimeOfDayController + WeatherController
            ↓
      WeatherState / SkyState
            ↓
        WorldSnapshot
            ↓
     SkyStateGPU / WeatherStateGPU
            ↓
Atmosphere LUT / Sky / Clouds / Fog / Precipitation
            ↓
      Environment Update Scheduler
            ↓
  environmentCube / SH / prefilter
            ↓
       PBR and post-process
```

### 3.2 责任划分

| 模块 | 职责 |
| --- | --- |
| `TimeOfDayController` | 游戏时间、日期、太阳/月亮方向、昼夜曲线和天体状态 |
| `WeatherController` | WeatherPreset、WeatherState、状态过渡、风、湿度、云量、能见度和降水强度 |
| `SkyAtmosphereRenderer` | transmittance、multi-scattering、sky-view LUT 和天空颜色 |
| `CloudRenderer` | 云密度、ray marching、云层合成、云影和时域重投影 |
| `FogRenderer` | height fog、distance fog、aerial perspective 和场景深度合成 |
| `PrecipitationSystem` | 雨雪粒子、风偏移、相机范围、遮挡和质量档位 |
| `SurfaceWeatherBridge` | 将湿润、积水、积雪和涟漪状态提供给材质/PBR |
| `EnvironmentUpdateScheduler` | environment cubemap、SH、prefilter 的 dirty、预算、分帧和资源切换 |
| `WeatherDebugView` | 参数查看、状态过渡、派生资源进度和性能信息 |

### 3.3 数据布局原则

- 长期天气和天空状态使用独立的 `SkyStateGPU`、`WeatherStateGPU`，不继续扩张 `UBOGlobal`
- CPU 只做状态计算、参数上传、dirty 标记和任务调度
- CPU 不采样天空、不执行 SH 投影、不生成 cubemap、不生成 prefilter
- lighting shader 不判断天气类型，只读取已经准备好的环境资源和天气结果
- Sky Pass、Cloud Pass、Fog Pass 使用明确的输入输出资源，不依赖隐式全局状态
- 所有天气效果必须有质量档位和可关闭路径

## 4. 分阶段执行计划

### Phase 0：环境链路稳定化（2–4 人周）

目标：让动态天空成为可持续更新的基础，而不是每帧全量重建环境。

- [x] 完成 environment cubemap、SH、prefilter 的职责拆分
- [x] 增加 `EnvironmentUpdateState` 和 dirty 标记
- [x] 实现 cubemap face、prefilter mip 和 SH 的更新游标
- [x] 支持旧资源在新资源完成前继续提供采样
- [x] 增加 compute-to-compute、compute-to-fragment 和 UBO 可见性验证
- [x] 用 Tracy/NVTX 记录每类环境更新的 GPU 时间
- [x] 验收 validation layer 未报告 layout、access 和 lifetime 错误

完成记录（2026-08-11）：

- 默认预算为每帧 1 个 cubemap face、1 次 SH、1 个 prefilter mip 和 1 次 commit
- 程序化 cubemap 与 prefilter 使用稳定 active image 加独立 pending image
- SH 每代际只计算一次，再把 144 字节结果广播到全部 swapchain Global UBO
- `--environmentstress` 由 `RuntimeTestHooks` 持有状态机，通过 CommandBus 验证连续参数 dirty、旧 active 资源窗口、稳定 descriptor 身份和四类 GPU timestamp
- 最终验收通过 `cmake --build build -j`、300 帧 smoke、3 次环境更新、3 次 UDS 场景重载、6 次 resize、6 次 render graph 重载和 2 次 HDRI 场景重载；全部退出码为 0，未出现 validation layer 错误
- 本机 Debug 构建最后一次环境样本为 cubemap `0.005888 ms`、SH `0.012288 ms`、prefilter `0.041728 ms`、commit `0.016320 ms`

### Phase 1：Time of Day 与天空控制（3–5 人周）

目标：完成可连续运行的昼夜系统，并保证光照方向一致。

- [ ] 新增 `TimeOfDayState`
- [ ] 支持时间倍率、暂停、跳转和循环
- [ ] 支持太阳方向曲线和主 Directional Light 同步
- [ ] 增加月亮方向、月相和基础星空
- [ ] 将太阳强度、颜色、曝光和环境强度纳入昼夜曲线
- [ ] 统一天空太阳、直接光、CSM 和阴影方向
- [ ] 添加时间和太阳方向运行时命令
- [ ] 验收日出、正午、黄昏、夜晚四个固定时间点

### Phase 2：物理大气与雾（5–7 人周）

目标：从艺术化天空升级到可解释的物理大气和场景空间雾效。

- [ ] 实现 transmittance LUT
- [ ] 实现 multi-scattering 或等价近似
- [ ] 实现 sky-view LUT
- [ ] 接入 Rayleigh/Mie 参数和太阳低角度变化
- [ ] 实现 height fog 和 distance fog
- [ ] 实现 aerial perspective 与 scene depth 合成
- [ ] 处理天空、雾、方向光和曝光之间的能量关系
- [ ] 增加大气 LUT 重建的 dirty 和分辨率质量档位

### Phase 3：云系统（2D 先行，体积云后置）

#### Phase 3A：2D 云层（2–3 人周）

- [ ] 增加云量、云层高度、覆盖率、软边和风向参数
- [ ] 支持云层 UV 漂移和多频噪声
- [ ] 支持太阳照明、边缘透光和基础银边
- [ ] 输出云遮挡率供太阳光和环境光使用
- [ ] 允许在低质量档位保持 2D 云层

#### Phase 3B：体积云（8–12 人周）

- [ ] 增加 3D 噪声/体积纹理资源链路
- [ ] 实现 weather noise、shape noise 和 detail noise
- [ ] 实现低分辨率云体 ray marching
- [ ] 实现深度感知上采样
- [ ] 实现时域重投影和历史颜色管理
- [ ] 实现云层光照、透射和银边
- [ ] 实现云阴影纹理或云阴影采样
- [ ] 支持云层高度、厚度、风速、密度和覆盖率
- [ ] 增加云体 debug view：密度、步进、重投影权重和阴影

### Phase 4：Weather State 与天气预设（3–4 人周）

目标：让所有环境效果由同一份天气状态驱动，而不是各自维护参数。

- [ ] 定义 `WeatherPreset`
- [ ] 定义 `WeatherState` 和 `WeatherStateGPU`
- [ ] 定义 clear、cloudy、overcast、foggy、rainy、storm preset
- [ ] 实现状态之间的平滑过渡和过渡曲线
- [ ] 统一驱动天空、云、雾、风、降水、IBL 和地表湿润
- [ ] 增加天气 JSON schema 或最小运行时校验
- [ ] 增加天气切换、强度和过渡时长控制台命令

### Phase 5：降水与地表反馈（8–12 人周）

#### 降水

- [ ] 建立通用 GPU 粒子或 billboard 降水系统
- [ ] 支持雨、雪两种基础粒子
- [ ] 支持风向、风速、重力和视距
- [ ] 支持相机跟随发射区域和远近质量档位
- [ ] 支持基础地面/天空遮挡
- [ ] 预留雾、尘土和碎片的共用粒子接口

#### 地表

- [ ] 增加全局 wetness、puddle、snow 和 ripple 参数
- [ ] 修改 PBR 材质的 roughness、specular 和 base color 响应
- [ ] 为材质提供可选 wetness mask 和 puddle mask
- [ ] 增加地面雨滴、积水和涟漪的基础表现
- [ ] 评估是否需要屏幕空间湿润 mask 或独立 accumulation texture
- [ ] 验收天气过渡时天空、云、雾、地表反馈方向一致

### Phase 6：雷暴、工具和性能收敛（5–8 人周）

- [ ] 实现闪电事件、曝光响应和局部光照变化
- [ ] 增加 Weather Debug View
- [ ] 显示当前天气状态、时间、风、云和降水参数
- [ ] 显示环境 IBL face/mip/SH 更新进度
- [ ] 增加低、中、高、电影级质量档位
- [ ] 增加运行时开关：云、雾、降水、湿润、闪电和 IBL 更新
- [ ] 建立固定天气场景和固定时间点截图回归
- [ ] 使用 Tracy/NVTX 验证 CPU/GPU 时间、显存和更新抖动

## 5. 推荐里程碑

### M0：动态环境基础

完成 Phase 0。

验收：程序化天空参数变化不会导致每帧无界的 IBL 重建，背景和 IBL 可以延迟收敛且没有 Vulkan 同步错误。

### M1：可玩的 UDS Lite

完成 Phase 1、Phase 3A 和 Phase 4 的最小子集。

验收：可以在 JSON 或控制台中运行：

```text
晴天 → 多云 → 阴天 → 雾 → 小雨 → 暴雨 → 雨后放晴
```

天空颜色、太阳方向、主光阴影、云量、雾和 SpeedTree 风动能够同步变化。

### M2：UDS 天空核心

完成 Phase 2 和 Phase 3B。

验收：物理大气和体积云可以在至少一个中等质量档位运行；云体具备时域稳定性、云阴影和与场景深度的一致合成。

### M3：UDS + UDW 视觉核心

完成 Phase 5 和 Phase 6。

验收：天气循环中天空、云、雾、IBL、风、降水、湿润材质和闪电能够统一过渡，并能在运行时观察性能预算和派生资源状态。

## 6. 验收标准

### 视觉一致性

- [ ] 天空背景与 environment cubemap 朝向一致
- [ ] 太阳盘、Directional Light、CSM 阴影方向一致
- [ ] 日出、正午、黄昏、夜晚有明显且稳定的视觉差异
- [ ] 云、雾和降水受天气状态统一控制
- [ ] 金属反射、非金属环境漫反射和场景曝光保持一致
- [ ] 雨后地表湿润不会只改变颜色而破坏能量关系

### 性能与稳定性

- [ ] IBL 不因每帧天气变化而全量无条件重建
- [ ] 体积云具备明确分辨率、步数和时域质量档位
- [ ] 降水粒子具备数量、距离和屏幕占用预算
- [ ] 云、雾、降水、湿润均可独立关闭
- [ ] GPU 时间、显存占用和历史资源数量可观测
- [ ] 无 validation layer 的 layout、barrier、descriptor 和 resource lifetime 错误

### 数据与工具

- [ ] 场景 JSON 可以描述初始时间和天气预设
- [ ] 运行时命令可以切换天气、时间、风力和质量档位
- [ ] 天气状态支持可重复 seed
- [ ] 固定场景可以执行截图和性能回归
- [ ] 新增 scene/material/weather 字段有对应文档和校验

## 7. 主要风险

| 风险 | 影响 | 缓解方式 |
| --- | --- | --- |
| 体积云缺少 3D 纹理和时域基础 | 高 | 先完成历史资源、重投影和 3D 纹理链路，再做云体细节 |
| 动态 IBL 全量更新成本过高 | 高 | Phase 0 先做 dirty、预算、分帧和旧资源保留 |
| 缺少通用粒子系统 | 高 | 降水先做专用 GPU billboard，稳定后再抽象通用粒子接口 |
| 材质没有全局天气参数集合 | 高 | 新增独立 WeatherStateGPU，避免把参数继续塞入 UBOGlobal |
| 视觉质量依赖大量内容资产 | 中 | 第一版使用程序化噪声和少量可替换测试纹理 |
| 质量档位和 GPU 预算不明确 | 中 | 每个 pass 从第一版开始记录 GPU 时间和资源规模 |
| 试图复制 UE 编辑器体验 | 高 | 第一阶段只提供 JSON、控制台和 Debug View |

## 8. 第一批实现任务

建议下一轮实际编码只做以下内容，不直接进入体积云：

- [x] 创建 Phase 0 的独立执行清单
- [x] 统计当前 procedural sky、SH 和 prefilter 每帧 GPU 时间
- [x] 将环境资源更新改成 dirty 驱动
- [ ] 增加 `TimeOfDayState` 的最小数据结构
- [ ] 增加 `WeatherState` 的最小数据结构
- [x] 将 Sky Pass 背景显示与 IBL 派生更新解耦
- [ ] 增加 clear、cloudy、rainy 三个最小天气 preset
- [ ] 添加固定时间点和天气切换的截图回归场景

Phase 0 和最小状态结构完成后，再根据实际 GPU 时间决定先做 2D 云层还是物理大气 LUT。

## 9. 未决决策

- [ ] 第一版天空采用物理大气为主，还是艺术化模型为主
- [ ] 体积云噪声使用离线 3D 纹理、运行时生成，还是二者混合
- [ ] 降水先实现专用系统，还是先建设通用 GPU 粒子基础设施
- [ ] wetness/puddle 使用材质全局参数、屏幕空间 mask，还是额外地表 accumulation 资源
- [ ] 是否需要月相、星空、银河和极光
- [ ] 是否需要局部天气体积、室内排除和地形遮挡
- [ ] 目标硬件、分辨率和各质量档位的 GPU 时间预算

## 10. 参考与关联文档

- `documents/plan/rendering/sky-pass-environment-roadmap.html`
  - 当前程序化天空、Sky Pass、GPU 派生环境和分帧更新学习路线
- `documents/plan/rendering/weather-gi-long-term-roadmap.html`
  - 天气、雾、云、降水、湿润材质和 GI 的长期路线
- `documents/plan/rendering/environment-cubemap-ibl-unification-plan.md`
  - HDRI、程序化天空、environment cubemap、SH 和 prefilter 的统一执行计划
- `documents/rendering/descriptor-imageinfo-management.md`
  - 环境纹理和 descriptor image info 的生命周期约束
- `documents/rendering/texture-asset-json-v1.md`
  - 后续 3D 噪声和天气纹理资源格式扩展时需要参考的纹理资产契约
