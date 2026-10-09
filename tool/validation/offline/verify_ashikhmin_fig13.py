"""Compare Fig. 13 black/red VL exports without retouching or reauthoring.

Run after rendering both scenes with the same samples:
  py -3 tool/validation/offline/verify_ashikhmin_fig13.py BLACK_EXPORT RED_EXPORT
This is an external paper-case check, not an engine runtime test.
"""

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from verify_vl_export import read_pfm, verify


def require(condition, label):
    if not condition:
        raise ValueError(label)


def cloth_material(manifest):
    matches = [m for m in manifest["materials"].values()
               if m["offlineModel"] == "AshikhminSlantedVelvet"]
    require(len(matches) == 1, "exactly one slanted Velvet material is required")
    return matches[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("black", type=Path)
    parser.add_argument("red", type=Path)
    args = parser.parse_args()
    sources = [args.black.resolve(), args.red.resolve()]
    for source in sources:
        verify(source)
    manifests = [json.loads((p / "manifest.json").read_text(encoding="utf-8"))
                 for p in sources]
    black, red = manifests
    matched = ("camera", "lights", "integrator", "display", "meshDigests",
               "environment", "modelImplementation")
    for key in matched:
        require(black[key] == red[key], f"mismatched {key}")
    require(black["complete"] and red["complete"], "partial exports are not comparisons")
    require(black["samples"] == red["samples"], "mismatched sample counts")
    require(black["integrator"]["bounces"] == 0, "Fig. 13 requires zero bounces")
    require(black["display"]["exposure"] == 1, "unexpected display exposure")
    require([(d["mesh"], d["model"], d["object"]) for d in black["draws"]] ==
            [(d["mesh"], d["model"], d["object"]) for d in red["draws"]],
            "geometry or transforms differ")
    require([m for m in black["materials"].values() if m["offlineModel"] == "Lambert"] ==
            [m for m in red["materials"].values() if m["offlineModel"] == "Lambert"],
            "background materials differ")

    materials = [cloth_material(m) for m in manifests]
    colors = [m["parameters"]["u_baseColor"][:3] for m in materials]
    for material in materials:
        params = material["parameters"]
        for key, expected in (("u_sigma", 0.5), ("u_slantDegrees", 40),
                              ("u_f0", 0.05), ("u_fiberRotationDegrees", 180)):
            require(params[key] == expected, f"unexpected paper/default orientation: {key}")
    controls = []
    for material in materials:
        item = dict(material)
        item.pop("name")
        item["parameters"] = dict(item["parameters"])
        item["parameters"].pop("u_baseColor")
        controls.append(item)
    require(controls[0] == controls[1], "cloth differs beyond its diffuse color")
    require(colors[0] == [0.002] * 3, "unexpected black diffuse reflectance")
    delta = [r - b for r, b in zip(colors[1], colors[0])]
    require(all(d > 0 for d in delta), "red must differ in all diffuse channels")

    forward = black["camera"]["forward"]
    direction = black["lights"][0]["direction"]
    cosine = sum(v * l for v, l in zip(forward, direction)) / math.sqrt(
        sum(v * v for v in forward) * sum(l * l for l in direction))
    angle = math.degrees(math.acos(max(-1, min(1, cosine))))
    require(angle < 1, "key light is not nearly parallel to the view axis")

    width, height, black_pixels = read_pfm(sources[0] / "combined.pfm")
    red_pixels = read_pfm(sources[1] / "combined.pfm")[2]
    # 共享白光、零反弹：红/黑差值只能来自 rho_d 的线性增量；
    # 若偷偷改变高光、曝光或背景，三个通道恢复的 diffuse kernel 会不一致。
    error = 0.0
    for index in range(0, len(black_pixels), 3):
        values = [(red_pixels[index + c] - black_pixels[index + c]) / delta[c]
                  for c in range(3)]
        error = max(error, max(values) - min(values))
    require(error < 1e-4, f"shared specular/diffuse linearity failed: {error}")

    views = [Image.open(p / "combined.ppm").convert("RGB") for p in sources]
    background = views[0].getpixel((0, 0))
    require(views[1].getpixel((0, 0)) == background, "background display differs")
    # 两臂保持同一裁切，不改曝光或色调；保留布面下缘和四周背景。
    crop = (round(width * 420 / 1280), round(height * 170 / 720),
            round(width * 860 / 1280), round(height * 690 / 720))
    bounds = ((0, 0, width, round(height * 0.25)),
              (0, 0, round(width * 0.30), height),
              (round(width * 0.70), 0, width, height),
              (0, round(height * 0.95), width, height))
    for view in views:
        for box in bounds:
            require(all(lo == hi == value for (lo, hi), value in
                        zip(view.crop(box).getextrema(), background)),
                    f"projected shadow/background change in {box}")

    left_sum = right_sum = 0
    left_count = right_count = 0
    for y in range(crop[1], crop[3]):
        for x in range(crop[0], crop[2]):
            pixel = views[0].getpixel((x, y))
            if max(abs(v - b) for v, b in zip(pixel, background)) <= 3:
                continue
            value = sum(pixel) / 3
            if x < width // 2:
                left_sum += value
                left_count += 1
            else:
                right_sum += value
                right_count += 1
    left = left_sum / left_count
    right = right_sum / right_count
    require(left > right, "black Velvet is not brighter on the left")

    tile_width, tile_height = crop[2] - crop[0], crop[3] - crop[1]
    comparison = Image.new("RGB", (tile_width * 2 + 40, tile_height + 50),
                           (245, 245, 245))
    draw = ImageDraw.Draw(comparison)
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 22)
    for i, (view, label) in enumerate(zip(views, ("Slanted Velvet - Black",
                                               "Slanted Velvet - Red"))):
        x = 10 + i * (tile_width + 20)
        comparison.paste(view.crop(crop), (x, 40))
        draw.text((x + 5, 7), label, fill=(25, 25, 25), font=font)
    output = sources[0].parent / "Comparisons"
    require(sources[1].parent == sources[0].parent, "exports have different resource roots")
    output.mkdir(exist_ok=True)
    name = "ashikhmin2000_fig13_slanted_velvet_black_red"
    path = output / f"{name}.png"
    comparison.save(path)
    record = {
        "sources": [{"export": str(p), "inputDigest": m["inputDigest"]}
                    for p, m in zip(sources, manifests)],
        "matchedControls": list(matched) + ["geometry", "transforms", "background",
                                           "specularParameters", "fiberOrientation"],
        "diffuseColors": colors,
        "cameraKeyAxisAngleDegrees": angle,
        "linearDiffuseKernelMaxChannelError": error,
        "blackDisplayMean": {"left": left, "right": right},
        "uniformBackgroundBordersVerified": True,
        "crop": crop,
        "retouched": False,
        "claim": "Local directional appearance; not paper pixel-level reproduction",
    }
    (output / f"{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(f"Shared controls verified; view/key angle={angle:.4f} degrees, "
          f"left/right mean={left:.2f}/{right:.2f}\nComparison: {path}")


if __name__ == "__main__":
    main()
