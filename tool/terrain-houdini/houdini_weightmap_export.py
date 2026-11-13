"""Heightfield -> per-layer weightmaps (+ VLP1 paint) exporter.

Run this from a Houdini *Python SOP*:

    exec(open(r"<repo>/tool/terrain-houdini/houdini_weightmap_export.py").read())

Input 0 must be a heightfield / volume geometry.  Every volume whose name is
listed in ``layer_spec`` becomes one terrain layer:

    layer_spec = "Gravel Ground:mask, Rock Face:bedrock, River Water:water"

Leave ``layer_spec`` empty to use every volume except ``height`` and ``id``
(in primitive order), naming layers after the volumes.

``id_mode`` instead decodes a single integer id volume (the
``PE_Heightfield_Layers_to_ID`` style map) into one-hot layers, then feathers
and normalizes them.  ``id_step`` is the id byte step (17 for the Pegasus
16-slot maps: ids are 0, 17, 34, ...).  In id mode ``layer_spec`` maps output
names to id indices: ``"Gravel Ground:0, Rock Face:1, ..."``; leave it empty
to emit every id found in the volume.

Layer order matters.  Houdini layer volumes are *coverage*, and overlapping
coverage is resolved by an explicit composite order, exactly like the
`Replace/Union/Subtract/Intersect/Difference` chain in
`PE_Heightfield_Layers_to_ID`.  ``composite=Stack`` (default) applies the
"over" operator in list order: each layer keeps only the coverage not already
claimed by the layers above it, and the leftover goes to the bottom layer.
Every `layer_spec` item may carry its own threshold: `Name:volume:threshold`
(default ``threshold``), or `Name:volume:threshold:1` to invert the sample.
Use ``composite=Normalize`` only when the input volumes are already final,
non-overlapping weights.

Outputs (all optional, on the Export button):
  * ``<prefix>_<layer>.png``   16-bit grayscale weightmaps
  * ``<vlpaint_prefix>_tile_x_y.vlpaint``  VLP1 tiles consumed by
    ``TR_*.json -> paintPath`` (layer weights + opaque alpha)

The blur runs on the global weightmaps before tiling, so tile borders stay
continuous.  Blur radius is in source texels; with a 2016^2 map over 4032 m
one texel is 2 m, i.e. a 4-texel feather is an 8 m transition.
"""

import math
import os
import struct
import zlib

import hou
import numpy


DEFAULTS = {
    "export": 0,
    "output_dir": "$HIP/render/weightmaps",
    "prefix": "Layer",
    "layer_spec": "",
    "id_mode": 0,
    "id_layer": "id",
    "id_step": 17,
    "threshold": 0.5,
    "soft_width": 0.05,
    "composite": 0,
    "order": 1,
    "feather_texels": 4.0,
    "normalize": 1,
    "write_png": 1,
    "write_vlpaint": 0,
    "tile_grid": 4,
    "tile_resolution": 512,
    "vlpaint_prefix": "terrain_tile",
}


def ensure_parms(node):
    existing = {parm.name() for parm in node.parms()}
    templates = []
    if "export" not in existing:
        templates.append(
            hou.ToggleParmTemplate("export", "Export (one-shot)", default_value=DEFAULTS["export"])
        )
    if "output_dir" not in existing:
        templates.append(
            hou.StringParmTemplate(
                "output_dir", "Output Directory", 1, default_value=(DEFAULTS["output_dir"],)
            )
        )
    if "prefix" not in existing:
        templates.append(
            hou.StringParmTemplate("prefix", "File Prefix", 1, default_value=(DEFAULTS["prefix"],))
        )
    if "layer_spec" not in existing:
        templates.append(
            hou.StringParmTemplate(
                "layer_spec",
                "Layer Spec (out:volume, ...)",
                1,
                default_value=(DEFAULTS["layer_spec"],),
            )
        )
    if "id_mode" not in existing:
        templates.append(
            hou.ToggleParmTemplate("id_mode", "Decode ID Volume", default_value=DEFAULTS["id_mode"])
        )
    if "id_layer" not in existing:
        templates.append(
            hou.StringParmTemplate("id_layer", "ID Volume", 1, default_value=(DEFAULTS["id_layer"],))
        )
    if "id_step" not in existing:
        templates.append(
            hou.IntParmTemplate(
                "id_step", "ID Byte Step", 1, default_value=(DEFAULTS["id_step"],), min=1, max=255
            )
        )
    if "threshold" not in existing:
        templates.append(
            hou.FloatParmTemplate(
                "threshold",
                "Coverage Threshold",
                1,
                default_value=(DEFAULTS["threshold"],),
                min=0.0,
                max=1.0,
            )
        )
    if "soft_width" not in existing:
        templates.append(
            hou.FloatParmTemplate(
                "soft_width",
                "Threshold Soft Width",
                1,
                default_value=(DEFAULTS["soft_width"],),
                min=0.0,
                max=0.5,
            )
        )
    if "composite" not in existing:
        templates.append(
            hou.MenuParmTemplate(
                "composite",
                "Composite",
                ("auto", "stack", "normalize", "additive"),
                ("Auto", "Stack (ordered over)", "Normalize", "Additive"),
                default_value=DEFAULTS["composite"],
            )
        )
    if "order" not in existing:
        templates.append(
            hou.MenuParmTemplate(
                "order",
                "Stack Order",
                ("first_on_top", "last_on_top"),
                ("First On Top", "Last On Top"),
                default_value=DEFAULTS["order"],
            )
        )
    if "feather_texels" not in existing:
        templates.append(
            hou.FloatParmTemplate(
                "feather_texels",
                "Feather (texels)",
                1,
                default_value=(DEFAULTS["feather_texels"],),
                min=0.0,
                max=128.0,
            )
        )
    if "normalize" not in existing:
        templates.append(
            hou.ToggleParmTemplate("normalize", "Normalize", default_value=DEFAULTS["normalize"])
        )
    if "write_png" not in existing:
        templates.append(
            hou.ToggleParmTemplate("write_png", "Write Layer PNGs", default_value=DEFAULTS["write_png"])
        )
    if "write_vlpaint" not in existing:
        templates.append(
            hou.ToggleParmTemplate(
                "write_vlpaint", "Write VLP1 Tiles", default_value=DEFAULTS["write_vlpaint"]
            )
        )
    if "tile_grid" not in existing:
        templates.append(
            hou.IntParmTemplate(
                "tile_grid", "VLP1 Tile Grid", 1, default_value=(DEFAULTS["tile_grid"],), min=1, max=16
            )
        )
    if "tile_resolution" not in existing:
        templates.append(
            hou.IntParmTemplate(
                "tile_resolution",
                "VLP1 Tile Resolution",
                1,
                default_value=(DEFAULTS["tile_resolution"],),
                min=0,
                max=4096,
            )
        )
    if "vlpaint_prefix" not in existing:
        templates.append(
            hou.StringParmTemplate(
                "vlpaint_prefix", "VLP1 Prefix", 1, default_value=(DEFAULTS["vlpaint_prefix"],)
            )
        )
    if templates:
        for template in templates:
            node.addSpareParmTuple(template)


def value(node, name):
    parm = node.parm(name)
    if parm is None:
        return DEFAULTS[name]
    return parm.eval()


def volume_name(prim):
    try:
        return str(prim.attribValue("name"))
    except Exception:
        return ""


def read_volumes(geo):
    volumes = {}
    for prim in geo.prims():
        name = volume_name(prim)
        if not name:
            continue
        resolution = prim.resolution()
        values = numpy.asarray(prim.allVoxels(), dtype=numpy.float32)
        # Houdini volumes are x-fastest: index = x + y*nx + z*nx*ny.
        if resolution[2] == 1:
            volumes[name] = values.reshape(resolution[1], resolution[0])
        else:
            volumes[name] = values.reshape(resolution[2], resolution[1], resolution[0])
    return volumes


def parse_layer_spec(spec):
    """Parse "Name:volume[:threshold[:invert]], ..." into layer records."""
    layers = []
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        parts = [part.strip() for part in chunk.split(":")]
        out_name = parts[0]
        volume = parts[1] if len(parts) > 1 and parts[1] else parts[0]
        threshold = None
        if len(parts) > 2 and parts[2]:
            threshold = float(parts[2])
        invert = len(parts) > 3 and parts[3] not in ("", "0")
        layers.append((out_name, volume, threshold, invert))
    return layers


def menu_index(node, name, tokens):
    raw = value(node, name)
    text = str(raw).strip()
    if text in tokens:
        return tokens.index(text)
    try:
        return int(text)
    except ValueError:
        return 0


def box_blur(array, radius):
    """Separable triple box blur (approximates a Gaussian), clamped edges."""
    radius = int(round(radius))
    if radius <= 0:
        return array
    result = array
    for _ in range(3):
        result = _box_pass(result, radius, axis=2)
        result = _box_pass(result, radius, axis=1)
    return result


def _box_pass(array, radius, axis):
    # Volumes are (z, y, x); allow 2D arrays (y, x) as well.
    axis -= 3 - array.ndim
    pad = [(0, 0)] * array.ndim
    pad[axis] = (radius, radius)
    padded = numpy.pad(array, pad, mode="edge")
    cumulative = numpy.cumsum(padded, axis=axis, dtype=numpy.float32)
    zero_shape = list(cumulative.shape)
    zero_shape[axis] = 1
    cumulative = numpy.concatenate(
        [numpy.zeros(zero_shape, dtype=numpy.float32), cumulative], axis=axis
    )
    window = 2 * radius + 1
    upper = numpy.take(cumulative, numpy.arange(window, cumulative.shape[axis]), axis=axis)
    lower = numpy.take(cumulative, numpy.arange(0, cumulative.shape[axis] - window), axis=axis)
    return (upper - lower) / float(window)


def smoothstep(edge0, edge1, value_array):
    if edge1 <= edge0:
        return (value_array >= edge1).astype(numpy.float32)
    t = numpy.clip((value_array - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def build_layer_weights(node, volumes):
    spec = str(value(node, "layer_spec")).strip()
    id_mode = bool(value(node, "id_mode"))
    feather = float(value(node, "feather_texels"))
    threshold = float(value(node, "threshold"))
    soft = float(value(node, "soft_width"))
    composite = menu_index(node, "composite", ("auto", "stack", "normalize", "additive"))
    last_on_top = menu_index(node, "order", ("first_on_top", "last_on_top")) == 1

    if id_mode:
        id_name = str(value(node, "id_layer")).strip() or "id"
        if id_name not in volumes:
            raise hou.OperationFailed("ID volume '%s' not found" % id_name)
        step = max(1, int(value(node, "id_step")))
        ids = numpy.rint(volumes[id_name] * 255.0 / float(step)).astype(numpy.int32)
        if spec:
            # id mode layer_spec is "Output Name:id index"
            names = []
            weights = []
            for out_name, index_text, _threshold, _invert in parse_layer_spec(spec):
                names.append(out_name)
                weights.append((ids == int(index_text)).astype(numpy.float32))
        else:
            count = int(ids.max()) + 1
            names = ["Layer_%02d" % index for index in range(count)]
            weights = []
            for index in range(count):
                weights.append((ids == index).astype(numpy.float32))
    else:
        if spec:
            layers = parse_layer_spec(spec)
        else:
            layers = [
                (name, name, None, False)
                for name in volumes
                if name.lower() not in ("height", "id", "mask")
            ]
        if not layers:
            print(
                "[weightmap] no layers selected: fill layer_spec or enable id_mode; "
                "available volumes: %s" % ", ".join(sorted(volumes))
            )
            return [], []
        names = []
        masks = []
        for out_name, volume, layer_threshold, invert in layers:
            if volume not in volumes:
                raise hou.OperationFailed("Volume '%s' not found" % volume)
            names.append(out_name)
            sample = numpy.clip(volumes[volume], 0.0, 1.0)
            if invert:
                sample = 1.0 - sample
            cutoff = threshold if layer_threshold is None else layer_threshold
            masks.append(smoothstep(cutoff - soft, cutoff + soft, sample))

        if composite == 0:
            # Auto: final weights sum to 1 per texel; coverage stacks overlap
            # and sum above 1.  Pick Normalize for final weights, Stack for
            # coverage, and say why in the log.
            coverage = numpy.stack(masks, axis=0).sum(axis=0)
            over = float((coverage > 1.02).mean())
            median = float(numpy.median(coverage))
            use_stack = over > 0.01 or median > 1.02
            composite = 1 if use_stack else 2
            print(
                "[weightmap] auto composite=%s (coverage median=%.3f, >1.02=%.1f%%)"
                % ("stack" if use_stack else "normalize", median, 100.0 * over)
            )

        if composite == 1:
            # Ordered "over": each layer keeps only the coverage not already
            # claimed by the layers above it, so overlaps are eliminated in
            # the authored list order instead of being double weighted.
            weights = [numpy.zeros_like(mask) for mask in masks]
            remaining = numpy.ones_like(masks[0], dtype=numpy.float32)
            order = range(len(masks) - 1, -1, -1) if last_on_top else range(len(masks))
            for index in order:
                weights[index] = masks[index] * remaining
                remaining = remaining * (1.0 - masks[index])
            # Uncovered texels belong to the bottom layer (last processed).
            bottom = len(masks) - 1 if last_on_top else 0
            weights[bottom] = weights[bottom] + remaining
        else:
            weights = list(masks)

    if feather > 0.0:
        weights = [box_blur(weight, feather) for weight in weights]

    if bool(value(node, "normalize")):
        stack = numpy.stack(weights, axis=0)
        total = stack.sum(axis=0, keepdims=True)
        empty = total < 1.0e-6
        stack = numpy.where(empty, 0.0, stack / numpy.maximum(total, 1.0e-6))
        if numpy.any(empty):
            stack[0][empty[0]] = 1.0
        weights = [stack[index] for index in range(stack.shape[0])]

    return names, weights


def write_png16(path, array):
    height, width = array.shape
    data = (numpy.clip(array, 0.0, 1.0) * 65535.0 + 0.5).astype(">u2").tobytes()
    rows = bytearray()
    stride = width * 2
    for row in range(height):
        rows.append(0)
        rows.extend(data[row * stride : (row + 1) * stride])

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def write_vlpaint(path, weights):
    """VLP1: magic, 5 uint32, float32 layer-major weights, uint8 layer-major alpha."""
    count, height, width = weights.shape
    header = b"VLP1" + struct.pack("<5I", width, height, count, 0, 0)
    payload = numpy.ascontiguousarray(weights, dtype="<f4").tobytes()
    alpha = numpy.full(count * height * width, 255, dtype=numpy.uint8).tobytes()
    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(payload)
        handle.write(alpha)


def resample(array, size):
    source_h, source_w = array.shape
    if size <= 0 or (source_h == size and source_w == size):
        return array
    u = (numpy.arange(size, dtype=numpy.float32) + 0.5) * (source_w / float(size)) - 0.5
    v = (numpy.arange(size, dtype=numpy.float32) + 0.5) * (source_h / float(size)) - 0.5
    x0 = numpy.clip(numpy.floor(u).astype(numpy.int32), 0, source_w - 1)
    y0 = numpy.clip(numpy.floor(v).astype(numpy.int32), 0, source_h - 1)
    x1 = numpy.clip(x0 + 1, 0, source_w - 1)
    y1 = numpy.clip(y0 + 1, 0, source_h - 1)
    fx = numpy.clip(u - x0, 0.0, 1.0)[numpy.newaxis, :]
    fy = numpy.clip(v - y0, 0.0, 1.0)[:, numpy.newaxis]
    top = array[numpy.ix_(y0, x0)] * (1.0 - fx) + array[numpy.ix_(y0, x1)] * fx
    bottom = array[numpy.ix_(y1, x0)] * (1.0 - fx) + array[numpy.ix_(y1, x1)] * fx
    return (top * (1.0 - fy) + bottom * fy).astype(numpy.float32)


def split_tiles(weights, grid, resolution):
    count, height, width = weights.shape
    tiles = []
    for tile_y in range(grid):
        for tile_x in range(grid):
            x0 = int(round(tile_x * width / grid))
            x1 = int(round((tile_x + 1) * width / grid))
            y0 = int(round(tile_y * height / grid))
            y1 = int(round((tile_y + 1) * height / grid))
            tile = weights[:, y0:y1, x0:x1]
            tile = numpy.stack([resample(tile[index], resolution) for index in range(count)], axis=0)
            tiles.append((tile_x, tile_y, tile))
    return tiles


def main():
    node = hou.pwd()
    ensure_parms(node)
    inputs = node.inputs()
    if not inputs:
        raise hou.OperationFailed("Connect a heightfield to input 0")
    geo = inputs[0].geometry()
    volumes = read_volumes(geo)
    if not volumes:
        raise hou.OperationFailed("Input 0 has no named volumes")

    names, weights = build_layer_weights(node, volumes)
    if not names:
        return
    height, width = weights[0].shape[-2:]
    print("[weightmap] layers=%d resolution=%dx%d feather=%.1f" %
          (len(names), width, height, float(value(node, "feather_texels"))))

    if not bool(value(node, "export")):
        print("[weightmap] preview only: enable 'Export (one-shot)' and cook to write files")
        return

    output_dir = os.path.expandvars(os.path.expanduser(str(value(node, "output_dir"))))
    output_dir = hou.text.expandString(output_dir)
    os.makedirs(output_dir, exist_ok=True)

    if bool(value(node, "write_png")):
        prefix = str(value(node, "prefix"))
        for name, weight in zip(names, weights):
            path = os.path.join(output_dir, "%s_%s.png" % (prefix, name.replace(" ", "_")))
            write_png16(path, weight)
            print("[weightmap] wrote", path)

    if bool(value(node, "write_vlpaint")):
        grid = max(1, int(value(node, "tile_grid")))
        resolution = int(value(node, "tile_resolution"))
        vlpaint_prefix = str(value(node, "vlpaint_prefix"))
        stack = numpy.stack(weights, axis=0)
        for tile_x, tile_y, tile in split_tiles(stack, grid, resolution):
            path = os.path.join(
                output_dir, "%s_tile_%d_%d.vlpaint" % (vlpaint_prefix, tile_x, tile_y)
            )
            write_vlpaint(path, tile)
            print("[weightmap] wrote", path)

    node.parm("export").set(0)


main()
