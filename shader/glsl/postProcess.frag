#version 450

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outColor;

// 绑定 0: 输入的颜色附件（上一个 Pass 的输出）
layout (set = 0, binding = 0) uniform sampler2D inputColor;

void main() 
{
    outColor = texture(inputColor, inUV);
    
    // 简单的 Tone Mapping 示例 (Reinhard)
    // vec3 mapped = outColor.rgb / (outColor.rgb + vec3(1.0));
    // outColor = vec4(mapped, 1.0);
}