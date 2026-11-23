"""Write and validate VLP2 sidecar manifests for per-tile VLP1 paints.

The VLP1 binary layout is unchanged.  A sidecar lives next to each
``*_paint.vlpaint`` as ``<paint>.meta.json`` and records the tile identity,
layer order, feather width and content hash so offline tools can validate a
tile set without loading the runtime.

Usage:
    python tool/terrain-houdini/paint_manifest.py write <Generated/terrain-set> [--feather 4]
    python tool/terrain-houdini/paint_manifest.py validate <Generated/terrain-set>
"""

import argparse
import hashlib
import json
import math
import os
import struct
import sys

import numpy


MANIFEST_VERSION = 2
MANIFEST_KIND = "terrainLayerWeights"
VLP1_HEADER_BYTES = 24
VLP1_MAGIC = b"VLP1"
VLP1_MAX_FILE_BYTES = 512 * 1024 * 1024
SUM_TOLERANCE = 1.0e-3
BORDER_TOLERANCE = 0.01
GRADIENT_THRESHOLD = 0.5
GRADIENT_PIXEL_RATIO_LIMIT = 0.005


class PaintManifestError(RuntimeError):
    pass


def tile_index(name):
    if not name.startswith("tile_"):
        raise PaintManifestError("not a tile directory: %s" % name)
    try:
        return int(name[5:])
    except ValueError:
        raise PaintManifestError("not a tile directory: %s" % name)


def tile_directories(root):
    names = [
        name for name in os.listdir(root)
        if name.startswith("tile_") and os.path.isdir(os.path.join(root, name))
    ]
    if not names:
        raise PaintManifestError("no tile_* directories under %s" % root)
    names.sort(key=tile_index)
    indices = [tile_index(name) for name in names]
    if indices != list(range(len(names))):
        raise PaintManifestError("tile indices are not contiguous from 0 under %s" % root)
    grid = int(round(math.sqrt(len(names))))
    if grid * grid != len(names):
        raise PaintManifestError("expected a square tile grid, got %d tiles" % len(names))
    return names, grid


def find_tr(tile_dir):
    names = sorted(
        name for name in os.listdir(tile_dir)
        if name.startswith("TR_") and name.endswith(".json")
    )
    if len(names) != 1:
        raise PaintManifestError(
            "expected exactly one TR_*.json in %s, found %d" % (tile_dir, len(names))
        )
    path = os.path.join(tile_dir, names[0])
    with open(path, encoding="utf-8") as handle:
        return json.load(handle), path


def tr_asset_path(tr_path, tile_dir):
    """Best-effort runtime assetPath for the ``<assetPath>#<name>`` fallback."""
    set_root = os.path.abspath(os.path.dirname(os.path.abspath(tile_dir)))
    parent = os.path.dirname(set_root)
    if os.path.basename(parent).lower() == "generated":
        resource_root = os.path.dirname(parent)
    else:
        resource_root = parent
    return os.path.relpath(os.path.abspath(tr_path), resource_root).replace(os.sep, "/")


def layer_keys(document, tr_path, tile_dir):
    fallback_asset = tr_asset_path(tr_path, tile_dir)
    keys = []
    for layer in document.get("layers", []):
        key = str(layer.get("key", "") or "")
        if not key:
            key = "%s#%s" % (fallback_asset, layer.get("name", ""))
        keys.append(key)
    return keys


def first_layer_key_mismatch(actual, expected):
    if not isinstance(actual, list):
        return 0, expected[0] if expected else None, actual
    for index in range(min(len(actual), len(expected))):
        if actual[index] != expected[index]:
            return index, expected[index], actual[index]
    if len(actual) != len(expected):
        index = min(len(actual), len(expected))
        expected_value = expected[index] if index < len(expected) else None
        actual_value = actual[index] if index < len(actual) else None
        return index, expected_value, actual_value
    return None


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1 << 20)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def read_vlp1_header(path):
    with open(path, "rb") as handle:
        header = handle.read(VLP1_HEADER_BYTES)
    if len(header) != VLP1_HEADER_BYTES:
        raise PaintManifestError("%s: truncated VLP1 header" % path)
    if header[:4] != VLP1_MAGIC:
        raise PaintManifestError("%s: invalid magic (expected VLP1)" % path)
    width, height, layer_count, flags, reserved = struct.unpack_from("<5I", header, 4)
    if width != height:
        raise PaintManifestError("%s: width and height must match" % path)
    if width < 2 or width > 4096:
        raise PaintManifestError("%s: invalid dimension %d" % (path, width))
    if layer_count < 1 or layer_count > 256:
        raise PaintManifestError("%s: invalid layerCount %d" % (path, layer_count))
    if flags != 0 or reserved != 0:
        raise PaintManifestError("%s: VLP1 flags/reserved must be zero" % path)
    expected = VLP1_HEADER_BYTES + width * height * layer_count * 5
    if expected > VLP1_MAX_FILE_BYTES:
        raise PaintManifestError(
            "%s: header-declared file size %d exceeds the 512 MiB paint budget"
            % (path, expected)
        )
    actual = os.path.getsize(path)
    if actual != expected:
        raise PaintManifestError(
            "%s: file size %d does not match header-declared %d" % (path, actual, expected)
        )
    return {
        "width": width,
        "height": height,
        "layerCount": layer_count,
    }


def load_weights(path, width, height, layer_count):
    count = width * height * layer_count
    weights = numpy.fromfile(path, dtype="<f4", count=count, offset=VLP1_HEADER_BYTES)
    if weights.size != count:
        raise PaintManifestError("%s: truncated VLP1 weight payload" % path)
    return weights.reshape(layer_count, height, width)


def write_json_atomic(path, document):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(document, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temporary, path)


def write_manifest_for_tile(tile_dir, paint_path, feather):
    document, tr_path = find_tr(tile_dir)
    keys = layer_keys(document, tr_path, tile_dir)
    header = read_vlp1_header(paint_path)
    if header["layerCount"] != len(keys):
        raise PaintManifestError(
            "%s: VLP1 layerCount %d does not match TR layer count %d"
            % (paint_path, header["layerCount"], len(keys))
        )

    index = tile_index(os.path.basename(os.path.abspath(tile_dir)))
    _, grid = tile_directories(os.path.dirname(os.path.abspath(tile_dir)))
    row, col = divmod(index, grid)
    manifest = {
        "version": MANIFEST_VERSION,
        "kind": MANIFEST_KIND,
        "tileX": col,
        "tileY": row,
        "tileGrid": grid,
        "width": header["width"],
        "height": header["height"],
        "layerCount": header["layerCount"],
        "layerKeys": keys,
        "featherTexels": float(feather),
        "contentSha256": sha256_file(paint_path),
    }
    manifest_path = paint_path + ".meta.json"
    write_json_atomic(manifest_path, manifest)
    print(
        "tile_%02d: wrote %s (layers=%d res=%d feather=%.1f)"
        % (index, os.path.basename(manifest_path), len(keys),
           header["width"], float(feather))
    )
    return manifest_path


def tile_paints(tile_dir):
    source_dir = os.path.join(tile_dir, "Source")
    if not os.path.isdir(source_dir):
        return []
    return sorted(
        os.path.join(source_dir, name)
        for name in os.listdir(source_dir)
        if name.endswith("_paint.vlpaint")
    )


def write_command(root, feather):
    root = os.path.abspath(root)
    tiles, _ = tile_directories(root)
    wrote = 0
    for tile in tiles:
        tile_dir = os.path.join(root, tile)
        for paint in tile_paints(tile_dir):
            write_manifest_for_tile(tile_dir, paint, feather)
            wrote += 1
    if wrote == 0:
        raise PaintManifestError("no *_paint.vlpaint files under %s" % root)
    print("wrote %d manifest(s) under %s" % (wrote, root))
    return 0


def gradient_pixel_ratio(weights):
    horizontal = numpy.abs(numpy.diff(weights, axis=1)) > GRADIENT_THRESHOLD
    vertical = numpy.abs(numpy.diff(weights, axis=0)) > GRADIENT_THRESHOLD
    horizontal_pairs = weights.shape[0] * (weights.shape[1] - 1)
    vertical_pairs = (weights.shape[0] - 1) * weights.shape[1]
    pairs = horizontal_pairs + vertical_pairs
    if pairs == 0:
        return 1.0
    return float(numpy.count_nonzero(horizontal) + numpy.count_nonzero(vertical)) / float(pairs)


def validate_command(root):
    root = os.path.abspath(root)
    tiles, grid = tile_directories(root)
    errors = []
    borders = {}
    max_border_difference = 0.0
    layer_total = 0

    for tile in tiles:
        index = tile_index(tile)
        row, col = divmod(index, grid)
        tile_dir = os.path.join(root, tile)

        try:
            document, tr_path = find_tr(tile_dir)
            expected_keys = layer_keys(document, tr_path, tile_dir)
        except PaintManifestError as error:
            errors.append("%s: %s" % (tile, error))
            continue

        paints = tile_paints(tile_dir)
        if not paints:
            errors.append("%s: no *_paint.vlpaint under Source/" % tile)
            continue
        if len(paints) > 1:
            errors.append("%s: expected one *_paint.vlpaint, found %d" % (tile, len(paints)))
            continue
        paint_path = paints[0]
        manifest_path = paint_path + ".meta.json"
        if not os.path.exists(manifest_path):
            errors.append("%s: missing manifest %s" % (tile, os.path.basename(manifest_path)))
            continue

        try:
            with open(manifest_path, encoding="utf-8") as handle:
                manifest = json.load(handle)
        except (OSError, ValueError) as error:
            errors.append("%s: cannot read manifest: %s" % (tile, error))
            continue

        if manifest.get("version") != MANIFEST_VERSION:
            errors.append("%s: manifest version %r != %d"
                          % (tile, manifest.get("version"), MANIFEST_VERSION))
        if manifest.get("kind") != MANIFEST_KIND:
            errors.append("%s: manifest kind %r != %r"
                          % (tile, manifest.get("kind"), MANIFEST_KIND))
        if manifest.get("tileX") != col or manifest.get("tileY") != row:
            errors.append("%s: tileX/tileY %r/%r != expected %d/%d"
                          % (tile, manifest.get("tileX"), manifest.get("tileY"), col, row))
        if manifest.get("tileGrid") != grid:
            errors.append("%s: tileGrid %r != %d" % (tile, manifest.get("tileGrid"), grid))

        try:
            header = read_vlp1_header(paint_path)
        except (OSError, PaintManifestError) as error:
            errors.append("%s: %s" % (tile, error))
            continue

        for field in ("width", "height", "layerCount"):
            if manifest.get(field) != header[field]:
                errors.append(
                    "%s: manifest %s %r != VLP1 header %d"
                    % (tile, field, manifest.get(field), header[field])
                )

        mismatch = first_layer_key_mismatch(manifest.get("layerKeys"), expected_keys)
        if mismatch is not None:
            layer_index, expected_value, actual_value = mismatch
            errors.append(
                "%s layer %d: layerKeys mismatch expected=%r got=%r"
                % (tile, layer_index, expected_value, actual_value)
            )

        actual_sha = sha256_file(paint_path)
        if manifest.get("contentSha256") != actual_sha:
            errors.append("%s: contentSha256 does not match file" % tile)

        if header["layerCount"] != len(expected_keys):
            errors.append("%s: VLP1 layerCount %d != TR layer count %d"
                          % (tile, header["layerCount"], len(expected_keys)))
            continue

        try:
            weights = load_weights(
                paint_path, header["width"], header["height"], header["layerCount"]
            )
        except (OSError, PaintManifestError) as error:
            errors.append("%s: %s" % (tile, error))
            continue

        sums = numpy.sum(weights, axis=0)
        sum_error = float(numpy.max(numpy.abs(sums - 1.0)))
        if sum_error > SUM_TOLERANCE:
            bad = numpy.argwhere(numpy.abs(sums - 1.0) > SUM_TOLERANCE)
            x = int(bad[0][1])
            y = int(bad[0][0])
            errors.append(
                "%s: layer weight sum error %.6f > %.6f at texel (%d, %d)"
                % (tile, sum_error, SUM_TOLERANCE, x, y)
            )

        ratios = []
        for layer_index, name in enumerate(expected_keys):
            ratio = gradient_pixel_ratio(weights[layer_index])
            ratios.append(ratio)
            if ratio >= GRADIENT_PIXEL_RATIO_LIMIT:
                errors.append(
                    "%s layer %d (%s): gradient>%.1f pixel ratio %.3f%% >= %.3f%%"
                    % (tile, layer_index, name, GRADIENT_THRESHOLD,
                       ratio * 100.0, GRADIENT_PIXEL_RATIO_LIMIT * 100.0)
                )
        layer_total += len(ratios)
        print(
            "tile_%02d: sum_err=%.6f transition_max=%.4f%%"
            % (index, sum_error, max(ratios) * 100.0 if ratios else 0.0)
        )

        borders[index] = {
            "left": weights[:, :, 0].copy(),
            "right": weights[:, :, -1].copy(),
            "top": weights[:, 0, :].copy(),
            "bottom": weights[:, -1, :].copy(),
        }

    for index in sorted(borders):
        row, col = divmod(index, grid)
        current = borders[index]
        if col + 1 < grid and index + 1 in borders:
            difference = numpy.max(
                numpy.abs(current["right"] - borders[index + 1]["left"])
            )
            max_border_difference = max(max_border_difference, float(difference))
            if difference >= BORDER_TOLERANCE:
                layer = int(numpy.argmax(numpy.max(
                    numpy.abs(current["right"] - borders[index + 1]["left"]), axis=1
                )))
                errors.append(
                    "tile_%02d/tile_%02d layer %d: horizontal border max diff %.6f >= %.3f"
                    % (index, index + 1, layer, float(difference), BORDER_TOLERANCE)
                )
        if row + 1 < grid and index + grid in borders:
            difference = numpy.max(
                numpy.abs(current["bottom"] - borders[index + grid]["top"])
            )
            max_border_difference = max(max_border_difference, float(difference))
            if difference >= BORDER_TOLERANCE:
                layer = int(numpy.argmax(numpy.max(
                    numpy.abs(current["bottom"] - borders[index + grid]["top"]), axis=1
                )))
                errors.append(
                    "tile_%02d/tile_%02d layer %d: vertical border max diff %.6f >= %.3f"
                    % (index, index + grid, layer, float(difference), BORDER_TOLERANCE)
                )

    print(
        "validated %d tile(s), %d layer(s); max border diff=%.6f"
        % (len(borders), layer_total, max_border_difference)
    )
    if errors:
        print("FAILED:", file=sys.stderr)
        for error in errors:
            print("  " + error, file=sys.stderr)
        return 1
    print("OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    write_parser = subparsers.add_parser("write", help="write VLP2 sidecars")
    write_parser.add_argument("root", help="Generated/<terrain set> directory with tile_XX/")
    write_parser.add_argument("--feather", type=float, default=4.0,
                              help="recorded feather width in texels")

    validate_parser = subparsers.add_parser("validate", help="validate VLP2 sidecars")
    validate_parser.add_argument("root", help="Generated/<terrain set> directory with tile_XX/")

    args = parser.parse_args(argv)
    try:
        if args.command == "write":
            return write_command(args.root, args.feather)
        return validate_command(args.root)
    except PaintManifestError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
