#ifndef PARTICLE_SPRITE_GLSL
#define PARTICLE_SPRITE_GLSL

// 只有 graphics billboard 阶段消费 flipbook 图集；compute 阶段不声明 sampler，
// 避免 compute descriptor layout 里出现未写入的 combined image sampler。
layout(set = 0, binding = 2) uniform sampler2D particleTexture;

#endif
