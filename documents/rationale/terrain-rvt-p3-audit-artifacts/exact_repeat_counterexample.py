import json
import math
from fractions import Fraction


def fraction(value):
    return {"exact": str(value), "decimal": float(value)}


def fractional(value):
    return value - math.floor(value)


def support(position):
    first = math.floor(position)
    remainder = position - first
    result = [(first, 1 - remainder)]
    if remainder:
        result.append((first + 1, remainder))
    return result


def reproduce():
    resolution = (129, 129)
    center_uv = (Fraction(515, 516), Fraction(1, 2))
    uv_dx = (Fraction(12, 129), Fraction(0))
    uv_dy = (Fraction(0), Fraction(1, 129))
    delta_x = tuple(uv_dx[axis] * resolution[axis] for axis in range(2))
    delta_y = tuple(uv_dy[axis] * resolution[axis] for axis in range(2))
    axis_x = delta_x[0] ** 2 + delta_y[0] ** 2
    axis_y = delta_x[1] ** 2 + delta_y[1] ** 2
    cross_axis = delta_x[0] * delta_x[1] + delta_y[0] * delta_y[1]
    discriminant = math.hypot(axis_x - axis_y, 2 * cross_axis)
    major_squared = max((axis_x + axis_y + discriminant) / 2, 1e-12)
    major = math.sqrt(major_squared)
    determinant = delta_x[0] * delta_y[1] - delta_x[1] * delta_y[0]
    minor = abs(determinant) / major
    first_axis = (cross_axis, major_squared - axis_x)
    second_axis = (major_squared - axis_y, cross_axis)
    direction = max((first_axis, second_axis), key=lambda vector: sum(
        component * component for component in vector))
    direction_length = math.hypot(*direction)
    direction = tuple(component / direction_length for component in direction)
    assert major == 12 and minor == 1 and direction == (1, 0)
    major_uv = (Fraction(12, 129), Fraction(0))
    max_mip = math.floor(math.log2(max(resolution)))
    filtered_minor = max(minor, major / 8)
    target_mip = min(max(math.log2(max(filtered_minor, 1e-6)), 0), max_mip)
    tap_count = min(max(math.ceil(major / max(filtered_minor, 1)), 1), 8)
    low_mip = math.floor(target_mip)
    high_mip = min(low_mip + 1, max_mip)
    mip_blend = fractional(target_mip)
    records = []
    for tap_index in range(tap_count):
        offset = Fraction(2 * tap_index + 1, 2 * tap_count) - Fraction(1, 2)
        tap_uv = tuple(center_uv[axis] + major_uv[axis] * offset for axis in range(2))
        delta_uv = tuple(tap_uv[axis] - center_uv[axis] for axis in range(2))
        assert all(abs(value) < Fraction(1, 2) for value in delta_uv)
        delta_uv = tuple(value - round(value) for value in delta_uv)
        for mip, mip_weight in ((low_mip, 1 - mip_blend), (high_mip, mip_blend)):
            extent = tuple(max(1, value >> mip) for value in resolution)
            center_texel = tuple(fractional(center_uv[axis]) * extent[axis]
                                 for axis in range(2))
            page = tuple(math.floor(value / 128) for value in center_texel)
            local = tuple(center_texel[axis] + delta_uv[axis] * extent[axis]
                          - page[axis] * 128 for axis in range(2))
            anchored = all(Fraction(-7, 2) <= value <= Fraction(263, 2) for value in local)
            center_page = page
            anchored_local = local
            if not anchored:
                tap_texel = tuple(fractional(tap_uv[axis]) * extent[axis] for axis in range(2))
                page = tuple(math.floor(value / 128) for value in tap_texel)
                local = tuple(tap_texel[axis] - page[axis] * 128 for axis in range(2))
            valid = tuple(min(128, extent[axis] - page[axis] * 128) for axis in range(2))
            evaluation_extent = tuple(value + 8 for value in valid)
            sampler_position = tuple(value + Fraction(7, 2) for value in local)
            bilinear_taps = []
            for stored_x, weight_x in support(sampler_position[0]):
                for stored_y, weight_y in support(sampler_position[1]):
                    stored = (stored_x, stored_y)
                    bilinear_weight = weight_x * weight_y
                    canonical_source = tuple(min(stored[axis], evaluation_extent[axis] - 1)
                                             for axis in range(2))
                    original_uv = tuple(fractional(Fraction(
                        page[axis] * 128 + stored[axis] - 4, extent[axis])
                        + Fraction(1, 2 * extent[axis])) for axis in range(2))
                    copied_uv = tuple(fractional(Fraction(
                        page[axis] * 128 + canonical_source[axis] - 4, extent[axis])
                        + Fraction(1, 2 * extent[axis])) for axis in range(2))
                    bilinear_taps.append({
                        "stored": stored,
                        "bilinearWeight": fraction(bilinear_weight),
                        "finalWeightIncludingVirtualMipAndEightTapAverage":
                            float(bilinear_weight) * mip_weight / tap_count,
                        "outsideHistoricalEvaluationDomain": stored != canonical_source,
                        "historicalCanonicalSource": canonical_source,
                        "originalTexelCenterUv": [fraction(value) for value in original_uv],
                        "copiedTexelCenterUv": [fraction(value) for value in copied_uv],
                    })
            assert sum(Fraction(item["bilinearWeight"]["exact"]) for item in bilinear_taps) == 1
            records.append({
                "tapIndex": tap_index,
                "tapUv": [fraction(value) for value in tap_uv],
                "wrappedDeltaUv": [fraction(value) for value in delta_uv],
                "virtualMip": mip,
                "virtualMipWeight": mip_weight,
                "extent": extent,
                "centerPage": center_page,
                "sampledPage": page,
                "valid": valid,
                "historicalEvaluationExtent": evaluation_extent,
                "anchored": anchored,
                "anchoredLocal": [fraction(value) for value in anchored_local],
                "local": [fraction(value) for value in local],
                "samplerPosition": [fraction(value) for value in sampler_position],
                "bilinearTaps": bilinear_taps,
            })
    witness = next(record for record in records
                   if record["tapIndex"] == 7 and record["virtualMip"] == 0)
    assert [item["stored"] for item in witness["bilinearTaps"]] == [(9, 68), (10, 68)]
    assert all(item["bilinearWeight"]["exact"] == "1/2" for item in witness["bilinearTaps"])
    assert all(item["outsideHistoricalEvaluationDomain"] for item in witness["bilinearTaps"])
    assert all(item["finalWeightIncludingVirtualMipAndEightTapAverage"] > 0
               for item in witness["bilinearTaps"])
    total_weight = sum(item["finalWeightIncludingVirtualMipAndEightTapAverage"]
                       for record in records for item in record["bilinearTaps"])
    assert math.isclose(total_weight, 1, abs_tol=1e-12)
    return {
        "status": "historical-counterexample-reproduced",
        "scope": "Independent consumer equations; historical unconditional valid+8 producer domain",
        "currentImplementationTested": False,
        "gpuExecuted": False,
        "assumptions": "Isolated Repeat surface; all selected pages at both requested mips resident",
        "resolution": resolution,
        "centerUv": [fraction(value) for value in center_uv],
        "uvDx": [fraction(value) for value in uv_dx],
        "uvDy": [fraction(value) for value in uv_dy],
        "major": major,
        "minor": minor,
        "filteredMinor": filtered_minor,
        "majorUv": [fraction(value) for value in major_uv],
        "targetMip": target_mip,
        "tapCount": tap_count,
        "witness": witness,
        "allTapMipRecords": records,
        "totalConsumerWeight": total_weight,
    }


if __name__ == "__main__":
    print(json.dumps(reproduce(), indent=2))
