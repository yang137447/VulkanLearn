"""Feather existing per-tile mask PNGs into VLP1 paints.

Migration helper for assets whose ``tile_XX/Source/mask_gn_<layer>_<i>.png``
were exported from Houdini without a transition width.  The per-tile masks are
first **stitched into one global field**, blurred and normalized there, and
only then split back into VLP1 tiles.  Blurring each tile independently would
extrapolate different values on the two sides of a shared border and leak a
visible seam (measured ~0.024 mean vs ~0.001 for interior neighbours).

The heightmap margin is read from each TR (``heightmapMargin``) and the mask
size is asserted against ``gridResolution + 2*margin + 1``.  Original paints
are saved as ``*.vlpaint.hard.bak`` on the first run; without ``--force`` an
already-feathered tile is skipped.

Usage:
    python tool/terrain-houdini/feather_tile_paints.py \
        <Generated/HoudiniPegasusIsland> [--feather 4] [--resolution 512] [--force]
"""

import argparse
import json
import os
import sys
import types

import numpy
from PIL import Image


def load_core():
    fake = types.ModuleType("hou")
    fake.OperationFailed = RuntimeError
    fake.pwd = lambda: None
    sys.modules["hou"] = fake
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "houdini_weightmap_export.py")
    namespace = {"__name__": "weightmap_core"}
    exec(
        compile(
            open(path, encoding="utf-8").read().replace("\nmain()\n", "\n"),
            path,
            "exec",
        ),
        namespace,
    )
    return namespace


def read_layer_names(tile_dir):
    for name in sorted(os.listdir(tile_dir)):
        if name.startswith("TR_") and name.endswith(".json"):
            document = json.loads(open(os.path.join(tile_dir, name), encoding="utf-8").read())
            return document, [layer["name"] for layer in document.get("layers", [])]
    raise RuntimeError("no TR_*.json in %s" % tile_dir)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("root", help="Generated/<terrain set> directory with tile_XX/")
    parser.add_argument("--feather", type=float, default=4.0, help="box-blur texels")
    parser.add_argument("--resolution", type=int, default=512, help="VLP1 tile side")
    parser.add_argument(
        "--margin",
        type=int,
        default=-1,
        help="mask margin texels (-1 = read heightmapMargin from each TR)",
    )
    parser.add_argument("--force", action="store_true", help="rewrite already feathered paints")
    args = parser.parse_args(argv)

    core = load_core()
    root = os.path.abspath(args.root)
    tiles = sorted(
        name for name in os.listdir(root)
        if name.startswith("tile_") and os.path.isdir(os.path.join(root, name))
    )
    if not tiles:
        raise SystemExit("no tile_* directories under %s" % root)

    grid = int(round(len(tiles) ** 0.5))
    if grid * grid != len(tiles):
        raise SystemExit("expected a square tile grid, got %d tiles" % len(tiles))

    # Read every tile first, then blur the stitched global field.  Blurring per
    # tile would extrapolate different values across a shared border and leak a
    # seam into the final weights.
    tile_data = {}
    layer_names = None
    interior_size = None
    for tile in tiles:
        index = int(tile.split("_")[1])
        source_dir = os.path.join(root, tile, "Source")
        if not os.path.isdir(source_dir):
            print("skip %s (no Source)" % tile)
            continue
        document, names = read_layer_names(os.path.join(root, tile))
        if not names:
            print("skip %s (TR has no layers)" % tile)
            continue
        if layer_names is None:
            layer_names = names
        elif names != layer_names:
            raise SystemExit("TR layer order differs between tiles")

        margin = args.margin
        if margin < 0:
            margin = int(document.get("heightmapMargin", 1))
        grid_resolution = int(document.get("gridResolution", 0))
        expected = grid_resolution + 2 * margin + 1

        masks = []
        for layer_index, name in enumerate(names):
            path = os.path.join(source_dir, "mask_gn_%s_%02d.png" % (name, layer_index))
            if not os.path.exists(path):
                raise SystemExit("missing %s" % path)
            array = numpy.asarray(Image.open(path)).astype(numpy.float32)
            if array.ndim == 3:
                array = array[..., 0]
            array = array / 65535.0
            if grid_resolution > 0 and array.shape != (expected, expected):
                raise SystemExit(
                    "%s mask is %dx%d, expected %dx%d (grid=%d margin=%d)"
                    % (
                        path,
                        array.shape[0],
                        array.shape[1],
                        expected,
                        expected,
                        grid_resolution,
                        margin,
                    )
                )
            masks.append(array[margin : array.shape[0] - margin, margin : array.shape[1] - margin])
        if interior_size is None:
            interior_size = masks[0].shape
        elif masks[0].shape != interior_size:
            raise SystemExit("tile interior sizes differ")
        tile_data[index] = {"source_dir": source_dir, "document": document, "masks": masks}

    if not tile_data or layer_names is None:
        raise SystemExit("no usable tiles")

    step = interior_size[0] - 1  # adjacent tiles share one boundary sample
    global_size = step * grid + 1
    global_layers = [
        numpy.zeros((global_size, global_size), dtype=numpy.float32) for _ in layer_names
    ]
    for index, data in tile_data.items():
        row, col = divmod(index, grid)
        y = row * step
        x = col * step
        for layer_index, mask in enumerate(data["masks"]):
            global_layers[layer_index][
                y : y + mask.shape[0], x : x + mask.shape[1]
            ] = mask

    if args.feather > 0:
        global_layers = [core["box_blur"](layer, args.feather) for layer in global_layers]
    total = numpy.maximum(numpy.sum(global_layers, axis=0), 1.0e-6)
    global_layers = [layer / total for layer in global_layers]
    empty = numpy.sum(global_layers, axis=0) < 1.0e-6
    if numpy.any(empty):
        global_layers[0][empty] = 1.0

    for index, data in tile_data.items():
        row, col = divmod(index, grid)
        y = row * step
        x = col * step
        weights = [
            core["resample"](
                layer[y : y + interior_size[0], x : x + interior_size[1]],
                args.resolution,
            )
            for layer in global_layers
        ]
        stack = numpy.stack(weights, axis=0)

        source_dir = data["source_dir"]
        document = data["document"]
        destination = os.path.join(
            source_dir,
            os.path.basename(str(document.get("paintPath", "terrain_tile_paint.vlpaint"))),
        )
        backup = destination + ".hard.bak"
        if os.path.exists(destination) and not os.path.exists(backup):
            os.replace(destination, backup)
        if os.path.exists(destination) and os.path.exists(backup) and not args.force:
            print("skip tile_%02d (already feathered; use --force)" % index)
            continue
        core["write_vlpaint"](destination, stack)
        print(
            "tile_%02d layers=%d res=%d feather=%.1f sum=%.4f"
            % (index, stack.shape[0], args.resolution, args.feather,
               float(stack.sum(axis=0).mean()))
        )


if __name__ == "__main__":
    main()
