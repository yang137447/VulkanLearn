#ifndef VL_PROCEDURAL_SKY_GLSL
#define VL_PROCEDURAL_SKY_GLSL

vec3 EvaluateProceduralSky(vec3 worldDir) {
  float skyFactor = clamp(worldDir.y, 0.0f, 1.0f);
  float skyGradient = pow(skyFactor, uboVP.scatteringControls.x);
  vec3 skyColor =
      mix(uboVP.horizonColor.xyz, uboVP.zenithColor.xyz, skyGradient);

  float groundFactor = clamp(-worldDir.y, 0.0f, 1.0f);
  float groundGradient = pow(groundFactor, uboVP.scatteringControls.y);
  vec3 lowerHemisphereColor =
      mix(uboVP.horizonColor.xyz, uboVP.groundColor.xyz, groundGradient);

  float blendFactor = step(worldDir.y, 0.0f);
  skyColor = mix(skyColor, lowerHemisphereColor, blendFactor);

  // 这里做一下 太阳盘 和 光晕
  vec3 sunDirection = normalize(uboVP.sunDirectionIntensity.xyz);
  float sunCos = dot(normalize(worldDir), sunDirection);
  float sunAngularRadius = uboVP.sunColorAngularRadius.w;
  float sunDisc =
      smoothstep(cos(sunAngularRadius), cos(sunAngularRadius * 0.35f), sunCos);
  float haloExponent = sunAngularRadius > 0.001f ? uboVP.scatteringControls.z / sunAngularRadius : uboVP.scatteringControls.z;
  float sunHalo = pow(max(sunCos, 0.0f), haloExponent) * uboVP.scatteringControls.w;
  vec3 sunColor = uboVP.sunColorAngularRadius.xyz * uboVP.sunDirectionIntensity.w;

  return skyColor + sunColor * (sunDisc + sunHalo);
}

#endif
