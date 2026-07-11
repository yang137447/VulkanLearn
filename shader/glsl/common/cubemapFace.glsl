#ifndef VL_CUBEMAP_FACE_GLSL
#define VL_CUBEMAP_FACE_GLSL

// 将 cubemap 某个 face 内的像素坐标映射为世界空间方向向量。
// pixelCoord:  当前 face 内的像素坐标
// faceSize:    cubemap 单个 face 的分辨率
// faceIndex:   0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
vec3 GetDirectionFromFace(ivec2 pixelCoord, ivec2 faceSize, int faceIndex)
{
    vec2 uv = (vec2(pixelCoord) + vec2(0.5)) / vec2(faceSize);
    uv = uv * 2.0 - 1.0;
    uv.y = -uv.y;

    vec3 dir;
    switch (faceIndex)
    {
        case 0: dir = vec3( 1.0, uv.y, -uv.x); break;
        case 1: dir = vec3(-1.0, uv.y,  uv.x); break;
        case 2: dir = vec3( uv.x, 1.0, -uv.y); break;
        case 3: dir = vec3( uv.x,-1.0,  uv.y); break;
        case 4: dir = vec3( uv.x, uv.y,  1.0); break;
        case 5: dir = vec3(-uv.x, uv.y, -1.0); break;
        default: dir = vec3(0.0, 0.0, 1.0); break;
    }
    return normalize(dir);
}

#endif
