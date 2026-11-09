# 粒子系统契约

> 状态：**当前契约**（实现已落地）。本文件描述 VulkanLearn 现在实际拥有的 Niagara 式粒子最小运行时；不含未来承诺。

## 当前能力

- 场景对象类型 `particleSystem`：作者在 `SC_*.json` 中用 `particlePath` 引用一个 `PS_*.json`。
- `particleSystem` 现在是正式场景 actor，出现在大纲与 Details；支持 Location/Rotation/Scale
  编辑和运行时可见性开关。场景 JSON 仍只支持独立 `particleSystem` 对象；v2 模块栈属于
  `PS_*.json` 作者层，运行时通过稳定槽位 API 做 emitter 级增删。Play 期的 C# 入口见
  [C# Blueprint 运行时](csharp-blueprint-runtime.md#particle-systems)，详见「场景 actor 集成」。
- `PS_*.json` 在 Content Browser 中作为独立 `ParticleSystem` 资产出现；双击进入自己的资源编辑器
  标签页。编辑器顶部是 emitter 选择器，左侧是
  System/Emitter/Shape/Velocity/Forces/Collision/Life/Color/Flipbook/Curves 模块，中间是可编辑字段表单，
  预览区通过 `SC_particle_preview.json` 注入当前 `PS_*.json` 的 `particleSystem` 对象。
- 粒子预览是 draft-live：首次进入时加载固定预览场景，并把当前未保存文档作为场景校验阶段的
  粒子覆盖传入，因此首帧不会先显示磁盘旧版本；之后同一粒子预览内的 revision 变化只发送
  `UiActionType::UpdateParticlePreview`，由运行时命令就地替换预览 World 中匹配 `particlePath`
  的 `WorldParticleSystem.asset`。该路径不调用 `LoadWorld`，不重建场景，也不重置相机、灯光、
  环境或粒子摆放；未保存的字段、曲线、flipbook 与发射器结构修改在下一帧直接可见。
- System 模块编辑 PS 根对象 `name`；Emitter 模块的 `name` 只改当前发射器名。资源标签标题
  对脏粒子文档追加 `*`，可选字段旁提供 `Reset` 按钮并提交 `reset=true`。
- 资源页工具栏提供 `Undo`、`Redo`、`Validate` 与 `Revert`。Undo/Redo 与 Edit 菜单共用
  当前粒子标签的有界历史；Revert 脏文档先弹确认；关闭脏文档走
  保存/丢弃/取消。丢弃依次提交 `particle.revert` 与 `particle.close`，不会静默删除草稿。
- 字段修改在途时新的输入继续累积在 pending，不并发提交；命令终态回来后才解除门禁。
  成功清空在途副本，失败把旧批次合并回 pending（同字段以更新的输入为准）并允许重试。
- `PS_*.json` 是 v1 多发射器容器：根对象用 `emitters[]` 描述 1..16 个发射器，每个发射器拥有
  池大小、连续发射率或一次性 burst、寿命区间、球形发射半径、初速度区间、重力、阻力、
  尺寸曲线端点、颜色曲线端点、additive/alpha 混合、可选的局部 `position` 偏移、
  `localSpace` 模拟空间、逐粒子 billboard 旋转与可选 `fixedBounds` 固定包围球。
- 没有 `emitters` 的旧 `PS_*.json` 仍按一个隐式发射器解析；根对象字段就是该发射器的字段，
  发射器名固定为 `emitter0`，局部位置为零。
- 发射器选择器支持复制新增、删除与上下移动（1..16 个）；结构变更以整条 `emitters`
  数组作为一次字段变更提交，不新增命令类型，并作为单个步骤进入粒子 undo 历史。
- GPU compute 每帧推进粒子池；graphics pass 按发射器 `renderMode` 读取同一段 storage
  buffer，用按 `billboardAlignment` 朝向的 billboard、最近 3 个历史位置组成的 ribbon
  条带、静态 SM 网格实例化，在作者化起止点之间生成多条带噪声的 beam 合成，或在独立
  distortion pass 中把屏幕高度归一化偏移写入 RGBA16F 缓冲。`renderMode: "light"`
  复用 GPU 模拟池并由独立 compute 生成最多 64 盏 per-particle light；
  `renderMode: "decal"` 复用普通 GPU 模拟池与 `particleSimulate.comp`，再由
  `particleDecalGather.comp` 压成最多 64 条紧凑记录；无 GPU 池时保留
  `ParticleCpuEmitterRuntime` 作为 fallback。
  粒子 decal 与静态 decal 共用 `decalStencil` D24S8 union mask（bit0）、64 槽实例
  排序和 `DecalRenderer`；粒子侧不额外创建 stencil 资源或独立 decal pass。
- 粒子图形 pass 可选接收完整场景光照：billboard/ribbon 用 wrapped Lambert 消费全部
  方向光/点光/聚光 + GPU 粒子灯 + IBL，mesh 走完整 DefaultLit；开关与强度由
  `billboardLighting` / `billboardLightingStrength` / `meshLighting` 控制。
- 透明顺序以 `sortOrder`（作者层）为主键、包围球视深度为次序；全局 alpha 池作为
  一个 draw item 参与同一份排序，不再固定最后绘制。
- Billboard 支持可选 flipbook 图集；没有纹理时回退到程序化软圆盘叠加 4 阶 FBM 噪声侵蚀。
- color/size 支持最多 8 个生命周期 key；没有 key 时回退到 `colorStart/colorEnd`、`size` 两个端点。
- 世界单位是 cm；速度与重力按 cm/s、cm/s^2 作者化。

## v2 模块栈（资产层）

- `PS_*.json` 可声明 `version: 2`；每个 emitter 用 `modules` 数组描述模块。模块形状为
  `{ "type": "...", "enabled": true, "fields": { ... } }`。`enabled: false` 的模块不参与展平，
  runtime 使用该字段的默认值。
- 当前模块类型：`emitter`、`renderer`、`spawn.rate`、`spawn.shapeSphere`（名称保留，现承载全部
  `shape.type`）、`spawn.velocity`、
  `update.lifetime`、`update.color`、`update.size`、`update.rotation`、`forces.gravity`、
  `forces.drag`、`forces.curlNoise`、`forces.vortex`、`forces.attractor`、
  `forces.wind`、`collision.analytic`、`events`、`flipbook`、`lod`、`curves`、`bounds`、
  `culling`。
  未知类型直接拒绝，避免拼写错误静默变成空模块。
- `fields` 与 v1 emitter 字段同名并递归合并；两个启用模块写同一字段且值不同会拒绝。
  v2 的模块顺序目前只属于作者层，runtime 消费展平后的 `ParticleEmitterAsset`，不重新解释顺序。
- `renderer.fields.additive` 是 v2 的混合开关：存在时布尔值直接决定 additive/alpha，
  并优先于底层 `blend` 字符串；两者同时出现时以 `additive` 为准。v1 的作者字段仍是
  `blend`，v2 renderer 模块公开 `additive`。
- 根对象可声明 `userParameters`：对象、最多 64 项、值为有限数。模块展平后，
  以 `$Name` 形式出现的字符串会替换为同名数值；未知引用在加载期报错。
- 没有 `version` 或 `version: 1` 的资产继续走原扁平字段路径；v2 根对象也允许直接携带一个
  隐式 emitter 的 `modules`。展平后的 runtime 校验与 v1 完全相同。
- 粒子编辑器对 v1 资产提供 “Upgrade to v2 module stack”；升级会先提交 `version: 2`，再把
  每个 emitter 的字段迁入模块并提交整条 `emitters`（隐式根提交 `modules`）。v2 编辑器显示
  模块行，可添加、删除、上下移动、启停；字段编辑按 `ParticleModuleTypeForField` 写回
  对应模块的 `fields`，不再写 emitter 根键。模块顺序仍是作者层信息，runtime 只消费展平值。

## 数据路径

```text
SC_*.json particleSystem
    -> SceneAssetValidator / ParticleAssetLoader（冻结并校验 PS_*.json）
    -> WorldBuilder（WorldParticleSystem：asset + model + scenePosition/sceneRotation/sceneScale + visible）
    -> WorldSnapshotBuilder -> ParticleSystemSnapshot
    -> RendererFrontend -> RenderScene::particleSystems
    -> ParticleRenderer::Synchronize（按 sceneName|assetPath|emitterName 建 GPU 池）
    -> compute 模拟 -> particle pass billboard/ribbon/mesh/beam 合成
    -> particleDistortion pass 写屏幕高度归一化偏移
    -> bloomPrefilter 与 toneMapping 按同一公式映射 sceneUV
    -> particleLightEmit compute（renderMode=="light"）
    -> ParticleLightSSBO（set 0 binding 4）-> deferred/forward lighting
    -> ParticleDecalRuntime::Synchronize/Update（GPU 回读优先，CPU fallback）
    -> 追加 RenderScene::decals -> DecalRenderer::Synchronize
    -> decalStencil pass（D24S8 bit0 union mask）-> decals pass（stencil gate + DBuffer）
```

`ParticleSystemAsset` 是 CPU 冻结值；没有运行时覆盖时不再重新解析作者 JSON。v2 资产只在
其模块实际引用过的 `$Name` 值发生变化时重解析，并按 collision type + path 复用已加载的
SDF/网格碰撞数据。`sourcePath` 使用相对资源根的逻辑路径。场景对象仍只引用一个 PS 资产；
`ParticleRenderer` 把每个子发射器展开成独立的 GPU emitter，身份由场景对象名、`assetPath`
与 emitter 名共同决定。GPU 模型矩阵是
`sceneObjectModel * translation(emitter.localPosition)`，局部偏移单位是 cm。

## 场景 actor 集成

- 每个由 `WorldBuilder` 构建的 `WorldParticleSystem` 都发布为
  `EditorSceneActorType::ParticleSystem`，大纲身份是 `particleSystem|<场景对象 name>`。
  `scenePosition`、`sceneRotation`、`sceneScale` 与 `visible` 由 World 实例持有，不从已烘焙的
  `model` 反分解。
- 大纲选中粒子行后，Details 提供 Location/Rotation/Scale。编辑通过
  `World::SetParticleSystemTransform` 按同一组作者 TRS 重算 `model`，只改活动 World 的实例，
  不修改 `PS_*.json`、不改场景对象名或 `particlePath`。
- 可见性开关写 `WorldParticleSystem.visible`，只影响运行时，不写入 `SC_*.json`。隐藏的实例
  仍保留在大纲与 World 中；渲染器冻结该实例的模拟与绘制（不推进时间、不清池、不排序、
  不 dispatch），重新显示后从冻结状态继续，不重建或重启粒子池。
- 粒子的资产编辑仍走 `PS_*.json` 文档；场景 JSON 仍只支持独立 `particleSystem` 对象。
  v2 模块栈是作者层结构；运行时通过 `particleemitter` / C# emitter API 对稳定槽位做
  spawn/destroy/restart/enable，destroy 保留 tombstone，不重排作者或运行时索引。
  实例增删仍由 `particlespawn` / `particledestroy` / `particlerestart` 与
  `Blueprint.Particles` 提供；逐实例运行时参数由 `particleparam` 或 C# setter 覆盖。

## 运行时实例管理

控制台的 `particlespawn` / `particledestroy` / `particlerestart` / `particleparam` 管理当前
active World 的粒子系统实例（命令格式见 [调试命令](debug-runtime-commands.md)）：

- `particlespawn` 在 GT 命令点读取并校验 `PS_*.json`，构造
  `WorldParticleSystem{name, assetPath, asset, translation(position)}` 并写入 World。
  同名实例覆盖；资产读取失败不会改变 World。
- `particledestroy` 只从 World 移除实例；GPU 池由下一帧 `Synchronize` 退休，
  不立即销毁仍在飞行帧引用的资源。
- `particlerestart` 按场景对象名匹配 emitter key（转义后的第一段），把匹配池的
  `elapsedSeconds/prewarm/needsClear/culled` 重置；不重新加载资产、不重建纹理与 GPU 池，
  也不改 World 中的资产数据。分线程渲染时先等待渲染线程空闲再改状态。
- `particleparam` 写实例级运行时覆盖：连续发射预算缩放、RGBA 颜色乘数与逐发射器启停。
  覆盖不写资产、不写场景；`reset` 恢复默认值。语义与范围见下面的「运行时参数覆盖」。

这些操作只影响当前 World 实例，不是资产编辑，也不会写回 `SC_*.json` 或 `PS_*.json`。

### C# Blueprint 入口

`Blueprint.Particles` 只在所属 Blueprint 的 live GT 回调中可用。Play 期的
`Spawn(assetPath, position, rotation?, scale?)` 返回 `ParticleSystem`，实例 ID 由当前
上下文生成 `~particle.<n>`；`Find(id)` 只查本 Blueprint 的 live 表，`DestroyAll()` 销毁
该表全部实例。`ParticleSystem` 提供 `Destroy`、`SetVisible`、`SetSpawnRateScale`、
`SetColorTint`、`SetEmitterEnabled` 与 `ResetOverrides`，没有 `Restart`；需要重启时
`Destroy` 后重新 `Spawn`。发射器级提供 `SpawnEmitter` / `DestroyEmitter` /
`RestartEmitter` / `SetEmitterEnabled(handle)`，以及附着、预算、用户参数、
遥测和 `Subscribe(Action<ParticleEvent>)`。完整签名与回调边界见
[C# Blueprint 运行时](csharp-blueprint-runtime.md#particle-systems)。

托管调用只做 TRS 与参数范围校验并入队，不读取资产；native 在 GT 命令点对
`particleCommands` 中的 `spawn` 同步执行 `ParticleAssetLoader::Prepare`。因此返回句柄
不代表资产已成功加载；路径或资产校验失败会停止该 Play 上下文并发布 `StopContext`
诊断。C# setter 与 console `particleparam` 共用同一组 `World` setter 和校验边界：
`spawnRateScale ∈ [0,16]`、`colorTint` RGB `∈ [0,16]` 且 alpha `∈ [0,1]`、
`emitter` index `∈ [0,15]`，全部要求有限值。Blueprint Destroy、故障隔离或 EndPlay
触发的 owner 清理会为仍在 live 表中的实例补发 `destroy`；整体上下文 Release 不经过
该补发路径。

### 运行时参数覆盖

每个 `WorldParticleSystem` 持有 `ParticleSystemRuntimeOverrides`，由 `particleparam`
或 C# `ParticleSystem` setter 在 GT 命令点写入，渲染器按实例消费。覆盖不进入
`PS_*.json` / `SC_*.json`，也不改变 `ParticleSystemAsset` 的冻结值。

- `spawnRateScale`：默认 `1`，范围 `[0, 16]`。它乘在连续发射预算上，并参与 prewarm 的
  稳态目标数 `min(maxParticles, spawnRate * spawnRateScale * 0.5 * (lifetimeMin +
  lifetimeMax))`。`burst: true` 的整池喷发不受它影响，`burstInterval` 周期重触发仍按整池
  执行。
- `colorTint`：默认 `(1,1,1,1)`。RGB 每分量在 `[0, 16]` 且全部有限，允许 HDR 增亮；
  alpha 在 `[0, 1]`。渲染时乘到 billboard、ribbon、mesh 与 beam 四种 renderMode 的最终颜色，
  alpha 乘到最终不透明度。
- `enabledEmitterMask`：默认低 16 位全 `1`。`particleparam <name> emitter <index> <0|1>`
  只切换一个下标；关闭的发射器走与实例隐藏相同的冻结路径：`elapsedSeconds` 不推进、
  粒子池不清空、不 prewarm、不排序、不 dispatch、不绘制，重新启用后从原状态继续，
  不重建 GPU 池。它不删除资产中的发射器，也不改变发射器顺序。
- `reset`：把三项覆盖恢复为默认值，不影响 `particlerestart` 的模拟时间/预热语义。

### Particle Data Channels

World 另维护一份与逐实例 `userParameters` 分开的全局命名标量表
`particleDataChannels`，最多 64 项；key 为 `1..64` UTF-8 字节且不含 NUL，value 必须
有限且 `|v| <= 1e6`。Console `particledata` 直接写入当前 active World；C#
`Particles.Data` 先校验所属 Blueprint owner。通道不写资产，World 表不随 Play/Stop 清除；
Stop 的 EndPlay 回调仍可读取托管镜像，整个 Play 上下文 release 后才释放；单个 owner
Destroy 的 EndPlay 也可读取，但 owner 清理不会清空其他 Blueprint 共享的全局镜像。快照通过
`WorldSnapshot::particleDataChannels` 发布冻结副本。

渲染器在 v2 模块 `$Name` 重解析时先读全局通道，再用该粒子实例的运行时
`userParameters` 覆盖同名项。作者资产应声明同名默认 `userParameters`，否则加载期
仍按未知参数拒绝；这样拼写错误不会静默变成 0。缓存签名只包含作者模块实际引用的
`$Name` 依赖，通道更新只让引用它的系统重解析；重解析复用已加载的 SDF/网格碰撞数据。

## GPU 布局

- 每个发射器拥有独立的 `ParticleBuffer`（`std430`，每粒子 288 字节 = 18 个 `vec4`）与每 swapchain image 一份
  `ParticleFrameUniforms`（`std140`）。
  `Particle` 依次是 `positionAge`、`velocityLife`、`history[16]`；
  `history[i]` 的 `xyz` 是记录时的模拟空间位置、`w` 是记录时的 age，
  `history[0]` 是上一帧，`history[15]` 最旧，供 ribbon 弧长重采样使用。
- compute 与 graphics 的公共 binding 语义：
  - `set 0, binding 0`：`ParticleFrameUniforms`
  - `set 0, binding 1`：粒子 storage buffer
  - graphics `set 0, binding 2`：可选 flipbook `sampler2D`；无纹理发射器绑定 1x1 白图
  - graphics `set 0, binding 3`：只读 `SortedPairs`；`xy = (viewDepth, originalParticleIndex)`
  - graphics `set 0, binding 4`：紧凑 `aliveIndices`，未排序路径使用
  - graphics `set 0, binding 5`：GPU overdraw 片元计数
  - compute `set 0, binding 2/3/4/5` 分别是 alive indices、事件计数、事件记录与 SDF
- graphics 额外绑定 `set 3, binding 0`：`sceneDepth` 只读输入；该描述符集由 RenderGraph 的
  pass input 计划创建，`ParticleRenderer` 只在管线反射使用 set 3 时绑定。`set 3, binding 1`
  是 particle pass 的 CSM `shadowMap`；全局 alpha billboard 的 fragment shader 不采样
  CSM，但仍显式声明这两个 binding 的管线布局，以匹配 pass 描述符集的固定布局。
  `set 3, binding 3` 是半分辨率 `particleSoftDepth` 线性视深度纹理。
  软粒子优先做双边上采样；资源为 1x1/不可用时回退中心 + 四向十字 5 个 tap。
  `softParticleTapRadius` 控制 fallback 像素半径，0 退回单采样。
- compute pipeline 名为 `particle/particleSimulate`；billboard graphics variant 名为
  `particle/particle`，ribbon variant 名为 `particle/particleRibbon`，mesh variant 名为
  `particle/particleMesh`，beam variant 名为 `particle/particleBeam`，distortion variant
  名为 `particle/particleDistortion`。billboard/ribbon/mesh/beam 各有 additive 与 alpha
  两条 blend pipeline，按 PS 的 `renderMode` 与 `blend` 选择；distortion 只有一条
  additive 管线，写入独立附件。mesh pipeline 使用 `Vertex` vertex input 合同并执行
  `drawIndexedIndirect`。beam pipeline 不消费顶点输入，始终直接
  `draw(segments * 6, count, 0, 0)`。
- 排序使用 `particle/particleSortInit` 与 `particle/particleSort` 两条 compute pipeline：
  init 把每个粒子写成 `vec4(viewDepth, index, 0, 0)`，然后执行标准 bitonic 网络。
  排序容量是 `maxParticles` 向上取 2 的幂、最低 256、最高 4096；关闭排序时仍分配 pair 表，
  但 graphics 走直接索引。排序开启时 `maxParticles` 必须 `<= 4096`，否则资产校验失败。
- 排序方向是 view-space 前向深度降序，即最远粒子先画；graphics 读最终 pair 表的 `y`
  作为原始粒子池索引。最终写在 A 还是 B 由 bitonic pass 数的奇偶决定。
- 模拟在图形 pass 之前录制；一次 compute -> vertex/fragment memory barrier 把 SSBO 写入
  移交给 billboard 读取。跨帧先做 vertex read -> compute write barrier。
- `shape` 为 `x = shapeRadius`、`y = burst`、`z = localSpace enabled`、`w = 0`。
- `ParticleFrame` 末尾的 `screenParams` 紧跟在 `sortPass` 之后：`x = viewportHeightPixels`，
  `y = maxScreenSize`，`z = 作者 spawnRate`（prewarm 池填充用），`w = 0`。
  全局排序路径把 `w` 作为 8 槽纹理数组的索引；其它路径保持 0。
  C++ 与 GLSL 的 std140 字段顺序必须保持一致。
- `screenParams` 之后依次是 `flipbookFrameDurations[64]`（每 `vec4` 打包 4 帧相对权重）、
  `flipbookFramePivots[128]`（每 `vec4` 打包 2 帧的 `(u0,v0,u1,v1)`，覆盖 256 帧）与 `flipbookParams3`
  （`x = hasDurations`、`y = hasPivots`、`z = frameCount`、`w = hasUVs`）。两张表都按帧区间内的
  局部索引 0..frameCount-1 写入，步长与 `std140` 的 `vec4` 数组一致。
- `flipbookParams3` 之后是 `flipbookFrameUVs[64]`：每帧独占一个 `vec4(u0,v0,u1,v1)`，
  按区间内局部索引写入；只有 `hasUVs = 1` 时读取。
- `flipbookFrameUVs` 之后是碰撞参数：`collisionParams`（`x = type`，0 none、1 plane、2 box、
  3 capsule；
  `y = bounce`、`z = friction`、`w = radius`）、`collisionPlane`（`normal.xyz, offset`）、
  `collisionBoxMin`（`min.xyz, killOnCollision`）与 `collisionBoxMax`（`max.xyz, 0`）。
  C++ 与 GLSL 必须保持完全相同的 std140 顺序。
- `burstParams` 之后是 `rotationParams`：`x = rotation`（度）、`y = rotationSpeed`（度/秒）、
  `z = rotationRandom`（0/1）、`w = 0`。
- `rotationParams` 之后是 UBO 末尾的 `flipbookFrameRotations[16]`（每 `vec4` 打包 4 帧的
  逐帧旋转角，按区间内局部索引写入）与 `flipbookParams4`（`x = hasRotations`，`y/z/w` 保留）。
  其后再追加 `collisionParams2`（`x = ccd`、`y = capsuleRadius`、`z/w` 保留）、
  `collisionCapsuleA` 与 `collisionCapsuleB`（线段端点 `xyz`，世界空间）。
  C++ 与 GLSL 的末尾字段必须同序。
- UBO 最后追加 `ribbonParams`：`x = ribbonSegments`（1..3）、`y = ribbonFade`（0..1）、
  `z = ribbonWidth`（世界空间半宽 cm）、`w` 保留。billboard 发射器也写入该字段，但
  billboard shader 不读取。
- `ribbonParams` 之后再追加 `meshParams`（`x = alignment`，0 world、1 camera、2 velocity）
  与 `meshScale`（`xyz = 网格局部缩放`）。billboard/ribbon 发射器也写入，但只有 mesh
  shader 读取。随后是 `colorTint`（RGBA 运行时颜色乘数）与 `billboardParams`
  （`x = billboardAlignment`，0 camera、1 velocity、2 fixed；`y/z/w` 保留）；`billboardParams`
  只有 billboard shader 读取。
- `billboardParams` 之后是 `shapeParams`（`x = shape code`，0 sphere、1 box、2 cone、
  3 cylinder、4 disc、5 ring；`y = height`、`z = innerRadius`、`w` 保留）与
  `shapeExtent`（`xyz = box 半尺寸`，cm）。
- UBO 末尾追加 `curlParams`（`x = strength`、`y = frequency`、`z = time speed`、
  `w` 保留）、`vortexParams`（`x = strength`、`y = radius`）、`vortexAxis`
  （`xyz = 未归一化轴`）、`attractorParams`（`x = strength`、`y = radius`）、
  `attractorPosition`（`xyz = 模拟空间位置`）与 `wind`（`xyz = 模拟空间常量加速度`）。
  这些字段由 compute shader 消费；C++ 与 GLSL 的追加顺序必须一致。
- `wind` 之后依次是 `lifetimeScaleKeys[8]` / `lifetimeScaleParams`、
  `meshLightParams` / `meshLightDirection` / `meshLightColor`、mesh 材质字段、
  CSM 矩阵/级联参数、确定性/事件参数与 SDF bounds/resolution。最后追加
  `billboardLightParams`：`x = billboard lighting enabled`、`y = billboardAmbient`、
  `z/w` 保留；方向与颜色复用 `meshLightDirection` / `meshLightColor`。
- `billboardLightParams` 之后追加 beam 字段：`beamStart`（`xyz = 世界空间起点`，
  `w = segments`）、`beamEnd`（`xyz = 世界空间终点`，`w = width`）、`beamNoise`
  （`x = amplitude`、`y = frequency`、`z = speed`、`w = endpointJitter`）与
  `beamParams`（`x = count`、`y = branchCount`、`z = branchStart`、
  `w = branchLength`）。四个字段追加在 `billboardLightParams` 之后，
  不重排既有字段。
  随后追加 fixed billboard 基：`billboardFixedNormal` 与 `billboardFixedUp`
  （`xyz` 为 emitter-local 向量乘模型线性部分并归一化后的世界空间基，`w` 保留），
  最后追加 `beamBranch`
  （`x = branchSpread`，已乘模型最大列缩放；`y/z/w` 保留）。因此 `ParticleFrame` 的
  末尾再追加 `distortionParams`：`x = distortionStrength`（屏幕高度归一化比例），
  `y = 纹理可用`（加载失败时为 0，shader 回退程序化方向场），`z/w` 保留。
  `distortionParams` 之后追加 beam 控制数组：`beamColorKeys[8]`、
  `beamColorKeyTimes[2]`、`beamWidthKeys[8]`、`beamCurveParams`、
  `beamArcColorStart[16]`、`beamArcColorEnd[16]`、`beamArcParams[16]`。
  `ParticleFrame` 的 std140 大小是 7456 字节，beam 字段偏移是
  6256、6272、6288、6304，fixed 字段偏移是 6320、6336，`beamBranch` 偏移是 6352，
  `distortionParams` 偏移是 6368；新数组偏移是 6384、6512、6544、6672、6688、
  6944、7200。旧字段偏移保持不变。
  C++ 的 `static_assert` 与 `particleCommon.glsl` 必须同时更新。
- 图集 `columns*rows > 1` 时禁用 mipmap：低 mip 会把相邻 cell 混在一起。顶点着色器在算出
  `vAtlasUV` 后用 `textureSize(particleTexture, 0)` 内缩半个 texel 到当前 cell 边界内，
  再交给线性过滤，避免帧间渗色；V 翻转保持不变。单帧纹理维持原有 mipmap 行为。
- 每个 emitter 持有一份 host-visible indirect 参数缓冲：billboard/ribbon 使用
  `drawIndirect`，mesh 使用 `drawIndexedIndirect`。CPU 只写固定顶点/索引步长与容量上界；
  simulate 原子累加 alive，`particleFinalizeCount.comp` 每帧把 alive 写回
  `instanceCount`。billboard/ribbon 使用“每实例固定顶点数 + 存活实例数”，vertex shader
  通过 `gl_InstanceIndex` 取紧凑 `aliveIndices`；未使用
  `VK_KHR_draw_indirect_count`，也不做 GPU 侧多命令重写。beam 不读取 indirect 参数，
  实例数直接来自 `beam.count * (1 + beam.branchCount)`；该发射器也跳过
  simulate/sort/finalize。distortion 仍正常模拟并使用同一套 indirect 参数，但绘制
  发生在 `particleDistortion` pass，不进入 particle pass 的 sceneColor 合成。

## 资源生命周期与图身份

- 每个 `EmitterResources` 由 `shared_ptr` 整体持有；发射器删除、池大小变化或排序开关变化时，
  活跃表把同一份所有权交给 `ResourceRetireQueue`，记录最近一次已提交 epoch。GPU 对应 fence
  完成后才析构 descriptor pool、粒子/pair/UBO buffer 与纹理，不能在帧仍在飞时逐字段释放。
- 粒子池是跨帧连续状态，不能复制成 per-image 双份；`RenderSystem` 在存在 emitter 时等待
  上一提交的 frame fence，再录制本帧模拟，因此粒子/全局 gather 缓冲不会与上一在飞帧交叠。
  无粒子场景不增加该等待。
- 粒子渲染器的图身份不只是 `ownerGeneration`：resize 会刻意写回旧 generation，因此
  `ParticleRenderIdentity` 同时比较 particle pass 的 `vk::RenderPass` 句柄、swapchain image
  数量、pass 宽高与管线契约。任一变化都会重建 particle pipeline 与每 image UBO 数组。
- 图身份变化只发生在启动、resize 或 RenderGraph reload。该路径先 `WaitIdle` 再立即重建；
  普通帧的发射器增删只走 retire queue，不做设备级等待。`Shutdown` 的调用方必须已经处于
  渲染线程停止且设备空闲的关闭边界。
- 纹理热切换身份是 `texture 路径 + 文件大小 + 修改时间 + mipmap 配置`。
  `Synchronize` 在池大小/排序开关不变时也会比较该 key；空→非空、A→B、同路径内容变化
  都会走 retire + `CreateEmitter`，
  重新解码并重写 graphics descriptor。读取失败时 key 变为稳定的 `missing`，文件恢复后
  会再次变化并触发重建；渲染线程始终不会因纹理异常退出。
- `ParticleRenderer` 以同一 key 维护 `weak_ptr<Texture>` 缓存：仍存活时多个发射器共享
  同一 GPU 纹理，最后一个持有者析构后条目自然失效；`Shutdown` 只清索引，不提前销毁
  仍被活跃或退休发射器持有的资源。
- mesh 引用身份是 `mesh.path + 文件大小 + 修改时间`。路径、文件内容或 renderMode
  变化都会 retire 并重建发射器；导入失败回退 billboard 并记录一次 warning，异常不会
  逃出渲染线程。网格顶点/索引缓冲随 `EmitterResources` 一起退休和析构。

## 预热与模拟时钟

- `prewarm` 只发生在发射器创建后的第一帧。连续发射器的目标数量按稳态近似
  `min(maxParticles, spawnRate * 0.5 * (lifetimeMin + lifetimeMax))`；只对
  `index < target` 的粒子调用 spawn，其余保持死亡。`spawnRate == 0` 的连续发射器预热后
  仍为空池，不能凭空填满。`burst` 发射器不受目标数量限制，仍整池喷发一次。
  这里的 `spawnRate` 取自 `screenParams.z` 的作者原值，不受 LOD 缩放影响。
- 预热粒子按随机年龄 `age in [0, lifetime)` 积分到当前状态，而不是只写入随机年龄：
  无 drag 时用 `p += v0*t + 0.5*g*t^2`、`v += g*t`；有 drag 时使用
  `dv/dt = g - kv` 的解析解
  `v(t) = g/k + (v0 - g/k)e^{-kt}`、`p(t) = g/k*t + (v0 - g/k)(1 - e^{-kt})/k`。
  这样首帧不会出现粒子聚在发射球附近的团块。运行时每帧积分保持原半隐式形式。
- 预热积分完成后立即调用与运行时相同的碰撞解析；如果 `killOnCollision=true` 且命中，
  预热粒子以 `age = lifetime` 写回，下一帧保持死亡。
- 粒子模拟时间来自 `WorldSnapshot::simulationDeltaSeconds`，由 `EngineLoop` 在
  `scripts.Tick` 之后写入 `RenderSystem`：`Paused` 为 0，`Playing` 为已乘 `timeScale`
  的 `time.delta`，`Editing`/无脚本时回退到编辑器 `simulationDelta`。因此暂停会冻结
  粒子，时间缩放同时作用于粒子；不再使用眼适应的 wall-clock delta。

### Lifetime Scale Curve

`lifetimeScaleKeys: [{ "time": 0, "scale": 1 }]` 是可选的生命周期倍率曲线，属于
`update.lifetime` 模块；缺省或空数组表示倍率恒为 `1`。

- `time` 是归一化年龄 `lifeT = age / lifetime`，取值 `[0, 1]`；`scale` 取值
  `[0, 4]`。两者都必须是有限数；缺省时 `time = 0`、`scale = 1`。最多 8 个 key，
  加载时按 `time` 稳定升序排序，允许相同 `time`；相同 `time` 段取后一个 key 的值。
- 每帧对每个存活粒子用当前 `lifeT` 做分段线性插值。`lifeT` 在首 key 之前或尾 key
  之后时取端点 key 的 `scale`（即钳制到 `[0,1]` 后查表），不做线性外推。
- 求得的倍率只乘到该粒子本帧的模拟 delta 上，用于位置、速度与 `age` 的推进；
  不改变作者的 `lifetime` 范围、发射预算、prewarm 目标数或渲染尺寸。
- `scale = 0` 表示该粒子在当前年龄被冻结：本帧 delta 为 0，位置、速度与 `age`
  都不推进，也不做碰撞与历史点更新，但粒子仍保持存活并继续渲染；它不会立即死亡。
  离开该段后继续按曲线推进。
- prewarm 例外：首帧预热仍按原来的解析解用完整随机年龄积分，不应用
  `lifetimeScaleKeys`；倍率只在预热完成后的正常运行帧生效。

## 局部空间与固定包围球

### localSpace

- `localSpace: true` 时粒子池保存发射器局部坐标：spawn 位置与速度不再乘
  `localToWorld`，重力与 drag 也按局部空间解释并在局部空间积分，prewarm 同样如此。
- 渲染顶点把粒子中心按 `localToWorld * vec4(position, 1)` 变换到世界后，再做 billboard
  朝向（`camera` 或 `velocity`）、深度与 `maxScreenSize` 尺寸钳制；`screenSize` 与
  `vViewDepth` 都使用变换后的世界中心。逐粒子透明排序的 `particleSortInit` 也用同一
  世界中心计算深度。
- 发射器模型仍是 `sceneObjectModel * translation(emitter.localPosition)`；`localSpace`
  只改变粒子状态所在的空间，不改变发射器摆放。
- `localSpace=true` 可与平面/盒体/胶囊/SDF 碰撞组合；碰撞体按发射器局部空间解释，
  与局部粒子坐标同一空间。mesh 仍拒绝 `localSpace=true`。

### fixedBounds

- JSON 为 `fixedBounds: { "center": [x,y,z], "extent": [x,y,z] }`，单位 cm；
  `center` 是发射器局部空间中心，`extent` 是各轴半尺寸且必须非负、有限。
- `useFixedBounds` 可省略：显式写出时以它为准；省略时只要存在 `fixedBounds` 对象就
  自动启用。显式 `useFixedBounds: false` 可关闭固定包围球而保留作者数据。
- 启用后包围球完全替代自动估算：
  `worldCenter = model * fixedBoundsCenter`，
  `worldRadius = |fixedBoundsExtent| * maxLinearScale`。因此 `extent` 不能全 0。
- 关闭时保持原有自动逻辑：`worldCenter = model * origin`，
  `worldRadius = 自动局部半径 * maxLinearScale`，自动局部半径仍包含速度、重力、
  发射半径与最大 visual extent；billboard/ribbon 使用尺寸或条带宽度，mesh 使用导入
  顶点乘 `mesh.scale` 后的最大局部半径。beam 是例外：`worldCenter` 取变换后的线段
  中点（除非启用 `fixedBounds`），局部半径只计
  `0.5 * |end-start| + branchLength * |end-start| + branchSpread +
  sqrt(2) * noiseAmplitude + width +
  (1 + 2 * branchLength) * sqrt(3) * endpointJitter`（无分支时分支项为 0），
  不叠加粒子寿命/速度。
- 视锥剔除、`maxDrawDistance`/`fadeDistance`、`minScreenSize` 与跨发射器透明排序
  都消费同一份 center/radius；固定包围球只需要覆盖实际可见粒子范围，不会改变粒子
  模拟、距离淡出公式或逐粒子排序。

## 外力

每个 emitter 可选 `curlNoise`、`vortex`、`attractor` 对象与 `wind` 向量。它们与
`gravity` 一样产生加速度，在同一个子步内先合成力，再对速度做指数 drag 阻尼：

```text
force = gravity + wind + curlNoise + vortex + attractor
velocity += force * dt
velocity *= exp(-drag * dt)
```

所有力都在**模拟空间**解释：普通发射器使用世界坐标/世界向量，`localSpace: true`
使用发射器局部坐标/局部向量。除特别说明外，强度单位是 `cm/s^2`，所有数值必须有限；
`localSpace` 不改变这些字段的作者化含义，只改变粒子状态所在的空间。

### Curl Noise

```json
"curlNoise": { "strength": 0, "frequency": 0.1, "speed": 0 }
```

- `strength`：默认 `0`，必须非负；`0` 时完全跳过噪声采样与差分开销。
- `frequency`：默认 `0.1`，单位 `1/cm`，必须非负；`0` 时同样关闭。
- `speed`：默认 `0`，单位 `1/s`，只把时间相位加到噪声第三轴，必须有限。

采样位置为 `position * frequency + vec3(0, 0, time * speed)`。噪声是一个三分量
value-noise 势场，三个分量使用互不相同的固定偏移；shader 以 `0.01` 的步长做中心
差分，近似势场旋度 `curl`，再乘 `strength / (2 * 0.01)` 得到加速度。因此该力无散度，
不会把粒子持续推向单一方向。`localSpace` 发射器直接对局部位置采样。

### Vortex

```json
"vortex": { "strength": 0, "radius": 50, "axis": [0, 1, 0] }
```

- `strength`：默认 `0`，必须非负。
- `radius`：默认 `50`，单位 cm，必须非负；`strength > 0` 时必须大于 `0`。
- `axis`：默认 `[0,1,0]`，必须有限；`strength > 0` 时长度不能为零，shader 内归一化。

涡心是模拟空间原点；世界空间发射器的模拟空间原点对应发射器世界位置
（`localToWorld` 平移），`localSpace` 发射器对应局部原点。径向向量
`radial = position - center`，切向为 `normalize(cross(axis, radial))`。作用半径内使用
线性衰减 `falloff = clamp(1 - length(radial) / radius, 0, 1)`，半径外为零；径向与轴
平行或重合时切向未定义，直接跳过。

### Point Attractor

```json
"attractor": { "position": [0, 0, 0], "strength": 0, "radius": 100 }
```

- `position`：默认 `[0,0,0]`，单位 cm，按模拟空间解释。
- `strength`：默认 `0`，必须非负。
- `radius`：默认 `100`，单位 cm，必须非负；`strength > 0` 时必须大于 `0`。

力方向为 `normalize(position - particlePosition)`，衰减为
`1 / (1 + (distance / radius)^2)`。距离为零时跳过，避免除零。`localSpace` 发射器的
`position` 是局部坐标，世界空间发射器则直接使用世界坐标。

### Wind

```json
"wind": [0, 0, 0]
```

`wind` 是模拟空间常量加速度向量，默认零，单位 `cm/s^2`；三个分量都必须有限。
它不随位置、速度或时间变化，也不是场景级 `wind` 对象；场景风由独立系统消费，
不会自动叠加到粒子发射器的 `wind` 字段。

## Burst 重触发

`burstInterval` 只对 `burst: true` 的发射器生效，默认 `0`、范围 `[0, 60]` 秒。

- `burstInterval == 0`：保持一次性 burst，prewarm 帧把整池粒子喷出，之后死亡粒子不再生成。
- `burstInterval > 0`：每帧计算 `cycle = floor(elapsedSeconds / interval)` 与
  `previousCycle = floor((elapsedSeconds - dt) / interval)`；周期变化时整池重新 spawn
  （所有存活/死亡粒子都覆盖），非跨周期帧保持粒子状态，死亡的不复活。大 `dt` 一次跨过
  多个周期也只重触发一次。
- prewarm 分支不读 `burstInterval`，仍然整池喷发一次作为首帧状态。

周期从发射器累计模拟时间 0 起算，没有逐发射器相位偏移；发射器被剔除时时间冻结，重新可见
后不会补算不可见期间的周期。CPU 镜像 `ParticleBurstCycleCrossed(previousTime, time,
interval)` 与 shader 使用同一公式。

## LOD 发射预算

多级 LOD 使用 `lod.tiers`（最多 4 档）：

```json
"lod": {
  "tiers": [
    { "distance": 1000, "spawnRateScale": 0.6 },
    { "distance": 3000, "spawnRateScale": 0.25 }
  ]
}
```

`distance` 非负且严格递增，`spawnRateScale` 在 `[0,1]`，所有数值必须有限。每帧
`RecordSimulation` 用发射器中心到相机的距离选择“已越过的最远档位”：

```text
scale = 1
for tier in lod.tiers:            # distance 严格递增
    if tier.distance <= cameraDistance: scale = tier.spawnRateScale
    else: break
spawnRate = asset.spawnRate * scale
```

边界包含：距离恰好等于 `tier.distance` 时该档位立即生效。没有越过任何档位时为原
`spawnRate`。旧字段 `lodDistance`（cm，默认 `0` 关闭）与 `lodSpawnRateScale`
（默认 `1`，范围 `[0,1]`）继续作为单档兼容写法；两者与 `lod.tiers` 同时存在时以
`lod.tiers` 为准，旧字段只在 tiers 为空时参与选择。

该值写入 `simulation.x`，因此只改变死亡粒子的重生概率；burst 整池喷发、prewarm 的池填充、
寿命、速度、碰撞、尺寸、透明排序与渲染都不受 LOD 影响。档位切换本身不做插值/淡入淡出，
超过档位阈值时缩放立即生效；可见性距离淡出仍由独立的 `maxDrawDistance`/`fadeDistance`
控制。

## 碰撞

每个 emitter 的可选 `collision` 对象描述世界空间碰撞。`type` 为 `none`（默认）、`plane`、
`box`、`capsule`、`sdf` 或 `mesh`；`none` 时 compute 在读取类型后直接返回，不执行距离计算、
求交或分支主体。`sdf` 与 `mesh` 共用有限体素 SDF 路径。

- `collision.plane.normal`：世界空间法线，解析时必须有限且长度 `> 1e-6`，解析后归一化。
- `collision.plane.offset`：沿法线的世界距离（cm），平面方程为
  `dot(p, normal) = offset`。
- `collision.box.min` / `collision.box.max`：世界轴对齐 AABB（cm），逐轴要求 `min <= max`。
- `collision.capsule.a` / `collision.capsule.b`：世界空间线段端点（cm），默认
  `[0,-50,0]` / `[0,50,0]`；允许重合，重合时退化为球。
- `collision.capsule.radius`：胶囊自身半径（cm），默认 `20`，必须非负。
- `collision.bounce` / `collision.friction`：`[0,1]`，默认 `0.3` / `0.2`。
- `collision.radius`：粒子碰撞球半径（cm），默认 `0`，必须非负。
- `collision.killOnCollision`：命中后把 `age` 设为 `lifetime`，粒子在写回后死亡；默认 `false`。
- `collision.ccd`：是否对运行时帧积分做连续碰撞子步，默认 `false`；只对
  `none` 以外的碰撞类型生效。

所有碰撞数值必须是有限数，NaN/Inf 在加载与编辑器写入阶段拒绝。`type` 只能是上述六个值。
未使用分支的字段仍完整校验，便于作者在类型间切换时复用配置。

平面碰撞使用粒子球的外扩表面：

```text
d = dot(p, n) - offset - radius
if d < 0:
    p -= n * d
    vn = dot(v, n)
    if vn < 0:
        v = (v - vn*n) * (1 - friction) - (vn*n) * bounce
```

第一式把粒子推到 `d = 0` 的表面；法向速度 `vn < 0` 时才做反射，因此已经在离开平面的粒子
只被推出，不会被再次弹回。`bounce=1` 保持法向速度大小，`bounce=0` 清除法向分量；切向分量
单独乘 `(1 - friction)`，法向分量不会被 friction 二次缩放。

盒体先把 `min/max` 按 `radius` 每轴外扩，再检查粒子是否在盒内。若在盒内，选择六个面中
距离最小的面（逐轴比较 `p - min` 与 `max - p`），把该轴推到对应面，并以该面的外法线做与
plane 相同的法向反射/阻尼。只有法向速度指向盒内时反射；已经在离开该面的速度分量保留。
另一个轴不移动。盒内命中时其余两轴始终乘 `(1 - friction)`；`bounce` 只作用于指向盒内的
法向速度，已经在离开该面的法向分量不反转。

碰撞发生在位置积分之后、写回 particle buffer 之前；运行时与 prewarm 共用同一解析函数。
碰撞修正不改变发射器包围球、`maxDrawDistance`、`minScreenSize` 或跨 emitter 排序：
这些仍按未碰撞的飞行外壳估算，因此碰撞后的粒子不会被更早裁剪，也不会因反射扩大排序半径。

胶囊碰撞先把粒子中心 `p` 投影到线段 `AB` 得到最近点 `q`：

```text
t = clamp(dot(p - A, B - A) / |B - A|^2, 0, 1)   // |B-A| ≈ 0 时 t = 0
q = A + (B - A) * t
d = |p - q|
if d < capsuleRadius + radius:
    n = d > 1e-6 ? (p - q) / d : (0, 1, 0)
    p = q + n * (capsuleRadius + radius)
    if dot(v, n) < 0:
        v = (v - dot(v,n)*n) * (1 - friction) - dot(v,n)*n * bounce
```

`collisionRadius` 复用为粒子半径，因此表面距离是胶囊半径与粒子半径之和；`d ≈ 0` 时用
`+Y` 稳定轴避免除零。推出、反射、friction 与 `killOnCollision` 语义与 plane 一致。

### CCD 子步

`collision.ccd=true` 时只改变运行时帧积分，不影响 prewarm：

```text
steps = clamp(ceil(|v| * dt / max(collisionRadius, 0.5)), 1, 8)
stepDt = dt / steps
重复 steps 次：
    v += gravity * stepDt
    v *= exp(-drag * stepDt)
    p += v * stepDt
    ResolveCollision(p, v)
```

子步数按本帧开始时的速度与扫过距离计算，最多 8 步；碰撞命中并触发 kill 时提前结束。
每个子步使用 `dt/steps` 的重力与指数 drag，总时间保持一致，但多步半隐式积分与单步结果
存在可接受的数值差异。`ccd=false` 或 `type=none` 时保持原有单步积分。
步长下限是 0.5cm，因此厚度明显小于该值的几何体在极端速度下仍可能被跨过；CCD 不是
精确的扫掠求交。
prewarm 的解析积分完成后只在末尾调用一次碰撞，不逐子步求解，因此首帧仍可能有一次
薄碰撞体穿透；该限制只影响编辑器打开/加载时的预热状态。

## 跨发射器排序

`ParticleRenderer` 每帧在模拟录制之后计算发射器级绘制顺序，graphics pass 按该顺序从远到近
绘制所有 billboard 发射器。排序主键是作者字段 `sortOrder`（升序，值小的先画），
同层内再按视深度降序；深度键是：

```text
depth = dot(emitterWorldCenter - cameraPosition, normalize(cameraForward))
      + emitterBoundsRadius
```

包围球中心取场景模型与发射器局部偏移作用后的原点。局部半径按
`source/particles/particleEmitterBounds.h` 的纯函数计算：

```text
maxSpeed[i] = max(abs(velocityMin[i]), abs(velocityMax[i]))
maxBillboardHalfSize = max(sizeStart, sizeEnd, 所有 sizeKeys[].size)
localRadius = shapeRadius
    + lifetimeMax * (|maxSpeed| + 0.5 * |gravity| * lifetimeMax)
    + maxBillboardHalfSize
worldRadius = localRadius * max(模型线性部分三列 scale 的范数)
```

beam 是例外：`localCenter` 取变换后的线段中点，包络半径为
`0.5 * |end-start| + branchLength * |end-start| + branchSpread +
sqrt(2) * noiseAmplitude + width +
(1 + 2 * branchLength) * sqrt(3) * endpointJitter`（无分支时分支项为 0），
再乘同一个 `maxColumnScale`。

逐轴取绝对值最大者保证非对称速度区间（如 `[-400, 20]`）也按最远可达速度外扩；
`maxBillboardHalfSize` 保证大尺寸低速烟雾不会被视锥/距离/屏幕尺寸提前剔除，也不会在
跨发射器排序中错位。
beam 的自动中心是变换后的线段中点，局部半径见「Beam 电弧」；它不会按发射器原点
参与剔除。
`sortOrder` 相同时再比较视深度；深度也相同时按稳定身份 key
（`sceneName|assetPath|emitterName`）字典序升序排序，不使用 `unordered_map` 的迭代
顺序参与 tie-break。`sortOrder` 是主序，否则深度排序会把作者的层级意图重新打散。

发射器级顺序仍按包围球远→近作为回退。满足 alpha billboard 或 alpha ribbon、
总容量 ≤ 8192 且唯一纹理 ≤ 8 时，所有参与发射器 gather 到一个全局记录池，
做一次全局逐 quad bitonic 排序后 `drawIndirect`；ribbon 容量按
`maxParticles × ribbonSegments` 估算。additive/mesh/beam/distortion 或超限场景
保留逐发射器路径。
beam 与 distortion 永远不进入 gather 记录池。全局路径与未参与发射器
之间仍是发射器级先后近似；发射器集合增删导致排序表短暂失配时，未进入排序表的池按原
map 顺序补画。全局 alpha 池本身作为一个特殊 draw item 参与同一份排序：`sortOrder`
取池内成员最小值，位置取池内**背景边缘**（最大视深）的成员，轮到该项时一次性录制
`RecordGlobalDraw`。因此它不再固定拖到所有逐发射器之后，中近景碎片/弹壳可以正确
盖在烟前；整池只有一个插入位置，池跨越很大深度范围时仍是近似。

可见性剔除在排序之前完成，但不会改写排序公式：`RecordSimulation` 仍为回退路径计算
远→近顺序，`RecordGraphics` 只跳过 `culled` 发射器。逐发射器与全局路径都使用
GPU 写回 instanceCount 的 indirect draw。

## RenderGraph 位置

`config/renderGraphConfig.json` 的 `particle` pass 位于 `forwardTransparent` 之后、`editorGrid`
与后处理之前，只输出 `sceneColor`（`loadOp=load`、`storeOp=store`），并把 `sceneDepth` 作为
`binding 0` 的图输入。`depthTestEnable=false`、`depthWriteEnable=false`；pass 不挂深度附件，
因此不会形成采样与附件写入的反馈环。

紧随其后的 `particleDistortion` pass（`type: "distortion"`）输出全分辨率 RGBA16F
`distortion` 资源（`clearColor = [0,0,0,0]`、`loadOp=clear`、`storeOp=store`），同样以
`sceneDepth` 为 `binding 0` 输入，不创建 pass material，`needMsaa=false`。它只绘制
`renderMode == "distortion"` 的 emitter；`RecordGraphics` 则跳过 distortion emitter，
其它 emitter 仍走正常粒子绘制。
`bloomPrefilter` 在 `binding 0/1` 的 sceneColor/autoExposure 之后追加 `binding 2` 的
`distortion` 输入，预过滤的四个 sceneColor tap 都先经过同一扰动映射；`toneMapping`
仍通过 `binding 5` 采样该资源。两端公式必须逐字一致：

```text
vec2 d  = distortion.rg
aspect  = width / height
sceneUV = clamp(inUV + vec2(d.x / aspect, d.y), 0, 1)
```

`distortion.rg` 的单位是**屏幕高度归一化偏移**：`1.0` 表示一个屏幕高度，横纵相同数值
对应相同像素距离；消费端把 x 除以宽高比后才是 UV 位移。因此该偏移与视口比例无关，
不会在非正方形视口下被横向拉伸。

Bloom 因而从扭曲后的 sceneColor 生成；`toneMapping` 仍在未偏移的 `inUV` 取 Bloom，
只把它叠加到偏移后的 sceneColor 上，避免 Bloom 被二次扭曲。`autoExposure` 继续以
未扭曲的 sceneColor 作为唯一输入，这是有意保留的曝光边界。raw debug 视图与速度可视化
不消费扰动。

当前 render graph 没有 SSR pass，因此没有可接入的 SSR 采样点。将来新增 SSR 时，应在
SSR 的 sceneColor/G-Buffer 屏幕采样入口复用上面的映射公式，而不是改变反射射线方向；
同时保持 autoExposure 读取未扭曲 sceneColor 的既有边界。

当前它不是并进 `forwardTransparent` 的：后者同时写 `selectionMask`，粒子 fragment 只写
`sceneColor` 时会让粒子覆盖区的选择遮罩变成未定义值。selection/outline 独立成 pass 之前，
粒子保持独立 pass 是当前的安全边界。

## 作者字段

`PS_*.json` 根对象必填 `type: "particleSystem"`、`name`。当前支持两种写法：

- 新写法：根对象包含非空 `emitters` 数组，最多 16 项；每项必须是对象，`name` 默认
  `emitter<index>` 且在整个系统内唯一，`position` 是可选的局部偏移 `[x,y,z]`（cm）。
- 旧写法：根对象没有 `emitters`，按一个隐式 emitter 解析；忽略根对象上的 `position` 语义，
  局部位置固定为零。

以下字段在多发射器写法中位于每个 `emitters[i]` 对象上；旧写法位于根对象上。当前支持：

- `spawnRate`：连续发射率（粒子/秒）；`burst: true` 时忽略连续发射，只在
  `burstInterval == 0` 时于首帧把整池喷出。
- `burstInterval`：秒，默认 `0`，范围 `[0, 60]`。`burst: true` 且该值 `> 0` 时按周期
  整池重触发；`0` 保持一次性 burst 语义。详见「Burst 重触发」。
- `lod.tiers: [{ "distance": cm, "spawnRateScale": 0..1 }, ...]`：多级 LOD，最多 4 档，
  距离严格递增。按发射器中心到相机距离选择已越过的最远档位缩放连续发射预算；
  burst、prewarm 池填充、寿命、尺寸与渲染不受影响。
- `lodDistance` / `lodSpawnRateScale`：单档兼容写法。`lod.tiers` 非空时被忽略；
  为空时发射器中心距离超过 `lodDistance` 后 `spawnRate *= lodSpawnRateScale`。
- `lifetime: [min, max]`：秒，`0 < min <= max <= 60`。
- `lifetimeScaleKeys`：可选的归一化年龄倍率曲线，最多 8 个
  `{ "time": 0..1, "scale": 0..4 }` key；空或缺省表示恒为 `1`。详见
  「Lifetime Scale Curve」。
- `shape`：发射形状，默认 `{ "type": "sphere", "radius": 10 }`。`type` 只能是
  `sphere`、`box`、`cone`、`cylinder`、`disc`、`ring`；未知类型直接拒绝。
  - `radius`：cm，默认 `10`，必须非负。`sphere` 用它定义球半径；`disc` 用它定义
    圆盘半径；`ring` 用它定义外半径；`cone`/`cylinder` 用它定义底面半径。
  - `extent`：`[x,y,z]` cm，默认 `[50,50,50]`，是 `box` 的三轴半尺寸。所有分量
    必须非负；`type == "box"` 时三轴都必须 `> 0`。其它形状仍校验该字段，避免切换
    形状时把非法值带进运行时。
  - `height`：cm，默认 `100`，必须非负；`cone`/`cylinder` 必须 `> 0`。锥体从
    底面 `y=0` 向 `+y` 收束，圆柱沿 `y` 轴居中于 `[-height/2, +height/2]`。
  - `innerRadius`：cm，默认 `50`，必须非负；`ring` 必须满足
    `0 <= innerRadius < radius` 且 `radius > 0`。
  - `disc`/`ring` 位于发射器局部 `XZ` 平面 `y=0`；`box` 以发射器原点为中心。
- `velocity: { "min": [x,y,z], "max": [x,y,z] }`：局部空间初速度，发射器矩阵的线性部分会旋转/缩放它。
- `gravity: [x,y,z]`、`drag`：世界空间重力与指数阻力。
- `size: [start, end]`：billboard 世界空间半边长（cm），按寿命线性插值。
- `renderMode`：`"billboard"`（默认）、`"ribbon"`、`"mesh"`、`"beam"`、
  `"distortion"`、`"light"` 或 `"decal"`。ribbon 用当前点与最近 3 个历史点生成面向相机的条带；
  mesh 用静态 SM 网格按粒子实例化；beam 在作者化起止点之间生成多条带噪声的并行弧；
  distortion 仍消费粒子池，但绘制到独立 UV 偏移缓冲；light 不渲染几何，复用 GPU
  模拟池并写入独立 per-particle light SSBO，详见「Light 粒子灯（GPU per-particle light）」；
  decal 同样不渲染几何；GPU simulate 后的存活粒子由 gather 回读并映射为
  BeforeLighting DBuffer decal，详见「Decal 粒子贴花（GPU 模拟 + CPU fallback）」。
- `billboardAlignment`：仅 `renderMode: "billboard"` 使用，默认 `"camera"`，可选
  `"camera"`、`"velocity"` 或 `"fixed"`。`velocity` 用粒子速度方向构造 billboard 基，
  让 quad 的法线对齐速度；`fixed` 用作者化的发射器局部基固定 quad 朝向，适合枪口闪光等
  不随相机转动的卡片。任何模式退化时都回退 camera 基，详见「Billboard 朝向」。
  ribbon、mesh 与 beam 忽略该字段。
- `billboardNormal`：`vec3`，仅 `billboardAlignment: "fixed"` 有意义，默认
  `[0, 0, 1]`。它是 quad 在发射器局部空间的面法线，上传前乘 `emitter.model` 线性部分；
  必须有限且非零。作者无需预先归一化，运行时归一化。
- `billboardUp`：`vec3`，仅 `billboardAlignment: "fixed"` 有意义，默认
  `[0, 1, 0]`。它是发射器局部空间上方向提示，必须有限、非零，且与归一化后的
  `billboardNormal` 不平行（`|dot| < 0.999`）。渲染时先构造
  `right = normalize(cross(up, normal))`，再用 `cross(normal, right)` 正交化上方向。
- `ribbonWidth`：ribbon 世界空间半宽（cm），默认 `5`，必须 `> 0` 且有限。
- `ribbonSegments`：ribbon 输出段数，默认 `3`，范围 `[1, 32]`；历史固定 16 点，
  顶点阶段按弧长重采样到该段数。
- `ribbonFade`：尾端 alpha 衰减，默认 `1`，范围 `[0, 1]`；`0` 不衰减，`1` 时最旧端完全透明。
- `ribbonUVTiling`：沿弧长的 UV 平铺次数，默认 `1`，范围 `(0, 64]`。
- `ribbonWidthKeys`：可选逐弧长宽度乘数，最多 8 个
  `{ "time": 0..1, "width": 0..8 }`；time 严格递增，空表恒为 1。
- `distortionStrength`：默认 `0.02`，范围 `[0, 0.25]`，仅
  `renderMode: "distortion"` 使用。它是屏幕高度归一化偏移的最大比例
  （`1.0` = 一个屏幕高度），与视口宽高比无关；消费端把 x 除以 `width / height`
  后才得到 UV 位移。最终偏移还会乘纹理/程序化方向与覆盖率。`0` 表示不扰动，
  非法或非有限值在加载期拒绝。
- `beam`：`renderMode: "beam"` 时使用；省略时每个字段取默认值。结构为
  `{ "start": [-200,0,0], "end": [200,0,0], "segments": 16, "width": 3.0,
  "count": 4, "noiseAmplitude": 18.0, "noiseFrequency": 3.0,
  "noiseSpeed": 5.0, "endpointJitter": 0.0, "branchCount": 0,
  "branchStart": 0.5, "branchLength": 0.3, "branchSpread": 20.0 }`。
  `start`/`end` 是发射器局部空间端点（cm），都必须有限且不能重合；`segments` 是每条弧的
  段数，范围 `[2, 64]`；`width` 是横向半宽（cm），必须 `> 0`；`count` 是并行弧数量，
  范围 `[1, 16]`；`noiseAmplitude`（cm）、`noiseSpeed` 与 `endpointJitter`（cm）必须
  非负；`noiseFrequency` 必须 `> 0`。`branchCount` 是每条并行弧的分支数，范围 `[0, 4]`；
  `branchStart` 是分支根在主弧上的参数位置，范围 `[0, 1]`；`branchLength` 是分支长度
  相对主线长度的比例，范围 `(0, 1]`；`branchSpread`（cm）是分支末端横向偏移，必须非负。
  `beam` 必须是对象；非对象、缺失/非法 `vec3`、越界计数与非有限值都在加载期拒绝。
  作者应让 `maxParticles >= count`，但加载器不会静默改写 `maxParticles`。
- `beam.colorKeys` / `beam.widthKeys`：可选的 beam 归一化弧长控制曲线，最多 8 个
  `{ "time": 0..1, "color": [r,g,b,a] }` / `{ "time": 0..1, "width": 0..1 }` key。
  time 必须严格递增；颜色通道范围 `[0,64]`，宽度是 `beam.width` 的乘数。空表保持
  `colorStart/colorEnd` 与恒定宽度。曲线同时作用于主弧和分支，按分支自身归一化
  `t` 采样。
- `beam.arcs`：可选的逐主弧覆盖，最多 16 项且非空时必须与 `count` 相等。每项可写
  `colorStart`/`colorEnd`（乘性 RGB tint）、`widthScale [0,1]`、`intensity [0,64]`、
  `noisePhase [-1000,1000]`；分支继承所属主弧。它不是逐弧 materialInstancePath；
  当前 beam 仍是单 draw / 单 set 0 descriptor，逐弧真实材质需要拆分 draw 与 UBO range。
- `beam.hitScan`：可选的 CPU 射线/平面命中，结构为
  `{ "planeNormal": [0,1,0], "planeOffset": 0, "maxDistance": 2000 }`。
  `planeNormal` 必须非零，`planeOffset` 是归一化平面法线上的世界距离，
  `maxDistance` 是沿 `start -> end` 方向的有限最大距离。命中时把 UBO 的 beam 终点
  替换为命中点；否则保留作者化 `end`。这是受控平面命中，不是任意物理 mesh raycast。
- `light`：`renderMode: "light"` 时必填，结构为
  `{ "intensity": 300.0, "radius": 700.0 }`。`intensity` 单位是 cd，`radius` 单位是 cm，
  两者都必须有限且 `> 0`。颜色与 alpha 来自 `EvaluateColorCurve(lifeT)`：有
  `colorKeys` 时使用曲线，否则 CPU 会填 `colorStart`/`colorEnd` 两个端点 key。
  alpha 参与强度衰减；`radius` 已由实时点光求值消费（见下文半径窗）。`light` 必须是
  对象；缺失、非对象或非有限/非正值在加载期拒绝。
  light 发射器的 `maxParticles` 必须在 `[1, 64]`，且不要求 alpha 排序开关。
- `decal`：`renderMode: "decal"` 时必填，结构为
  `{ "size": [100,400,400], "rotation": [0,0,0], "color": [0.02,0.018,0.015],
  "opacity": 0.85, "edgeSoftness": 0.35, "noiseStrength": 0.55, "roughness": 1.0,
  "metallic": 0.0, "ambientOcclusion": 1.0, "normalStrength": 0.35,
  "decalResponse": "colorNormalRoughness", "albedoMap": "", "normalMap": "",
  "ormMap": "", "sortOrder": 0 }`。`size` 是完整局部尺寸（cm），X 是投影深度、
  Y/Z 是投影平面宽高，每轴必须在 `(0, 100000]`；`rotation` 单位是度且必须有限；
  `color` 三分量在 `[0, 64]`；`opacity`/`edgeSoftness`/`noiseStrength`/
  `roughness`/`metallic`/`ambientOcclusion`/`normalStrength` 都在 `[0, 1]`。
  `decalResponse` 复用场景 decal 的 UE 枚举映射；三个贴图字段必须是字符串，空字符串
  表示程序化外观，非空路径由 `DecalRenderer::Synchronize` 加载并在失败时回退默认纹理。
  `sortOrder` 是 `[0, 255]` 整数。`decal` 必须是对象；缺失、非对象、未知 response
  或任何越界/非有限值都在加载期拒绝。decal 发射器不要求也不需要 `sortParticles`。
- `renderMode: "skeletalMeshTris"` / `"skeletalMeshBones"`：骨骼网格粒子。两者都通过
  `mesh.path` 指向骨骼 SM 资产（含 SK/片段），运行时用 `MeshAssetLoader` 导入为
  `SkeletalAsset`；加载失败与 mesh 相同，回退 billboard 并只告警一次。
  - `skeletalMeshTris` 为每个粒子输出一个从网格表面采样到的三角面片：中心是面片重心，
    顶点为重心相对坐标；面法线是网格模型空间叉积。
  - `skeletalMeshBones` 为每个粒子输出一段骨棒（圆柱薄片）：中心是骨段中点，端点为
    重心相对坐标，面法线取切线与视线平面的垂线；`skeletalBoneRadius`（默认 `0.35` cm，
    范围 `[0.01, 100]`）是骨棒半宽。
  - 两种模式都**没有顶点输入**，逐粒子展开 3/6 个顶点；`maxParticles` 决定同屏面片数量。
  - GPU skinning：顶点缓冲保存绑定姿态，`particleSkeletalSkin.glsl` 在顶点着色器里按
    set 2 的关节 palette（每个 swapchain image 一份）与 CSR 影响流计算
    `Σ w · (model_j · inverseBind_j) · p_bind`。palette 每帧写入当前姿态，因此同一帧的
    可见网格、CSM/局部阴影 caster 看到完全相同的姿态。
  - 每 section 独立材质：每个骨骼 section 拥有自己的 `materialInstancePath`（section 的
    `sectionPlans[].materialInstancePath`，缺失时回退发射器级
    `materialInstancePath`）、数值参数与四张贴图，绘制时逐 section 绑定 set 1。
- `skeletalPose`：可选，`{ "clip": "", "time": 0, "rate": 0, "loop": true }`。
  `clip` 为空时使用绑定/静止姿态；否则必须是资产中存在的片段 id（未知片段在加载期拒绝）。
  `time` 是起始秒数（`>= 0`），`rate` 是播放倍率（`[-16, 16]`；`0` 表示冻结在该帧），
  `loop` 决定时钟回绕到片段时长还是夹在末尾。姿态时间由**发射器自己的**
  `elapsedSeconds` 驱动，因此同一骨骼网格的不同发射器可以停在不同帧。
- `spawnSource`：可选，`{ "mode": "none", "parentEmitter": "", "parentEvent": "death",
  "perEvent": 1, "offset": [0,0,0], "normalVelocityScale": 0 }`。
  - `mode` 可为 `none`、`skeletalTris`、`skeletalBones`、`parentEmitter`。缺省时骨骼粒子
    renderMode 自动推导为对应的 `skeletalTris`/`skeletalBones`；载入的网格不是骨骼网格时
    运行期降级为 `none`（回落到形状采样），不会读空源。
  - 骨骼源的记录由**绑定姿态**的网格烘焙：三角形按面积、骨段按长度累加成前缀和，
    shader 以总长做逆 CDF 采样，因此大面积三角形/长骨头分到更多粒子；同一帧内每个
    源项由原子计数器独占，不会出现两个粒子挤在同一面片。
  - `parentEmitter` 必须指向同一 `PS_*.json` 内另一个存在的发射器名（不能是自身），
    `parentEvent` 只能是 `spawn`、`death`、`collision`，`perEvent` 在 `[1, 64]`。
    父发射器每帧最多写 1024 条事件记录；子发射器消费**同一帧**父发射器的事件，
    模拟顺序按依赖深度排序（与 unordered_map 迭代顺序无关）。`offset` 是加到事件
    位置上的模拟空间偏移（cm）。
  - `normalVelocityScale > 0` 时用源法线（三角面法线或骨段轴向）替换作者初速度方向，
    大小即该值。
- `dissolve`：可选，mesh（含骨骼网格）粒子的溶解/alpha erosion，
  `{ "enabled": false, "start": 0.2, "end": 0.9, "edgeWidth": 0.08,
  "edgeColor": [1,0.35,0.05], "edgeStrength": 4, "noiseScale": 4, "noiseSpeed": 0.6 }`。
  片元用 4 阶 value-noise FBM 与 `lifeT` 推进的阈值做 `smoothstep` erosion：阈值从
  `start` 推进到 `end`（都是归一化寿命，`end >= start`），`edgeWidth` 是边缘宽度
  （`[0, 0.5]`），阈值附近保留 `edgeColor × edgeStrength` 的自发光边缘。顶点阶段同时把
  顶点沿粒子重心方向按同一段溶解区间微微外推（外推量 = `edgeWidth × 4`，夹到 `[0, 1]`），
  使面片变透明前略微膨胀。`noiseScale` 在 `(0, 64]`，`noiseSpeed` 在 `[0, 16]`。
- `materialBindings`：可选，把 World 级 typed Data Channel 绑定到材质/精灵参数。key 只能是
  `baseColor`、`emissive`、`emissiveStrength`、`roughness`、`metallic`、`opacity` 或
  `color`，value 是非空通道名。绑定在每个发射器每帧读取 World
  `particleDataChannels`（通道不存在时保留作者值），数值子集与 MI 数值参数共享同一套
  范围夹取（`roughness >= 0.02`、`metallic/opacity ∈ [0,1]`，`baseColor` 与 `color`
  RGBA 通道 `[0, 64]`）。它不是 Material Graph：只覆盖 `ParticleMaterialUniforms` 与
  `colorTint` 已有的标量/颜色槽位。
- `mesh`：`renderMode: "mesh"` 时必填，结构为
  `{ "path": "Common/Meshes/SM_cube.json", "scale": [1,1,1], "alignment": "world",
  "materialInstancePath": "..."? }`。
  `path` 必须是资源根内、相对路径、无 `..` 的 `.json`；service 保存/validate 还会检查
  文件存在与 resource root 边界。`scale` 三个分量必须有限且向量非零；`alignment` 只能是
  `world`、`camera` 或 `velocity`。`materialInstancePath` 可选，只读取
  `u_baseColor/u_emissive/u_emissiveStrength/u_roughness/u_metallic` 数值子集。
  mesh v1 不允许与 `localSpace: true` 组合。
- `meshLighting`：默认 `true`，仅 `renderMode: "mesh"` 使用。开启时按场景主方向光计算
  完整 DefaultLit 光照：全部方向光/点光/聚光 + 全部 GPU 粒子灯，GGX/Smith/Fresnel
  直接光 + 预过滤 cubemap/BRDF LUT 的 IBL，方向光额外采样 CSM；关闭时 mesh 仍走
  unlit 输出。billboard 与 ribbon 忽略该字段。
- `castShadows`：默认 `false`，`renderMode` 为 `mesh` / `billboard` / `ribbon` 时可用；
  beam、light 与 decal 不投影。mesh 用同一份 `particleMeshTransform`；billboard/ribbon
  生成程序化卡片/条带并以固定 `0.35` alpha test 做 masked shadow。二进制 shadow map
  只适合有较密芯部的效果；半透明烟/尘需要逐灯 transmittance 累积（当前未实现），
  所以案例里默认只给 mesh 碎块开启。CSM 的 4 级会提交同一批 caster（已按级联
  视锥剔除），局部阴影按灯范围对发射器包围球做保守剔除。
- mesh 粒子的 `materialInstancePath` 现在除数值参数外还读取 `textures`
  （albedo/normal/pbrParam/emission）与 UV scale/offset；纹理走独立 set 1，
  缺失时回退白图/flat normal/白 ORM/黑 emissive。仍是合并 section 的单材质，
  没有 Material Graph；非均匀 mesh scale 的切线基未做逆转置。
- `meshAmbient`：默认 `0.2`，仅 `renderMode: "mesh"` 且 `meshLighting: true` 时参与光照。
  它同时缩放常量 ambient 与 IBL 环境项，范围 `[0, 1]`；负值、超过 1、NaN 与 Infinity
  在加载和编辑器写入阶段都会被拒绝。
- `billboardLighting`：默认 `false`，仅 `renderMode: "billboard"` 使用。开启后以实际
  billboard 平面的世界空间面法线（camera alignment 时朝向相机，velocity alignment 时
  朝向速度）计算**全部**方向光/点光/聚光 + GPU 粒子灯的 wrapped Lambert 直接光
  （wrap = 0.5），并叠加常量 ambient 与 IBL 漫反射；方向光可按 `billboardShadows`
  采样 CSM。unlit 路径保持不变。
- `billboardAmbient`：默认 `0.2`，仅 `renderMode: "billboard"` 且
  `billboardLighting: true` 时参与光照，范围 `[0, 1]`；它乘粒子颜色作为常量环境项。
- `billboardLightingStrength`：默认 `1.0`，范围 `[0, 1]`，只有
  `billboardLighting: true` 时生效。
  这是“场景光照替换 authored 颜色”的比例：0 保留原 unlit 颜色，1 完全使用场景光照；
  用插值而非纯乘法，避免 authored 颜色在暗场景掉亮度或在强光附近过曝。
  ribbon 使用同一对开关（法线取面向相机的视线方向）。
  `billboardLighting` 与 `billboardShadows` 同时开启时，直接光乘 CSM shadow，而不是
  再叠加旧的 50% 调暗。additive billboard 也消费同一光照；需要自发光时应显式关闭
  `billboardLighting`。
- `billboardSpecular` / `billboardRoughness` / `billboardForwardScattering`：
  默认 `0.1` / `0.6` / `0.35`，范围 `[0,1]` / `[0.02,1]` / `[0,2]`。
  前两者给 sprite 一个廉价 GGX 镜面 lobe；后者是归一化 Henyey-Greenstein
  前向散射项，让逆光烟产生透光感。三项都只在 `billboardLighting: true` 时
  生效，并共享 `billboardLightingStrength` 的 authored→lit 插值。ribbon
  也使用同一组参数（法线取面向相机的视线方向）。
- `billboardDensity`：默认 `0`，范围 `[0, 0.25]`（1/cm）。大于 0 时片元按
  `exp(-density * 2 * halfSize / max(|dot(L,V)|, 0.15))` 逐灯衰减直接光与 GPU 粒子灯，
  用薄片厚度近似半透明烟的透光；ambient/IBL 不衰减。
- `rotation`：billboard 初始旋转角，单位度，任意有限数，默认 `0`。
- `rotationSpeed`：随时间累加的旋转速度，单位度/秒，默认 `0`，范围 `[-3600, 3600]`。
- `rotationRandom`：默认 `false`；为 `true` 时每个粒子用稳定相位叠加 `[0,360)` 度的初始角。
- `colorStart` / `colorEnd`：RGBA，按寿命线性插值；alpha 会乘 `opacity`。
- `colorKeys: [{ "time": 0..1, "color": [r,g,b,a] }, ...]`：最多 8 个，按时间严格递增。
- `sizeKeys: [{ "time": 0..1, "size": cm }, ...]`：最多 8 个，按时间严格递增。
- `texture`：相对资源根的图集路径；为空时走程序化形状。作者侧保存/validate 只接受
  `png/jpg/jpeg/tga/bmp`，必须留在 resource root 内且文件存在；运行时仍保留存在性检查，
  单个纹理解码或 GPU 创建失败时回退 1x1 白图并记录一次 warning，不终止渲染线程。
- `eooPacked`：默认 `false`。UE NiagaraExamples 的 EOO 图集按
  `R=Emissive / G=Occlusion / B=unused / A=Opacity` 解码；albedo 使用作者颜色，
  R 进入独立自发光项，避免 B=0 被当成彩色贴图或整张卡片被当成常量自发光。
- `eooEmissiveStrength`：默认 `1`，范围 `[0, 16]`；只乘 EOO 的 R 自发光项。
- `flipbook: { "columns": 1..16, "rows": 1..16, "fps": 0..240, "startFrame", "endFrame",
  "loop", "randomStartFrame", "perParticle", "blend", "playRate" }`：`startFrame`/`endFrame`
  是 atlas 内的闭区间帧号，省略
  `endFrame` 时解析为 `columns*rows-1`；两者都必须落在 atlas 内且 `endFrame >= startFrame`。
  `fps=0` 时按寿命在区间内播放一轮；`fps>0` 时按模拟时间推进，`loop=true` 回绕，
  `loop=false` 停在 `endFrame`。`randomStartFrame` 控制是否用粒子 index 打散起始相位。
  `perParticle=true` 时相位由逐粒子年龄驱动；`blend=true` 时在相邻两帧之间线性混合
  （几何/pivot/rotation 使用当前帧，纹理在 frame A/B 间插值）；`playRate` 默认 `1`、
  范围 `[0,16]`，同时乘到 fps 与寿命驱动模式。
- `flipbook.frameDurations: [weight, ...]`：可选逐帧相对时长权重表，长度必须为 `0` 或精确等于
  `endFrame-startFrame+1`，每项 `>0`。非空时按权重分配帧时间，但一轮总时长仍由
  `frameCount/fps`（或 `fps=0` 时的寿命）决定，语义见「Flipbook 逐帧时长与轴心」。
- `flipbook.framePivots: [[u,v], ...]`：可选逐帧轴心表，长度同样为 `0` 或精确等于帧数，
  每项分量在 `[0,1]`。默认 `(0.5,0.5)`；非空时移动 billboard quad 中心。
- `flipbook.frameUVs: [[u0,v0,u1,v1], ...]`：可选逐帧 sprite UV 子矩形，长度同样为 `0`
  或精确等于帧数；分量都在 `[0,1]` 且 `u0 < u1`、`v0 < v1`。GPU 表每帧一个 `vec4`，
  因此带 UV rect 的帧区间上限是 64；超过 64 帧的资产必须省略该表，否则校验失败。
- `flipbook.frameRotations: [deg, ...]`：可选逐帧旋转角，长度同样为 `0` 或精确等于帧数，
  每项必须是有限数。GPU 表每 `vec4` 打包 4 帧，帧区间上限 64；超过时必须省略该表。
- `softness: 0..1`：圆盘边缘软化程度。
- `softParticles`：默认 `true`。片元始终按 `sceneDepth` 剔除被不透明几何遮挡的粒子；该开关只控制
  交线附近的 alpha 淡出。
- `softParticleDistance`：cm，默认 `25`，范围 `[0, 10000]`。片元用 `clamp((sceneDepth -
  particleDepth) / softParticleDistance, 0, 1)` 缩放 alpha。
- `softParticleTapRadius`：软粒子 sceneDepth 十字采样半径（像素），默认 `4`，
  范围 `[0, 32]`；0 退回单采样。5 个 tap 取最近不透明表面，兼顾深度碰撞与交线淡出。
- `softParticleThickness`：cm，默认 `0`，范围 `[0, 10000]`。非零时按局部深度差
  限制 fade 增长率，避免薄几何在软深度过滤边缘出现硬切；0 保持旧行为。
- `sortParticles`：默认 `false`。所有 alpha 发射器（billboard/ribbon/mesh）必须为 `true`，否则资产校验以
  “alpha blend requires sortParticles=true” 拒绝；每帧在 compute 中按相机 view-space
  深度执行 GPU bitonic sort。additive 发射器应保持关闭，排序不会改变加法结果。
  beam 不参与逐粒子排序，因此 alpha beam 不要求该开关；即使作者写 `true` 也没有排序
  dispatch。distortion 也豁免该检查，因为它的累积统一走加法缓冲；开启排序时
  distortion 仍可使用局部排序结果，但不会进入全局 gather。
- `blend: "additive" | "alpha"`。
- `maxDrawDistance`：cm，默认 `0`（不限）。按发射器包围球最近点判断，最近点超过阈值时
  整个发射器剔除。
- `fadeDistance`：cm，默认 `0`（关闭）。仅在 `maxDrawDistance > 0` 时有效，必须在
  `[0, maxDrawDistance]` 内；在 `maxDrawDistance - fadeDistance` 到 `maxDrawDistance`
  之间把 alpha 从 1 线性淡到 0。
- `minScreenSize`：px，默认 `0`（关闭）。发射器包围球的透视投影直径小于阈值时剔除。
- `maxScreenSize`：px，默认 `0`（关闭）。逐 billboard 的屏幕半边长超过阈值时，只缩小
  该 billboard 的世界半边长；粒子中心位置、寿命和曲线数据不变。
- `collision`：可选世界空间碰撞对象，字段为 `type`（`none`/`plane`/`box`/`capsule`/`sdf`/`mesh`）、
  `plane.normal`、`plane.offset`、`box.min`、`box.max`、`capsule.a`、`capsule.b`、
  `capsule.radius`、`sdf.path`、`mesh.path`、`bounce`、`friction`、`radius`、
  `killOnCollision` 与 `ccd`。各类型公式、默认值和边界见「碰撞」。

`maxParticles` 上限 65536；`spawnRate` 上限 100000。球体采样使用 `cbrt` 半径分布，避免粒子堆在球心。
排序开启时 `maxParticles` 上限为 4096。
light 发射器的 `maxParticles` 另受 64 上限约束。

所有数值字段（位置、速度、重力、寿命、尺寸、旋转、spawnRate、burstInterval、LOD、
drag、shapeRadius、曲线 key、逐帧时长/轴心/UV 等）必须是有限数；NaN/Inf 在加载与
编辑器写入阶段都会被拒绝。
`maxParticles`、`flipbook.columns/rows`、帧号等计数字段必须是整数，小数不再静默截断。

四个可见性字段都必须是非负有限数。`maxDrawDistance == 0` 时 `fadeDistance` 必须也是 `0`；
`maxDrawDistance > 0` 时 `fadeDistance` 不得大于 `maxDrawDistance`。

`burstInterval` 必须在 `[0, 60]`；`lodDistance` 非负；`lodSpawnRateScale` 在 `[0, 1]`；
`lod.tiers` 最多 4 项，`distance` 非负且严格递增，`spawnRateScale` 在 `[0, 1]`。
`rotationSpeed` 必须在 `[-3600, 3600]`；`rotation` 只要求有限，不做角度回绕。

编辑器 `particle-emitter-select` 从文档的 `emitters` 数组填充；旧格式文档显示一个
`emitter0`。字段修改在显式多发射器文档中写入 `emitters.<index>.<field>`，旧格式继续使用根路径。

选择器右侧提供 `+`（复制当前发射器）、`Del`、`Up`、`Down` 四个结构按钮，上限 16、下限 1；
无文档、到达上限、只剩一个或已在首/尾时对应按钮禁用。新增把选中项完整复制（含 `position`）
插到其后，名称以 `<当前名>_copy` 为基，冲突时追加 `_2`、`_3`…；选择跟随新项。删除后选择
`min(index, count-1)`；移动后选择跟随被移动项。

旧隐式文档第一次执行新增时升级为显式 `emitters`：根字段复制成 `emitter0`
（移除 `type`/`emitters`/`position`，名称设为 `emitter0`），根对象保留 `name`/`type` 等
系统字段，再复制它插入第二项。非法条目缺少可用 `name` 时按 `emitter<index>` 参与唯一性判断。

结构编辑不新增命令类型，仍走 `ModifyParticleDocument`，一次提交整条
`EditorAssetFieldChange{field = "emitters", value = <array>}`。提交前先把尚未 flush 的
字段草稿合并进工作副本（先应用整数组、再应用字段路径），因此两帧内连续操作不会互相覆盖，
同一帧的多次增删/移动会合并成一条整数组命令。变更进入 pending 草稿，由下一帧 `Build()` 的
flush 序列提交，不等待保存；结构操作失败时保留 pending 字段草稿，不丢弃用户输入。
`ParticleAssetLoader::ParseJson` 仍会拒绝 0 项、超过 16 项、重名或其他非法结果。

### 能力表、身份与命令反馈

`particle.get_capabilities` 按文档形态发布字段表：

- 显式多发射器文档的根 `fields` 只有系统级 `name` 与受控结构字段 `emitters`；
  每个 `emitters[i]` 额外带 `name`、`index` 与完整的 `fields`（emitter 名称、局部 position、
  发射/寿命/速度/重力/碰撞/尺寸/颜色/flipbook 等，不含 `emitters` 自身）。
- 旧隐式文档的根 `fields` 保持原有可写字段但去掉 `position`；`emitters[0].fields`
  给出同一组 emitter 字段（同样不含 `emitters`）。

因此显式文档上写根级 emitter 字段（如 `spawnRate`、`maxParticles`、`colorKeys`）会被拒绝，
错误要求使用 `emitters.<index>.<field>`；根 `name` 仍表示系统名。字段变更可带
`expectedEmitterName`：服务在应用整批修改前记录每个发射器的原名，再逐字段比较；
同一批次里先改名再改其他字段不会误报身份变化，名字与 `emitters[index]` 不匹配则拒绝。

UI 对所有多发射器字段编辑和曲线/重置操作都带当前 emitter 名；整条 `emitters` 结构编辑不带。
`ModifyParticleDocument`、保存、回退、重载、校验与关闭的终态通过
`MaterialInstanceEditorSnapshot::particleCommandFeedback` 发布（含 sequence、路径、命令类型、
成功标志与 message）。面板只把 `ModifyParticleDocument` 的终态当作在途修改的解除条件；
若快照 revision 已超过提交版本或文档消失，也会解除在途状态，避免永久堵塞。

### Flipbook 逐帧表

`flipbook.frameDurations`、`flipbook.framePivots`、`flipbook.frameUVs` 与
`flipbook.frameRotations` 都是可选的逐帧表，长度为 `0` 或精确等于
`endFrame-startFrame+1`；四者独立，可以只提供其中几张。图集最大 16×16，所以区间帧数上限是
256。`frameDurations` 每项必须是有限且 `>0` 的**相对时长权重**；`framePivots` 每项是 `[u,v]`，
两个分量都必须在 `[0,1]`；`frameRotations` 每项是有限角度（度）。
长度不匹配、`<=0` 的时长、越界轴心或非有限旋转都会在加载阶段拒绝。

Flipbook 模块的普通字段下方提供逐帧表格。行数严格等于 `endFrame-startFrame+1`，帧区间为
`0` 或超过 `256` 时只显示提示，不生成行或操作按钮。数组缺失时表格仍按 `duration=1`、
`pivot=(0.5,0.5)`、`UV rect=(0,0,1,1)` 显示全部行；用户首次编辑某一行时才创建完整数组，
已有数组会先补齐/截断到当前帧数。`Seed durations` 创建全 1 权重数组，`Uniform pivots`
创建全 `(0.5,0.5)` 轴心数组，`Seed UV rects` 创建全 `(0,0,1,1)` 矩形，
`Seed rotations` 创建全 0 角度数组；四个 `Reset` 按钮分别提交对应字段的 `reset=true`。
逐行与批量按钮都提交整条
`flipbook.frameDurations`、`flipbook.framePivots` 或 `flipbook.frameUVs` 字段，并在
pending/在途期间继续显示用户刚输入的表值；表格使用独立滚动容器，不改变运行时字段或
帧选择语义。帧区间超过 64 时仍显示时长/轴心表，但隐藏 UV 列、旋转列与对应按钮，避免写出
会被加载器拒绝的数组。

有 `frameDurations` 时，帧选择改用累计时长：

```text
totalWeight = sum(frameDurations[0..frameCount-1])
fps > 0:
    totalSeconds = frameCount / (fps * playRate)
    timeBase = perParticle ? particleAge : elapsedSeconds
    cycle = timeBase / totalSeconds
    cycle += randomStartFrame ? fract(seed) : 0
    cycle = loop ? mod(cycle, 1) : min(cycle, 1)
    phaseWeight = cycle * totalWeight
fps == 0:
    phaseWeight = clamp(lifeT * playRate, 0, 1) * totalWeight
selected = 第一个满足 phaseWeight < accumulated + weight[i] 的 i，否则回落最后一帧
```

因此 `durations` 不再覆盖 `fps`：`fps > 0` 时一轮总时长固定为 `frameCount / fps` 秒，
每帧实际秒数 = `weight / totalWeight * frameCount / fps`。`fps == 0` 时整轮按粒子寿命播放，
权重只决定各帧占比。`randomStartFrame` 在 `fps > 0` 时把 `fract(seed) * totalWeight`
加到相位上，再按 `loop` 处理；`fps == 0` 的寿命驱动不使用随机相位。选中帧仍加
`startFrame` 并夹在 `[startFrame, endFrame]`，atlas cell 与 UV 计算保持原样。没有
`frameDurations` 时继续使用等长帧的 `fps` / 寿命节奏；`randomStartFrame` 统一使用
`fract(particleIndex * 0.61803398875)`，保证相位落在 `[0,1)`。

`flipbook.blend=false` 时所有模式在采样图集前都把帧号取整成离散整数
（`fps>0` 分支先 `floor(phase)` 再按 `loop` 取模或夹住），因此 `atlasCell`
不会出现小数、不会在相邻图集单元之间跨 cell 混合。`blend=true` 时几何/pivot/rotation
仍使用当前整数帧，fragment 对 frame A/B 的 sprite 采样做线性插值；几何不会在
相邻 cell 之间变形。

有 `framePivots` 时，取当前帧在区间内的局部索引（0..frameCount-1）对应的 `(u,v)`，反向
平移 billboard quad 中心，使精灵图上的该锚点落在粒子中心：

```text
pivotOffset = (vec2(0.5) - pivot) * 2.0
worldPosition = particleCenter
    + (cameraRight * (rotatedCorner.x + pivotOffset.x)
     + cameraUp    * (rotatedCorner.y + pivotOffset.y)) * size
```

`rotatedCorner` 在下一节定义；旋转为 0 时它等于 `corner`。`maxScreenSize` 的屏幕封顶仍
作用在 `size` 上；pivot 只移动 quad 中心，不修改粒子中心、寿命或曲线。

### Billboard 朝向

`renderMode: "billboard"` 的 quad 默认使用相机基：

```text
billboardRight = cameraRight
billboardUp    = cameraUp
```

资产可把 `billboardAlignment` 设为 `"velocity"`，改用粒子池中的模拟空间速度 `v`
构造基：

```text
forward        = normalize(v)
billboardRight = normalize(cross(vec3(0, 1, 0), forward))
billboardUp    = normalize(cross(forward, billboardRight))
```

此时 quad 的法线对齐速度方向，适合拖尾/火花。`|v|^2 <= 1e-6`、`v` 与世界上方
平行，或叉积/归一化后的基向量退化时，整个 quad 回退相机基，避免零面积 quad 与 NaN。
`localSpace` 发射器先把模拟空间速度乘 `localToWorld` 的线性部分，再构造世界空间基。

资产也可把 `billboardAlignment` 设为 `"fixed"`，用作者化的发射器局部向量固定 quad 朝向：

```text
normal         = normalize(emitterLinear * billboardNormal)
upHint         = normalize(emitterLinear * billboardUp)
billboardRight = normalize(cross(upHint, normal))
billboardUp    = normalize(cross(normal, billboardRight))
```

因此 quad 的几何法线 `cross(billboardRight, billboardUp)` 精确等于 `normal`；上方向只作为
提示，最终基与 `normal` 正交。`billboardNormal` / `billboardUp` 是发射器局部数据，上传前
统一乘 `emitter.model` 的线性部分，因此旋转/缩放粒子系统会带着卡片一起转。任一向量为零、二者平行，或构造出的
基退化时，整个 quad 回退相机基。固定朝向与逐粒子旋转、pivot、UV、照明和 `maxScreenSize`
正交组合；同一发射器的所有粒子共享该固定基。

`billboardAlignment` 只影响 billboard；ribbon、mesh 与 beam 各有独立的朝向逻辑。逐粒子旋转
仍在该朝向基内绕 quad 中心应用，pivot 与 `maxScreenSize` 语义不变。

### 逐粒子 billboard 旋转

每个 billboard 在当前朝向基内绕自身中心旋转；默认 camera 基等价于相机平面。
`age` 是粒子的模拟年龄，`seed` 是稳定的
`fract(particleIndex * 0.61803398875)`：

```text
angle = radians(rotation + rotationSpeed * age
                + (rotationRandom ? fract(seed) * 360 : 0)
                + frameRotation)
rotatedCorner = vec2(corner.x * c - corner.y * s,
                     corner.x * s + corner.y * c)
worldPosition = particleCenter
    + (billboardRight * (rotatedCorner.x + pivotOffset.x)
     + billboardUp    * (rotatedCorner.y + pivotOffset.y)) * size
```

其中 `c = cos(angle)`、`s = sin(angle)`。pivot 偏移作用于旋转后的 quad；`maxScreenSize`
仍通过缩放 `size` 封顶屏幕尺寸，不改变粒子中心、旋转角或曲线。顶点 UV 继续使用未旋转的
`corner`（`localUV = corner * 0.5 + 0.5`），因此旋转只改变几何朝向，不旋转精灵内容。

有 `frameUVs` 时，先按当前整数局部帧号取 `rect = (u0,v0,u1,v1)`，再把 quad 局部 UV
映射到 sprite 子矩形，最后走 atlas cell 与半 texel 内缩：

```text
localUV  = corner * 0.5 + 0.5
spriteUV = hasUVs ? mix(rect.xy, rect.zw, localUV) : localUV
atlasUV  = (cell + vec2(spriteUV.x, 1 - spriteUV.y)) / vec2(columns, rows)
atlasUV  = clamp(atlasUV, cellMin + halfTexel, cellMax - halfTexel)
```

UV rect 是 sprite 空间（`[0,1]`）而不是 atlas 绝对空间；V 翻转只发生在映射到 atlas 之后。
`frameRotation` 取当前整数局部帧号的表值（无表为 0），叠加到发射器级
`rotation/rotationSpeed/rotationRandom` 的角度上；它只旋转几何，不旋转 UV 内容。

### Ribbon 条带

`renderMode: "ribbon"` 时每个粒子改成一条面向相机的条带；`billboard`（默认）路径完全不变。
粒子池里的 `history[16]` 每帧在积分前右移（`history[15]=history[14]` …
`history[1]=history[0]`、`history[0]=当前 position`），随后执行原有运动/碰撞积分；
死亡帧不更新历史。spawn/burst 用速度沿时间回填 16 个历史点，prewarm 同样回填，
因此首帧即可看到稳定尾迹，而不是所有段重合的 bow-tie。

顶点数由段数决定：`maxParticles * ribbonSegments * 6`，`ribbonSegments` 范围 `[1,32]`；
每个粒子内 `segment = localVertex / 6`。顶点阶段把
`points = {current, history[0], …, history[15]}` 组成 17 点折线，计算累计弧长，
再按输出段等分弧长重采样。低帧率或不等距历史不会让条带变成折线，也不再把模拟
帧率直接暴露给宽度/UV。

条带宽度与朝向：

```text
viewport 宽度方向：
t      = normalize(pointAt(t + eps) - pointAt(t - eps))
view   = normalize(cameraPosition - pointAt(t))
side   = normalize(cross(t, view))
width  = ribbonWidth * EvaluateRibbonWidth(t)
world  = pointAt(t) + side * v * width    // t∈[0,1] 当前→尾部, v∈{-1,+1}
```

`t` 过短或 `cross(t, view)` 退化时用 `cameraRight` 作为横向，避免 normalize(0)。
`ribbonWidth` 是世界空间半宽（cm），所以条带总宽是 `2 * ribbonWidth`。
`ribbonWidthKeys` 是逐弧长乘数曲线（最多 8 个 key，`width ∈ [0,8]`）；
空表恒为 1。尾端 alpha：

```text
alpha *= mix(1.0, 1.0 - t, ribbonFade)
```

`ribbonFade=0` 不衰减，`=1` 时最旧端完全透明。`vUV = (t * ribbonUVTiling, v*0.5+0.5)`：
`x` 沿弧长从当前端到尾部并按 `ribbonUVTiling` 平铺（>1 时在 shader 内 `fract`），
`y` 是横向。片元沿用 billboard 的软粒子逻辑；alpha 形状只软化 `vUV.y` 两端，
不做圆盘 radial；有纹理时采样整张 `particleTexture`，不消费 flipbook 帧选择。
EOO 打包纹理（见下文）只在 ribbon 纹理分支按 R/A 解码。

暂停时 `simulationDeltaSeconds = 0`，历史不会重复写入同一点。`ribbonWidth` 与
最大宽度 key 会额外计入发射器包围球半径，因此剔除、距离淡出与跨发射器排序
都覆盖条带横向范围。

### Beam 电弧

`renderMode: "beam"` 不消费粒子池：它按作者化的 `beam.start`/`beam.end` 生成
`count` 条并行弧，每条弧有 `segments` 段。CPU 每帧把两个端点乘 `emitter.model`
变换到世界空间并写入 UBO；因此 `localSpace` 对 beam 没有额外含义，光束也随场景对象
平移/旋转/缩放。端点的位置由矩阵直接变换；`width`、`noiseAmplitude` 和
`endpointJitter` 是局部空间长度，CPU 按模型线性部分的最大列缩放后写入 UBO。
该发射器跳过 simulate、局部排序、finalize 与全局 gather，池可以保持零存活粒子。
World 的粒子预算不计入 beam，`emitterBudgetScale=0` 或其它 emitter 耗尽预算时
电弧仍会绘制；`ParticleSystemBudget.effectiveCapacity` 与
`telemetry.budgetedCapacity` 也只累计非 beam 发射器。

绘制形状固定为 `segments * 6` 个顶点、`count * (1 + branchCount)` 个实例：

```text
arcsPerInstance  = 1 + branchCount
totalInstances   = count * arcsPerInstance
draw(segments * 6, totalInstances, 0, 0)
drawnVertices = segments * 6 * totalInstances
```

实例布局是 `[arc0 main, arc0 branch1..N, arc1 main, ...]`：
`arcIndex = gl_InstanceIndex / arcsPerInstance`、
`branchIndex = gl_InstanceIndex % arcsPerInstance`，`branchIndex == 0` 是主弧。
着色器按 `segment = localVertex / 6` 生成四边形；起止点使用稳定哈希做逐弧
`endpointJitter`，弧内噪声使用

```text
envelope = sin(pi * t)                    // 两端为零
offset   = (sideA * waveA + sideB * waveB) * amplitude * envelope
point(t) = mix(start, end, t) + offset
```

分支（`branchIndex > 0`）先用稳定哈希在主弧上取
`rootT = clamp(branchStart + 0.2 * hash, 0, 1)`，取主弧在 `rootT` 的噪声点作为 root，
再按主切线旋转到 `sideA/sideB` 平面内：

```text
lateral      = normalize(sideA * cos(angle) + sideB * sin(angle))
branchDir    = normalize(mainTangent * cos(bend) + lateral * sin(bend))
branchEnd    = root + branchDir * (branchLength * |end - start|) +
               lateral * branchSpread
pointBranch(t) = BeamPoint(root, branchEnd, ..., seed(t), t)
```

`bend` 与 `angle` 都来自 `BeamHash`，分支方向由主切线旋转得到；`seed` 由主弧编号
和分支编号决定。包络 `sin(pi * t)` 保证 `t = 0` 时分支点与主弧 root 完全重合。
分支的噪声频率与幅度额外乘以 `clamp(branchLength, 0.1, 1.0)`，使短分支在世界空间
保持接近主弧的空间频率，而不是在更短的 t 区间内被压成高频锯齿。
分支使用更暗的颜色（`0.55x`）、更细的宽度（`0.55x`），alpha 沿长度淡出；根部不做
收窄，末端仍按 `smoothstep` 收窄。

`waveA/waveB` 是频率、速度与弧相位错开的三层正弦。横向宽度用
`cross(tangent, viewDirection)` 面向相机，退化时回退 `cameraRight`；宽度在两端按
`smoothstep` 收窄。颜色按 `mix(colorStart, colorEnd, t)` 取色，逐弧乘以
`[0.75, 1]` 的闪烁，alpha 只取 `render.w`（即 `opacity` 与运行时淡出）。有纹理时
采样整张 `particleTexture`，不消费 flipbook cell。片元沿用 ribbon 的 `sceneDepth`
剔除、软粒子淡出与 overdraw 计数，但横截面是“亮核 + 快速衰减”，不做 FBM 侵蚀。

可选 `beam.colorKeys` / `beam.widthKeys` 按归一化弧长 `t` 采样；颜色表覆盖基础
`colorStart/colorEnd`，宽度表乘到 `beam.width`，主弧与分支共用同一采样函数。逐主弧
`beam.arcs` 提供颜色 tint、宽度乘数、强度乘数与独立噪声相位，分支继承所属主弧。
这些数据在 UBO 末尾的固定数组中上传，旧字段偏移不变。`beam.hitScan` 在每个模拟帧
把世界空间 start/end 当作有限射线，对作者化平面做一次确定性求交；命中时替换 UBO
end，未命中保留原 end。它不读取物理场景，因此不是任意 mesh 的 HitScan。

限制：beam 有固定的直线主段和程序化分叉，控制曲线仍是分段线性，不提供逐段控制点
或 Bezier/Hermite 切线；分支形状由 `branchStart/branchLength/branchSpread` 与哈希共同
决定，不能逐条手工摆放。它只支持 additive/alpha 两种既有混合，不进入全局排序或 8 槽
gather 纹理数组，也不支持逐弧独立材质/纹理：当前所有弧共用单 pipeline、单 set 0
descriptor，逐弧 materialInstancePath 需要拆分 draw 与 UBO/descriptor range。自动包围球以变换后的线段中点为球心，半径是
`(halfDistance +
(branchCount > 0 ? branchLength * |end - start| + branchSpread +
2 * branchLength * sqrt(3) * endpointJitter : 0) +
sqrt(2) * noiseAmplitude + width + sqrt(3) * endpointJitter) * maxColumnScale`；
`useFixedBounds` 仍覆盖该自动估算。

### Distortion 屏幕空间热扰动

`renderMode: "distortion"` 复用 billboard 的模拟、池、尺寸/颜色曲线、flipbook 与
`billboardAlignment`，但绘制目标是独立的 RGBA16F `distortion` 附件，而不是 sceneColor。
发射器仍按正常预算分配粒子池、执行 simulate 与可选局部排序；`RecordGraphics` 跳过它们，
`RecordDistortion` 只用同一份 set 0 描述符集与 `drawIndirect` 绘制。

顶点 shader `particle/particleDistortion.vert` 复制 billboard 的实例索引、寿命曲线、
尺寸封顶、旋转、pivot、flipbook 与 fixed/velocity/camera 基逻辑；fragment shader
`particle/particleDistortion.frag`：

```text
覆盖位置 = discard if particleDepth >= sceneDepth
coverage = radial(softness) * opacity * colorAlpha * textureAlpha * softFade
direction = normalize(texture.rg * 2 - 1)          // 有纹理
          | FBM 有限差分梯度                        // 无纹理
output    = vec4(direction * distortionStrength, 0, coverage)
```

该 pass 使用 additive blend（`src.rgb * src.a + dst.rgb`、`src.a + dst.a`），因此
fragment 不再把 coverage 预乘进 RGB；重叠粒子的偏移按覆盖率加权累加。`distortion` 不清
sceneColor，也不写深度。输出的 `rgb` 是屏幕高度归一化偏移；`bloomPrefilter` 与
`toneMapping` 都执行
`d = distortion.rg`、`sceneUV = clamp(inUV + vec2(d.x / aspect, d.y), 0, 1)`，其中
`aspect = width / height`。

限制：偏移参与 Bloom 预过滤和最终 tone mapping；仓库当前没有 SSR pass，无法验证 SSR
接入，将来应在 SSR 的屏幕空间采样入口复用同一映射。当前没有 per-emitter distortion
强度上限之外的形状控制，也不支持折射法线/深度差驱动的真实折射。它只影响后处理采样，
不改变 G-Buffer 或 decal。

### Mesh 实例化

`renderMode: "mesh"` 时，运行时用 `MeshAssetLoader::Prepare(mesh.path)` 读取 SM 描述并
`ImportSections` 导入静态网格。v1 拒绝 `buildPlan.IsSkeletal()`；所有 section 的
`Vertex` 与 index 合并成一份顶点缓冲/索引缓冲，index 在合并时加 section 顶点偏移。
两个缓冲都是 host-visible，随发射器一起 retire；导入或创建失败时该发射器回退 billboard
并记录一次 warning，渲染线程不会退出。

绘制使用同一条 particle pass、同一组 `ParticleFrameUniforms`、粒子 SSBO 与 sortedPairs：

```text
particleIndex = sortParticles ? sortedPairs.pairs[gl_InstanceIndex].y
                              : aliveIndices.indices[gl_InstanceIndex]
center        = positionAge.xyz          // 非 localSpace 已保存世界坐标
local         = vertex.position * mesh.scale
world         = center + rotation * basis * local
gl_Position   = viewProjection * vec4(world, 1)
```

`basis` 的三列对应网格局部 X/Y/Z：

- `world`：`mat3(localToWorld)`，保留场景对象与发射器的旋转/缩放。
- `camera`：`(cameraRight, cameraUp, -cameraForward)`，局部 +Z 朝向观察者，基保持右手系。
- `velocity`：`forward = normalize(velocity)`、`right = normalize(cross(worldUp, forward))`、
  `up = cross(forward, right)`；速度为零或与世界上方平行时回退 camera 基。

`rotation`/`rotationSpeed`/`rotationRandom` 绕当前朝向基的局部 Y 轴（`basis[1]`）应用；
`rotationRandom` 使用原始粒子索引的稳定小数相位。mesh v1 **不消费 size/sizeKeys 曲线**，
视觉尺寸只由网格几何与 `mesh.scale` 决定。颜色为 `EvaluateColorCurve(lifeT) * 网格顶点色`，
materialInstancePath 存在时再乘 MI 的 `u_baseColor`；片元沿用 billboard 的 `sceneDepth`
遮挡与软粒子淡出，不接入完整 Material 图、纹理采样、法线贴图或宏变体。

每个实例固定索引数 `meshIndexCount`，draw call 为
`drawIndexedIndirect`，GPU finalize 把实例数写为 alive。GPU 池里死亡粒子在该实例的所有顶点上输出
clip-space `z=2,w=1`，直接裁剪。统计的 `estimatedDrawnVertices` 对 mesh 使用
`meshIndexCount * maxParticles`。

### Mesh 照明

`meshLighting: true`（默认）时，mesh 片元用一个场景主方向光计算 GGX/Smith/Fresnel：

```text
alpha    = roughness * roughness
D        = alpha^2 / (pi * ((N·H)^2 * (alpha^2 - 1) + 1)^2)
G        = SmithSchlick(N·V, N·L, k=(roughness+1)^2/8)
F        = F0 + (1 - F0) * (1 - V·H)^5
direct   = (baseColor * (1 - metallic) / pi + D*G*F/(4*N·L*N·V)) * lightColor * N·L * shadow
ambient  = (baseColor + diffuseIbl + specularIbl) * meshAmbient
color    = direct + ambient + emissive * emissiveStrength
```

- `direct` 对全部方向光/点光/聚光 + GPU 粒子灯逐盏求和；`L` 指向该灯，
  `lightColor` 使用线性空间颜色（点光/聚光已含 `cd * 100^2` 与半径窗）。
  `N` 是实例旋转后的世界空间法线，`V` 是相机方向，`H` 是半程向量。
- `materialInstancePath` 提供 `baseColor/roughness/metallic/emissive` 数值子集；
  没有 MI 时 `baseColor` 来自粒子颜色曲线，`roughness` 默认 `0.5`、`metallic` 默认 `0`。
- 方向光的 `shadow` 来自 CSM `SampleParticleCsmShadow`；场景点光/聚光可各自开启
  `shadow.castShadows` 并采样局部阴影图集，GPU 粒子灯不投影。`diffuseIbl` 用预过滤
  cubemap 的最高 mip 近似 irradiance，
  `specularIbl` 用预过滤 cubemap + BRDF LUT；`meshAmbient` 默认 `0.2` 同时缩放常量
  ambient 与 IBL。
- 这是着色近似，不是完整材质系统：不接完整 Material 图、纹理/法线贴图或
  alpha-to-coverage。billboard/ribbon/beam 通过 `billboardShadows` 开关走各自的 CSM
  采样路径。
- `meshLighting: false` 保持原有 unlit 路径；beam 完全忽略这两个字段，ribbon 复用
  `billboardLighting` / `billboardAmbient` / `billboardLightingStrength`（法线取面向
  相机的视线方向，属于尽力近似），billboard 使用同一组开关。

### Billboard 照明

`billboardLighting: true` 时，逐发射器与全局 alpha billboard 路径使用同一个片元近似：

```text
N       = normalize(cross(billboardRight, billboardUp))
wrapped = clamp((N·L + 0.5) / 1.5, 0, 1)       // 每盏方向光/点光/聚光/粒子灯
direct  = Σ baseColor / pi * radiance * wrapped * lightShadow
ambient = (baseColor + diffuseIbl) * billboardAmbient
color   = mix(baseColor, direct + ambient, billboardLightingStrength)
```

- `billboardRight` / `billboardUp` 是实际 quad 基。camera alignment 时面法线朝向相机，
  velocity alignment 时面法线朝向速度；不再一律使用相机中心方向。
- `radiance` 对方向光/点光/聚光/粒子灯分别使用各自的光色、强度与半径窗；`lightShadow`
  只在 `billboardShadows: true` 时来自方向光 CSM；场景点光/聚光可开启
  `shadow.castShadows` 并通过局部阴影图集参与（粒子灯不投影）。
- `billboardLightingStrength` 是 unlit 作者颜色与场景光照之间的插值比例；0 完全保留
  authored 颜色，1 完全使用光照，避免纯乘法在暗场景掉亮度、在强光附近过曝。
- 全局 alpha 排序仍排除 `billboardShadows=true` 的发射器，因此 CSM 路径始终
  走逐发射器绘制；全局路径的 gather 记录携带同一组 lighting 参数。
- 全局 gather 记录包含 `lightParams`、`lightDirection`、`lightColor`、`corner3Kind`
  与 `color2`；`GatheredBillboard` 的 std430 stride 为 17 个 `vec4`。gather、
  sort-init、global vertex/fragment 四处结构体必须同步。
- 该照明已覆盖全部场景灯与 GPU 粒子灯并接入 IBL，但 sprite 只做 wrapped Lambert
  （没有镜面 lobe），仍不提供法线贴图或完整 Material Graph；GPU 粒子灯不投影。

### Curves 结构化编辑

Curves 模块不再显示 `colorKeys`/`sizeKeys` 的 JSON 文本框，而是为 Color 与 Size 两条曲线
各显示一条 0..1 时间轴、24 段预览和逐 key 行。Color 预览使用 24 个等宽采样色块，Size
预览使用 24 根按当前最大采样值归一的底对齐柱子。每条曲线可增删 1..8 个 key：新增 key
取当前最大空隙的中点，并写入该时刻的插值结果；时间输入夹在 [0,1] 内，且必须与相邻 key
保持 0.001 的间隔。删除到 0 个 key 时提交 `reset` 移除字段，运行时回退到
`colorStart/colorEnd` 或 `size` 两端。时间轴与预览只是编辑投影，持久化数据仍是规范化后的
`[{time,color:[r,g,b,a]}]` / `[{time,size}]` 数组；三分量颜色在编辑时按不透明补
alpha。RGB 通道按 HDR 处理：编辑层只拒绝非有限值与负数，允许 `>1` 的线性强度，
不会把 additive 粒子的高亮颜色静默压到 1；alpha 仍夹在 `[0,1]`。颜色 hex 只用于
UI 预览，显示时才夹到 `[0,1]`。

### 可见性剔除与淡出

`ParticleRenderer::RecordSimulation` 每帧从 `camera.viewProjection` 提取 6 个归一化视锥平面
（left/right/bottom/top/near/far，平面内侧为正，适配 Vulkan 0..1 深度）。发射器包围球与任一
平面满足 `distance(n,p) < -radius` 时整球在平面外，发射器剔除；包围球跨过平面时保留。

距离使用包围球最近点：

```text
distance = max(0, |worldCenter - cameraPosition| - worldRadius)
```

`maxDrawDistance > 0 && distance > maxDrawDistance` 时剔除。淡出同样使用该最近点距离：

```text
opacityScale = maxDrawDistance > 0 && fadeDistance > 0
    ? clamp((maxDrawDistance - distance) / fadeDistance, 0, 1)
    : 1
```

可见发射器的 `render.w` 写为 `opacity * opacityScale`。剔除发生在写 uniform、清池、模拟
dispatch、排序 dispatch 之前，因此发射器被剔除时 `elapsedSeconds` 与粒子池都冻结；重新进入
视野后从冻结状态继续，不会补算不可见期间的时间。

`minScreenSize > 0` 时取相机前向深度 `depth = dot(worldCenter - cameraPosition,
normalize(cameraForward))`。`depth <= 0` 不做屏幕尺寸测试，交给视锥；否则：

```text
pixelDiameter = worldRadius * projectionScaleY * viewportHeightPixels / depth
```

其中 `projectionScaleY = abs(camera.projection(1,1))`。小于 `minScreenSize` 时剔除。距离、
尺寸为负按 0 处理；相机前向为零向量时跳过该测试。

`maxScreenSize > 0` 不参与发射器剔除，而是在 billboard vertex shader 中逐粒子计算屏幕
半边长：

```text
pixelHalfSize = size * projectionScaleY * viewportHeightPixels * 0.5 / depth
```

超过阈值时按 `maxScreenSize / pixelHalfSize` 同比缩小用于 billboard 偏移的 `size`。四个角
仍围绕原粒子中心展开，因此不会裁掉中心，也不会改动 CPU 侧寿命/尺寸曲线。透视与正交投影
共用同一封顶逻辑；正交分支没有深度除法。

## 调试回读与统计 dump

控制台 `particledump [file.json] [maxPerEmitter]` 会把当前 GPU 粒子池回读到 CPU，
输出到 `<resourcePath>/Generated/ParticleDumps/<file>`。文件名必须相对、非空、不含 `..`，
扩展名统一为 `.json`；`maxPerEmitter` 默认 128，范围 `0..4096`，0 表示只写统计。
详见 [调试命令](debug-runtime-commands.md)。

回读前停止可选的渲染线程并等待设备空闲，然后对每个 emitter 创建 host-visible
`eTransferDst` staging buffer，用一次性 command buffer 从粒子池 `copyBuffer`（源池因此
声明了 `eTransferSrc`），提交并等待 fence 后映射读取。JSON 根字段：

```text
totalEmitters, visibleEmitters, totalParticles, aliveParticles,
estimatedDrawnVertices, maxPerEmitter,
gpuTimingSupported, gpuMilliseconds, gpuAverageMilliseconds,
gpuMaxMilliseconds, gpuSampleCount, emitters[]
```

每个 emitter 输出 `key`、`maxParticles`、`aliveCount`、`totalParticles`、`culled`、
`sortParticles`、`localSpace`、`renderMode`、`bounds.center`、`bounds.radius`、
`elapsedSeconds`、`drawInstances`、`drawnVertices`，以及最多 N 个活粒子（池顺序）的 `index`、`position`、
`age`、`velocity`、`lifetime`。billboard 的 `drawnVertices = maxParticles * 6`；
ribbon 为 `maxParticles * ribbonSegments * 6`；mesh 为
`meshIndexCount * maxParticles`；beam 的 `maxParticles`/`totalParticles` 为 0，
`drawInstances = beam.count * (1 + beam.branchCount)`，
`drawnVertices = segments * 6 * drawInstances`；
`estimatedDrawnVertices` 只累计未 cull 的 emitter。被 cull 的 emitter 仍会被回读，
但其模拟状态处于冻结状态。

位置与速度是模拟空间原始值：`localSpace: true` 时是发射器局部坐标，否则是世界坐标。
粒子容量与明细来自调试用 CPU 回读，回读本身会等待设备空闲，不进入正常帧路径；
根部的 GPU 字段与 `particlestats` 同源，来自 particle graphics pass 的 timestamp
计时（见下节），但 dump 不额外触发 GPU 查询。

## 帧级统计预算

控制台 `particlestats` 打印 `ParticleRenderer` 上一帧的 CPU 侧预算快照，
不等待设备空闲、不做 GPU 回读，也不改变渲染行为。正常帧仍由
`RecordSimulation` 与 particle graphics pass 推进；统计只是同一帧已经完成的
发射器遍历/录制结果的计数。

字段语义：

- `totalEmitters`：当前 GPU 发射器包总数；`totalParticles` 是这些发射器
  `maxParticles` 容量之和，不是存活粒子数。
- `visibleEmitters` / `culledEmitters`：本帧通过剔除的发射器与未通过（或被
  `frameUniforms` 早退跳过）的发射器；两者之和等于 `totalEmitters`。
- `sortedEmitters`：可见且 `sortParticles=true` 的发射器数量。
- `sortDispatchCount`：这些发射器实际录制的 bitonic pass 总数，
  `Σ levels*(levels+1)/2`，`levels=log2(sortCapacity)`；`sortCapacityTotal`
  是它们排序容量的总和。全局 alpha 池只计一次全局网络容量/pass 数，不再按成员 emitter
  重复累加。被剔除的发射器不推进、不排序，因此不计入。
- `estimatedDrawCalls` / `estimatedDrawnVertices`：`RecordGraphics` 中真正执行
  `drawIndirect`/`drawIndexedIndirect` 的 emitter 数量与顶点数；billboard 为 `maxParticles*6`，
  ribbon 为 `maxParticles*ribbonSegments*6`，mesh 为
  `meshIndexCount*maxParticles`。culled、descriptor 不足、mesh 回退或 pipeline 为空的
  早退按实际路径统计。全局池的顶点数是容量上界（`capacity*6`），不是 GPU 实际 alive
  数；逐 emitter 路径同样按容量上界估算。
- `frameIndex`：`RecordSimulation` 调用次数，从 1 开始；空场景也递增。
  `elapsedSeconds` 跨帧累计非负模拟 `dt`（与模拟使用同一个 `[0, 0.1]` 夹取）。
- `gpuTimingSupported` / `gpuMilliseconds` / `gpuAverageMilliseconds` /
  `gpuMaxMilliseconds` / `gpuSampleCount`：particle graphics pass 的 GPU
  timestamp 统计，见下节。
- `emitterGpu`：逐 emitter 的 GPU 图形区间快照；全局排序池使用
  `key="__global__"`。每项含 `supported/milliseconds/averageMilliseconds/
  maxMilliseconds/sampleCount`，按 key 排序以保证 JSON 稳定。

`particlestats` 本身不等待设备空闲，因此仍不包含 alive 回读、GPU overdraw
或显存带宽。alive 由 C# 订阅驱动的 `particletelemetry` 回填；`particledump`
输出每个非全局 emitter 的 `overdrawFragments`（通过 discard 的片元原子计数）和
`estimatedParticleBufferReadBytes`（aliveCount × 288 字节的读取规模估算，
不是硬件显存带宽计数器）。全局排序池的对应值在 dump 根字段
`globalAliveParticles` / `globalOverdrawFragments` /
`globalEstimatedParticleBufferReadBytes`；其绘制耗时由 `__global__` GPU 计时覆盖。
overdraw 计数要求逻辑设备启用 `fragmentStoresAndAtomics`；缺失时设备初始化直接报错，
不会静默退化成无效 shader。
culling 期间的发射器保持冻结，不产生排序
dispatch 或绘制。

### GPU pass 计时

`ParticleGpuTimer` 为每个 swapchain image 独占一对 timestamp query（begin/end），
在 `ParticleGpuTimer::BeginFrame` 回收该 image 上一轮已完成的结果并 reset，
`RecordParticlePass` 用 `eTopOfPipe` / `eBottomOfPipe` 只包住
`particleRenderer.RecordGraphics`。因此：

- 计时范围是 particle graphics pass 的绘制提交，不含 `RecordSimulation` 的
  compute 模拟、bitonic 排序 dispatch，也不含场景几何与环境 IBL。
- `gpuMilliseconds` 是最近一次回收完成的样本；`gpuAverageMilliseconds` 是本次
  运行期的算术均值，`gpuMaxMilliseconds` 是历史峰值，`gpuSampleCount` 是样本数。
- 样本在该 image 下一次 `BeginFrame`（已等待其 fence）时回收，所以刚启动的几帧
  可能仍是 0，`particlestats` 不是即时查询。
- 后端未暴露 graphics timestamp（`GetGraphicsTimestampValidBits()==0` 或
  `GetTimestampPeriodNanoseconds()<=0`）时返回 `gpuTimingSupported=false`，
  数值保持 0，不写 query，不影响渲染。image index 越界、query pool / reset /
  writeTimestamp / 回读抛异常时也只禁用计时并输出一条 warning，不向帧循环抛异常。
- 逐 emitter query 与全局 pass query 独立；全局 alpha billboard 的粒子不再重复计入
  逐 emitter 计时，而由 `__global__` 项表示。Vulkan 没有可直接读取的硬件显存带宽计数器；
  dump 中的 `estimatedParticleBufferReadBytes` 明确是规模估算，不是实测值。

## Light 粒子灯（GPU per-particle light）

`renderMode: "light"` 拥有正常 GPU 粒子池（每个发射器最多 64 个粒子），复用
`particleSimulate.comp` 的固定步长、burst 绝对周期、shape/gravity/drag、局部/世界空间、
`colorTint`、`spawnRateScale`、事件/碰撞与确定性随机。它不进入 billboard/ribbon/mesh/
beam/indirect draw、全局 gather 或排序。World 的 authored/budgeted capacity 仍把 light
发射器排除在普通粒子总预算之外；模拟池按 `min(maxParticles, 64)` 独立分配。

- 模拟后由独立 `particle/particleLightEmit.comp` 读取存活粒子，写入每个 swapchain image
  一份固定 64 槽的 `ParticleLightSSBO`。`RenderSystem` 不再调用 CPU
  `ParticleLightRuntime`，`RenderScene::lights` 中也没有对应动态灯，因此不存在
  CPU/GPU 双重发光。
- `ParticleLightSSBO` 布局：`uvec4 header`（x = count high-water mark、y = capacity 64）
  后接 32 字节条目 `{positionRadius, colorIntensity}`。条目位置是粒子世界坐标（cm），
  `colorIntensity.w` 是 `cd * 100^2`，与场景 `LightGPU` 的点光约定一致。
- 槽位按 light 发射器 key 的稳定顺序 first-fit 分配；总请求超过 64 时后面的槽位容量为 0，
  截断结果不依赖 `unordered_map` 迭代顺序。每个粒子写 `base + particleIndex`，用
  `atomicMax(header.count, slot + 1)` 发布 high-water；空槽保持零强度，由着色器跳过。
- 每个存活粒子的颜色/强度：
  `color = mix(colorStart.rgb, colorEnd.rgb, lifeT) * colorTint.rgb *
  light.brightnessKeys.color(lifeT)`，
  `alpha = clamp(mix(colorStart.a, colorEnd.a, lifeT) * colorTint.a, 0, 1)`，
  `brightness = light.brightnessKeys.brightness(lifeT)`（空表为 1），
  `intensity = light.intensity * (1 - lifeT) * alpha * brightness`，
  `radius = light.radius`。颜色曲线空表时为白色。发射时乘 `100^2`，不经过 CPU
  量化或回读。
- `light.brightnessKeys` 是逐寿命曲线，最多 8 个 key；每个 key 为
  `{time, brightness, color?}`，`time` 必须在 `[0,1]` 且严格递增（加载器不排序、
  不截断作者数据），`brightness` 与 `color` 各通道必须在 `[0,64]`。`brightness`
  也可写成 `value`。求值规则与 CPU `EvaluateParticleLightBrightness` /
  `EvaluateParticleLightColor` 一致：首尾外 clamp，区间内分段线性插值，单 key
  为常量。空表等价于亮度 1、颜色白。
- `light.iesProfile` 是 64 个线性样本、覆盖 `0..pi` 极角的可选角度剖面，空表为
  各向同性。`light.iesTexture` 是同一字段的兼容别名；两者可以单独出现，但同一
  light 对象同时声明两者会在加载期拒绝，避免作者误以为后者覆盖前者。
- 每帧 `BeginFrame` 等待该 image fence 后，host 清零 `header + 64 entries`，发布
  host-write → compute/fragment barrier；粒子 simulate 后插入 compute write → compute
  read/write barrier，再执行 light emit；`RecordSimulation` 末尾的 compute →
  vertex/fragment barrier 让 deferred 和 forward 读取同一份结果。
- deferred 与 forward 共用 set 0 binding 4 的 `ParticleLightSSBO`，并把它合并进现有
  点光循环。该 SSBO 不扩容，也不做逐帧 descriptor 热刷新；容量固定 64。
- 隐藏/禁用/剔除沿用普通 emitter 冻结路径：不推进模拟，本帧输出为零。Pause 时
  `simulationDeltaSeconds == 0`，粒子保持当前年龄；runtime restart 会重置池并在下一帧
  prewarm。World 换代通过 `ParticleRenderer::Synchronize` 重建/回收池与描述符包。
- 编辑器粒子缩略图在创建 `RendererFrameResources` 后绑定同一 light buffer；离线
  `OfflineParticleSimulation` 仍跳过 light 发射器，不分配粒子数组、不推进时间。
- 已有逐发射器 IES 角度剖面与逐寿命亮度/颜色曲线；仍没有逐粒子光纹理（cookie）、
  逐粒子阴影贴图或逐粒子独立 IES/曲线；light 发射器本身不产生可见 draw call。

编辑器能力表公开 `renderMode` 的 `light` 选项以及 `light.intensity`、`light.radius`、
`light.iesProfile`、`light.brightnessKeys`（Array，可 reset）、`light.castShadows`、
`light.shadowBias`；`light.iesTexture` 只作为兼容别名参与加载，不重复暴露成第二个
字段。字段仍写入 v2 renderer 模块的 `fields`，Emitter 页显示同一组 light 字段。
没有独立的 Light 模块页，也没有 Material Graph。

## Decal 粒子贴花（GPU 模拟 + CPU fallback）

`renderMode: "decal"` 现在与 billboard/ribbon 一样分配普通 GPU 粒子池并复用
`particleSimulate.comp` 的固定步长、burst、shape/gravity/drag、局部/世界空间与确定性随机。
它不进入粒子 indirect draw、全局 gather 或透明排序；模拟结束后由独立
`particleDecalGather.comp` 读取存活索引，按最多 64 条紧凑记录写入
`ParticleDecalGpuInstance`（position/lifeT、size、rotation、color、opacity、slot）。
World 的 authored/budgeted capacity 把 decal 计入普通 GPU 池预算；`beam` 与 `light`
仍不参与该预算。没有 GPU 池（或池尚未建立）时，`ParticleCpuEmitterRuntime` 继续作为
fallback 按旧的 CPU 语义模拟。

- `ParticleRenderer::RecordSimulation` 在 simulate 后 dispatch gather，把输出复制到
  逐 swapchain image 的 host-visible 读回缓冲；下一次同 image 提交前在上一帧 fence 之后
  解析，最多回读 64 条。缓存按 emitter key 发布，`ParticleDecalRuntime` 在下一帧消费，
  因此 GPU decal 有 1 帧确定性延迟，但不读取在飞帧数据。
- 运行时仍由 `ParticleDecalRuntime` 拥有场景顺序的 decal emitter 列表，身份是
  场景对象名 + `assetPath` + 发射器名。每帧先移除上一帧追加到 `RenderScene::decals`
  的动态条目，再按当前 GPU 记录（无 GPU 池时用 CPU particles）重建，不累加旧快照。
- 每个存活粒子输出一个 `DecalSnapshot`。`decalId` 由发射器 key 与稳定粒子槽位派生，
  不包含 burst 代际；槽位取发射器内最小空闲索引。因此同一槽位反复 burst/重生成时
  `DecalRenderer` 可以继续复用同一纹理集。
- 变换约定与场景 decal / `WorldBuilder` 一致。decalToWorld 为
  `T(粒子世界位置) * R(发射器模型归一化方向) * R(decal.rotation) *
  scale(decal.size * 0.5)`；单位 cube 的 X 是投影深度、Y/Z 是投影平面，uv 使用局部 ZY。
  `localSpace=true` 的粒子先在输出前乘发射器模型；父级缩放不进入 decal 体积，
  decal 的尺寸完全由 `decal.size` 控制。
- `color = decal.color * mix(colorStart.rgb, colorEnd.rgb, lifeT) * colorTint.rgb`；
  `opacity = clamp(decal.opacity * mix(colorStart.a, colorEnd.a, lifeT) *
  colorTint.a, 0, 1)`。roughness/metallic/AO/normalStrength、response、贴图与 sortOrder
  直接来自 `decal`。程序化噪声相位由稳定 decalId 派生。
- `DecalRenderer` 固定 64 个实例槽。`ParticleDecalRuntime` 先删除上一帧动态条目，再统计
  当前剩余可见静态 `RenderScene::decals` 数量；动态容量是 `64 - 静态可见数量`。超出时按
  particleSystems 顺序与 emitter 顺序确定性截断。GPU gather 同样把记录数限制在 64；
  静态 decal 优先占槽并原样保留，不会被运行时改写或排序。
- `RenderSystem` 在 `DecalRenderer::Synchronize` 之前完成
  `ParticleDecalRuntime::Synchronize/Update`。实例 SSBO、逐 swapchain image 实例缓冲和
  每槽 material descriptor 都是固定容量，没有逐帧 SSBO/descriptor 扩容路径。
- World/实例隐藏、`enabledEmitterMask` 与 Pause 语义不变：隐藏/禁用时 GPU 池冻结且
  动态 decal 被移除；Pause 时模拟 dt 为 0，decal 保持原位。离线渲染和编辑器缩略图
  仍不运行该运行时；`OfflineParticleSimulation` 跳过 decal 发射器，不分配粒子数组、
  不推进 `elapsedSeconds`，只保留发射器槽位供索引/掩码稳定。

粒子级运行时命令通过 GT 投递到 `ParticleRenderer`：

- `particlekill <system> <emitter> <particleIndex>`：在下一次 simulate 中把稳定槽位
  标记为死亡；越界索引、未知 system/emitter、beam 无池或隐藏/禁用 emitter 会被拒绝。
- `particlespawnat <system> <emitter> <x> <y> <z> [lifetime]`：在 CPU 侧轮转选择稳定
  槽位，下一次 simulate 写入显式位置与寿命；`localSpace` emitter 会把世界坐标转换回
  模拟空间，lifetime 省略时使用 emitter 的 `lifetimeMax`。
- 请求缓冲固定为 header + 16 条 32 字节记录，绑定在 simulate set 0 binding 9；
  超量请求或非有限参数会被拒绝。C# 暂无专用包装，console 是当前主入口。

编辑器能力表公开 `renderMode` 的 `decal` 选项，以及 `decal.size`、`decal.rotation`、
`decal.color`、材质/响应/贴图/sortOrder 字段；它们归入 v2 renderer 模块并显示在 Emitter 页。
decal 仍走代码维护的 DBuffer shader，没有 Material Graph。

## 当前边界

已落地：多发射器容器与结构编辑（复制新增/删除/移动，作为一次 `modify` 支持整体
undo/redo）、draft-live 预览、曲线/Flipbook 结构化编辑、逐帧时长/轴心/UV rect/旋转、平面/盒体
碰撞、`localSpace`、`fixedBounds`、`burstInterval` 周期重触发、多级 LOD、发射器级跨发射器
透明排序、逐粒子 billboard 旋转、ribbon 条带（16 点历史 + 弧长重采样 + 宽度/UV 曲线）、
逐粒子 SubUV 播放与线性混合、5-tap 软粒子、EOO 通道解码、静态 mesh 实例化、纹理热切换、
shader reload 提交后粒子管线重建、v2 模块栈与运行时用户参数、GPU 存活计数/indirect
instanceCount、全局 alpha billboard 排序、平面/盒体/胶囊/SDF/三角网格碰撞、全局标量
Data Channels、发射器级运行时 spawn/destroy/restart/enable、C# 事件与遥测、固定步长/
确定性随机、mesh 材质数值子集与 CSM 接收阴影、离线 CPU 粒子渲染、动态缩略图与逐发射器
GPU 计时、billboard/ribbon 的场景光照（全部灯 + IBL）、逐发射器/全局路径的光照记录，以及
`renderMode: "light"` 的 GPU per-particle 点光（独立 compute，每场景最多 64 盏）、
`renderMode: "decal"` 的 GPU 模拟/回读粒子贴花（CPU fallback；与静态 decal 共享每场景最多
64 个可见实例）。

仍未完成：

- 曲线 key 上限 8；`curveInterpolation` 取 `linear`（默认，旧资产按分段线性）、
  `hermite` 或 `bezier`。覆盖 `colorKeys`、`sizeKeys`、`ribbonWidthKeys`、
  `ribbonColorKeys`、`beam.colorKeys` 与 `beam.widthKeys`：每个 key 可带
  可选 `tangentIn`/`tangentOut`，颜色曲线是 vec4、标量曲线是 float，缺省为 0。
  `hermite` 把切线解释为归一化时间上的 `dValue/dTime`；`bezier` 把切线解释为
  控制柄相对端点的偏移（`c1 = p0 + tangentOut`、`c2 = p1 - tangentIn`），
  CPU 上传边界按 `m0 = 3*(c1-p0)`、`m1 = 3*(p1-c2)` 转成 Hermite 斜率，
  GPU 只实现 Hermite 求值，因此 Bezier 不会产生第二套 shader 数学。
  `lifetimeScaleKeys` 始终线性；flipbook 时间线复用同一 `curveInterpolation`。
  剔除半径对 Hermite 超调按每段采样最大值保守外扩，不只读 key 端点值。
- flipbook 已有逐帧时长表、逐帧静态轴心、逐帧 UV rect 与逐帧旋转；新增
  `flipbook.framePivotKeys` / `frameRotationKeys` 播放相位时间线（最多 8 key，
  可带 tangent，复用 `curveInterpolation` 的 Hermite 求值）。时间线按
  `flipbookPerParticle` 选择粒子年龄或 emitter 模拟时间，非空时与逐帧表叠加，
  pivot key 以 0.5 为中性、叠加的是相对锚点偏移，rotation key 直接加角度；
  空表保持旧行为；UV rect 仍没有时间线。图集最大 16×16（256 帧），帧区间仍由
  `startFrame`/`endFrame` 指定。
  UV rect 与旋转表受 64 帧 GPU 表限制，超过 64 帧的区间不能作者化这两张表。
- 软粒子新增 `particleSoftDepth` 半分辨率 compute pass：读取 `sceneDepth`，转换为
  线性视深度并做 3x3 深度感知模糊，输出 `R32_SFLOAT` texture2D；particle pass 的
  set 3 binding 3 采样它并以深度感知双边上采样还原。资源不可用/1x1 时保留中心 +
  四向十字 5 tap fallback；没有时域去噪。alpha billboard/ribbon 在
  `sortParticles && !additive` 且总容量（ribbon 按 quad 数）≤ 8192、
  唯一纹理 ≤ 8 时走
  全局 gather + 全局 bitonic + 单次 `drawIndirect`（`particleGlobalGather.comp` /
  `particleGlobalSortInit.comp` / `particleGlobal.vert/frag`）；mesh 与超限场景回退到
  逐发射器排序。全局路径使用固定索引分支选择 8 槽纹理，无纹理时保留程序化 FBM 形状；
  gather 记录携带逐发射器 `billboardLighting` / `billboardAmbient` /
  `billboardLightingStrength` / 方向光参数，因此 lit 与 unlit alpha billboard 可以同池
  排序。ribbon 记录携带四角/首尾颜色/kind，global vertex 按 kind 展开；
  ribbon shadow 始终逐 emitter。开启 `billboardShadows` 的 billboard 仍留在逐发射器
  路径以保留 CSM。全局池作为
  一个 draw item 参与同一份排序表（最小 `sortOrder` + 池的背景边缘深度），与其它
  未参与发射器之间仍是发射器级先后近似。
  事件 v1 已支持 spawn/death/collision 三类有界 GPU 记录（4096 条/发射器）与
  `ParticleRenderer::DrainParticleEvents` / `RenderSystem::DrainParticleEvents` 回读；
  C# 通过 `ParticleSystem.Subscribe` 的 owner 级兴趣触发最多 10 Hz 的回读，
  EngineLoop 把事件写入 World 队列并在下一帧脚本 tick 交给 C#。该回读是无读游标的
  最新帧快照，轮询之间可能截断/覆盖事件；没有 Niagara Data Channels 的读写拓扑、
  粒子级 spawn/destroy 或精确逐事件时间戳（桥使用批次模拟时间）。全局标量数据通道
  已单独实现，见「Particle Data Channels」。
  调试回读、CPU 帧级预算统计与 particle graphics pass 的 GPU timestamp 计时已支持
  （`particlestats`/`particledump` 的 `gpuMs` 与逐 emitter 项）；非全局 emitter
  的 GPU overdraw 通过 `particledump.overdrawFragments` 提供，带宽仅有读取规模估算。
  World 级和发射器级运行时增删都已支持，粒子级 spawn/destroy 仍没有。LOD 最多 4 档：
  每档同时缩放连续发射预算（`spawnRateScale`）与渲染尺寸（`renderScale`），
  `fadeRange>0` 时在档位边界前用 smoothstep 插值，`fadeRange=0` 保持旧硬切；
  LOD 不改变池容量，alpha 仍由既有 `maxDrawDistance`/`fadeDistance` 距离 fade 独占，
  避免重复相乘。
- 碰撞支持无限平面、轴对齐盒体、胶囊、有限体素 SDF 和三角网格：
  `collision.type=sdf` 用 `collision.sdf.path` 直接读 `particleSdf`；
  `collision.type=mesh` 用 `collision.mesh.path` 读 `particleCollisionMesh`
  并在加载期烘成有符号 SDF（最近三角形距离 + 固定非轴对齐射线奇偶判定）。
  两者运行时都走同一 SDF GPU/CPU 路径，负值表示实体内部，三线性采样 + 中心差分梯度。
  mesh 上限为 16384 顶点、4096 三角形、分辨率每轴 `[2,48]`，体素×三角形最多 64M；
  输入必须是闭合、绕序一致的流形网格，开放/非流形/零面积三角形会在加载期拒绝。
  `padding` 同时定义 SDF bounds 的扩展量，必须不小于 `collision.radius`，否则粒子球
  接触面但中心仍在 bounds 外时会漏碰撞。
  `localSpace=true` 时碰撞体按发射器局部空间解释，允许与局部粒子坐标组合。
  `collision.ccd` 提供最多 8 步子步；碰撞事件走同一 GPU 事件路径。没有运行时逐三角形
  直接求交、碰撞回调或 `prewarm` 子步。
- 排序仍是 bitonic；全局路径上限 8192，逐发射器路径上限 4096（4096 需要 78 个 pass）。
  每个 pass 都会写一次 `sortPass` UBO 并插入 compute→compute / transfer→compute barrier；
  未做多 pass 合批或 GPU 可变 dispatch。
- 发射器结构编辑支持复制新增、删除、上下移动与整体 undo/redo，但没有空白新建或重命名
  对话框；复制会连同 `position` 一起复制，偏移仍需手动修改。
- 模拟 compute 每帧原子写 `alive`、紧凑 `aliveIndices` 与事件计数，`particleFinalizeCount.comp`
  把存活数写回 `drawIndirect` 的 `instanceCount`；图形阶段不再使用 CPU 容量/存活猜测。
  未使用 `VK_KHR_draw_indirect_count`；CPU 仍提交容量上界的单条间接命令。
- light 发射器已有 GPU 模拟池与独立 `particleLightEmit` compute，颜色/alpha 消费
  `colorKeys` 曲线，并可叠加逐发射器 IES 角度剖面与逐寿命亮度/颜色曲线；输出仍是
  固定 64 槽的点光 SSBO。没有光纹理 cookie、逐粒子阴影贴图或逐粒子独立 IES/曲线；
  越过 64 槽的请求按稳定 emitter key 顺序截断。light 发射器不产生可见 draw call，
  也不参与排序/indirect。
- 粒子场景光照：billboard/ribbon 在 `billboardLighting` 开关下接收全部方向光/点光/聚光
  + GPU 粒子灯 + IBL，mesh 在 `meshLighting` 下走完整 DefaultLit。仍有的边界：sprite 只做
  wrapped Lambert、无镜面 lobe；场景点光/聚光可开启局部阴影（八面体点光 +
  透视聚光图集），GPU 粒子灯不投影；CSM 只作用于方向光；
  IBL 漫反射用预过滤 cubemap 的最高 mip 近似 irradiance，因为粒子 UBO 不携带 GPU 写的
  SH 系数；没有 UE 的 Translucency Volume 模式。
- decal 发射器已有 GPU 模拟与逐 emitter gather/回读，但没有 particle-generated decal
  pass：它把紧凑 GPU 记录（无 GPU 池时用 CPU particles）映射为 `DecalSnapshot`，
  再走现有静态 decal 的 64 槽 DBuffer 路径。因此它仍不产生 HitScan/碰撞事件，
  不会按命中面自动对齐，也没有逐粒子贴花图集或
  UE Niagara Decal Renderer 的逐粒子材质图；硬件 stencil 只提供 bit0 union mask，
  不区分单个 decal ID。DBuffer 现在由 deferred 与 forward opaque
  共同采样；forward transparent 不接收，normal 合成使用 whiteout/大圆混合，而不是逐层 RNM。
  动态与静态 decal 共享 64 个可见实例；静态 decal 先占槽，超出时粒子路径按 emitter
  顺序截断。
- ribbon 固定 16 点历史 + 顶点阶段弧长重采样，支持逐弧长宽度/颜色曲线、曲线切线、
  UV 平铺、`ribbonPivot` 横向偏移与 `ribbonTwist` 绕切线扭转；可见与 shadow 共用
  `BuildParticleRibbonVertex`。历史在每个实际推进的 fixed step 内右移一次，CCD
  子步不额外采样（等价 `ribbonHistoryInterval=1`）；spawn/burst 仍用速度回填历史，
  首帧不会退化为零长度，死亡分支不写历史。逐粒子跨发射器全局排序仍未覆盖 ribbon。
- mesh 支持静态 SM；有 `sectionPlans` 时按 section 建独立顶点/索引/材质与
  逐 section `drawIndexedIndirect`（共享的 `instanceCount` 由 finalize compute
  按命令步长写入），没有 sectionPlans 的旧资产回退为全部 section 合并；粒子
  颜色曲线与顶点色相乘；可选
  `materialInstancePath`（MI_*.json）只读取 `u_baseColor/u_emissive/u_emissiveStrength/
  u_roughness/u_metallic` 数值子集，缺失或解析失败回退内置默认值并只告警一次。
  片元使用完整 DefaultLit（全部方向光/点光/聚光 + GPU 粒子灯，GGX + Fresnel + Smith
  + IBL），方向光采样 particle pass 的 CSM `shadowMap`（set 3 binding 1）。静态与骨骼
  mesh 都支持 `localSpace`：粒子中心整体经 `localToWorld` 变换，法线/切线用线性部分；
  `mesh.useSizeCurve` 开启时按 `EvaluateSizeCurve(lifeT)/EvaluateSizeCurve(0)` 乘
  `mesh.sizeScale` 到局部缩放，`lifeT=0` 保持原尺寸；可见、shadow 与剔除共用该乘数。
  仍没有完整 Material 图/纹理/宏与 mesh LOD。网格朝向旋转
  只绕局部 Y 轴，mesh scale 不参与 `maxScreenSize` 的 billboard 钳制。导入失败静默
  回退 billboard 而不是显示错误几何。
- `burstInterval > 0` 的周期重触发没有相位偏移、次数上限或单次 burst 计数；周期从
  发射器累计模拟时间 0 起算，`particlerestart` 会把它重新归零。
- 粒子 compute/graphics pipeline 不在 shader reload 的候选事务内：reload 提交后
  `ParticleRenderer::InvalidatePipelines` 等待设备空闲并丢弃旧管线，下一次 Render 用当前
  shader 源码重建，粒子年龄/排序结果/纹理缓存会重置。若新粒子 shader 编译失败，失败发生在
  渲染线程重建阶段，不能回滚到旧管线。
- Content Browser 粒子缩略图使用 `SC_particle_preview.json` 与当前 PS 资产构建独立
  预览 World，在缩略图会话内按 30 个有界固定步（默认 `1/60` 秒）推进 simulation 后
  绘制 particle pass；成图进入 GPU image token，不写磁盘缓存。离线渲染路径在导出结果中
  发布 CPU 粒子 overlay，支持 billboard/ribbon 的确定性固定步合成；mesh/SDF 纹理细节与
  实时 Vulkan 路径不完全等价；离线仿真消费作者资产并忽略 World 级 Data Channels 与
  运行时 `userParameters`（见 rationale/particle-offline-rendering.md）。

编辑器字段修改走标准 `particle.modify` 草稿命令，保存走 `particle.save`；
`particle.get_document/get_capabilities/validate/revert/reload/close/undo/redo` 与
Scene/Mesh/Texture/Blueprint 共用同一命令总线。`particle.close` 只关闭已打开的内存文档，
脏文档以 `document_dirty` 拒绝；面板的“丢弃并关闭”先回退再关闭。值校验复用
`ParticleAssetLoader::ParseJson`。粒子 undo/redo 使用与 Blueprint 相同的有界历史：
32 步/16 MiB、保存保留、revert/reload 清空、新编辑清空 redo；历史只改内存草稿。

## M3/M4/M5 核心字段

- `materialInstancePath`（顶层或 `mesh.materialInstancePath`）：mesh 粒子的 MI_*.json
  数值子集；路径必须是资源根相对的 `.json`。
- `billboardShadows`：billboard/ribbon/beam 的显式简化阴影开关；默认 unlit，开启后按
  实际 billboard 面法线采样 CSM 并调暗 50%；与 `billboardLighting` 同时开启时改为把
  CSM shadow 乘进方向光直接项。
- `billboardLighting` / `billboardAmbient` / `billboardLightingStrength`：billboard 与
  ribbon 的运行时场景光照开关 / 环境缩放 / 光照替换比例，默认关闭 / `0.2` / `1.0`；
  不改变 mesh 或 beam 的着色。
- `events.{spawn,death,collision}`：三类 GPU 事件记录开关。事件写入
  `source/particles/particleEvents.h` 的 64-byte 记录；计数超过 4096 表示截断。
- `randomSeed`（24 位参与 GPU hash）、`deterministicRandom`、`fixedTimeStep`
  （0 关闭，范围 `[0,0.25]` 秒）：固定步长按帧累积，最多 8 步/帧；确定性随机用
  `fixedStepIndex` 而不是 wall clock。
- `collision.type="sdf"` + `collision.sdf.path`：有限体素 SDF（`type=particleSdf`，
  分辨率每轴 `[2,64]`，`boundsMin/Max`，`distances` 为 x-fastest 的有符号距离数组）。
- `collision.type="mesh"` + `collision.mesh.path`：`type=particleCollisionMesh`，
  `vertices` + `indices` 三角网格；`resolution` 缺省 `[24,24,24]`（每轴 `[2,48]`），
  `padding` 缺省 `5`（cm，且不得小于 `collision.radius`）。顶点直接处于模拟空间
  （世界空间，或 `localSpace=true` 时的发射器局部空间），没有独立碰撞网格 TRS。
  加载期烘成与 SDF 相同的有符号体素数据；网格必须闭合、绕序一致，每条无向边恰好
  由两个三角形共享。自交网格不做修复，其奇偶内外判定由作者负责。路径按
  resource-root-relative 解析并拒绝 traversal/绝对路径/越界 symlink。
- v2 `userParameters` 中以 `$Name` 形式出现的字符串会在模块展平后被替换为同名数值；
  未知参数在加载期报错。
- World 级 `particleDataChannels` 是全局命名标量表；运行时解析先以通道值为基底，
  再用逐实例 `userParameters` 覆盖同名项。Console `particledata` 与 C#
  `Particles.Data` 写同一个 active World 表，快照冻结后交给渲染器重解析。
- 运行时预算：`Synchronize` 用 compact emitter 的稳定 `name` 匹配
  `ParticleSystemSnapshot::telemetry.emitters[]`，池容量取
  `min(authored maxParticles, budgetedMaxParticles)`；0 容量不进入 `desired`，
  已有池走正常 retire。预算容量变化会触发池重建，发射器 key 仍由
  scene 名 + assetPath + emitter name 组成。
- 单发射器重启：`telemetry.emitters[].restartEpoch` 变化只重置该池的
  `elapsedSeconds/fixedStepAccumulator/fixedStepIndex/prewarm/needsClear/culled`，
  不重建 GPU buffer/descriptor/texture。
- 运行时用户参数：仅在 v2 且作者 `userParameters` 非空时冻结
  `authoringJson`；渲染器按 system + 参数签名 + authoring 指针缓存重解析结果，
  合并值写入根 `userParameters` 后重新 `ParseJson`。解析失败保留 last-good
  并只告警一次，不向渲染线程抛异常，也不会每帧重解析未变化的参数。

## 验证入口

- 构建：`cmake --build build -j`
- 场景 actor/保存：`ctest --test-dir build -R "^(SceneDocumentTest|EditorActorProperty|ParticleWorldRuntime)\." --output-on-failure`。
  `editor_ui_tests` 覆盖 `particleSystem` actor 快照（类型、身份、TRS、可见性）、World 变换与
  可见性行为，以及 `SceneDocumentTest` 的场景 TRS 保存保留 `particlePath` 与未管理字段；
  测试仍由现有 `editor_ui_tests` / ctest 入口注册，不新增目标。
- 运行时加载：`main.exe --initial-scene Maps/SC_particle_demo/SC_particle_ue_demo.json --no-dev-ui`
- 截图：启动命令加 `--console-command "screenshot build/particle.bmp" --console-command-delay 20`；
  文件落在 `<resourcePath>/Generated/Screenshots/<path>`。
- 调试回读：启动命令加 `--console-command "particledump dump_smoke.json 32"`；
  文件落在 `<resourcePath>/Generated/ParticleDumps/<path>`。
- 帧级预算：启动命令加 `--console-command "particlestats" --console-command-delay 60`；
  stdout 先输出一行 `particles frame=...`，再输出一行同字段 JSON。
- GPU pass 计时：`--console-command "particlestats" --console-command-delay 90`；
  输出应含 `gpuTimingSupported=1` 与 `gpuMs/gpuAvgMs/gpuMaxMs/gpuSamples`；
  timestamp 不可用时为 `gpuTimingSupported=0`，渲染不受影响。
- mesh 运行时：`--console-command "particlespawn MeshDebris Maps/SC_particle_demo/PS_mesh_debris.json 0 100 0"`、
  `particlestats` 与 `screenshot particle_mesh.bmp` 按启动脚本每帧一条的顺序提交；stats 的
  `emitters/drawCalls/vertices` 应包含 mesh emitter 的 `meshIndexCount*maxParticles`。
- 缩略图：`main.exe --initial-scene Maps/SC_particle_demo/SC_particle_ue_demo.json --dev-ui --frame-limit 30`；
  Content Browser 完成粒子缩略图时输出 `Particle thumbnail rendered: <PS 路径>`，
  失败则输出 `Particle thumbnail failed` 诊断。运行时验证只确认请求完成与无 Vulkan validation，
  不做像素级比对。
- C# 粒子入口：托管 API 在 `script/VulkanLearn.Runtime/ParticleSystems.cs` 与
  `GameplayScheduler.cs`，`particleCommands` 的原生消费在
  `source/script/scriptRuntime.cpp`；验收需开启 C# 宿主并执行 Play 冒烟。该入口没有新增
  runtime test flag。

Particle 容器、逐 emitter 校验和 `emitters.<index>.<field>` 路径的回归位于
`tool/material-instance-editor-tests/particleAssetTests.cpp`，由
`material_instance_editor_production_tests` 注册。改动 `ParticleAssetLoader` 的校验边界时必须
扩展现有测试；粒子 undo/redo 的字段、结构与 32 步预算由
`particleDocumentHistoryTests.cpp` 覆盖；纹理引用校验与图身份比较分别由
`particleTextureReferenceTests.cpp`、`particleMeshReferenceTests.cpp`、
`particleRenderIdentityTests.cpp` 覆盖；mesh 朝向基与绘制计数由
`particleMeshTests.cpp` 覆盖；平面/盒体/
胶囊碰撞与 CCD 子步的 CPU 镜像公式由 `particleCollisionTests.cpp` 覆盖；dump 的统计、明细截断与
路径安全由 `particleDumpTests.cpp` 覆盖。真实 Vulkan
资源的 retire timing 与 resize 需要运行时 validation 冒烟，不能由纯 CPU GTest 证明。

- M3/M4/M5 新增回归：`particleMaterialSdfTests.cpp` 覆盖 MI 数值子集、
  SDF 解析/三线性采样、全局排序顺序与容量/纹理槽位决策、固定步长累积；
  `particleCollisionTests.cpp` 覆盖 SDF 推出与越界不修改；`particleAssetTests.cpp`
  覆盖 v2 `$userParameter` 替换、events/确定性字段解析、localSpace+碰撞组合与
  `materialInstancePath` 路径校验。
- 全局排序运行时冒烟：`SC_particle_ue_demo` 应输出
  `[Particles][Info] Global particle sort active: emitters=2 capacity=4096 textures=1`，
  `particlestats` 的 `drawCalls` 从逐发射器绘制的 7 降为 6（5 个逐发射器 + 1 个全局），
  且无 `VUID-` / `Validation Error`。

## 高级展示案例

`Maps/SC_particle_advanced/SC_particle_advanced_impact.json` 是可直接启动的
Niagara 式综合案例：`PS_advanced_impact.json` 用 5 个发射器组合火焰、火星、烟柱、
冲击尘和岩石碎片，覆盖 v2 模块、全局 alpha 排序、mesh 实例化与 CSM、三角网格碰撞、
Data Channels 和 C# 事件订阅。场景自带 ground/rock mesh、程序化焦痕地面和火/烟
翻转书图集；详细观察点见该目录的 `README.md`。

```powershell
main.exe --initial-scene Maps/SC_particle_advanced/SC_particle_advanced_impact.json --no-dev-ui --focus-window
```

验收时应看到 `Global particle sort active: emitters=2`（烟柱 + 冲击尘），且无
`VUID-` / `Validation Error`。编辑态直接使用 `PS_*.json` 默认 `userParameters`；
Play 态由 `Gameplay.ParticleDirector` 通过 `Particles.Data` 覆盖发射率、湍流、风场
和重力，并输出碰撞事件日志。

### UE 5.8 Niagara 示例复刻

`Maps/SC_particle_ue_explosion/SC_particle_ue_explosion.json` 对齐 UE 5.8
`NiagaraExamples` 的 `NS_Explosion` / `NS_Dirt_Explosion` / `NS_Smoke_Plume`：
UE 侧爆炸系统固定边界为 `1500 x 1500 x 850 cm`、效果类型
`NET_Gameplay_Burst`，并使用 sprite、mesh、light、decal 多个渲染器。
VulkanLearn 版本用 8 个发射器（`flash`、`core_burst`、`roil_smoke`、
`dirt_burst`、`spark_burst`、`debris`、`smoke_plume`、`explosion_light`）重建火球、翻滚烟、
地面尘、火花、岩石碎片和残余烟柱；`burstInterval = 3.0s` 循环，火花/碎片走
SDF 三角网格碰撞，核心/烟/尘/烟柱进入全局 alpha 排序；`explosion_light` 用
3 盏 GPU per-particle 点光复刻 UE Light Renderer；地面尘与残余烟柱开启
`billboardLighting`，由火球灯光与场景补光染色（`roil_smoke` 保留 unlit，
避免蓝色彩光盖过火球本体）。mesh 碎片走完整 DefaultLit。

该案例是结构/视觉对齐，不是 UE 资产的直接移植：`ExplosionScorch` 使用
BeforeLighting DBuffer decal（见 `decal-runtime.md`）；Light Renderer 由 GPU 粒子池和
独立 light emit compute 生成；
没有 UE Material Graph；SubUV 采用逐粒子播放/线性混合子集；粒子图集与 decal 图案
为程序化等价物。
启动与验收细节见该目录的 `README.md`。

`Maps/SC_particle_ue_firework/SC_ue_firework.json` 对齐 UE 5.8
`NiagaraExamples/FX_Misc/NS_FireworkBurst`（Leaders + Burst、GPU sprite、
`User.Burst Color/Brightness`、`Ring Radius`、`Spread`）。VulkanLearn 用
`rocket_leader`、`burst_core`、`ring_sparks`、`willow_sparks`、`crackle`、
`burst_smoke`、`firework_light` 重建升空拖尾、8×8 爆心 SubUV、环形火花、
柳叶长尾、连续碎火、残烟和 900 cd GPU 粒子灯。`rocket_leader` 是 28 段
ribbon，使用 16 点历史、弧长重采样、逐弧长宽度曲线与 UV 平铺；爆心/碎火使用
UE 导出的 EOO 图集（`eooPacked=true`），R 通道作为自发光、A 作为不透明度。
该案例没有 UE 的 Leader→Burst 属性绑定：爆心与拖尾在同一 4 s 周期触发，
环形速度来自盒式随机分布而不是 UE 径向速度模块。

`Maps/SC_particle_ue_fire/SC_ue_fire.json` 对齐 UE 5.8
`NiagaraExamples/FX_Misc/NS_Fire`（BaseLight / Lights / FlamesOnly /
FlamesAndEmbers / Smoke）。VulkanLearn 用 `flames_core`、`flames_roil`、
`smoke_lit`、`embers`、`heat_haze`、`fire_light`、`wispy_smoke` 重建火球、
翻滚火舌、受光烟、余烬、热扰动、火焰灯和细烟；火焰/烟图集来自 UE EOO 导出，
SubUV 按逐粒子年龄播放并在相邻帧线性混合。`smoke_lit` 开启 5-tap 软粒子、
`billboardLighting`、`billboardDensity` 逐灯透光与全局 alpha 排序；
`heat_haze` 走独立 distortion 缓冲。它不复制 UE 的 Material Graph、真实
StaticMeshLocation 绑定或逐粒子碰撞。

`Maps/SC_particle_ue_smoke/SC_ue_smoke.json` 同时对齐 UE 5.8
`NiagaraExamples/FX_Smoke/NS_Chimney_Smoke` 与 `NS_Smoke_Plume`，场景内放
`PS_ue_chimney_smoke`（`chimney_smoke` + `ne_smoke`）和
`PS_ue_smoke_plume`（`smoke_plume` + `wispy`）两个粒子系统。四个发射器都用
`T_SmokePuffLight_EOO_Loop` / `T_Smoke_Wispy` 的 8×8 SubUV、逐粒子混合、
wind/curl、5-tap 软粒子、`billboardDensity` 与全局 alpha 排序；烟柱尺寸/寿命
比烟囱更大。没有 UE 的 VectorNoise 节点图、逐灯 transmittance 阴影或真实
Translucency Volume。

`Maps/SC_particle_ue_impact/SC_ue_impact_concrete.json` 对齐 UE 5.8
`NiagaraExamples/FX_Weapons/Impacts/NS_Impact_Concrete`。UE 系统由
`NE_Impact_LightDecal`、`NE_Impact_MeshAndSprite`、`NE_Impact_SecondarySprite`
和 `NE_Impact_Sprite` 组成；VulkanLearn 用 `impact_flash`、`impact_sparks`、
`impact_dust`、`impact_chips`、`impact_smoke`、`impact_light`、`impact_decal`
七个发射器重建闪光、火花、尘、混凝土碎块、残留烟和墙面贴花，并以
`burstInterval = 2.5s` 循环。墙面 decal 由 `impact_decal` 的 GPU 模拟/gather 回读生成
（无 GPU 池时回落到 CPU 运行时），
使用 `T_Impact_Concrete_*` 的 albedo/normal/ORM 贴图，经
`decalResponse = colorNormalRoughness` 写入 DBuffer。
尘与残留烟开启 `billboardLighting`，由撞击闪光与场景补光染色；碎块 mesh 走
完整 DefaultLit。

该案例同样不是 UE 资产直接移植：`NE_Impact_LightDecal` 的光强由
`impact_light` 的 GPU per-particle 点光复刻，墙面 decal 由 `impact_decal` 的
particle-driven decal 生成，不再保留静态 `ImpactCrater` 场景对象。没有 UE Material Graph；
碎块复用 `SM_rock`，贴图与 decal
图案为程序化等价物；只有碎块使用现有三角网格 ground 碰撞，火花不启用碰撞。

```powershell
main.exe --initial-scene Maps/SC_particle_ue_impact/SC_ue_impact_concrete.json --no-dev-ui --focus-window
```

验收时应看到墙面撞击坑 decal、短暂闪光、向外喷出的火花与碎块，以及尘/烟
层；日志不应出现 `VUID-`、`Validation Error` 或纹理 fallback。启动与近似
细节见该目录的 `README.md`。

`Maps/SC_particle_ue_muzzle/SC_ue_muzzle_flash.json` 对齐 UE 5.8
`NiagaraExamples/FX_Weapons/MuzzleFlashes/NS_MuzzleFlash`。UE 系统导入
`NE_MuzzleFlash_Base`、`NE_MuzzleFlash_Smoke`、`NE_MuzzleFlash_Sparks_Base`
和 `NE_BulletShells`，实例包含 `Flash_Center`、`MuzzleFlash_Front`、
`MuzzleFlash_Side`、`Muzzle_Smoke`、`Muzzle_Sparks`、`Bullet_Shell` /
`Bullet_Shell_Eject`。VulkanLearn 用 `flash_front`、`flash_side`、
`flash_center`、`muzzle_smoke`、`muzzle_sparks`、`muzzle_heat_haze`、
`bullet_shell`、`muzzle_light` 八个发射器重建，`burstInterval = 1.0s` 循环；程序化步枪与弹壳为
静态 SM，闪光/烟/扰动贴图为程序化 TGA；火花使用引擎程序化圆盘，不接纹理。

该案例是结构/视觉近似：UE 的 particle-driven light 用 `muzzle_light` 的
GPU per-particle 点光复刻；
front 卡片使用 camera billboard，side 卡片使用 fixed billboard；
`muzzle_heat_haze` 走 `renderMode: "distortion"` 的独立屏幕高度归一化偏移缓冲，由
bloomPrefilter 与 toneMapping 共同消费；没有 UE flipbook 动画；没有 Material Graph，
贴图与材质全部由代码/JSON
维护。启动与截图见该目录的 `README.md`。

`Maps/SC_particle_ue_tesla/SC_ue_tesla_coil.json` 对齐 UE 5.8
`NiagaraExamples/FX_Ribbons/NS_TeslaCoil`。`PS_ue_tesla_coil.json` 用
`arc_main`、左右环境弧、左右落地弧、`arc_cross`、左右环形辉光、左右节点火花
和左右 `tesla_light_*`、`tesla_smoke`、左右 `tesla_node_decal_*` 共 15 个发射器重建
双极放电；beam 使用 `branchCount` /
`branchStart` / `branchLength` / `branchSpread` 生成分支，两个
`renderMode: "light"` 发射器每 0.7 s 生成 150 cd / 900 cm 的 GPU per-particle 灯。
左右落地弧启用 `beam.hitScan`（y=10 的地面平面），`tesla_smoke` 以连续 billboard
补充线圈上方的薄烟，左右 `tesla_node_decal_*` 通过粒子 decal 重建
`Tesla_Node_Decals` 的节点辉光。它仍不是 UE 资产直接移植：HitScan 是受控平面命中，
不是任意物理 mesh raycast；节点 decal 没有材质图；Material Graph 仍未复刻。
