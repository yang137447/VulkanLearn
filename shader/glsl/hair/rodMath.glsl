#ifndef VL_ROD_MATH
#define VL_ROD_MATH

const float RodRotationLimit = 3.141492653589793;

vec4 RodMul(vec4 left, vec4 right)
{
    return vec4(left.w * right.xyz + right.w * left.xyz +
        cross(left.xyz, right.xyz), left.w * right.w - dot(left.xyz, right.xyz));
}

vec4 RodConjugate(vec4 rotation)
{
    return vec4(-rotation.xyz, rotation.w);
}

vec4 RodExp(vec3 angle)
{
    float theta = length(angle);
    float scale = theta < 1.0e-4
        ? 0.5 - theta * theta / 48.0 : sin(theta * 0.5) / theta;
    return normalize(vec4(angle * scale, cos(theta * 0.5)));
}

vec3 RodLog(vec4 rotation)
{
    rotation = normalize(rotation);
    if (rotation.w < 0.0)
        rotation = -rotation;
    float magnitude = length(rotation.xyz);
    return magnitude < 1.0e-7 ? 2.0 * rotation.xyz :
        rotation.xyz * (2.0 * atan(magnitude, rotation.w) / magnitude);
}

mat3 RodRotation(vec4 rotation)
{
    vec3 vector = rotation.xyz;
    mat3 skew = mat3(vec3(0, vector.z, -vector.y),
        vec3(-vector.z, 0, vector.x), vec3(vector.y, -vector.x, 0));
    return mat3(1) + 2.0 * rotation.w * skew + 2.0 * skew * skew;
}

mat3 RodSkew(vec3 vector)
{
    return mat3(vec3(0, vector.z, -vector.y),
        vec3(-vector.z, 0, vector.x), vec3(vector.y, -vector.x, 0));
}

mat3 RodDiagonal(vec3 diagonal)
{
    return mat3(vec3(diagonal.x, 0, 0), vec3(0, diagonal.y, 0),
        vec3(0, 0, diagonal.z));
}

mat3 RodJrInverse(vec3 angle)
{
    float squared = dot(angle, angle);
    float theta = sqrt(squared);
    float coefficient = squared < 1.0e-4 ?
        1.0 / 12.0 + squared / 720.0 + squared * squared / 30240.0 :
        (1.0 - 0.5 * theta / tan(0.5 * theta)) / squared;
    mat3 skew = RodSkew(angle);
    return mat3(1) + 0.5 * skew + coefficient * skew * skew;
}

bool RodFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool RodSolve(mat3 matrix, vec3 rhs, out vec3 solution)
{
    float scale = max(max(length(matrix[0]), length(matrix[1])), length(matrix[2]));
    if (!(scale > 0.0) || isnan(scale) || isinf(scale))
        return false;
    matrix /= scale;
    float determinantValue = determinant(matrix);
    if (!(abs(determinantValue) > 1.0e-20))
        return false;
    solution = inverse(matrix) * (rhs / scale);
    return RodFinite(solution);
}

vec4 RodFromTo(vec3 first, vec3 last)
{
    float cosine = dot(first, last);
    if (cosine < -0.999999)
    {
        vec3 reference = abs(first.x) < 0.8 ? vec3(1, 0, 0) : vec3(0, 1, 0);
        return vec4(normalize(cross(first, reference)), 0);
    }
    return normalize(vec4(cross(first, last), 1.0 + cosine));
}

mat3 RodRigidRotation(mat4 transform)
{
    vec3 first = normalize(transform[0].xyz);
    vec3 second = normalize(transform[1].xyz - first * dot(first, transform[1].xyz));
    return mat3(first, second, cross(first, second));
}

#endif
