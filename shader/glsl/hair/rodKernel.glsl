#include "rodCommon.glsl"
#ifdef VL_ROD_SDF
#include "tressfxSdfCommon.glsl"
#include "rodSdfHistory.glsl"
#endif

layout(local_size_x = 32) in;

layout(set = 0, binding = 2, std430) buffer PreviousPositions
{
    vec4 positions[];
} previousPositions;
layout(set = 0, binding = 3, std430) buffer PreviousPreviousPositions
{
    vec4 positions[];
} previousPreviousPositions;

struct RodBones
{
    uvec4 indices;
    vec4 weights;
};
layout(set = 0, binding = 14, std430) readonly buffer RodBoneWeights
{
    RodBones bones[];
} rodBones;
layout(set = 0, binding = 25, std430) readonly buffer RodPoseSamples
{
    mat4 matrices[];
} rodSamples;

shared vec4 positions[32];
shared vec4 velocities[32];
shared vec4 orientations[31];
shared vec4 angularVelocities[31];
shared vec4 oldPositions[32];
shared vec4 oldOrientations[31];
shared vec4 restOrientations[31];
shared vec4 rootLocals[32];
shared vec4 masses[32];
shared vec4 restStrains[31];
shared vec4 restJoints[31];
shared vec4 inertias[31];
shared vec3 lambdaShear[31];
shared vec3 lambdaBend[30];
shared vec3 lambdaTether[32];
shared vec3 targets[32];
shared vec3 targetVelocities[32];
shared uint failure;
shared float totalLength;
shared vec3 rootPosition;
shared mat3 rootRotation;
shared vec4 rootOrientation;
shared float simulatedSeconds;
shared float maxDefect;
shared float predictorResidual;
shared float lanePredictorResidual[32];
shared float laneDefect[32];
shared vec4 laneEnergy[32];
shared vec3 contactCorrection[32];
shared vec3 contactNormal[32];
shared vec3 contactVelocity[32];
shared float capsuleLambda[32][8];
shared uint currentSubstep;
shared float sdfLambda[32];
shared vec3 oldVelocities[32];
shared vec3 oldAngularVelocities[31];
shared vec3 externalAcceleration[32];
shared vec3 shearForces[31];
shared vec3 shearTorques[31];
shared vec3 bendLeftTorques[30];
shared vec3 bendRightTorques[30];
shared vec4 laneResiduals[32];
shared uint minimumPiMarginBits;
shared uint maximumQuaternionNormErrorBits;

layout(set = 0, binding = 7, std430) readonly buffer RodCapsules
{
    GuideCollider colliders[];
} rodCapsules;

void RodFail(uint status)
{
    atomicMax(failure, status);
}

vec4 RodMatrixQuaternion(mat3 rotation)
{
    return TressfxQuatFromMatrix(mat4(rotation));
}

mat4 RodRootMatrix(uint guide, uint sampleIndex)
{
    if (rod.flags.z == 0u)
        return rod.roots[sampleIndex];
    RodBones influence = rodBones.bones[guide];
    uint offset = sampleIndex * rod.counts.z;
    return rodSamples.matrices[offset + influence.indices.x] * influence.weights.x +
        rodSamples.matrices[offset + influence.indices.y] * influence.weights.y +
        rodSamples.matrices[offset + influence.indices.z] * influence.weights.z +
        rodSamples.matrices[offset + influence.indices.w] * influence.weights.w;
}

vec3 RodInverseInertia(uint segment)
{
    return segment == 0u ? vec3(0) : 1.0 / inertias[segment].xyz;
}

void RodApplyAngle(uint segment, vec3 correction)
{
    atomicMin(minimumPiMarginBits,
        floatBitsToUint(max(0.0, RodRotationLimit - length(correction))));
    if (!RodFinite(correction))
    {
        RodFail(5u);
        return;
    }
    if (length(correction) >= RodRotationLimit)
    {
        RodFail(2u);
        return;
    }
    orientations[segment] = normalize(RodMul(orientations[segment], RodExp(correction)));
    atomicMax(maximumQuaternionNormErrorBits,
        floatBitsToUint(abs(length(orientations[segment]) - 1.0)));
}

void RodShear(uint segment)
{
    float restLength = restStrains[segment].w;
    mat3 rotation = RodRotation(orientations[segment]);
    mat3 positional = transpose(rotation) / restLength;
    vec3 strain = positional * (positions[segment + 1u].xyz - positions[segment].xyz);
    mat3 angular = RodSkew(strain);
    vec3 inverseInertia = RodInverseInertia(segment);
    vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[segment].shearCompliance.xyz /
        (rod.gravityStep.w * rod.gravityStep.w);
    mat3 matrix = (positions[segment].w + positions[segment + 1u].w) *
        positional * transpose(positional) +
        angular * RodDiagonal(inverseInertia) * transpose(angular) + RodDiagonal(compliance);
    vec3 delta;
    if (!RodSolve(matrix, -(strain - restStrains[segment].xyz) -
        compliance * lambdaShear[segment], delta))
    {
        RodFail(4u);
        return;
    }
    positions[segment].xyz -= positions[segment].w * transpose(positional) * delta;
    positions[segment + 1u].xyz += positions[segment + 1u].w * transpose(positional) * delta;
    RodApplyAngle(segment, inverseInertia * (transpose(angular) * delta));
    lambdaShear[segment] += delta;
}

void RodBend(uint joint)
{
    vec4 relative = RodMul(RodConjugate(orientations[joint]), orientations[joint + 1u]);
    vec3 angle = RodLog(relative);
    atomicMin(minimumPiMarginBits,
        floatBitsToUint(max(0.0, RodRotationLimit - length(angle))));
    if (length(angle) >= RodRotationLimit)
    {
        RodFail(2u);
        return;
    }
    mat3 right = RodJrInverse(angle);
    mat3 left = -right * transpose(RodRotation(relative));
    vec3 inverseLeft = RodInverseInertia(joint);
    vec3 inverseRight = RodInverseInertia(joint + 1u);
    vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[joint].bendCompliance.xyz /
        (rod.gravityStep.w * rod.gravityStep.w);
    mat3 matrix = left * RodDiagonal(inverseLeft) * transpose(left) +
        right * RodDiagonal(inverseRight) * transpose(right) + RodDiagonal(compliance);
    vec3 delta;
    if (!RodSolve(matrix, -(angle - restJoints[joint].xyz) -
        compliance * lambdaBend[joint], delta))
    {
        RodFail(4u);
        return;
    }
    RodApplyAngle(joint, inverseLeft * (transpose(left) * delta));
    RodApplyAngle(joint + 1u, inverseRight * (transpose(right) * delta));
    lambdaBend[joint] += delta;
}

void RodContact(uint lane)
{
    if (positions[lane].w == 0.0)
        return;
    for (uint collider = 0u; collider < params.counts.w; ++collider)
    {
        vec3 first = rod.capsules[currentSubstep + 1u][collider].a.xyz;
        vec3 last = rod.capsules[currentSubstep + 1u][collider].b.xyz;
        float radius = rod.capsules[currentSubstep + 1u][collider].a.w;
        vec3 axis = last - first;
        float denominator = dot(axis, axis);
        float fraction = denominator > 0.0 ?
            clamp(dot(positions[lane].xyz - first, axis) / denominator, 0.0, 1.0) : 0.0;
        vec3 delta = positions[lane].xyz - first - fraction * axis;
        float distanceValue = length(delta);
        vec3 normal = vec3(0, 1, 0);
        if (distanceValue > 1.0e-12)
            normal = delta / distanceValue;
        else if (denominator > 1.0e-24)
        {
            vec3 unitAxis = normalize(axis);
            normal = normalize(cross(unitAxis, abs(unitAxis.y) < 0.9 ?
                vec3(0, 1, 0) : vec3(1, 0, 0)));
        }
        float oldLambda = capsuleLambda[lane][collider];
        float nextLambda = max(0.0, oldLambda -
            (distanceValue - radius) / positions[lane].w);
        vec3 correction = positions[lane].w * (nextLambda - oldLambda) * normal;
        capsuleLambda[lane][collider] = nextLambda;
        positions[lane].xyz += correction;
        contactCorrection[lane] += correction;
        if (nextLambda > 0.0)
        {
            contactNormal[lane] = normal;
            vec3 firstVelocity = (first - rod.capsules[currentSubstep][collider].a.xyz) /
                rod.gravityStep.w;
            vec3 lastVelocity = (last - rod.capsules[currentSubstep][collider].b.xyz) /
                rod.gravityStep.w;
            contactVelocity[lane] = mix(firstVelocity, lastVelocity, fraction);
        }
    }
#ifdef VL_ROD_SDF
    if (positions[lane].w == 0.0 || (rod.flags.w & 1u) == 0u)
        return;
    vec3 local = (rod.worldToHair * vec4(positions[lane].xyz * 100.0, 1)).xyz;
    float distanceValue = SdfTrilinear(local);
    if (distanceValue >= SdfOutOfRangeDistance)
    {
        sdfLambda[lane] = 0.0;
        return;
    }
    vec3 gradient = SdfGradient(local);
    float gradientSquared = dot(gradient, gradient);
    if (!(gradientSquared > 1.0e-12) || !RodFinite(gradient))
        return;
    vec3 normal = normalize(mat3(rod.hairToWorld) * gradient);
    float oldLambda = sdfLambda[lane];
    float nextLambda = max(0.0, oldLambda -
        (distanceValue - sdf.range.x) * 0.01 / positions[lane].w);
    vec3 worldCorrection = positions[lane].w * (nextLambda - oldLambda) * normal;
    sdfLambda[lane] = nextLambda;
    positions[lane].xyz += worldCorrection;
    contactCorrection[lane] += worldCorrection;
    contactNormal[lane] = normal;
    contactVelocity[lane] = RodSdfSurfaceVelocity(positions[lane].xyz, normal, distanceValue);
#endif
}

void RodInitialize(uint slot, uint guide, uint lane)
{
    uint point = guide * 32u + lane;
    vec3 rest = restPositions.positions[point].xyz * 0.01;
    rootLocals[lane] = vec4(rest - restPositions.positions[guide * 32u].xyz * 0.01, 0);
    positions[lane] = vec4(rest, lane == 0u ? 0.0 : 1.0);
    velocities[lane] = vec4(0);
    if (lane == 0u)
    {
        totalLength = 0;
        vec3 previousTangent = vec3(0, 0, 1);
        vec4 frame = vec4(0, 0, 0, 1);
        for (uint segment = 0u; segment < 31u; ++segment)
        {
            vec3 delta = (restPositions.positions[guide * 32u + segment + 1u].xyz -
                restPositions.positions[guide * 32u + segment].xyz) * 0.01;
            float restLength = length(delta);
            if (!(restLength > 1.0e-12) || !RodFinite(delta))
                RodFail(1u);
            vec3 tangent = delta / max(restLength, 1.0e-12);
            if (segment > 0u && dot(previousTangent, tangent) <= -1.0 + 1.0e-8)
                RodFail(2u);
            frame = normalize(RodMul(RodFromTo(previousTangent, tangent), frame));
            restOrientations[segment] = frame;
            restStrains[segment] = vec4(transpose(RodRotation(frame)) * delta /
                max(restLength, 1.0e-12), restLength);
            previousTangent = tangent;
            totalLength += restLength;
        }
    }
    barrier();
    rootLocals[lane].xyz = transpose(RodRotation(restOrientations[0])) * rootLocals[lane].xyz;
    float radius = rod.material.x;
    float area = 3.141592653589793 * radius * radius;
    float areaMoment = area * radius * radius * 0.25;
    float linearDensity = rod.material.y * area;
    float mass = 0.5 * linearDensity *
        ((lane > 0u ? restStrains[lane - 1u].w : 0.0) +
         (lane < 31u ? restStrains[lane].w : 0.0));
    bool pinned = lane == 0u || restPositions.positions[point].w == 0.0;
    float stiffness = pinned ? 0.0 : mass * rod.support.z / (rod.support.x * totalLength);
    float damping = 2.0 * rod.support.y * sqrt(mass * stiffness);
    masses[lane] = vec4(mass, stiffness, damping, 0);
    positions[lane].w = pinned ? 0.0 : 1.0 / mass;
    rodState.guides[slot].points[lane].rootLocal = rootLocals[lane];
    rodState.guides[slot].points[lane].massSupport = masses[lane];
    if (lane < 31u)
    {
        float restLength = restStrains[lane].w;
        float segmentMass = linearDensity * restLength;
        float transversePerMass = radius * radius * 0.25;
#ifndef VL_ROD_SECTION_INERTIA
        transversePerMass += restLength * restLength / 12.0;
#endif
        inertias[lane] = vec4(segmentMass * vec3(
            transversePerMass, transversePerMass, radius * radius * 0.5),
#ifdef VL_ROD_SECTION_INERTIA
            2.0);
#else
            1.0);
#endif
        float averageLength = lane < 30u ?
            0.5 * (restLength + restStrains[lane + 1u].w) : restLength;
        restJoints[lane] = vec4(lane < 30u ?
            RodLog(RodMul(RodConjugate(restOrientations[lane]),
                restOrientations[lane + 1u])) : vec3(0), averageLength);
        rodState.guides[slot].segments[lane].restOrientation = restOrientations[lane];
        rodState.guides[slot].segments[lane].restStrainLength = restStrains[lane];
        rodState.guides[slot].segments[lane].restJointLength = restJoints[lane];
        rodState.guides[slot].segments[lane].inertia = inertias[lane];
        rodState.guides[slot].segments[lane].shearCompliance = vec4(
            1.0 / (restLength * area * vec3(rod.material.w, rod.material.w, rod.material.z)), 0);
        rodState.guides[slot].segments[lane].bendCompliance = vec4(
            averageLength / (areaMoment * vec3(rod.material.z, rod.material.z,
                2.0 * rod.material.w)), 0);
    }
    barrier();
}

#ifndef VL_ROD_EXTERNAL_MAIN
void main()
{
    uint slot = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationID.x;
    if (slot >= rod.counts.x)
        return;
    uint guide = rod.flags.y != 0u ? simulationGuides.indices[slot] : slot;
    uint point = guide * 32u + lane;
    if (lane == 0u)
    {
        failure = rod.flags.x != 0u ? 0u : uint(rodState.guides[slot].diagnostic.x);
        simulatedSeconds = rod.flags.x != 0u ? 0.0 : rodState.guides[slot].timing.x;
        maxDefect = 0.0;
        predictorResidual = 0.0;
        minimumPiMarginBits = floatBitsToUint(failure != 0u ?
            rodState.guides[slot].diagnostic.w : RodRotationLimit);
        maximumQuaternionNormErrorBits = failure != 0u ?
            floatBitsToUint(rodState.guides[slot].energies.w) : 0u;
    }
    barrier();
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
        mat4 rootMatrix = RodRootMatrix(guide, 0u);
        mat3 rotation = RodRigidRotation(rootMatrix);
        vec3 anchor = (rootMatrix *
            vec4(restPositions.positions[guide * 32u].xyz, 1)).xyz * 0.01;
        positions[lane].xyz = anchor +
            rotation * RodRotation(restOrientations[0]) * rootLocals[lane].xyz;
        if (lane < 31u)
        {
            orientations[lane] = RodMul(RodMatrixQuaternion(rotation), restOrientations[lane]);
            angularVelocities[lane] = vec4(0);
        }
    }
    barrier();
    targets[lane] = positions[lane].xyz;
    laneResiduals[lane] = vec4(0);
    externalAcceleration[lane] = rod.gravityStep.xyz;
    float step = rod.gravityStep.w;
    for (uint substep = 0u; substep < rod.counts.y; ++substep)
    {
        if (failure != 0u)
            break;
        if (lane == 0u)
            currentSubstep = substep;
        barrier();
        mat4 initialRoot = RodRootMatrix(guide, substep);
        mat4 finalRoot = RodRootMatrix(guide, substep + 1u);
        mat3 initialRotation = RodRigidRotation(initialRoot);
        mat3 finalRotation = RodRigidRotation(finalRoot);
        vec3 authoredRoot = restPositions.positions[guide * 32u].xyz;
        vec3 initialAnchor = (initialRoot * vec4(authoredRoot, 1)).xyz * 0.01;
        vec3 finalAnchor = (finalRoot * vec4(authoredRoot, 1)).xyz * 0.01;
        vec3 omegaWorld = initialRotation *
            RodLog(RodMul(RodConjugate(RodMatrixQuaternion(initialRotation)),
                RodMatrixQuaternion(finalRotation))) / step;
        vec3 restOffset = RodRotation(restOrientations[0]) * rootLocals[lane].xyz;
        targets[lane] = finalAnchor + finalRotation * restOffset;
        targetVelocities[lane] = (finalAnchor - initialAnchor) / step +
            cross(omegaWorld, finalRotation * restOffset);
        oldPositions[lane] = positions[lane];
        oldVelocities[lane] = velocities[lane].xyz;
        lambdaTether[lane] = vec3(0);
        contactCorrection[lane] = vec3(0);
        contactNormal[lane] = vec3(0);
        contactVelocity[lane] = vec3(0);
        lanePredictorResidual[lane] = 0.0;
        sdfLambda[lane] = 0.0;
        for (uint collider = 0u; collider < 8u; ++collider)
            capsuleLambda[lane][collider] = 0.0;
        barrier();
        if (positions[lane].w != 0.0)
        {
            float dampingRate = masses[lane].z / masses[lane].x;
            vec3 tangent = lane < 31u ?
                oldPositions[lane + 1u].xyz - oldPositions[lane].xyz :
                oldPositions[lane].xyz - oldPositions[lane - 1u].xyz;
            tangent = normalize(tangent);
            vec3 relativeWind = rod.windDrag.xyz - oldVelocities[lane];
            vec3 normalWind = relativeWind - tangent * dot(relativeWind, tangent);
            float dragRate = rod.windDrag.w * length(normalWind);
            externalAcceleration[lane] = rod.gravityStep.xyz + rod.probeAcceleration.xyz +
                normalWind * (dragRate / (1.0 + step * dragRate));
            velocities[lane].xyz = (velocities[lane].xyz + step *
                (externalAcceleration[lane] + dampingRate * targetVelocities[lane])) /
                (1.0 + step * dampingRate);
            positions[lane].xyz += step * velocities[lane].xyz;
        }
        else
            positions[lane].xyz = targets[lane];
        if (lane < 31u)
        {
            oldOrientations[lane] = orientations[lane];
            oldAngularVelocities[lane] = angularVelocities[lane].xyz;
            lambdaShear[lane] = vec3(0);
            if (lane < 30u)
                lambdaBend[lane] = vec3(0);
            if (lane == 0u)
                orientations[lane] = RodMul(RodMatrixQuaternion(finalRotation), restOrientations[lane]);
            else
            {
                vec3 inertia = inertias[lane].xyz;
                vec3 previousOmega = angularVelocities[lane].xyz;
                vec3 predicted = previousOmega;
                float scale = max(length(inertia * previousOmega) / step,
                    min(min(inertia.x, inertia.y), inertia.z) / (step * step));
                bool converged = false;
                for (uint iteration = 0u; iteration < 12u; ++iteration)
                {
                    vec3 defect = inertia * (predicted - previousOmega) / step +
                        cross(predicted, inertia * predicted);
                    lanePredictorResidual[lane] = length(defect) / max(scale, 1.0e-30);
                    if (length(defect) <= 1.0e-5 * scale)
                    {
                        converged = true;
                        break;
                    }
                    mat3 jacobian = RodDiagonal(inertia / step) -
                        RodSkew(inertia * predicted) + RodSkew(predicted) * RodDiagonal(inertia);
                    vec3 correction;
                    if (!RodSolve(jacobian, defect, correction))
                        break;
                    predicted -= correction;
                }
                if (!converged)
                    RodFail(3u);
                RodApplyAngle(lane, step * predicted);
            }
        }
        barrier();
        for (uint sweep = 0u; sweep < rod.flags.w >> 1u; ++sweep)
        {
            if (lane < 31u && (lane & 1u) == 0u)
                RodShear(lane);
            barrier();
            if (lane < 31u && (lane & 1u) == 1u)
                RodShear(lane);
            barrier();
            if (lane < 30u && (lane & 1u) == 0u)
                RodBend(lane);
            barrier();
            if (lane < 30u && (lane & 1u) == 1u)
                RodBend(lane);
            barrier();
            if (positions[lane].w != 0.0)
            {
                float compliance = 1.0 / (masses[lane].y * step * step);
                vec3 delta = -(positions[lane].xyz - targets[lane] +
                    compliance * lambdaTether[lane]) / (positions[lane].w + compliance);
                positions[lane].xyz += positions[lane].w * delta;
                lambdaTether[lane] += delta;
            }
            RodContact(lane);
            barrier();
        }
        velocities[lane].xyz = (positions[lane].xyz - oldPositions[lane].xyz -
            contactCorrection[lane]) / step;
        if (positions[lane].w > 0.0)
            for (uint collider = 0u; collider < params.counts.w; ++collider)
            {
                vec3 first = rod.capsules[substep + 1u][collider].a.xyz;
                vec3 last = rod.capsules[substep + 1u][collider].b.xyz;
                vec3 axis = last - first;
                float denominator = dot(axis, axis);
                float fraction = denominator > 0.0 ?
                    clamp(dot(positions[lane].xyz - first, axis) / denominator, 0.0, 1.0) : 0.0;
                vec3 delta = positions[lane].xyz - first - fraction * axis;
                float distanceValue = length(delta);
                float radius = rod.capsules[substep + 1u][collider].a.w;
                if (capsuleLambda[lane][collider] <= 0.0 && distanceValue > radius + 1.0e-7)
                    continue;
                vec3 normal = distanceValue > 1.0e-12 ? delta / distanceValue : vec3(0, 1, 0);
                if (distanceValue <= 1.0e-12 && denominator > 1.0e-24)
                {
                    vec3 unitAxis = normalize(axis);
                    normal = normalize(cross(unitAxis, abs(unitAxis.y) < 0.9 ?
                        vec3(0, 1, 0) : vec3(1, 0, 0)));
                }
                vec3 surfaceVelocity = mix(
                    first - rod.capsules[substep][collider].a.xyz,
                    last - rod.capsules[substep][collider].b.xyz, fraction) / step;
                float inward = dot(velocities[lane].xyz - surfaceVelocity, normal);
                if (inward < 0.0)
                    velocities[lane].xyz -= inward * normal;
            }
#ifdef VL_ROD_SDF
        if (positions[lane].w > 0.0 && (rod.flags.w & 1u) != 0u)
        {
            vec3 local = (rod.worldToHair * vec4(positions[lane].xyz * 100.0, 1)).xyz;
            float distanceValue = SdfTrilinear(local);
            if (sdfLambda[lane] > 0.0 || distanceValue <= sdf.range.x + 1.0e-5)
            {
                vec3 gradient = SdfGradient(local);
                if (RodFinite(gradient) && dot(gradient, gradient) > 1.0e-12)
                {
                    vec3 normal = normalize(mat3(rod.hairToWorld) * gradient);
                    vec3 surfaceVelocity = RodSdfSurfaceVelocity(
                        positions[lane].xyz, normal, distanceValue);
                    float inward = dot(velocities[lane].xyz - surfaceVelocity, normal);
                    if (inward < 0.0)
                        velocities[lane].xyz -= inward * normal;
                }
            }
        }
#endif
        if (lane < 31u)
        {
            vec3 angle = RodLog(RodMul(RodConjugate(oldOrientations[lane]), orientations[lane]));
            atomicMin(minimumPiMarginBits,
                floatBitsToUint(max(0.0, RodRotationLimit - length(angle))));
            if (length(angle) >= RodRotationLimit)
                RodFail(2u);
            angularVelocities[lane] = vec4(angle / step, 0);
        }
        if (!RodFinite(positions[lane].xyz) || !RodFinite(velocities[lane].xyz))
            RodFail(5u);
        barrier();
        if (lane < 31u)
        {
            mat3 rotation = RodRotation(orientations[lane]);
            vec3 strain = transpose(rotation) *
                (positions[lane + 1u].xyz - positions[lane].xyz) / restStrains[lane].w;
            vec3 constraint = strain - restStrains[lane].xyz;
            vec3 compliance = rodState.guides[slot].segments[lane].shearCompliance.xyz;
            vec3 resultant = constraint / (compliance * restStrains[lane].w);
            shearForces[lane] = rotation * resultant;
            shearTorques[lane] = restStrains[lane].w * cross(strain, resultant);
            if (lane < 30u)
            {
                vec4 relative = RodMul(RodConjugate(orientations[lane]), orientations[lane + 1u]);
                vec3 angle = RodLog(relative);
                vec3 bendCompliance = rodState.guides[slot].segments[lane].bendCompliance.xyz;
                vec3 moment = transpose(RodJrInverse(angle)) *
                    ((angle - restJoints[lane].xyz) / bendCompliance);
                bendLeftTorques[lane] = RodRotation(relative) * moment;
                bendRightTorques[lane] = -moment;
            }
        }
        barrier();
        vec3 elasticForce = (lane < 31u ? shearForces[lane] : vec3(0)) -
            (lane > 0u ? shearForces[lane - 1u] : vec3(0));
        vec3 supportForce = masses[lane].y * (targets[lane] - positions[lane].xyz) -
            masses[lane].z * (velocities[lane].xyz - targetVelocities[lane]);
        vec3 linearResidual = masses[lane].x *
            (velocities[lane].xyz - oldVelocities[lane]) / step - elasticForce -
            supportForce - masses[lane].x * externalAcceleration[lane];
        laneResiduals[lane] = vec4(positions[lane].w > 0.0 ? length(linearResidual) : 0.0,
            0.0, length(elasticForce + supportForce), 0.0);
        if (lane < 31u)
        {
            vec3 torque = shearTorques[lane] +
                (lane < 30u ? bendLeftTorques[lane] : vec3(0)) +
                (lane > 0u ? bendRightTorques[lane - 1u] : vec3(0));
            vec3 transported = transpose(RodRotation(orientations[lane])) *
                RodRotation(oldOrientations[lane]) *
                (inertias[lane].xyz * oldAngularVelocities[lane]);
            vec3 angularResidual = (inertias[lane].xyz * angularVelocities[lane].xyz -
                transported) / step - torque;
            laneResiduals[lane].y = lane > 0u ? length(angularResidual) : 0.0;
            laneResiduals[lane].w = length(torque);
        }
        barrier();
        if (lane == 0u)
            for (uint index = 0u; index < 32u; ++index)
                predictorResidual = max(predictorResidual, lanePredictorResidual[index]);
        barrier();
        if (failure != 0u)
        {
            positions[lane] = oldPositions[lane];
            if (lane < 31u)
                orientations[lane] = oldOrientations[lane];
            barrier();
            break;
        }
        if (lane == 0u)
            simulatedSeconds += step;
        barrier();
    }
    laneDefect[lane] = 0.0;
    laneEnergy[lane] = vec4(0);
    if (failure == 0u && rod.counts.y > 0u)
    {
        vec3 tether = positions[lane].xyz - targets[lane];
        laneEnergy[lane].z = 0.5 * masses[lane].y * dot(tether, tether);
        laneEnergy[lane].w = 0.5 * masses[lane].x * dot(velocities[lane].xyz, velocities[lane].xyz);
        if (positions[lane].w != 0.0)
            laneDefect[lane] = length(tether + lambdaTether[lane] /
                (masses[lane].y * step * step));
        if (lane < 31u)
        {
            vec3 strain = transpose(RodRotation(orientations[lane])) *
                (positions[lane + 1u].xyz - positions[lane].xyz) / restStrains[lane].w -
                restStrains[lane].xyz;
            vec3 compliance = rodState.guides[slot].segments[lane].shearCompliance.xyz;
            laneEnergy[lane].x = 0.5 * dot(strain / compliance, strain);
            laneDefect[lane] = max(laneDefect[lane],
                length(strain + compliance * lambdaShear[lane] / (step * step)));
            if (lane < 30u)
            {
                vec3 bend = RodLog(RodMul(RodConjugate(orientations[lane]),
                    orientations[lane + 1u])) - restJoints[lane].xyz;
                vec3 bendCompliance = rodState.guides[slot].segments[lane].bendCompliance.xyz;
                laneEnergy[lane].y = 0.5 * dot(bend / bendCompliance, bend);
                laneDefect[lane] = max(laneDefect[lane],
                    length(bend + bendCompliance * lambdaBend[lane] / (step * step)));
            }
        }
    }
    barrier();
    if (!RodFinite(positions[lane].xyz) || !RodFinite(velocities[lane].xyz) ||
        (lane < 31u && (!RodFinite(orientations[lane].xyz) ||
            isnan(orientations[lane].w) || isinf(orientations[lane].w))))
        RodFail(5u);
    barrier();
    if (failure == 0u)
    {
        rodState.guides[slot].points[lane].position = positions[lane];
        rodState.guides[slot].points[lane].velocity = velocities[lane];
        rodState.guides[slot].points[lane].target = vec4(targets[lane], 0);
        if (lane < 31u)
        {
            rodState.guides[slot].segments[lane].orientation = orientations[lane];
            rodState.guides[slot].segments[lane].angularVelocity = angularVelocities[lane];
        }
        vec4 localPosition = vec4((rod.worldToHair *
            vec4(positions[lane].xyz * 100.0, 1)).xyz, positions[lane].w == 0.0 ? 0.0 : 1.0);
        previousPreviousPositions.positions[point] = rod.flags.x != 0u ?
            localPosition : previousPositions.positions[point];
        previousPositions.positions[point] = rod.flags.x != 0u ?
            localPosition : currentPositions.positions[point];
        currentPositions.positions[point] = localPosition;
    }
    if (lane == 0u)
    {
        vec4 energy = vec4(0);
        vec4 residual = vec4(0);
        for (uint index = 0u; index < 32u; ++index)
        {
            maxDefect = max(maxDefect, laneDefect[index]);
            energy += laneEnergy[index];
            residual = max(residual, laneResiduals[index]);
        }
        rodState.guides[slot].energies = energy;
        if (failure != 0u)
            rodState.guides[slot].energies.w = uintBitsToFloat(maximumQuaternionNormErrorBits);
        rodState.guides[slot].residuals = residual;
        rodState.guides[slot].diagnostic = vec4(float(failure), maxDefect, predictorResidual,
            uintBitsToFloat(minimumPiMarginBits));
        float accepted = failure == 0u ? simulatedSeconds :
            (rod.flags.x != 0u ? 0.0 : rodState.guides[slot].timing.x);
        rodState.guides[slot].timing = vec4(accepted, rod.timing.x,
            rod.timing.y, float(rod.counts.y));
    }
}
#endif
