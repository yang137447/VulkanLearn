import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import struct


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def decode(value):
    return value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4


def encode(value):
    return 12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055


def rounded_byte(value):
    return math.floor(255 * encode(value) + .5)


def coverage(uv):
    position = [min(127., max(0., (offset + coordinate * scale) * 128 - .5))
                for coordinate, offset, scale in zip(uv, (.125, .25), (.75, .5))]
    base = [math.floor(value) for value in position]
    fraction = [value - integer for value, integer in zip(position, base)]
    mass = {}
    taps = []
    for vertical in range(2):
        for horizontal in range(2):
            factor = (fraction[0] if horizontal else 1 - fraction[0]) * (
                fraction[1] if vertical else 1 - fraction[1])
            column = min(127, base[0] + horizontal)
            row = min(127, base[1] + vertical)
            ids = (19, 3, 7) if (column // 16 + row // 16) % 2 else (3, 7, 19)
            for identity, weight in zip(ids, (64 / 255, 32 / 255, 159 / 255)):
                mass[identity] = mass.get(identity, 0.) + factor * weight
            if factor:
                taps.append({"texel": [column, row], "factor": factor, "ids": ids})
    total = sum(mass.values())
    return {identity: value / total for identity, value in mass.items()}, taps


def source_sample(uv, coordinate_precision=None, decode_precision=None):
    position = [coordinate * 32 - .5 for coordinate in uv]
    column, row = [math.floor(value) for value in position]
    fraction = [value - math.floor(value) for value in position]
    if coordinate_precision is not None:
        scale = 2 ** coordinate_precision
        fraction = [math.floor(value * scale + .5) / scale for value in fraction]
    result = 0.
    taps = []
    for vertical in range(2):
        for horizontal in range(2):
            weight = (fraction[0] if horizontal else 1 - fraction[0]) * (
                fraction[1] if vertical else 1 - fraction[1])
            selected = (column + horizontal) % 32
            code = 30 + selected * 6
            linear = decode(code / 255)
            if decode_precision is not None:
                scale = 2 ** decode_precision
                linear = math.floor(linear * scale + .5) / scale
            result += weight * linear
            if weight:
                taps.append({"texel": [selected, (row + vertical) % 32],
                             "sourceSrgbByte": code, "weight": weight, "linear": linear})
    return result, position, fraction, taps


def evaluate(uv, use_float=False, coordinate_precision=None, decode_precision=None):
    arithmetic = f32 if use_float else float
    weights, coverage_taps = coverage(uv)
    result = 0.
    layers = []
    for identity, weight in sorted(weights.items()):
        scale = 1 + (identity % 3) * .25
        angle = (identity % 3) * .25
        cosine, sine = arithmetic(math.cos(angle)), arithmetic(math.sin(angle))
        horizontal = arithmetic(arithmetic(cosine * uv[0]) - arithmetic(sine * uv[1]))
        vertical = arithmetic(arithmetic(sine * uv[0]) + arithmetic(cosine * uv[1]))
        layer_uv = [arithmetic(horizontal * scale), arithmetic(vertical * scale)]
        sample, position, fraction, taps = source_sample(
            layer_uv, coordinate_precision, decode_precision)
        result = arithmetic(result + arithmetic(sample * weight))
        layers.append({"globalId": identity, "sourceSlice": {19: 0, 3: 1, 7: 2}[identity],
                       "weight": weight, "layerUv": layer_uv, "sourcePosition": position,
                       "sourceFraction": fraction, "sampleLinear": sample, "taps": taps})
    return {"linear": result, "encodedContinuous": encode(result) * 255,
            "byte": rounded_byte(result), "coverageTaps": coverage_taps, "layers": layers}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    directory = options.run / "repeat-129-tail-consumer"
    actual = (directory / "atlas-0.bin").read_bytes()
    reference = (directory / "reference-0.bin").read_bytes()
    report = {"gpuExecuted": False, "source": str(options.run),
              "scope": "independent Python double reconstruction; rounding models are hypotheses, not measured GPU intermediates",
              "badPixels": [], "spirvMetrics": {}}
    for offset, (gpu, cpu) in enumerate(zip(actual[:221952], reference)):
        if abs(gpu - cpu) <= 1:
            continue
        channel = offset // 73984
        texel = (offset % 73984) // 4
        column, row = texel % 136, texel // 136
        raw = [Fraction(128 + column - 4, 129) + Fraction(1, 258),
               Fraction(row - 4, 129) + Fraction(1, 258)]
        uv = [float(value - math.floor(value)) for value in raw]
        double = evaluate(uv)
        gpu_uv = [f32(f32(f32(float(128 + column - 4)) + .5) / f32(129)),
                  f32(f32(f32(float(row - 4)) + .5) / f32(129))]
        gpu_uv = [f32(value - math.floor(value)) for value in gpu_uv]
        single = evaluate(gpu_uv, use_float=True)
        report["badPixels"].append({
            "stored": [column, row], "channel": channel, "component": offset % 4,
            "rawUvExact": [str(value) for value in raw], "wrappedUvDouble": uv,
            "wrappedUvFloat": gpu_uv, "gpuByte": gpu, "cppOracleByte": cpu,
            "independentDouble": double, "floatArithmeticModel": single,
            "coordinateQuantizationModels": {
                str(bits): evaluate(gpu_uv, True, bits)["byte"] for bits in (4, 6, 8, 10)},
            "srgbDecodeQuantizationModels": {
                str(bits): evaluate(gpu_uv, True, 8, bits)["byte"] for bits in (8, 10, 12, 16)},
        })
        if double["byte"] != cpu:
            raise RuntimeError("Independent double reconstruction disagrees with saved C++ oracle")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"badPixelCount": len(report["badPixels"]),
                      "pixels": [{"stored": item["stored"], "uv": item["wrappedUvDouble"],
                                  "gpu": item["gpuByte"], "cpp": item["cppOracleByte"],
                                  "double": item["independentDouble"]["byte"],
                                  "floatModel": item["floatArithmeticModel"]["byte"],
                                  "coord8Model": item["coordinateQuantizationModels"]["8"],
                                  "decodeModels": item["srgbDecodeQuantizationModels"]}
                                 for item in report["badPixels"]]}, indent=2))


if __name__ == "__main__":
    main()
