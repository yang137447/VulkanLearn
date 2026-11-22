"""Author the fixed 128x128 lit ROI masks from a real TVQ1 quality capture.

CPU only. The masks are fixed *before* the replay: `terrainRoi` marks ROI pixels
whose mapped feedback texel carries a valid terrain claim and matching page
owner, and `silhouetteDomain` marks the interior of those claims (every
4-neighbour feedback texel claims the same surface), so silhouette XOR is
measured away from claim discontinuities.

Nothing here is derived from the replay output. `cameraViewProjectionBlake3`
is either derived from the real scene camera + capture-plan pose through the
engine's own matrix path (default, pre-registered) or copied from a completed
run's `native-lit-replay.json` (`--view-projection-source observed`, identity
rebinding only; the mask arrays never change).
"""

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np
import terrain_lit_evidence as T


def build_masks(capture):
    width, height = T.RECT[2], T.RECT[3]
    terrain_roi = np.zeros((height, width), dtype=np.bool_)
    surface_id = np.full((height, width), -1, dtype=np.int32)
    mapped = np.zeros((height, width, 2), dtype=np.int32)
    for y in range(height):
        for x in range(width):
            fx, fy = T.roi_pixel_texel(x, y, capture.render_extent())
            mapped[y, x] = (fx, fy)
            valid, record = capture.valid_pixel(fx, fy)
            if valid:
                terrain_roi[y, x] = True
                surface_id[y, x] = record["surfaceIndex"]
    silhouette = np.zeros_like(terrain_roi)
    for y in range(height):
        for x in range(width):
            if not terrain_roi[y, x]:
                continue
            fx, fy = mapped[y, x]
            current = surface_id[y, x]
            interior = True
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                neighbour_valid, neighbour = capture.valid_pixel(fx + dx, fy + dy)
                if not neighbour_valid or neighbour["surfaceIndex"] != int(current):
                    interior = False
                    break
            silhouette[y, x] = interior
    return terrain_roi, silhouette, mapped


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, required=True,
                        help="lit-case-plan.json produced by prepare_lit_case.py")
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--view-projection-source", choices=("engine", "observed"), default="engine")
    parser.add_argument("--native-metadata", type=Path, default=None)
    parser.add_argument("--fov-degrees", type=float, default=None)
    parser.add_argument("--near", type=float, default=None)
    parser.add_argument("--far", type=float, default=None)
    options = parser.parse_args()
    plan = T.read(options.plan)
    name = options.name or Path(options.plan).stem.replace("lit-case-plan", "masks")
    output_directory = options.output or (Path(plan["caseDirectory"]).parent / ("masks-" + name))
    output_directory.mkdir(parents=True, exist_ok=True)
    capture = T.QualityCapture(plan["captureBinary"], plan["capture"])

    camera_entry = None
    scene_file = Path(T.read(Path(T.ASSET_ROOT) / "config" / "config.json")["initScene"])
    for obj in T.read(Path(T.ASSET_ROOT) / "resources" / scene_file)["objects"]:
        if obj.get("type") == "camera":
            camera_entry = obj
            break
    if camera_entry is None:
        raise ValueError("Scene has no camera object; cannot derive the view-projection")
    fov = options.fov_degrees if options.fov_degrees is not None else float(camera_entry.get("fov", 60.0))
    near = options.near if options.near is not None else float(camera_entry.get("near_clip", 1.0))
    far = options.far if options.far is not None else float(camera_entry.get("far_clip", 10000.0))
    aspect = float(T.FULL_VIEWPORT[0]) / float(T.FULL_VIEWPORT[1])
    engine = T.engine_view_projection(plan["posePosition"], plan["poseLookAt"], fov, aspect, near, far)
    blake = T.Blake3()
    derived = T.blake3_of_float32(blake, engine["viewProjectionColumnMajor"])
    if options.view_projection_source == "observed":
        if not options.native_metadata:
            raise ValueError("observed view-projection requires --native-metadata")
        observed = T.read(options.native_metadata)
        view_projection_blake3 = observed["cameraViewProjectionBlake3"]
        source = "observed:" + str(Path(options.native_metadata).resolve())
    else:
        view_projection_blake3 = derived
        source = "engine:scene-camera+capture-plan-pose"

    terrain_roi, silhouette, mapped = build_masks(capture)
    if not terrain_roi.any():
        raise ValueError("Fixed terrain ROI is empty at this pose")
    if not silhouette.any():
        raise ValueError("Fixed silhouette domain is empty at this pose")
    payload = output_directory / "masks.npz"
    np.savez(payload, terrainRoi=terrain_roi, silhouetteDomain=silhouette)
    masks = {
        "rect": list(T.RECT),
        "cameraViewProjectionBlake3": view_projection_blake3,
        "path": payload.name,
        "sha256": T.sha256(payload),
        "deviceDepthClear": 1.0,
        # Independent provenance (compare_native_lit_roi.py reads only the keys above).
        "terrainRoiPixels": int(terrain_roi.sum()),
        "silhouetteDomainPixels": int(silhouette.sum()),
        "viewProjectionSource": source,
        "engineDerivedViewProjectionBlake3": derived,
        "cameraViewProjection": engine["viewProjectionColumnMajor"],
        "camera": {"position": plan["posePosition"], "look_at": plan["poseLookAt"],
                   "fovDegrees": fov, "aspect": aspect, "near": near, "far": far},
        "pose": plan["pose"], "poseId": plan["poseId"],
        "qualityCapture": plan["capture"],
        "qualityCapturePayloadSha256": T.sha256(plan["captureBinary"]),
        "mapping": "ROI pixel (x,y) -> feedback texel floor((rect.xy + p + 0.5) * 128 / renderExtent)",
        "terrainRoiCriterion": ("mapped feedback texel has one visible terrain claim and every tap "
                                "resolves to an owner row matching its surface/mip/page/generation/"
                                "revision/content identity"),
        "silhouetteDomainCriterion": ("terrainRoi pixels whose four 4-neighbour feedback texels are "
                                      "valid claims of the same surface; excludes claim/surface "
                                      "discontinuities within one 128-domain texel"),
        "silhouetteDomainLimitation": ("claim-topology edge criterion, not a depth-derivative edge; a "
                                       "depth-derived domain would have to be authored from an "
                                       "independent depth source, not from the replay"),
        "gpuExecuted": False,
    }
    target = output_directory / "masks.json"
    T.write(target, masks)
    print(str(target))


if __name__ == "__main__":
    main()
