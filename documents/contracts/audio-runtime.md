# 音频运行时

> 状态：当前契约（实现已落地）。描述 `AudioRuntime` 的 SDL3 双声道输出、固定容量
> voice 池、块混音、等功率 pan、距离衰减、距离低通，以及
> `<resourcePath>/Common/Audio/` 下 WAV 资产的加载与回退。它不描述
> HRTF、遮挡、混响或 spatial audio 中间件；这些能力尚未接入。

## 输出格式

`AudioRuntime` 用 `SDL_OpenAudioDeviceStream` 打开默认播放设备，格式固定为：

- 48 kHz
- `SDL_AUDIO_F32`
- 2 声道（左/右交错）

混音策略在 `source/audio/audioMixer.*` 中实现，不依赖 SDL，因此可以被
GoogleTest 直接覆盖。`AudioRuntime` 只负责设备生命周期、资产/程序化样本准备和
把混音块写入 SDL 流。

## 音频资产

`AudioRuntime` 构造函数接收 resource root，并在音频设备可用时预加载
`<resourcePath>/Common/Audio/` 下的以下文件。资产只读一次，播放时复制
已重采样到 48 kHz 的 mono 样本，不在 GT 上做磁盘 I/O 或重采样。

| 文件 | 用途 | 来源与许可 |
| --- | --- | --- |
| `glass_break.wav` | 玻璃破碎、开裂切片、碎片落地切片 | [Glass breaking (Gravity Sound)](https://commons.wikimedia.org/wiki/File:Glass_breaking_(Gravity_Sound).wav)，Gravity Sound，CC BY 4.0 |
| `balloon_pop.wav` | 气球爆裂 | [Balloon Sounds](https://opengameart.org/content/balloon-sounds)，AntumDeluge（pop 录音 Gniffelbaf），CC0 |
| `balloon_inflate.wav` | 漏气时的橡胶膜层（反向播放） | 同上，inflate 录音 d.n.audio.uk，CC0 |
| `gas_leak.wav` | 漏气嘶声 | [Compressed Gas Leak SFX](https://opengameart.org/content/compressed-gas-leak-sfx)，0new4y，CC0 |

素材在 `D:\YYBWorkSpace\GitHub\VukanLearnResources\Common\Audio\`，不随引擎
源码仓库提交。完整署名与修改说明同时写在同目录的 `ATTRIBUTION.md`。
玻璃素材的修改为：44.1 kHz 24-bit stereo → 48 kHz mono、截取攻击段与尾音
切片、与合成的同步 click/低频冲击叠加。气球素材的修改为：FLAC → PCM16 WAV、
重采样、截取、与合成的压力/橡胶回弹层叠加；充气录音末段反向后作为漏气膜层。

解码器 `source/audio/audioClip.*` 支持 RIFF/WAVE 的 PCM 8/16/24/32-bit、
IEEE float 32/64-bit 和 `WAVE_FORMAT_EXTENSIBLE`；多声道按算术平均降混为
mono，再用线性插值重采样到 48 kHz。压缩 WAV、截断 chunk 和不支持的
bit depth 会被拒绝。

任一文件缺失或解码失败时只报告一次 warning，对应音效回退到本文件后述的
程序化合成；渲染、物理和 GT 循环不受影响。

## Voice 预算与抢占

全局 voice 上限为 **32**（`VL::Audio::MaxVoices`），所有资产/程序化音效共享同一池。
每个 voice 记录：

- 剩余样本（`samples.size() - cursor`）与播放游标；
- 优先级；
- 声源世界位置；
- 一阶低通滤波状态。

优先级从高到低：

1. `Impact`：`Burst` / `Shatter` / `Crack`；
2. `Leak`：`Leak`；
3. `Landing`：`Landing` / `Subfracture`。

池满时按以下顺序挑选被抢占的 voice：

1. 优先级最低者优先；
2. 同级中距离 listener 最远者优先；
3. 距离差小于 1 cm 时，剩余样本更短者优先；
4. 仍然完全相同时，保留更早创建的槽位，抢占后创建的同权重 voice。

如果新 voice 不比池里最不重要的 voice 更重要，则直接丢弃，不打断已有声音。
因此 `Landing` 不会抢占 `Impact`/`Leak`，而一次新的爆裂可以抢占远处的落地声。

落地音在 API 层仍有事件冷却：气球碎片 24 ms，玻璃碎片 30 ms；冷却用于抑制
同一帧大量接触事件，不替代 voice 预算。

## 块混音

音效不再把整段样本直接追加到 SDL 流。`VoiceMixer::Submit` 只登记 voice，
`MixBlock` 每次混合 `128` 帧（`MixBlockFrames`）：

- 先清零 stereo block；
- 每个活动 voice 把自己的 mono 样本乘以距离增益，经一阶低通后按 pan 增益
  叠加到左右声道；
- 播完最后一个样本的 voice 立即停用，复用的槽位保留样本容量；
- 输出做硬限幅到 `[-1, 1]`，并拦截非有限值。

`EngineLoop::Tick` 每帧调用一次 `AudioRuntime::Update()`。`Update` 以
128 帧为单位把 SDL 流补到约 `1024` 帧（≈21 ms）的排队深度；在 60 FPS
下至少覆盖一帧，单次最多补 `24` 个 block 只作为掉帧保护。事件提交后会立即
`Pump`，但已经排队的旧样本不会被缩短，实时延迟由目标队列深度决定。
没有活动 voice 时仍然写静音块，保持设备流连续。

混音在 GT 上同步完成，不使用 SDL 回调线程；因此 GT 长时间卡顿时音频可能
欠载（断音），但不存在跨线程共享 voice 池的竞态。

## 空间化

listener 状态由 `SetListenerPosition` 与 `SetListenerOrientation` 维护。
`EngineLoop` 每帧从当前渲染相机读取位置、前向和上向量；默认前向为 `+Z`、
上方向为 `+Y`。右向量取 `up × forward`，默认得到 `+X`。

### 等功率 pan

设声源相对 listener 的单位方向在右向量上的投影为 `pan ∈ [-1, 1]`，则

```text
θ = (pan + 1) · π/4
leftGain  = cos(θ)
rightGain = sin(θ)
leftGain² + rightGain² = 1
```

`pan = 0` 时左右各 `1/√2`；`pan = ±1` 时声音完全落在单侧。声源与 listener
重合时方向无定义，pan 取 0。

### 距离衰减

```text
distance = |source - listener|
gain = clamp(1 / (1 + distance / 200), 0.04, 1)
```

距离单位为厘米，`200 cm` 时增益为 0.5。

### 距离低通

远距离声音用一阶低通压暗：

```text
t      = clamp(distance / 3000, 0, 1)
cutoff = 20000 · (2000 / 20000)^t      // Hz
α      = 1 - exp(-2π · cutoff / 48000)
y[n]   = y[n-1] + α · (x[n] - y[n-1])
```

截止频率随距离从 `20 kHz` 指数下降到 `2 kHz`，`3000 cm` 之后保持不变。
滤波状态跨 block 保留，避免每 128 帧重启滤波器产生咔哒声。空间参数按
block 重算，listener 在一个 block 内视为静止。

## 设备不可用

SDL 音频子系统或播放设备打开失败时只报告一次 warning，之后 `PlayImpact`
静默跳过；渲染、物理和 GT 循环不受影响。

## 声音内容

运行时只发 `ImpactAudioEvent { audioProfile, kind, position, intensity,
sizeScale, seed }`。`audioProfile` 选择玻璃/气球，`kind` 选择 Crack/Shatter/
Leak/Burst/Landing/Subfracture；具体采样选择留在 `AudioRuntime` 内部。
资产存在时，录音负责提供材质与尾音，程序化层只补齐命中同步和低频体感：

| audioProfile + kind | 资产路径 | 程序化叠加 |
| --- | --- | --- |
| `balloon` + `Burst` | `balloon_pop.wav` 的 0.56 s 片段，3 个播放速率变体 | 78 Hz 压力脉冲 + 橡胶回弹 + 尾部 flutter |
| `balloon` + `Leak` | `gas_leak.wav` 1.15 s + 反向 `balloon_inflate.wav` 0.55 混音增益 | 150 Hz 轻微橡胶抖动 |
| `balloon` + `Landing` | 无（橡胶碎片保持程序化） | 衰减共振 + 噪声点击 |
| `glass` + `Shatter` | `glass_break.wav` 截掉前 52 ms 的 0.58 s 片段，3 个速率变体 | 同步高通 click + 118 Hz 低频冲击 |
| `glass` + `Crack` | 同一录音的 0.16 s 攻击切片 | 高频 click + 5200/7800 Hz 短模态 |
| `glass` + `Landing` | 破碎尾音的 4 个位置 × 3 个速率切片 | 同步噪声 click |
| `glass` + `Subfracture` | 同 Crack 切片，降低强度 | 高频 click |

任一路径缺失时，对应事件完整回退到原有程序化合成（气球爆裂 0.55 s、
漏气 0.90 s、玻璃破碎 0.48 s、开裂 0.14 s、玻璃落地 0.12 s）。

## 测试

`tool/audio-runtime-tests/` 通过根 `vulkanlearn_add_gtest` 注册，覆盖：

- 32 voice 上限与优先级抢占顺序；
- 同级按距离、剩余样本长度抢占；
- 等功率 pan 的左右/中心取值与 listener 朝向；
- 距离增益、截止频率与低通系数的单调性；
- 多 voice 叠加、满池饱和、非有限样本净化和播完释放；
- WAV PCM16 立体声降混、PCM24/float32 解码、截断/压缩格式拒绝；
- 线性重采样的长度与端点、切片淡入淡出、峰值/增益限幅。

## 当前边界

- 只有等功率 pan 和距离衰减，没有 HRTF、头部遮蔽、遮挡、混响或空气吸收；
- 输出是硬限幅，没有总线压缩/限幅器；
- voice 优先级是编译期常量，场景 JSON 不暴露音量、音高或 voice 参数；
- 混音由 GT 帧循环驱动，不使用音频回调线程，也不支持后台/暂停时独立播放；
- 没有全局 master volume、静音开关或音频设备热插拔处理；
- 资产根固定为 `<resourcePath>/Common/Audio/`，没有按场景覆盖或运行时换包的接口；
- 重采样是线性插值，没有带限 sinc 滤波；资产采样率高于 48 kHz 时高频会有折返风险，
  当前素材都是 44.1 kHz。
