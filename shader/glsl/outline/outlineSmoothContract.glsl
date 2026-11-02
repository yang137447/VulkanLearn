// 描边法线平滑（compute 版）共享契约。
// 顶点缓冲被当作裸 float 数组访问：位置 = f[base+0..2]，法线 = f[base+3..5]，
// 这样 compute 可以直接读写 RenderableObject 的顶点缓冲，无需额外拷贝。
#ifndef VL_OUTLINE_SMOOTH_CONTRACT_GLSL
#define VL_OUTLINE_SMOOTH_CONTRACT_GLSL

struct SmoothParams
{
    uint vertexCount;   // 顶点数
    uint indexCount;    // 索引数
    uint strideFloats;  // 每顶点 float 数（sizeof(Vertex)/4）
    uint hashSize;      // 哈希槽数（2 的幂）
    float quantScale;   // 位置量化：key = round(pos * quantScale)
};

// 槽位里存的是整条 key，而不是只存代表顶点下标；否则同位置顶点
// 只能看到"该槽已被别人占用"，会一路线性探测到不同槽，焊接失效。
// key 0 保留为空槽，因此最终 key 强制非零。
uint OutlineKey(ivec3 q)
{
    uint h = uint(q.x) * 73856093u ^ uint(q.y) * 19349663u ^ uint(q.z) * 83492791u;
    return h == 0u ? 1u : h;
}

uint OutlineHash(ivec3 q, uint hashSize)
{
    return OutlineKey(q) & (hashSize - 1u);
}

#endif
