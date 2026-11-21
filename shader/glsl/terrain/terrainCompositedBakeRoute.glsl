#ifndef VL_TERRAIN_BAKE_ROUTE_GLSL
#define VL_TERRAIN_BAKE_ROUTE_GLSL

uint TerrainBakeResolve(vec2 uv, uint source, uint chartCount, bool rectangular)
{
    if (all(greaterThanEqual(uv, vec2(0))) && all(lessThanEqual(uv, vec2(1))))
        return source;
    if (rectangular)
    {
        const vec2 bounded = clamp(uv, charts.entries[0].metricU.zw, charts.entries[0].metricV.zw);
        for (uint target = 0u; target < chartCount; ++target)
        {
            const Chart chart = charts.entries[target];
            precise vec2 mapped = vec2(
                (chart.rowU.x * bounded.x + chart.rowU.y * bounded.y) + chart.rowU.z,
                (chart.rowV.x * bounded.x + chart.rowV.y * bounded.y) + chart.rowV.z);
            if (all(greaterThanEqual(mapped, vec2(0))) && all(lessThanEqual(mapped, vec2(1))))
            {
                const vec2 edgeDistance = min(mapped, 1.0 - mapped);
                if ((edgeDistance.x > 0.0 && edgeDistance.x < 1.0e-4) ||
                    (edgeDistance.y > 0.0 && edgeDistance.y < 1.0e-4))
                    break;
                return target;
            }
        }
    }
    float bestDistance = uintBitsToFloat(0x7f800000u);
    uint selected = 0xffffffffu;
    vec2 bestDelta = vec2(0);
    for (uint target = 0u; target < chartCount; ++target)
    {
        const Chart chart = charts.entries[target];
        precise vec2 mapped = vec2(
            (chart.rowU.x * uv.x + chart.rowU.y * uv.y) + chart.rowU.z,
            (chart.rowV.x * uv.x + chart.rowV.y * uv.y) + chart.rowV.z);
        const vec2 delta = clamp(mapped, 0.0, 1.0) - mapped;
        precise vec2 componentDelta = vec2(
            chart.metricU.x * delta.x + chart.metricU.y * delta.y,
            chart.metricV.x * delta.x + chart.metricV.y * delta.y);
        precise float distanceSquared =
            componentDelta.x * componentDelta.x + componentDelta.y * componentDelta.y;
        precise float difference = (componentDelta.x - bestDelta.x) *
            (componentDelta.x + bestDelta.x) +
            (componentDelta.y - bestDelta.y) * (componentDelta.y + bestDelta.y);
        if (distanceSquared < bestDistance ||
            (distanceSquared == bestDistance && difference < -bestDistance * 2.220446049250313e-16))
        {
            bestDistance = distanceSquared;
            selected = target;
            bestDelta = componentDelta;
        }
    }
    return selected;
}

#endif
