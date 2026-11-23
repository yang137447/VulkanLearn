#define VL_ROD_EXTERNAL_MAIN 1
#include "rodKernel.glsl"
#include "rodV2Pcr.glsl"
#ifdef VL_ROD_V3_CR_SOLVE
#include "rodCrBand.glsl"
#endif
#ifdef VL_ROD_V2_THOMAS_SOLVE
#include "rodV2Thomas.glsl"
#endif

layout(set = 0, binding = 27, std140) uniform RodV2Controls
{
    vec4 originTolerance;
    vec4 differences;
} implicitControls;
#ifdef VL_ROD_V3_DISPLACEMENT
#include "rodV3Reference.glsl"
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
layout(set = 0, binding = 30, std430) buffer RodPositionCompensation
{
    vec4 values[];
} positionCompensation;
shared dvec3 v2AccuratePositions[32];
shared dvec3 v2AccurateOldPositions[32];
shared dvec3 v2AccurateBasePositions[32];
shared dvec3 v2AccurateTargets[32];

dmat3 RodV2AccurateRotation(vec4 quaternion)
{
    dvec4 normalizedQuaternion = normalize(dvec4(quaternion));
    dvec3 vector = normalizedQuaternion.xyz;
    dmat3 skew = dmat3(dvec3(0, vector.z, -vector.y),
        dvec3(-vector.z, 0, vector.x), dvec3(vector.y, -vector.x, 0));
    return dmat3(1) + 2.0 * normalizedQuaternion.w * skew + 2.0 * skew * skew;
}
#endif
#ifdef VL_ROD_V2_FAILURE_EXPORT
layout(set = 0, binding = 28, std430) buffer RodLateMatrices
{
    vec4 values[];
} lateMatrices;
layout(set = 0, binding = 29, std430) buffer RodLateState
{
    vec4 values[];
} lateState;
#endif

shared vec4 v2Positions[32];
shared vec4 v2Orientations[31];
shared vec4 v2BasePositions[32];
shared vec4 v2BaseOrientations[31];
shared vec3 v2OldVelocity[32];
shared vec3 v2OldOmega[31];
shared vec4 v2Metric[32];
shared float v2Merit;
shared float v2Maximum;
shared float v2ForceScale;
shared float v2TorqueScale;
shared float v2LengthScale;
#ifdef VL_ROD_V3_COMPENSATED_STATE
#include "rodCompensatedState.glsl"
layout(set = 0, binding = 30, std430) buffer RodLowParts
{
    vec4 values[];
} compensatedState;
shared vec3 v3PositionLow[32];
shared vec3 v3CandidateLow[32];
shared vec3 v3BaseLow[32];
shared vec3 v3OldLow[32];
shared vec4 v3QuaternionLow[31];
shared vec4 v3CandidateQuaternionLow[31];
shared vec4 v3BaseQuaternionLow[31];
shared vec4 v3OldQuaternionLow[31];
RodV2Matrix RodV2Tangent(uint row, uint column);
#endif
shared uint v2Iterations;
shared uint v2Accepted;
shared float v2Alpha;
shared uint v2DomainValid;
shared uint v3ContinuationUsed;
shared uint v3CompletedLoadStages;
shared uint v3BacktrackCount;
shared float v3CubicStiffness[32];

// 牛顿更新预算。无约束的 6 分量系统 12 轮足够。带上接触活跃集后逐轮重选活跃集、
// 建立接触力需要更多轮：16-guide 胶囊场景实测 24 轮仍有 1/16 guide 停在 1.6 mm 穿透，
// 64 轮 16/16 收敛到 4.3e-5。预算只在难帧上真正用满（稳态 0 轮即退出）。
#ifdef VL_ROD_V3_KKT10_SOLVE
const uint RodV2IterationBudget = 64u;
#else
const uint RodV2IterationBudget = 12u;
#endif

mat3 RodV2JrInverse(vec3 angle)
{
    float squared = dot(angle, angle);
    float magnitude = sqrt(squared);
    float coefficient = magnitude < 0.1 ?
        1.0 / 12.0 + squared / 720.0 + squared * squared / 30240.0 +
            squared * squared * squared / 1209600.0 :
        (1.0 - 0.5 * magnitude / tan(0.5 * magnitude)) / squared;
    mat3 skew = RodSkew(angle);
    return mat3(1) + 0.5 * skew + coefficient * skew * skew;
}

vec3 RodV2Position(uint point, int changed, uint component, float epsilon)
{
    vec3 position = v2Positions[point].xyz;
    if (int(point) == changed && component < 3u)
        position[component] += epsilon;
    return position;
}

vec4 RodV2Orientation(uint segment, int changed, uint component, float epsilon)
{
    vec4 orientation = v2Orientations[segment];
    if (int(segment) == changed && component >= 3u)
    {
        vec3 angle = vec3(0);
        angle[component - 3u] = epsilon;
        orientation = normalize(RodMul(orientation, RodExp(angle)));
    }
    return orientation;
}

mat3 RodV2PhysicalRotation(uint segment, vec4 orientation)
{
#ifdef VL_ROD_V3_DISPLACEMENT
    return RodRotation(displacementReferences.guides[gl_WorkGroupID.x].frames[segment]) *
        RodRotation(orientation);
#else
    return RodRotation(orientation);
#endif
}

vec4 RodV2RelativeJoint(uint joint, vec4 first, vec4 last)
{
#ifdef VL_ROD_V3_DISPLACEMENT
    vec4 authoredRelative = RodMul(RodConjugate(restOrientations[joint]), restOrientations[joint + 1u]);
    return RodMul(RodMul(RodConjugate(first), authoredRelative), last);
#else
    return RodMul(RodConjugate(first), last);
#endif
}

vec3 RodV2StableShear(uint segment, vec4 orientation, vec3 displacementDifference)
{
#ifdef VL_ROD_V3_DISPLACEMENT
    mat3 restRotation = RodRotation(displacementReferences.guides[gl_WorkGroupID.x].frames[segment]);
    mat3 skew = RodSkew(orientation.xyz);
    mat3 inverseDeltaMinusIdentity = -2.0 * orientation.w * skew + 2.0 * skew * skew;
    vec3 materialDisplacement = transpose(restRotation) * displacementDifference /
        restStrains[segment].w;
    return inverseDeltaMinusIdentity * restStrains[segment].xyz +
        transpose(RodRotation(orientation)) * materialDisplacement;
#else
    return transpose(RodRotation(orientation)) * displacementDifference /
        restStrains[segment].w - restStrains[segment].xyz;
#endif
}

RodV2Vector RodV2Elastic(uint row, int changed, uint component, float epsilon)
{
    RodV2Vector result = RodV2Vector(vec3(0), vec3(0));
#ifdef VL_ROD_V2_MIXED_GEOMETRY
    dvec3 accurateForce = dvec3(0);
    dvec3 accurateTorque = dvec3(0);
#endif
    uint begin = row == 0u ? 0u : row - 1u;
    uint end = min(row + 1u, 31u);
    for (uint segment = begin; segment < end; ++segment)
    {
        vec4 orientation = RodV2Orientation(segment, changed, component, epsilon);
        mat3 rotation = RodV2PhysicalRotation(segment, orientation);
        vec3 delta = RodV2Position(segment + 1u, changed, component, epsilon) -
            RodV2Position(segment, changed, component, epsilon);
        vec3 constraint = RodV2StableShear(segment, orientation, delta);
        vec3 strain = restStrains[segment].xyz + constraint;
        vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[segment].shearCompliance.xyz;
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        dmat3 accurateRotation = RodV2AccurateRotation(orientation);
        dvec3 accurateStrain = transpose(accurateRotation) *
            (v2AccuratePositions[segment + 1u] - v2AccuratePositions[segment]) /
            double(restStrains[segment].w);
        dvec3 accurateResultant = (accurateStrain - dvec3(restStrains[segment].xyz)) /
            (dvec3(compliance) * double(restStrains[segment].w));
        dvec3 force = accurateRotation * accurateResultant;
        accurateForce += segment == row ? force : -force;
        if (segment == row)
            accurateTorque += double(restStrains[segment].w) *
                cross(accurateStrain, accurateResultant);
#else
        vec3 resultant = constraint /
            (compliance * restStrains[segment].w);
        vec3 force = rotation * resultant;
        result.linear += segment == row ? force : -force;
        if (segment == row)
            result.angular += restStrains[segment].w * cross(strain, resultant);
#endif
    }
    if (row < 31u)
        for (uint joint = begin; joint < min(row + 1u, 30u); ++joint)
        {
            vec4 left = RodV2Orientation(joint, changed, component, epsilon);
            vec4 right = RodV2Orientation(joint + 1u, changed, component, epsilon);
            vec4 relative = RodV2RelativeJoint(joint, left, right);
            vec3 angle = RodLog(relative);
            vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[joint].bendCompliance.xyz;
            vec3 moment = transpose(RodV2JrInverse(angle)) * ((angle - restJoints[joint].xyz) / compliance);
#ifdef VL_ROD_V2_MIXED_GEOMETRY
            accurateTorque += dvec3(joint == row ? RodRotation(relative) * moment : -moment);
#else
            result.angular += joint == row ? RodRotation(relative) * moment : -moment;
#endif
        }
#ifdef VL_ROD_V2_MIXED_GEOMETRY
    result.linear = vec3(accurateForce);
    result.angular = vec3(accurateTorque);
#endif
    return result;
}

RodV2Vector RodV2ElasticDerivative(uint row, uint column, uint component)
{
    RodV2Vector result = RodV2Vector(vec3(0), vec3(0));
    vec3 basis = vec3(0);
    basis[component % 3u] = 1.0;
    uint begin = row == 0u ? 0u : row - 1u;
    for (uint segment = begin; segment < min(row + 1u, 31u); ++segment)
    {
        mat3 rotation = RodV2PhysicalRotation(segment, v2Orientations[segment]);
        float restLength = restStrains[segment].w;
        vec3 constraint = RodV2StableShear(segment, v2Orientations[segment],
            v2Positions[segment + 1u].xyz - v2Positions[segment].xyz);
        vec3 strain = restStrains[segment].xyz + constraint;
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        rotation = mat3(RodV2AccurateRotation(v2Orientations[segment]));
        strain = vec3(transpose(RodV2AccurateRotation(v2Orientations[segment])) *
            (v2AccuratePositions[segment + 1u] - v2AccuratePositions[segment]) / double(restLength));
#endif
        vec3 stiffness = 1.0 /
            (rodState.guides[gl_WorkGroupID.x].segments[segment].shearCompliance.xyz * restLength);
        vec3 stress = stiffness * constraint;
        vec3 edgeDelta = component < 3u ?
            (column == segment ? -basis : column == segment + 1u ? basis : vec3(0)) : vec3(0);
        vec3 angleDelta = component >= 3u && column == segment ? basis : vec3(0);
        vec3 strainDelta = transpose(rotation) * edgeDelta / restLength + cross(strain, angleDelta);
        vec3 stressDelta = stiffness * strainDelta;
        vec3 forceDelta = rotation * (cross(angleDelta, stress) + stressDelta);
        result.linear += segment == row ? forceDelta : -forceDelta;
        if (segment == row)
            result.angular += restLength *
                (cross(strainDelta, stress) + cross(strain, stressDelta));
    }
    if (row < 31u && component >= 3u)
        for (uint joint = begin; joint < min(row + 1u, 30u); ++joint)
        {
            vec4 quaternion = RodV2RelativeJoint(joint, v2Orientations[joint], v2Orientations[joint + 1u]);
            mat3 relative = RodRotation(quaternion);
            vec3 angle = RodLog(quaternion);
            mat3 inverseJacobian = RodV2JrInverse(angle);
            mat3 skew = RodSkew(angle);
            float squared = dot(angle, angle);
            float magnitude = sqrt(squared);
            float coefficient;
            float derivative;
            if (magnitude < 0.1)
            {
                coefficient = 1.0 / 12.0 + squared / 720.0 +
                    squared * squared / 30240.0 + squared * squared * squared / 1209600.0;
                derivative = 1.0 / 360.0 + squared / 7560.0 + squared * squared / 201600.0;
            }
            else
            {
                float cotangent = 1.0 / tan(0.5 * magnitude);
                float numerator = 1.0 - 0.5 * magnitude * cotangent;
                float numeratorDerivative = -0.5 * cotangent +
                    0.25 * magnitude * (1.0 + cotangent * cotangent);
                coefficient = numerator / squared;
                derivative = numeratorDerivative / (squared * magnitude) -
                    2.0 * numerator / (squared * squared);
            }
            vec3 relativeDelta = column == joint ? -transpose(relative) * basis :
                column == joint + 1u ? basis : vec3(0);
            vec3 angleDelta = inverseJacobian * relativeDelta;
            mat3 skewDelta = RodSkew(angleDelta);
            mat3 jacobianDelta = 0.5 * skewDelta +
                coefficient * (skewDelta * skew + skew * skewDelta) +
                derivative * dot(angle, angleDelta) * skew * skew;
            vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[joint].bendCompliance.xyz;
            vec3 stress = (angle - restJoints[joint].xyz) / compliance;
            vec3 dual = transpose(inverseJacobian) * stress;
            vec3 dualDelta = transpose(jacobianDelta) * stress +
                transpose(inverseJacobian) * (angleDelta / compliance);
            result.angular += joint == row ?
                relative * (cross(relativeDelta, dual) + dualDelta) : -dualDelta;
        }
    return result;
}

#ifdef VL_ROD_V3_COMPENSATED_STATE
RodV2Vector RodV3LowElastic(uint row)
{
    RodV2Vector result = RodV2Vector(vec3(0), vec3(0));
    uint begin = row == 0u ? 0u : row - 1u;
    for (uint segment = begin; segment < min(row + 1u, 31u); ++segment)
    {
        mat3 rotation = RodV2PhysicalRotation(segment, v2Orientations[segment]);
        float restLength = restStrains[segment].w;
        vec3 constraint = RodV2StableShear(segment, v2Orientations[segment],
            v2Positions[segment + 1u].xyz - v2Positions[segment].xyz);
        vec3 strain = restStrains[segment].xyz + constraint;
        vec3 stiffness = 1.0 /
            (rodState.guides[gl_WorkGroupID.x].segments[segment].shearCompliance.xyz * restLength);
        vec3 stress = stiffness * constraint;
        vec3 angleDelta = segment == 0u ? vec3(0) :
            RodCompensatedBodyOffset(v2Orientations[segment], v3CandidateQuaternionLow[segment]);
        vec3 edgeDelta = (positions[segment + 1u].w == 0.0 ? vec3(0) : v3CandidateLow[segment + 1u]) -
            (positions[segment].w == 0.0 ? vec3(0) : v3CandidateLow[segment]);
        vec3 strainDelta = transpose(rotation) * edgeDelta / restLength + cross(strain, angleDelta);
        vec3 stressDelta = stiffness * strainDelta;
        vec3 forceDelta = rotation * (cross(angleDelta, stress) + stressDelta);
        result.linear += segment == row ? forceDelta : -forceDelta;
        if (segment == row)
            result.angular += restLength *
                (cross(strainDelta, stress) + cross(strain, stressDelta));
    }
    if (row < 31u)
        for (uint joint = begin; joint < min(row + 1u, 30u); ++joint)
        {
            vec4 quaternion = RodV2RelativeJoint(joint, v2Orientations[joint], v2Orientations[joint + 1u]);
            mat3 relative = RodRotation(quaternion);
            vec3 angle = RodLog(quaternion);
            mat3 inverseJacobian = RodV2JrInverse(angle);
            mat3 skew = RodSkew(angle);
            float squared = dot(angle, angle);
            float magnitude = sqrt(squared);
            float coefficient, derivative;
            if (magnitude < 0.1)
            {
                coefficient = 1.0 / 12.0 + squared / 720.0 +
                    squared * squared / 30240.0 + squared * squared * squared / 1209600.0;
                derivative = 1.0 / 360.0 + squared / 7560.0 + squared * squared / 201600.0;
            }
            else
            {
                float cotangent = 1.0 / tan(0.5 * magnitude);
                float numerator = 1.0 - 0.5 * magnitude * cotangent;
                float numeratorDerivative = -0.5 * cotangent +
                    0.25 * magnitude * (1.0 + cotangent * cotangent);
                coefficient = numerator / squared;
                derivative = numeratorDerivative / (squared * magnitude) -
                    2.0 * numerator / (squared * squared);
            }
            vec3 leftOffset = joint == 0u ? vec3(0) :
                RodCompensatedBodyOffset(v2Orientations[joint], v3CandidateQuaternionLow[joint]);
            vec3 rightOffset = RodCompensatedBodyOffset(v2Orientations[joint + 1u],
                v3CandidateQuaternionLow[joint + 1u]);
            vec3 relativeDelta = rightOffset - transpose(relative) * leftOffset;
            vec3 angleDelta = inverseJacobian * relativeDelta;
            mat3 skewDelta = RodSkew(angleDelta);
            mat3 jacobianDelta = 0.5 * skewDelta +
                coefficient * (skewDelta * skew + skew * skewDelta) +
                derivative * dot(angle, angleDelta) * skew * skew;
            vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[joint].bendCompliance.xyz;
            vec3 stress = (angle - restJoints[joint].xyz) / compliance;
            vec3 dual = transpose(inverseJacobian) * stress;
            vec3 dualDelta = transpose(jacobianDelta) * stress +
                transpose(inverseJacobian) * (angleDelta / compliance);
            result.angular += joint == row ?
                relative * (cross(relativeDelta, dual) + dualDelta) : -dualDelta;
        }
    return result;
}
#endif

RodV2Vector RodV2Residual(uint row)
{
    float step = rod.gravityStep.w;
    RodV2Vector elastic = RodV2Elastic(row, -1, 0u, 0.0);
    vec3 displacement = v2Positions[row].xyz - oldPositions[row].xyz;
    RodV2Vector residual;
    residual.linear = (masses[row].x * (displacement - step * v2OldVelocity[row]) / (step * step) +
        masses[row].z * (displacement / step - targetVelocities[row]) -
        elastic.linear + masses[row].y * (v2Positions[row].xyz - targets[row]) -
        masses[row].x * externalAcceleration[row]) / v2ForceScale;
    residual.angular = vec3(0);
#ifdef VL_ROD_V2_MIXED_GEOMETRY
    dvec3 accurateDisplacement = v2AccuratePositions[row] - v2AccurateOldPositions[row];
    dvec3 supportDisplacementAccurate = v2AccuratePositions[row] - v2AccurateTargets[row];
    dvec3 accurateResidual = double(masses[row].x) *
        (accurateDisplacement - double(step) * dvec3(v2OldVelocity[row])) / double(step * step) +
        double(masses[row].z) * (accurateDisplacement / double(step) - dvec3(targetVelocities[row])) -
        dvec3(elastic.linear) + double(masses[row].y) * supportDisplacementAccurate -
        double(masses[row].x) * dvec3(externalAcceleration[row]);
    residual.linear = vec3(accurateResidual / double(v2ForceScale));
#endif
#ifdef VL_ROD_V3_PROGRESSIVE
#ifdef VL_ROD_V2_MIXED_GEOMETRY
    residual.linear += vec3(double(v3CubicStiffness[row]) *
        dot(supportDisplacementAccurate, supportDisplacementAccurate) *
        supportDisplacementAccurate / double(v2ForceScale));
#else
    vec3 supportDisplacement = v2Positions[row].xyz - targets[row];
    residual.linear += v3CubicStiffness[row] * dot(supportDisplacement, supportDisplacement) *
        supportDisplacement / v2ForceScale;
#endif
#endif
    if (row < 31u)
    {
        vec3 angle = RodLog(RodMul(RodConjugate(oldOrientations[row]), v2Orientations[row]));
        vec3 transported = transpose(RodRotation(v2Orientations[row])) *
            RodRotation(oldOrientations[row]) * (inertias[row].xyz * v2OldOmega[row]);
        residual.angular = (inertias[row].xyz * angle / (step * step) -
            transported / step - elastic.angular) / v2TorqueScale;
    }
    if (positions[row].w == 0.0)
        residual.linear = vec3(0);
    if (row == 0u || row == 31u)
        residual.angular = vec3(0);
#ifdef VL_ROD_V3_COMPENSATED_STATE
#ifdef VL_ROD_V3_LOW_DIRECTIONAL
    RodV2Vector lowElastic = RodV3LowElastic(row);
    vec3 low = v3CandidateLow[row];
    vec3 support = v2Positions[row].xyz - targets[row];
    residual.linear += ((masses[row].x / (step * step) + masses[row].z / step + masses[row].y) * low +
        v3CubicStiffness[row] * (dot(support, support) * low +
            2.0 * support * dot(support, low)) - lowElastic.linear) / v2ForceScale;
    if (row > 0u && row < 31u)
    {
        vec3 angle = RodLog(RodMul(RodConjugate(oldOrientations[row]), v2Orientations[row]));
        vec3 transported = transpose(RodRotation(v2Orientations[row])) *
            RodRotation(oldOrientations[row]) * (inertias[row].xyz * v2OldOmega[row]);
        vec3 bodyOffset = RodCompensatedBodyOffset(v2Orientations[row], v3CandidateQuaternionLow[row]);
        residual.angular += (inertias[row].xyz * (RodV2JrInverse(angle) * bodyOffset) /
            (step * step) + cross(bodyOffset, transported) / step - lowElastic.angular) / v2TorqueScale;
    }
#else
    for (uint column = row == 0u ? 0u : row - 1u; column < min(row + 2u, 32u); ++column)
    {
        RodV2Vector offset;
        offset.linear = positions[column].w == 0.0 ? vec3(0) :
            v3CandidateLow[column] / v2LengthScale;
        offset.angular = column == 0u || column == 31u ? vec3(0) :
            RodCompensatedBodyOffset(v2Orientations[column], v3CandidateQuaternionLow[column]);
        residual = RodV2Add(residual, RodV2Multiply(RodV2Tangent(row, column), offset));
    }
#endif
    if (positions[row].w != 0.0)
        residual.linear -= masses[row].x * v3OldLow[row] /
            (step * step * v2ForceScale);
    if (row > 0u && row < 31u)
    {
        vec3 oldOffset = RodCompensatedBodyOffset(oldOrientations[row], v3OldQuaternionLow[row]);
        vec4 relative = RodMul(RodConjugate(oldOrientations[row]), v2Orientations[row]);
        mat3 transport = transpose(RodRotation(relative));
        residual.angular -= (inertias[row].xyz *
            (RodV2JrInverse(RodLog(relative)) * transport * oldOffset) / (step * step) +
            transport * cross(oldOffset, inertias[row].xyz * v2OldOmega[row]) / step) / v2TorqueScale;
    }
    if (positions[row].w == 0.0)
        residual.linear = vec3(0);
    if (row == 0u || row == 31u)
        residual.angular = vec3(0);
#endif
    return residual;
}

bool RodV2Fixed(uint row, uint component)
{
    return component < 3u ? positions[row].w == 0.0 : row == 0u || row == 31u;
}

RodV2Matrix RodV2Tangent(uint row, uint column)
{
    RodV2Matrix matrix = RodV2Zero();
    for (uint component = 0u; component < 6u; ++component)
    {
        if (RodV2Fixed(column, component))
            continue;
        vec3 linear;
        vec3 angular;
#ifdef VL_ROD_V2_ANALYTIC
        RodV2Vector derivative = RodV2ElasticDerivative(row, column, component);
        linear = -derivative.linear / v2ForceScale;
        angular = -derivative.angular / v2TorqueScale;
#else
        float epsilon = component < 3u ?
            implicitControls.differences.x * v2LengthScale : implicitControls.differences.y;
        RodV2Vector positive = RodV2Elastic(row, int(column), component, epsilon);
        RodV2Vector negative = RodV2Elastic(row, int(column), component, -epsilon);
        linear = -(positive.linear - negative.linear) /
            (2.0 * epsilon * v2ForceScale);
        angular = -(positive.angular - negative.angular) /
            (2.0 * epsilon * v2TorqueScale);
#endif
        if (component < 3u)
        {
            linear *= v2LengthScale;
            angular *= v2LengthScale;
        }
        for (uint entry = 0u; entry < 3u; ++entry)
        {
            RodV2Set(matrix, entry, component, linear[entry]);
            RodV2Set(matrix, entry + 3u, component, angular[entry]);
        }
    }
    if (row == column)
    {
        float step = rod.gravityStep.w;
        matrix.linearLinear += mat3((masses[row].x / (step * step) +
            masses[row].z / step + masses[row].y) * v2LengthScale / v2ForceScale);
#ifdef VL_ROD_V3_PROGRESSIVE
        vec3 supportDisplacement = v2Positions[row].xyz - targets[row];
        matrix.linearLinear += (mat3(v3CubicStiffness[row] *
            dot(supportDisplacement, supportDisplacement)) +
            2.0 * v3CubicStiffness[row] * outerProduct(supportDisplacement, supportDisplacement)) *
            (v2LengthScale / v2ForceScale);
#endif
        if (row < 31u)
        {
            vec3 angle = RodLog(RodMul(RodConjugate(oldOrientations[row]), v2Orientations[row]));
            vec3 transported = transpose(RodRotation(v2Orientations[row])) *
                RodRotation(oldOrientations[row]) * (inertias[row].xyz * v2OldOmega[row]);
            matrix.angularAngular += (RodDiagonal(inertias[row].xyz) *
                RodV2JrInverse(angle) / (step * step) - RodSkew(transported) / step) /
                v2TorqueScale;
        }
    }
    for (uint component = 0u; component < 6u; ++component)
    {
        if (RodV2Fixed(row, component))
            for (uint entry = 0u; entry < 6u; ++entry)
                RodV2Set(matrix, component, entry, row == column && component == entry ? 1.0 : 0.0);
        if (RodV2Fixed(column, component))
            for (uint entry = 0u; entry < 6u; ++entry)
                RodV2Set(matrix, entry, component, row == column && component == entry ? 1.0 : 0.0);
    }
    return matrix;
}

void RodV2Measure(uint lane, bool incrementValid)
{
    RodV2Vector residual = RodV2Residual(lane);
    bool valid = incrementValid && RodFinite(v2Positions[lane].xyz) &&
        RodFinite(residual.linear) && RodFinite(residual.angular);
    if (lane < 31u)
    {
        valid = valid && length(RodLog(RodMul(RodConjugate(oldOrientations[lane]),
            v2Orientations[lane]))) < RodRotationLimit;
        if (lane < 30u)
            valid = valid && length(RodLog(RodV2RelativeJoint(lane, v2Orientations[lane],
                v2Orientations[lane + 1u]))) < RodRotationLimit;
    }
    v2Metric[lane] = vec4(dot(residual.linear, residual.linear) + dot(residual.angular, residual.angular),
        max(max(max(abs(residual.linear.x), abs(residual.linear.y)), abs(residual.linear.z)),
            max(max(abs(residual.angular.x), abs(residual.angular.y)), abs(residual.angular.z))),
            0, valid ? 1.0 : 0.0);
    barrier();
    if (lane == 0u)
    {
        v2Merit = 0.0;
        v2Maximum = 0.0;
        v2DomainValid = 1u;
        for (uint point = 0u; point < 32u; ++point)
        {
            v2Merit += v2Metric[point].x;
            v2Maximum = max(v2Maximum, v2Metric[point].y);
            if (v2Metric[point].w == 0.0)
                v2DomainValid = 0u;
        }
        if (isnan(v2Merit) || isinf(v2Merit))
            v2DomainValid = 0u;
    }
    barrier();
}

#ifdef VL_ROD_V3_KKT10_SOLVE
// 10 分量活跃集 KKT 求解。必须放在 RodV2Tangent / RodV2Residual / RodV2Measure 之后：
// 装配要复用同一套切线，测量要复用同一套 metric 共享量。
#include "rodKkt10BandSolve.glsl"
#endif

// 单一测量入口：带接触 / 上拉伸约束时用 10 分量 KKT merit，否则沿用 6 分量 merit。
void RodActiveMeasure(uint lane, bool incrementValid)
{
#ifdef VL_ROD_V3_KKT10_SOLVE
    RodKkt10Measure(lane, incrementValid);
#else
    RodV2Measure(lane, incrementValid);
#endif
}

#ifdef VL_ROD_V2_FAILURE_EXPORT
void RodV2ExportLate(uint lane, uint slot)
{
#ifdef VL_ROD_RUNTIME_FAILURE_EXPORT
    if (failure == 0u || implicitControls.differences.w == 0.0 ||
        slot != uint(implicitControls.differences.z))
        return;
    slot = 0u;
#endif
    uint matrixOffset = slot * 932u + lane * 29u;
    RodV2Matrix bands[3];
    bands[0] = lane > 0u ? RodV2Tangent(lane, lane - 1u) : RodV2Zero();
    bands[1] = RodV2Tangent(lane, lane);
    bands[2] = lane < 31u ? RodV2Tangent(lane, lane + 1u) : RodV2Zero();
    for (uint band = 0u; band < 3u; ++band)
        for (uint entry = 0u; entry < 36u; ++entry)
            lateMatrices.values[matrixOffset + band * 9u + entry / 4u][entry % 4u] =
                RodV2Entry(bands[band], entry / 6u, entry % 6u);
    RodV2Vector residual = RodV2Residual(lane);
    lateMatrices.values[matrixOffset + 27u] = vec4(-residual.linear, 0);
    lateMatrices.values[matrixOffset + 28u] = vec4(-residual.angular, 0);
    uint stateOffset = slot * 416u;
    lateState.values[stateOffset + lane] = v2Positions[lane];
    lateState.values[stateOffset + 63u + lane] = oldPositions[lane];
    lateState.values[stateOffset + 126u + lane] = vec4(v2OldVelocity[lane], 0);
    lateState.values[stateOffset + 189u + lane] = vec4(targets[lane], 0);
    lateState.values[stateOffset + 221u + lane] = vec4(targetVelocities[lane], 0);
    lateState.values[stateOffset + 253u + lane] = vec4(externalAcceleration[lane], 0);
    lateState.values[stateOffset + 285u + lane * 2u] = vec4(residual.linear, 0);
    lateState.values[stateOffset + 285u + lane * 2u + 1u] = vec4(residual.angular, 0);
    lateState.values[stateOffset + 353u + lane] = v2BasePositions[lane];
    if (lane < 31u)
    {
        lateState.values[stateOffset + 32u + lane] = v2Orientations[lane];
        lateState.values[stateOffset + 95u + lane] = oldOrientations[lane];
        lateState.values[stateOffset + 158u + lane] = vec4(v2OldOmega[lane], 0);
        lateState.values[stateOffset + 385u + lane] = v2BaseOrientations[lane];
    }
    if (lane == 0u)
    {
        vec4 scales = vec4(v2LengthScale, v2ForceScale, v2TorqueScale, rod.gravityStep.w);
        vec4 diagnostic = vec4(float(failure), v2Maximum, v2Merit, float(v2Iterations));
        lateMatrices.values[slot * 932u + 928u] = scales;
        lateMatrices.values[slot * 932u + 929u] = implicitControls.originTolerance;
        lateMatrices.values[slot * 932u + 930u] = rod.gravityStep;
        lateMatrices.values[slot * 932u + 931u] = diagnostic;
        lateState.values[stateOffset + 349u] = scales;
        lateState.values[stateOffset + 350u] = implicitControls.originTolerance;
        lateState.values[stateOffset + 351u] = rod.gravityStep;
        lateState.values[stateOffset + 352u] = diagnostic;
    }
}
#endif

#ifdef VL_ROD_V3_DISPLACEMENT
void RodV3InitializeReference(uint slot, uint guide, uint lane)
{
    mat4 root = RodRootMatrix(guide, 0u);
    mat3 sourceRotation = RodRigidRotation(root);
    vec4 sourceQuaternion = TressfxQuatFromMatrix(mat4(sourceRotation));
    if (lane < 31u)
    {
        vec4 frame = normalize(RodMul(sourceQuaternion, restOrientations[lane]));
        displacementReferences.guides[slot].frames[lane] = frame;
        displacementReferences.guides[slot].edges[lane] =
            vec4(RodRotation(frame) * restStrains[lane].xyz * restStrains[lane].w,
                restStrains[lane].w);
    }
    if (lane == 0u)
    {
        displacementReferences.guides[slot].rootAnchor = vec4(
            ((root * vec4(restPositions.positions[guide * 32u].xyz, 1)).xyz -
                implicitControls.originTolerance.xyz * 100.0) * .01, 0);
        displacementReferences.guides[slot].rootRotation = sourceQuaternion;
    }
    memoryBarrierBuffer();
    barrier();
    vec3 referencePosition = displacementReferences.guides[slot].rootAnchor.xyz;
    for (uint segment = 0u; segment < lane; ++segment)
        referencePosition += displacementReferences.guides[slot].edges[segment].xyz;
    displacementReferences.guides[slot].positions[lane] = vec4(referencePosition, 1);
    rootLocals[lane].xyz = transpose(RodRotation(displacementReferences.guides[slot].frames[0])) *
        (referencePosition - displacementReferences.guides[slot].rootAnchor.xyz);
    rodState.guides[slot].points[lane].rootLocal = rootLocals[lane];
    positions[lane].xyz = vec3(0);
    if (lane < 31u)
        orientations[lane] = vec4(0, 0, 0, 1);
    memoryBarrierBuffer();
    barrier();
}
#endif

#ifndef VL_ROD_V2_EXTERNAL_MAIN
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uint slot = gl_WorkGroupID.x;
    if (slot >= rod.counts.x)
        return;
    uint guide = rod.flags.y != 0u ? simulationGuides.indices[slot] : slot;
    if (lane == 0u)
    {
        failure = rod.flags.x != 0u ? 0u : uint(rodState.guides[slot].diagnostic.x);
        v2Iterations = 0u;
        v2Maximum = 0.0;
        v2Merit = 0.0;
        v3ContinuationUsed = 0u;
        v3CompletedLoadStages = 0u;
        v3BacktrackCount = 0u;
        simulatedSeconds = rod.flags.x != 0u ? 0.0 : rodState.guides[slot].timing.x;
    }
    barrier();
    if (failure != 0u && rod.flags.x == 0u)
        return;
    if (rod.flags.x != 0u)
        RodInitialize(slot, guide, lane);
    else
    {
        positions[lane] = rodState.guides[slot].points[lane].position;
        velocities[lane] = rodState.guides[slot].points[lane].velocity;
        rootLocals[lane] = rodState.guides[slot].points[lane].rootLocal;
        masses[lane] = rodState.guides[slot].points[lane].massSupport;
        if (lane < 31u)
        {
            orientations[lane] = rodState.guides[slot].segments[lane].orientation;
            angularVelocities[lane] = rodState.guides[slot].segments[lane].angularVelocity;
            restOrientations[lane] = rodState.guides[slot].segments[lane].restOrientation;
            restStrains[lane] = rodState.guides[slot].segments[lane].restStrainLength;
            restJoints[lane] = rodState.guides[slot].segments[lane].restJointLength;
            inertias[lane] = rodState.guides[slot].segments[lane].inertia;
        }
    }
    barrier();
    if (rod.flags.x != 0u)
    {
        mat4 root = RodRootMatrix(guide, 0u);
        mat3 rotation = RodRigidRotation(root);
        vec3 anchor = ((root * vec4(restPositions.positions[guide * 32u].xyz, 1)).xyz -
            implicitControls.originTolerance.xyz * 100.0) * .01;
        positions[lane].xyz = anchor + rotation *
            (restPositions.positions[guide * 32u + lane].xyz -
                restPositions.positions[guide * 32u].xyz) * .01;
        if (lane < 31u)
        {
            orientations[lane] = RodMul(TressfxQuatFromMatrix(mat4(rotation)), restOrientations[lane]);
            angularVelocities[lane] = vec4(0);
        }
    }
#ifdef VL_ROD_V2_MIXED_GEOMETRY
    if (rod.flags.x != 0u)
    {
        dmat4 root = dmat4(RodRootMatrix(guide, 0u));
        dvec3 authoredRoot = dvec3(restPositions.positions[guide * 32u].xyz);
        dvec3 anchor = (root * dvec4(authoredRoot, 1)).xyz * .01 -
            dvec3(implicitControls.originTolerance.xyz);
        v2AccuratePositions[lane] = anchor + dmat3(root) *
            (dvec3(restPositions.positions[guide * 32u + lane].xyz) - authoredRoot) * .01;
    }
    else
        v2AccuratePositions[lane] = dvec3(positions[lane].xyz) +
            dvec3(positionCompensation.values[slot * 32u + lane].xyz);
    positions[lane].xyz = vec3(v2AccuratePositions[lane]);
#endif
#ifdef VL_ROD_V3_DISPLACEMENT
    if (rod.flags.x != 0u)
        RodV3InitializeReference(slot, guide, lane);
    else if (lane < 31u)
        orientations[lane] = frameDeltas.values[slot * 31u + lane];
#endif
    barrier();
#ifdef VL_ROD_V3_COMPENSATED_STATE
    v3PositionLow[lane] = rod.flags.x != 0u ? vec3(0) :
        compensatedState.values[slot * 63u + lane].xyz;
    if (lane < 31u)
        v3QuaternionLow[lane] = rod.flags.x != 0u ? vec4(0) :
            compensatedState.values[slot * 63u + 32u + lane];
    barrier();
#endif
    if (lane == 0u)
    {
        float mass = 0.0;
        float stiffness = 0.0;
        v2LengthScale = 0.0;
        for (uint point = 0u; point < 32u; ++point)
        {
            mass += masses[point].x;
            stiffness = max(stiffness, masses[point].y);
            if (point < 31u)
                v2LengthScale += restStrains[point].w;
        }
        v2ForceScale = mass * 9.8 + stiffness * v2LengthScale;
        v2TorqueScale = v2ForceScale * v2LengthScale;
    }
    barrier();
    float designDisplacement = .08 * v2LengthScale;
    v3CubicStiffness[lane] = positions[lane].w == 0.0 ? 0.0 :
        max(0.0, (masses[lane].x * rod.windDrag.w * 400.0 -
            masses[lane].y * designDisplacement) /
            (designDisplacement * designDisplacement * designDisplacement));
    barrier();
    for (uint substep = 0u; substep < rod.counts.y; ++substep)
    {
        if (failure != 0u)
            break;
        float step = rod.gravityStep.w;
        oldPositions[lane] = positions[lane];
#ifdef VL_ROD_V3_COMPENSATED_STATE
        v3OldLow[lane] = v3PositionLow[lane];
        v3CandidateLow[lane] = positions[lane].w == 0.0 ? vec3(0) : v3PositionLow[lane];
        if (lane < 31u)
        {
            v3OldQuaternionLow[lane] = v3QuaternionLow[lane];
            v3CandidateQuaternionLow[lane] = lane == 0u ? vec4(0) : v3QuaternionLow[lane];
        }
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        v2AccurateOldPositions[lane] = v2AccuratePositions[lane];
#endif
        v2OldVelocity[lane] = velocities[lane].xyz;
        mat4 firstRoot = RodRootMatrix(guide, substep);
        mat4 lastRoot = RodRootMatrix(guide, substep + 1u);
        mat3 firstRotation = RodRigidRotation(firstRoot);
        mat3 lastRotation = RodRigidRotation(lastRoot);
        vec3 authoredRoot = restPositions.positions[guide * 32u].xyz;
        vec3 firstAnchor = ((firstRoot * vec4(authoredRoot, 1)).xyz -
            implicitControls.originTolerance.xyz * 100.0) * .01;
        vec3 lastAnchor = ((lastRoot * vec4(authoredRoot, 1)).xyz -
            implicitControls.originTolerance.xyz * 100.0) * .01;
        vec3 offset = lastRotation * RodRotation(restOrientations[0]) * rootLocals[lane].xyz;
        vec3 omegaWorld = firstRotation * RodLog(RodMul(
            RodConjugate(TressfxQuatFromMatrix(mat4(firstRotation))),
            TressfxQuatFromMatrix(mat4(lastRotation)))) / step;
        targets[lane] = lastAnchor + offset;
        targetVelocities[lane] = (lastAnchor - firstAnchor) / step + cross(omegaWorld, offset);
#ifdef VL_ROD_V3_DISPLACEMENT
        vec4 rootPhysicalFrame = normalize(RodMul(TressfxQuatFromMatrix(mat4(lastRotation)),
            restOrientations[0]));
        vec4 initialRootFrame = displacementReferences.guides[slot].frames[0];
        vec3 anchorDelta = lastAnchor - displacementReferences.guides[slot].rootAnchor.xyz;
        targets[lane] = anchorDelta +
            (RodRotation(rootPhysicalFrame) - RodRotation(initialRootFrame)) * rootLocals[lane].xyz;
        targetVelocities[lane] = (lastAnchor - firstAnchor) / step +
            cross(omegaWorld, RodRotation(rootPhysicalFrame) * rootLocals[lane].xyz);
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        dvec3 accurateAnchor = (dmat4(lastRoot) * dvec4(dvec3(authoredRoot), 1)).xyz * .01 -
            dvec3(implicitControls.originTolerance.xyz);
        v2AccurateTargets[lane] = accurateAnchor +
            RodV2AccurateRotation(TressfxQuatFromMatrix(mat4(lastRotation))) *
            RodV2AccurateRotation(restOrientations[0]) * dvec3(rootLocals[lane].xyz);
        if (positions[lane].w == 0.0)
            v2AccuratePositions[lane] = v2AccurateTargets[lane];
#endif
        externalAcceleration[lane] = rod.gravityStep.xyz + rod.probeAcceleration.xyz;
#ifdef VL_ROD_V3_PROGRESSIVE
        vec3 tangent = lane < 31u ?
            positions[lane + 1u].xyz - positions[lane].xyz :
            positions[lane].xyz - positions[lane - 1u].xyz;
#ifdef VL_ROD_V3_DISPLACEMENT
        tangent += displacementReferences.guides[slot].edges[min(lane, 30u)].xyz;
#endif
        tangent = normalize(tangent);
        vec3 relativeAir = rod.windDrag.xyz - v2OldVelocity[lane];
        vec3 normalAir = relativeAir - tangent * dot(relativeAir, tangent);
        externalAcceleration[lane] += rod.windDrag.w * length(normalAir) * normalAir;
#endif
        v2Positions[lane] = positions[lane];
        if (positions[lane].w == 0.0)
            v2Positions[lane].xyz = targets[lane];
        if (lane < 31u)
        {
            oldOrientations[lane] = orientations[lane];
            v2OldOmega[lane] = angularVelocities[lane].xyz;
            v2Orientations[lane] = orientations[lane];
            if (lane == 0u)
                v2Orientations[lane] = RodMul(TressfxQuatFromMatrix(mat4(lastRotation)),
                    restOrientations[lane]);
#ifdef VL_ROD_V3_DISPLACEMENT
            if (lane == 0u)
                v2Orientations[lane] = normalize(RodMul(RodConjugate(initialRootFrame), rootPhysicalFrame));
#endif
        }
        barrier();
#ifdef VL_ROD_V3_CONTINUATION
        vec3 fullExternalAcceleration = externalAcceleration[lane];
        for (uint solveAttempt = 0u; solveAttempt < 10u; ++solveAttempt)
        {
            if (solveAttempt > 0u)
            {
                if (solveAttempt == 1u)
                {
                    v2Positions[lane] = oldPositions[lane];
                    if (positions[lane].w == 0.0)
                        v2Positions[lane].xyz = targets[lane];
                    if (lane < 31u)
                    {
                        v2Orientations[lane] = oldOrientations[lane];
                        if (lane == 0u)
                            v2Orientations[lane] = normalize(RodMul(
                                RodConjugate(initialRootFrame), rootPhysicalFrame));
                    }
                }
                externalAcceleration[lane] = rod.gravityStep.xyz +
                    float(solveAttempt - 1u) * .125 *
                        (fullExternalAcceleration - rod.gravityStep.xyz);
                if (lane == 0u)
                {
                    failure = 0u;
                    v3ContinuationUsed += solveAttempt == 1u ? 1u : 0u;
                }
                barrier();
            }
#endif
#ifdef VL_ROD_V3_KKT10_SOLVE
        // 每个 substep 的活跃集从空集开始（与 CPU SolveStage 一致：乘子归零、
        // excluded 位清零）。乘子只在本 substep 内连续迭代。
        if (lane == 0u)
            rodKktSubstep = substep;
        barrier();
        // 深重叠先做一步位置投影修复（对应 CPU RepairInitialContactOverlap），
        // 否则隐式牛顿的 merit 线搜索只能退到极小步。
        RodKkt10RepairOverlap(lane);
        for (uint contactSlot = 0u; contactSlot < 3u; ++contactSlot)
            rodKktContacts[contactSlot] = vec4(-1, 0, 0, 0);
        rodKktExcludedContacts = 0u;
        rodKktAxialExcluded = false;
        rodKktAxial[lane] = vec4(0);
        barrier();
#endif
        for (uint iteration = 0u; iteration <= RodV2IterationBudget; ++iteration)
        {
            RodV2Vector direction;
#ifdef VL_ROD_V3_KKT10_SOLVE
            // 活跃集每轮重选：保留正乘子身份 + 按几何 gap 补足新穿透的接触。
            RodKkt10UpdateActiveSet(lane);
            RodKkt10UpdateAxial(lane);
            barrier();
            RodActiveMeasure(lane, true);
#else
            RodActiveMeasure(lane, true);
#endif
            if (v2DomainValid == 0u)
                RodFail(2u);
            barrier();
#ifdef VL_ROD_V3_KKT10_SOLVE
            // 带约束时额外要求存在负乘子时不宣告收敛（互补量已由 merit 的 gap 分量承担）。
            if (failure != 0u || (v2Maximum <= implicitControls.originTolerance.w &&
                rodKktFeasible != 0u))
                break;
#else
            if (failure != 0u || v2Maximum <= implicitControls.originTolerance.w)
                break;
#endif
            if (iteration == RodV2IterationBudget)
            {
                RodFail(3u);
                barrier();
                break;
            }
#ifdef VL_ROD_V3_KKT10_SOLVE
            uint bandStatus = RodKkt10SolveBand(32u);
            if (bandStatus != 0u)
            {
                RodFail(4u);
                barrier();
                break;
            }
            direction = RodV2Vector(
                vec3(rodKktDirection[lane].values[0u], rodKktDirection[lane].values[1u],
                    rodKktDirection[lane].values[2u]),
                vec3(rodKktDirection[lane].values[3u], rodKktDirection[lane].values[4u],
                    rodKktDirection[lane].values[5u]));
#else
            RodV2Vector residual = RodV2Residual(lane);
            RodV2Matrix lower = lane > 0u ? RodV2Tangent(lane, lane - 1u) : RodV2Zero();
            RodV2Matrix diagonal = RodV2Tangent(lane, lane);
            RodV2Matrix upper = lane < 31u ? RodV2Tangent(lane, lane + 1u) : RodV2Zero();
#ifdef VL_ROD_V3_CR_SOLVE
            bool linearSuccess = RodCrSolveBand32(lane, lower, diagonal, upper,
                RodV2Vector(-residual.linear, -residual.angular), direction);
            if (!linearSuccess)
                RodFail(4u);
#else
#ifdef VL_ROD_V2_THOMAS_SOLVE
            direction = RodV2Thomas(lane, lower, diagonal, upper,
#else
            direction = RodV2Pcr(lane, lower, diagonal, upper,
#endif
                RodV2Vector(-residual.linear, -residual.angular));
#endif
#ifndef VL_ROD_V3_CR_SOLVE
            if (rodV2LinearStatus != 0u)
                RodFail(4u);
#endif
#endif
            v2BasePositions[lane] = v2Positions[lane];
#ifdef VL_ROD_V3_COMPENSATED_STATE
            v3BaseLow[lane] = v3CandidateLow[lane];
            if (lane < 31u)
                v3BaseQuaternionLow[lane] = v3CandidateQuaternionLow[lane];
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
            v2AccurateBasePositions[lane] = v2AccuratePositions[lane];
#endif
            if (lane < 31u)
                v2BaseOrientations[lane] = v2Orientations[lane];
            float merit = v2Merit;
#ifdef VL_ROD_V3_KKT10_SOLVE
            // 乘子的基线：每个 trial 都从「本轮起点」重新延拓 α·F·dλ，
            // 回溯不累积。contact 是私有状态，axial 是共享状态（邻点要读）。
            vec4 contactBase[3];
            for (uint contactSlot = 0u; contactSlot < 3u; ++contactSlot)
                contactBase[contactSlot] = rodKktContacts[contactSlot];
            vec4 axialBase = rodKktAxial[lane];
#endif
            if (lane == 0u)
            {
                v2Accepted = 0u;
                v2Alpha = 1.0;
            }
            barrier();
            for (uint trial = 0u; trial <= 20u; ++trial)
            {
                v2Positions[lane].xyz = v2BasePositions[lane].xyz +
                    v2Alpha * v2LengthScale * direction.linear;
#ifdef VL_ROD_V2_MIXED_GEOMETRY
                v2AccuratePositions[lane] = v2AccurateBasePositions[lane] +
                    double(v2Alpha * v2LengthScale) * dvec3(direction.linear);
                v2Positions[lane].xyz = vec3(v2AccuratePositions[lane]);
#endif
                if (lane < 31u)
                    v2Orientations[lane] = normalize(RodMul(v2BaseOrientations[lane],
                        RodExp(v2Alpha * direction.angular)));
#ifdef VL_ROD_V3_COMPENSATED_STATE
                RodCompensatedPosition(v2BasePositions[lane].xyz, v3BaseLow[lane],
                    v2Alpha * v2LengthScale * direction.linear,
                    v2Positions[lane].xyz, v3CandidateLow[lane]);
                if (lane < 31u)
                    RodCompensatedQuaternion(v2BaseOrientations[lane], v3BaseQuaternionLow[lane],
                        RodExp(v2Alpha * direction.angular), v2Orientations[lane],
                        v3CandidateQuaternionLow[lane]);
#endif
#ifdef VL_ROD_V3_KKT10_SOLVE
                for (uint contactSlot = 0u; contactSlot < 3u; ++contactSlot)
                {
                    rodKktContacts[contactSlot] = contactBase[contactSlot];
                    if (contactBase[contactSlot].w != 0.0)
                        rodKktContacts[contactSlot].z += v2Alpha * v2ForceScale *
                            rodKktDirection[lane].values[6u + contactSlot];
                }
                rodKktAxial[lane] = axialBase;
                if (axialBase.z != 0.0)
                    rodKktAxial[lane].x += v2Alpha * v2ForceScale *
                        rodKktDirection[lane].values[9u];
#endif
                barrier();
                RodActiveMeasure(lane, lane >= 31u ||
                    length(v2Alpha * direction.angular) < RodRotationLimit);
                if (lane == 0u && v2DomainValid != 0u &&
                    v2Merit <= (1.0 - 1.0e-4 * v2Alpha) * merit)
                    v2Accepted = 1u;
                barrier();
                if (v2Accepted != 0u || failure != 0u)
                    break;
                if (lane == 0u)
                {
                    v2Alpha *= 0.5;
                    ++v3BacktrackCount;
                }
                barrier();
            }
            if (v2Accepted == 0u)
                RodFail(3u);
            if (lane == 0u)
                ++v2Iterations;
            barrier();
            if (failure != 0u)
                break;
        }
#ifdef VL_ROD_V3_CONTINUATION
            if (solveAttempt == 0u && failure == 0u)
                break;
            if (solveAttempt > 0u)
            {
                if (failure != 0u)
                    break;
                if (lane == 0u)
                    ++v3CompletedLoadStages;
                barrier();
            }
        }
#endif
        if (failure != 0u)
            break;
        positions[lane] = v2Positions[lane];
        velocities[lane].xyz = (positions[lane].xyz - oldPositions[lane].xyz) / step;
#ifdef VL_ROD_V3_COMPENSATED_STATE
        velocities[lane].xyz += (v3CandidateLow[lane] - v3OldLow[lane]) / step;
        v3PositionLow[lane] = v3CandidateLow[lane];
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        velocities[lane].xyz = vec3((v2AccuratePositions[lane] - v2AccurateOldPositions[lane]) / double(step));
#endif
        if (lane < 31u)
        {
            orientations[lane] = v2Orientations[lane];
            angularVelocities[lane].xyz = RodLog(RodMul(
                RodConjugate(oldOrientations[lane]), orientations[lane])) / step;
#ifdef VL_ROD_V3_COMPENSATED_STATE
            vec4 relative = RodMul(RodConjugate(oldOrientations[lane]), orientations[lane]);
            angularVelocities[lane].xyz += RodV2JrInverse(RodLog(relative)) *
                (RodCompensatedBodyOffset(orientations[lane], v3CandidateQuaternionLow[lane]) -
                    transpose(RodRotation(relative)) *
                    RodCompensatedBodyOffset(oldOrientations[lane], v3OldQuaternionLow[lane])) / step;
            v3QuaternionLow[lane] = v3CandidateQuaternionLow[lane];
#endif
        }
        if (lane == 0u)
            simulatedSeconds += step;
        barrier();
    }
#ifdef VL_ROD_V2_FAILURE_EXPORT
    RodV2ExportLate(lane, slot);
    barrier();
#endif
    if (failure == 0u)
    {
        rodState.guides[slot].points[lane].position = positions[lane];
        rodState.guides[slot].points[lane].velocity = velocities[lane];
#ifdef VL_ROD_V3_COMPENSATED_STATE
        compensatedState.values[slot * 63u + lane] = vec4(v3PositionLow[lane], 0);
        if (lane < 31u)
            compensatedState.values[slot * 63u + 32u + lane] = v3QuaternionLow[lane];
#endif
#ifdef VL_ROD_V2_MIXED_GEOMETRY
        positionCompensation.values[slot * 32u + lane] =
            vec4(vec3(v2AccuratePositions[lane] - dvec3(positions[lane].xyz)), 0);
#endif
        if (lane < 31u)
        {
#ifdef VL_ROD_V3_DISPLACEMENT
            frameDeltas.values[slot * 31u + lane] = orientations[lane];
            rodState.guides[slot].segments[lane].orientation = normalize(RodMul(
                displacementReferences.guides[slot].frames[lane], orientations[lane]
#ifdef VL_ROD_V3_COMPENSATED_STATE
                    + v3QuaternionLow[lane]
#endif
                ));
#else
            rodState.guides[slot].segments[lane].orientation = orientations[lane];
#endif
            rodState.guides[slot].segments[lane].angularVelocity = angularVelocities[lane];
        }
        vec3 publishedPosition = positions[lane].xyz + implicitControls.originTolerance.xyz;
#ifdef VL_ROD_V3_COMPENSATED_STATE
        publishedPosition += v3PositionLow[lane];
#endif
#ifdef VL_ROD_V3_DISPLACEMENT
        publishedPosition += displacementReferences.guides[slot].positions[lane].xyz;
#endif
        vec4 publishedLocal = vec4((rod.worldToHair *
            vec4(publishedPosition * 100.0, 1)).xyz,
            positions[lane].w == 0.0 ? 0.0 : 1.0);
        uint publishedPoint = guide * 32u + lane;
        previousPreviousPositions.positions[publishedPoint] = rod.flags.x != 0u ?
            publishedLocal : previousPositions.positions[publishedPoint];
        previousPositions.positions[publishedPoint] = rod.flags.x != 0u ?
            publishedLocal : currentPositions.positions[publishedPoint];
        currentPositions.positions[publishedPoint] = publishedLocal;
    }
    if (lane == 0u)
    {
        rodState.guides[slot].diagnostic = vec4(float(failure), v2Maximum, v2Merit, float(v2Iterations));
#ifdef VL_ROD_V3_CONTINUATION
        rodState.guides[slot].energies = vec4(float(v3ContinuationUsed),
            float(v3CompletedLoadStages), float(v3BacktrackCount), 0);
#endif
#ifdef VL_ROD_KKT10_DEBUG
        // 诊断：活跃 contact 槽数 / contact 分量残差 / 位置分量残差 / max |lambda|/F。
        rodState.guides[slot].residuals = vec4(float(rodKktDebug.x),
            uintBitsToFloat(rodKktDebug.y), uintBitsToFloat(rodKktDebug.z),
            uintBitsToFloat(rodKktDebug.w));
#ifndef VL_ROD_V3_CONTINUATION
        // energies.x = 本帧累计的回溯减半次数，y = 重叠修复触发次数，
        // z = 修复前的最深重叠（米），w 保留。
        rodState.guides[slot].energies = vec4(float(v3BacktrackCount),
            float(rodKktRepairCount), uintBitsToFloat(rodKktRepairDepthBits), 0);
#endif
#endif
        rodState.guides[slot].timing = vec4(failure == 0u ? simulatedSeconds :
            (rod.flags.x != 0u ? 0.0 : rodState.guides[slot].timing.x),
            rod.timing.x, rod.timing.y, float(rod.counts.y));
    }
}
#endif
