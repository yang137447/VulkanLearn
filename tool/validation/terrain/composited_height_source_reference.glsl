float TestSourceBilinear(sampler2DArray sourceTexture, vec2 uv, int slice, int mip)
{
    ivec2 extent=textureSize(sourceTexture,mip).xy;
    vec2 position=fract(uv)*vec2(extent)-0.5;
    ivec2 base=ivec2(floor(position));
    vec2 weights=position-vec2(base);
    ivec2 first=(base%extent+extent)%extent;
    ivec2 second=((base+1)%extent+extent)%extent;
    return mix(
        mix(texelFetch(sourceTexture,ivec3(first,slice),mip).r,
            texelFetch(sourceTexture,ivec3(second.x,first.y,slice),mip).r,weights.x),
        mix(texelFetch(sourceTexture,ivec3(first.x,second.y,slice),mip).r,
            texelFetch(sourceTexture,ivec3(second,slice),mip).r,weights.x),weights.y);
}
