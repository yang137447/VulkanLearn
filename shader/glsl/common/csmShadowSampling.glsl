#ifndef VL_COMMON_CSM_SHADOW_SAMPLING_GLSL
#define VL_COMMON_CSM_SHADOW_SAMPLING_GLSL

// CSM 采样纯函数。这里只使用调用方已经声明好的 uboVP 与 uboLight：
//   - 常规材质路径由 common/lighting.glsl 在声明 UBOLight 之后 include；
//   - 体积雾 compute 自己声明一份同布局 UBOLight 后 include，避免为了
//     复用 CSM 而把 IBL 的 cubemap / BRDF LUT 采样器绑进 compute 布局。
// 改动这里的级联选择/过渡/淡出公式时，deferred、forward、粒子和体积雾会
// 同时变化，必须一起复核。
// 依赖：调用方必须先 include common/shadowFiltering.glsl（FilterShadowMap），
// 并在 include 本文件之前声明 uboVP 与 uboLight。

int SelectShadowCascade(vec3 worldPos)
{
    vec4 viewPos = uboVP.view * vec4(worldPos, 1.0);
    float viewDepth = -viewPos.z;
    int activeCascadeCount = int(uboVP.csmParameters.z + 0.5);
    for (int cascadeIndex = 0; cascadeIndex < 4; ++cascadeIndex)
    {
        if (cascadeIndex >= activeCascadeCount - 1 ||
            viewDepth <= uboVP.cascadeSplits[cascadeIndex])
        {
            return cascadeIndex;
        }
    }
    return activeCascadeCount - 1;
}

float SampleCsmCascade(
    in sampler2DArrayShadow inputShadowMap,
    vec3 worldPos,
    float receiverSlope,
    int cascadeIndex)
{
    vec4 lightViewProjPos =
        uboVP.lightViewProj[cascadeIndex] *
        vec4(worldPos, 1.0);
    vec3 shadowNdc =
        lightViewProjPos.xyz / lightViewProjPos.w;
    vec2 shadowUv =
        shadowNdc.xy * 0.5 + 0.5;
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 ||
        shadowUv.y < 0.0 || shadowUv.y > 1.0 ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    vec4 biasParameters =
        uboVP.shadowBias[cascadeIndex];
    float receiverBias =
        biasParameters.x *
        (1.0 + biasParameters.y * receiverSlope);

    // 阴影采样器使用 LESS_OR_EQUAL 比较。将接收者深度向光源方向偏移，可以减少
    // 自阴影痤疮；Bias 过大则会让接触阴影与投射物分离，产生 Peter-panning。
    float compareDepth =
        shadowNdc.z - receiverBias;
    return FilterShadowMap(
        inputShadowMap,
        shadowUv,
        cascadeIndex,
        compareDepth);
}

float CalculateCsmShadow(
    in sampler2DArrayShadow inputShadowMap,
    vec3 worldPos,
    vec3 worldNormal,
    out int cascadeIndex)
{
    cascadeIndex = 0;
    int activeCascadeCount =
        int(uboVP.csmParameters.z + 0.5);
    if (activeCascadeCount <= 0)
    {
        return 1.0;
    }

    cascadeIndex = SelectShadowCascade(worldPos);
    float viewDepth = -(uboVP.view * vec4(worldPos, 1.0)).z;
    float shadowDistance = uboVP.csmParameters.w;
    if (viewDepth > shadowDistance)
    {
        return 1.0;
    }

    // DirectionalLight.directionPad 保存的是光线传播方向（光源指向场景）。
    // 这里取反得到表面指向光源的方向，用于计算接收面随法线夹角变化的 Bias。
    vec3 surfaceToLight = normalize(
        -uboLight.lights[
            uboLight.directionalLightOffset]
            .directionPad.xyz);

    // 正对光源时 slope 为 0；接近掠射角或背光时 slope 趋近 1，需要更大的 Bias。
    float receiverSlope =
        1.0 -
        max(
            dot(
                normalize(worldNormal),
                surfaceToLight),
            0.0);
    float shadow = SampleCsmCascade(
        inputShadowMap,
        worldPos,
        receiverSlope,
        cascadeIndex);

    // UE 风格 Cascade Transition Fraction：在当前级联末端同时采样下一层，
    // 用深度区间平滑混合，避免级联分界处出现突变。
    if (cascadeIndex + 1 < activeCascadeCount)
    {
        float splitNear =
            cascadeIndex == 0
                ? 0.0
                : uboVP.cascadeSplits[
                    cascadeIndex - 1];
        float splitFar =
            uboVP.cascadeSplits[cascadeIndex];
        float transitionLength =
            (splitFar - splitNear) *
            uboVP.csmParameters.x;
        if (transitionLength > 0.0)
        {
            float transitionWeight = smoothstep(
                splitFar - transitionLength,
                splitFar,
                viewDepth);
            float nextCascadeShadow = SampleCsmCascade(
                inputShadowMap,
                worldPos,
                receiverSlope,
                cascadeIndex + 1);
            shadow = mix(
                shadow,
                nextCascadeShadow,
                transitionWeight);
        }
    }

    // UE 风格 Shadow Distance Fadeout Fraction：最后一段距离逐渐淡到全亮，
    // 避免超过 Dynamic Shadow Distance 时阴影突然消失。
    float fadeoutLength =
        shadowDistance *
        uboVP.csmParameters.y;
    if (fadeoutLength > 0.0)
    {
        float fadeoutWeight = smoothstep(
            shadowDistance - fadeoutLength,
            shadowDistance,
            viewDepth);
        shadow = mix(
            shadow,
            1.0,
            fadeoutWeight);
    }

    return shadow;
}

#endif
