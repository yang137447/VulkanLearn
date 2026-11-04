#ifndef VL_COMMON_LOCAL_SHADOW_SAMPLING_GLSL
#define VL_COMMON_LOCAL_SHADOW_SAMPLING_GLSL

// 局部光阴影采样（八面体点光 / 透视聚光）。
//
// 为什么 sampler 由调用方传入而不在这里声明：
//   deferredLighting 用 set 3 binding 15，forward 用 binding 6，particle 用 binding 2。
//   三条 pass 的 set 3 布局不同，公共头只能提供纯函数，具体 pass shader 声明自己的
//   sampler 后再传进来，避免在 compute 或无关 pass 的 descriptor layout 里凭空多出采样器。
//
// 数据合同（录制端必须逐条一致，否则阴影会整体错位而不是轻微偏移）：
//   - 点光：一个点光占 texture2DArray 的一个 slice。方向
//         dir = normalize(pixelPosition - lightPosition)
//     按标准八面体映射编码到 [0,1]^2；存储深度是线性的
//         length(pixelPosition - lightPosition) / far
//     不是透视投影深度。录制端必须写 gl_FragDepth，不能只依赖光栅化深度。
//   - 聚光：一个聚光占一个 slice，存标准透视深度。view-proj 由 shader 用
//         lookAt(lightPosition, lightPosition + lightDirection, up)
//         fov = 2 * coneOuter, aspect = 1, near = 1 cm, far = shadowParams.z
//     重建，up 选择必须与录制端相同（见 BuildLocalSpotShadowWorldToNdc）。

// 5×5 PCF：每个 tap 先折回有效区域，再做一次硬件 comparison sampling。
//
// 为什么不是 PCSS：blocker search 需要读取阴影图的原始深度，而局部阴影图
// 目前只以 comparison sampler（sampler2DArrayShadow）绑定；同一 descriptor
// 槽位无法再以普通 sampler 读取。要上真正的 PCSS 需要给三条 pass 各加一个
// 非比较采样器绑定，属于 descriptor 合同变更，不在本次质量修复范围内。
// 这里先把固定核从 3×3 提到 5×5，并用接收面斜率 bias 抑制条纹/摩尔纹。
#define VL_LOCAL_SHADOW_PCF_RADIUS 2
// 聚光阴影 near 固定 1 cm；点光不使用透视投影。
#define VL_LOCAL_SHADOW_SPOT_NEAR_CM 1.0
// 接收面 bias：作者值（cm）只消费一次，拆成 normal offset 与 slope depth bias 两部分。
// normal offset 在掠射时沿法线推开采样点；depth bias 补偿纹素足迹与深度量化。
#define VL_LOCAL_SHADOW_NORMAL_OFFSET_SCALE 0.5
#define VL_LOCAL_SHADOW_SLOPE_BIAS 1.5
// 深度 bias 上限（相对作者值）：防止极端掠射把接触阴影推离投射物。
#define VL_LOCAL_SHADOW_MAX_DEPTH_BIAS_SCALE 4.0
// N·L 下限避免掠射时 tan(theta) 爆炸；0.2 对应约 78.5°，已经覆盖可见掠射面。
#define VL_LOCAL_SHADOW_MIN_NDOTL 0.2

// 八面体上下半球的折叠。0 必须按 +1 处理：sign(0) = 0 会把 -Z 极点
// 折到 (0,0)，与解码端的 corner = -Z 约定不一致，导致极点处出现暗斑。
vec2 FoldOctahedralHemisphere(vec2 value)
{
    return (1.0 - abs(value.yx)) *
        vec2(
            value.x >= 0.0 ? 1.0 : -1.0,
            value.y >= 0.0 ? 1.0 : -1.0);
}

// 输入世界空间方向，输出 [-1,1]^2 的八面体坐标（菱形是前半球，
// 四个角是后半球；方形四边是展开切口）。
vec2 EncodeOctahedralDirection(vec3 direction)
{
    vec3 projected = direction / max(
        abs(direction.x) + abs(direction.y) + abs(direction.z),
        1.0e-8);
    return projected.z >= 0.0
        ? projected.xy
        : FoldOctahedralHemisphere(projected.xy);
}

vec3 DecodeOctahedralDirection(vec2 encoded)
{
    vec3 direction = vec3(
        encoded,
        1.0 - abs(encoded.x) - abs(encoded.y));
    if (direction.z < 0.0)
    {
        direction.xy = FoldOctahedralHemisphere(direction.xy);
    }
    return normalize(direction);
}

// 把越界 tap 折到真实邻居：
//   - 菱形边界（赤道）两侧本来就是连续数据，该变换是恒等的；
//   - 方形四边是八面体展开切口，直接采样会被 CLAMP_TO_EDGE 拉到错误像素，
//     解码-编码会把切口外侧的 tap 投到对侧的真实纹素。
// 恒等式 Decode(Fold(u)) == Decode(u) 保证折叠只改 UV，不改它代表的方向。
vec2 FoldOctahedralTap(vec2 octahedralUv)
{
    return EncodeOctahedralDirection(
        DecodeOctahedralDirection(octahedralUv * 2.0 - 1.0)) * 0.5 + 0.5;
}

float SampleOctahedralShadowPcf(
    in sampler2DArrayShadow localShadowMap,
    vec3 directionToPixel,
    float slice,
    float referenceDepth)
{
    ivec3 shadowSize = textureSize(localShadowMap, 0);
    vec2 texelSize = 1.0 / vec2(max(shadowSize.xy, ivec2(1)));
    vec2 shadowUv =
        EncodeOctahedralDirection(directionToPixel) * 0.5 + 0.5;
    float visibility = 0.0;
    for (int offsetY = -VL_LOCAL_SHADOW_PCF_RADIUS;
        offsetY <= VL_LOCAL_SHADOW_PCF_RADIUS;
        ++offsetY)
    {
        for (int offsetX = -VL_LOCAL_SHADOW_PCF_RADIUS;
            offsetX <= VL_LOCAL_SHADOW_PCF_RADIUS;
            ++offsetX)
        {
            vec2 tapUv =
                shadowUv +
                vec2(float(offsetX), float(offsetY)) * texelSize;
            // 方形内部的 tap 已经是合法八面体坐标；只有越界 tap 需要折叠，
            // 避免为每个 5×5 采样都做一次 decode/encode。
            if (tapUv.x < 0.0 || tapUv.x > 1.0 ||
                tapUv.y < 0.0 || tapUv.y > 1.0)
            {
                tapUv = FoldOctahedralTap(tapUv);
            }
            visibility += texture(
                localShadowMap,
                vec4(tapUv, slice, referenceDepth));
        }
    }
    const float tapCount = float(
        (2 * VL_LOCAL_SHADOW_PCF_RADIUS + 1) *
        (2 * VL_LOCAL_SHADOW_PCF_RADIUS + 1));
    return visibility / tapCount;
}

// 与 source/render/shadow/localShadowMath.cpp 的 BuildSpotShadowMatrices
// 逐元素等价，直接展开成 world → Vulkan NDC（z ∈ [0,1]）。
// 注意：录制端透视矩阵的 y 比例是 -1/tan(cone)，再经 NDC y 翻转后回到
// +1/tan(cone)；这与 Camera::SetProjection 的最终 y 符号相反。采样必须
// 复刻“录制端实际使用的矩阵”，所以这里跟随 BuildSpotShadowMatrices，
// 不能改成 Camera 的符号，否则阴影图会上下翻转。
mat4 BuildLocalSpotShadowWorldToNdc(
    vec3 lightPosition,
    vec3 lightDirection,
    float coneOuterRadians,
    float nearCm,
    float farCm)
{
    // 光传播方向（light → scene）取反得到相机前向（Camera::SetCamera 的 f）。
    // 方向为零时录制端回退到 (0,0,-1)，这里必须一致。
    float directionLength = length(lightDirection);
    vec3 viewForward = directionLength > 1.0e-6
        ? -lightDirection / directionLength
        : vec3(0.0, 0.0, 1.0);
    // 录制端必须使用同一规则，否则整张阴影图会绕光轴旋转。
    vec3 upHint = abs(viewForward.y) > 0.99
        ? vec3(1.0, 0.0, 0.0)
        : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(upHint, viewForward));
    vec3 up = cross(viewForward, right);

    mat4 view;
    view[0] = vec4(right.x, up.x, viewForward.x, 0.0);
    view[1] = vec4(right.y, up.y, viewForward.y, 0.0);
    view[2] = vec4(right.z, up.z, viewForward.z, 0.0);
    view[3] = vec4(
        -dot(right, lightPosition),
        -dot(up, lightPosition),
        -dot(viewForward, lightPosition),
        1.0);

    float aspect = 1.0;
    // fov = 2 * coneOuter，所以 tan(fov/2) = tan(coneOuter)。
    // 上界与录制端的 clamp 一致，超过 89° 的锥角按 89° 处理。
    float tangent = tan(clamp(coneOuterRadians, 0.001, 1.5533431));
    float scale = -1.0 / tangent;
    float nearValue = -nearCm;
    float farValue = -farCm;
    float depthA = -(nearValue + farValue) / (nearValue - farValue);
    float depthB = 2.0 * nearValue * farValue / (nearValue - farValue);

    // 等价于 NdcMatrix * Projection：y 翻转，深度从 GL [-1,1] 映射到 [0,1]。
    // 因为中间投影的 y 也是 scale（不是 Camera 的 -k），翻转后得到 +1/tan，
    // 与 BuildSpotShadowMatrices 的最终矩阵一致。
    mat4 projection;
    projection[0] = vec4(scale, 0.0, 0.0, 0.0);
    projection[1] = vec4(0.0, -aspect * scale, 0.0, 0.0);
    projection[2] = vec4(0.0, 0.0, -0.5 * depthA - 0.5, -1.0);
    projection[3] = vec4(0.0, 0.0, -0.5 * depthB, 0.0);
    return projection * view;
}

float SampleSpotShadowPcf(
    in sampler2DArrayShadow localShadowMap,
    vec2 shadowUv,
    float slice,
    float referenceDepth)
{
    ivec3 shadowSize = textureSize(localShadowMap, 0);
    vec2 texelSize = 1.0 / vec2(max(shadowSize.xy, ivec2(1)));
    float visibility = 0.0;
    for (int offsetY = -VL_LOCAL_SHADOW_PCF_RADIUS;
        offsetY <= VL_LOCAL_SHADOW_PCF_RADIUS;
        ++offsetY)
    {
        for (int offsetX = -VL_LOCAL_SHADOW_PCF_RADIUS;
            offsetX <= VL_LOCAL_SHADOW_PCF_RADIUS;
            ++offsetX)
        {
            // 聚光视锥外没有数据；锥外本来也被 angleIntensity 判为不受光，
            // 这里 clamp 只是避免 PCF 边缘 tap 读到无关 slice 内容。
            vec2 tapUv = clamp(
                shadowUv +
                vec2(float(offsetX), float(offsetY)) * texelSize,
                vec2(0.0),
                vec2(1.0));
            visibility += texture(
                localShadowMap,
                vec4(tapUv, slice, referenceDepth));
        }
    }
    const float tapCount = float(
        (2 * VL_LOCAL_SHADOW_PCF_RADIUS + 1) *
        (2 * VL_LOCAL_SHADOW_PCF_RADIUS + 1));
    return visibility / tapCount;
}

// 接收面 bias 的中间结果。
//   position    ：沿法线偏移后的世界坐标，后续方向/深度都从这里重算；
//   depthBiasCm ：已按 N·L 做 slope 放大的世界深度偏置（cm）。
struct LocalShadowReceiverBias
{
    vec3 position;
    float depthBiasCm;
};

// 把作者的 bias（cm）换算成接收面偏置：
//   normalOffset = bias * 0.5 * (1 - N·L)
//   depthBiasCm  = bias * clamp(1 + 1.5 * tan(theta), 1, 4)
// 正常入射时 normal offset 为 0、depth bias 保持作者值；掠射时两者的
// normal offset 主要是横向位移。深度项封顶 4×bias，避免极端掠射时把
// 接触阴影整体推离投射物（peter-panning）。
//
// pixelNormal 长度为 0 表示调用方没有几何法线（旧 6 参数入口）：退回
// N·L = 1，也就是只保留作者深度 bias，行为与升级前一致。
LocalShadowReceiverBias BuildLocalShadowReceiverBias(
    vec3 pixelPosition,
    vec3 pixelNormal,
    vec3 lightPosition,
    float biasCm)
{
    LocalShadowReceiverBias result;
    result.position = pixelPosition;
    result.depthBiasCm = max(biasCm, 0.0);

    vec3 toLight = lightPosition - pixelPosition;
    float normalLengthSquared = dot(pixelNormal, pixelNormal);
    float lightDistanceSquared = dot(toLight, toLight);
    if (normalLengthSquared <= 1.0e-12 ||
        lightDistanceSquared <= 1.0e-12 ||
        biasCm <= 0.0)
    {
        return result;
    }

    vec3 normal = pixelNormal * inversesqrt(normalLengthSquared);
    vec3 surfaceToLight = toLight * inversesqrt(lightDistanceSquared);
    float nDotL = clamp(dot(normal, surfaceToLight), 0.0, 1.0);
    // tan(theta) = sqrt(1 - N·L²) / N·L，分母设置下限避免掠射发散。
    float sine = sqrt(max(1.0 - nDotL * nDotL, 0.0));
    float slope = sine / max(nDotL, VL_LOCAL_SHADOW_MIN_NDOTL);

    result.position =
        pixelPosition +
        normal *
            (biasCm *
             VL_LOCAL_SHADOW_NORMAL_OFFSET_SCALE *
             (1.0 - nDotL));
    result.depthBiasCm =
        biasCm *
        clamp(
            1.0 + VL_LOCAL_SHADOW_SLOPE_BIAS * slope,
            1.0,
            VL_LOCAL_SHADOW_MAX_DEPTH_BIAS_SCALE);
    return result;
}

// 透视投影的深度对世界视距的导数：
//   ndc.z = far * (d - near) / ((far - near) * d)
//   d(ndc.z)/dd = near * far / ((far - near) * d²)
// 作者 bias 以 cm 存储，必须按当前接收点距离换算成 ndc 单位，
// 否则近处会过偏、远处会欠偏。
float LocalShadowSpotDepthBiasPerCm(
    float viewDistanceCm,
    float nearCm,
    float farCm)
{
    float safeDistance = max(viewDistanceCm, nearCm + 1.0e-3);
    return
        (nearCm * farCm) /
        (max(farCm - nearCm, 1.0e-3) *
         safeDistance * safeDistance);
}

// 统一入口：shadowParams = (slice, type, farCm, biasCm)；
// type: 1 = 聚光，2 = 八面体点光。slice < 0 或 type == 0 表示不投影。
// 返回 1.0 = 完全可见；0.0 = 完全遮蔽。
float SampleLocalLightShadow(
    in sampler2DArrayShadow localShadowMap,
    in vec4 shadowParams,
    in vec3 lightPosition,
    in vec3 lightDirection,
    in float coneOuterRadians,
    in vec3 pixelPosition,
    in vec3 pixelNormal)
{
    if (shadowParams.x < 0.0 || shadowParams.y < 0.5)
    {
        return 1.0;
    }
    float farCm = max(shadowParams.z, 1.0e-3);
    LocalShadowReceiverBias receiverBias =
        BuildLocalShadowReceiverBias(
            pixelPosition,
            pixelNormal,
            lightPosition,
            shadowParams.w);

    if (shadowParams.y < 1.5)
    {
        mat4 worldToNdc = BuildLocalSpotShadowWorldToNdc(
            lightPosition,
            lightDirection,
            coneOuterRadians,
            VL_LOCAL_SHADOW_SPOT_NEAR_CM,
            farCm);
        vec4 clipPosition =
            worldToNdc * vec4(receiverBias.position, 1.0);
        if (clipPosition.w <= 1.0e-6)
        {
            return 1.0;
        }
        vec3 ndc = clipPosition.xyz / clipPosition.w;
        if (ndc.x < -1.0 || ndc.x > 1.0 ||
            ndc.y < -1.0 || ndc.y > 1.0 ||
            ndc.z < 0.0 || ndc.z > 1.0)
        {
            return 1.0;
        }
        // 用 ndc.z 反解视距，再把 cm 偏置换算成 ndc 深度单位。
        float nearCm = VL_LOCAL_SHADOW_SPOT_NEAR_CM;
        float viewDistanceCm =
            (nearCm * farCm) /
            max(farCm - ndc.z * (farCm - nearCm), 1.0e-3);
        float normalizedBias =
            receiverBias.depthBiasCm *
            LocalShadowSpotDepthBiasPerCm(
                viewDistanceCm,
                nearCm,
                farCm);
        return SampleSpotShadowPcf(
            localShadowMap,
            ndc.xy * 0.5 + 0.5,
            shadowParams.x,
            ndc.z - normalizedBias);
    }

    vec3 toPixel = receiverBias.position - lightPosition;
    float distanceCm = length(toPixel);
    if (distanceCm <= 1.0e-6)
    {
        // 灯内部的位置没有可靠方向，按受光处理，避免灯体积里出现黑点。
        return 1.0;
    }
    // 点光存的是线性距离 / far，因此 cm 偏置直接除以 far。
    float normalizedBias = receiverBias.depthBiasCm / farCm;
    return SampleOctahedralShadowPcf(
        localShadowMap,
        toPixel / distanceCm,
        shadowParams.x,
        distanceCm / farCm - normalizedBias);
}

// 兼容入口：调用方没有几何法线时只保留作者深度 bias。
// 粒子 billboard/ribbon 的法线语义与材质不同，迁移到带法线入口前继续使用
// 这个重载，避免在缺少法线时凭空假设 N·L。
float SampleLocalLightShadow(
    in sampler2DArrayShadow localShadowMap,
    in vec4 shadowParams,
    in vec3 lightPosition,
    in vec3 lightDirection,
    in float coneOuterRadians,
    in vec3 pixelPosition)
{
    return SampleLocalLightShadow(
        localShadowMap,
        shadowParams,
        lightPosition,
        lightDirection,
        coneOuterRadians,
        pixelPosition,
        vec3(0.0));
}

#endif
