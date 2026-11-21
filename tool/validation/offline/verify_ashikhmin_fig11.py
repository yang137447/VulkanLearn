"""Check the shared Fig.11 controls before comparing Satin and Velvet appearance."""

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw
from verify_vl_export import verify


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("satin", type=Path)
    parser.add_argument("velvet", type=Path)
    parser.add_argument("--expected-light-exposure", type=float, default=5.0)
    args = parser.parse_args()
    sources = [args.satin.resolve(), args.velvet.resolve()]
    for source in sources:
        verify(source)
    manifests = [json.loads((source / "manifest.json").read_text()) for source in sources]
    for field in ["camera", "lights", "integrator", "display", "environment", "modelImplementation"]:
        require(manifests[0][field] == manifests[1][field], f"Different {field}")
    require(all(manifest["complete"] for manifest in manifests), "Incomplete films")
    require(manifests[0]["samples"] == manifests[1]["samples"], "Different sample counts")
    require(manifests[0]["integrator"]["bounces"] == 0, "Expected direct-only transport")
    require(sorted(manifests[0]["meshDigests"].values()) ==
            sorted(manifests[1]["meshDigests"].values()), "Different source geometry")
    require([draw["model"] for draw in manifests[0]["draws"]] ==
            [draw["model"] for draw in manifests[1]["draws"]], "Different transforms")
    require(math.isclose(manifests[0]["lights"][0]["intensity"] *
                         manifests[0]["display"]["exposure"],
                         args.expected_light_exposure, rel_tol=1e-6),
            "Display calibration differs from the historical shared reference")
    for manifest, model in zip(manifests, ["AshikhminSatin", "AshikhminVelvet"]):
        matches = [material for material in manifest["materials"].values()
                   if material["offlineModel"] == model]
        require(len(matches) == 1, f"Expected one {model}")
        material = matches[0]
        require(material["parameters"]["u_baseColor"][:3] == [0.002] * 3,
                "Different diffuse tint")
        require(material["parameters"]["u_f0"] == 0.05, "Different Fresnel")
        require(not material["textures"], "Unexpected texture modulation")
        expected = ({"u_sigmaFlat": 0.1, "u_sigmaEnds": 0.3, "u_flatWeight": 0.7}
                    if model == "AshikhminSatin" else {"u_sigma": 0.5})
        for key, value in expected.items():
            require(material["parameters"][key] == value, f"Unexpected {key}")
    views = [Image.open(source / "combined.ppm").convert("RGB") for source in sources]
    width, height = views[0].size
    crop = (round(width * 420 / 1280), round(height * 170 / 720),
            round(width * 860 / 1280), round(height * 690 / 720))
    statistics = []
    for view in views:
        background = view.getpixel((0, 0))
        levels = sorted(sum(pixel) / 3 for pixel in view.crop(crop).getdata()
                        if max(abs(channel - ref) for channel, ref in zip(pixel, background)) > 3)
        require(bool(levels), "Empty cloth mask")
        statistics.append({"mean": sum(levels) / len(levels),
                           "p95": levels[int((len(levels) - 1) * 0.95)],
                           "p99": levels[int((len(levels) - 1) * 0.99)]})
    require(statistics[0]["p95"] > statistics[1]["p95"],
            "Satin bright-band percentile does not exceed Velvet")
    tile_width, tile_height = crop[2] - crop[0], crop[3] - crop[1]
    comparison = Image.new("RGB", (tile_width * 2, tile_height + 32), (245, 245, 245))
    draw = ImageDraw.Draw(comparison)
    for index, (view, label) in enumerate(zip(views, ["Satin", "Velvet"])):
        comparison.paste(view.crop(crop), (index * tile_width, 32))
        draw.text((index * tile_width + 10, 8), label, fill="black")
    output = sources[0].parent / "Comparisons"
    output.mkdir(exist_ok=True)
    comparison.save(output / "ashikhmin2000_fig11_satin_velvet_current.png")
    record = {"sources": [str(source) for source in sources],
              "statistics": dict(zip(["satin", "velvet"], statistics)),
              "crop": crop, "retouched": False,
              "claim": "Shared-control local appearance; not paper pixel-level reproduction"}
    (output / "ashikhmin2000_fig11_satin_velvet_current.json").write_text(json.dumps(record, indent=2))
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
