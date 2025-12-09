vec3 GetNormal_WS(mat4 modelMatrix, vec3 normal, vec3 vertexPosition)
{
    // 点差法计算世界空间下的法线
        // TODO: 以后可优化为法线矩阵
    vec3 normalOffsetPosition = vertexPosition + normal;
    vec3 transNorOffsetPos = (modelMatrix * vec4(normalOffsetPosition, 1.0)).xyz;
    vec3 transVertexPos = (modelMatrix * vec4(vertexPosition, 1.0)).xyz;
    vec3 newNormal = normalize(transNorOffsetPos - transVertexPos);
    newNormal = normalize(newNormal);
    return newNormal;
}