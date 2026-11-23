"""Strict material authoring defaults and complete legacy Paint input receipts."""

import hashlib
import json
import math
from pathlib import Path
import struct

from build_composited_fixture import read_json, reference_path


def same_default(value, default):
    if type(value) in (int, float) and type(default) in (int, float):
        return math.isfinite(value) and math.isfinite(default) and value == default
    if type(value) is not type(default):
        return False
    if isinstance(value, list):
        return len(value) == len(default) and all(same_default(first, second) for first, second in zip(value, default))
    return value == default


def author_parameter(instance, schema, name, value):
    if name not in schema["parameters"]:
        raise ValueError(f"Unknown authored material parameter: {name}")
    parameters = instance.setdefault("parameters", {})
    if same_default(value, schema["parameters"][name]["default"]):
        parameters.pop(name, None)
    else:
        parameters[name] = value


def remove_default_parameters(instance, schema):
    parameters = instance.get("parameters", {})
    removed = {}
    for name, value in list(parameters.items()):
        specification = schema["parameters"].get(name)
        if specification and same_default(value, specification.get("default")):
            removed[name] = parameters.pop(name)
    return removed


def paint_semantics(resources, terrain, terrain_name):
    paint_name = terrain.get("paintPath")
    if not paint_name:
        return {"status": "no-authored-paint", "terrain": terrain_name}
    path = reference_path(resources, paint_name)
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("Truncated VLP1 author Paint")
    magic, width, height, layers, flags, reserved = struct.unpack_from("<4s5I", data)
    if magic != b"VLP1" or not width or not height or flags or reserved or layers != len(terrain["layers"]):
        raise ValueError("Paint header/layer table differs from TR")
    if len(data) != 24 + width * height * layers * 5 or len(data) > 512 * 1024 * 1024:
        raise ValueError("Paint body size differs from complete weights+opacity layout")
    weight_count = width * height * layers
    if any(not math.isfinite(value[0]) or value[0] < 0 for value in
           struct.iter_unpack("<f", memoryview(data)[24:24 + weight_count * 4])):
        raise ValueError("Invalid Paint weight payload")
    keys = [layer.get("key") or f"{terrain_name}#{layer['name']}" for layer in terrain["layers"]]
    if len(set(keys)) != len(keys):
        raise ValueError("Paint layer identity is ambiguous")
    digest = hashlib.sha256(data).hexdigest()
    sidecar_name = paint_name + ".meta.json"
    sidecar = resources / sidecar_name
    record = {"terrain": terrain_name, "paintPath": paint_name, "paintBytes": len(data),
              "paintSha256": digest, "format": "VLP1", "width": width, "height": height,
              "layerCount": layers, "layerKeys": keys,
              "weightLayout": "complete layer-major float32 little-endian",
              "opacityLayout": "complete layer-major uint8 after float32 weights",
              "sidecarPath": sidecar_name, "sidecarExists": sidecar.is_file(),
              "v2ProductionRequirement": "Freeze actual restored coverage and lossless local-weight overlay plus layer mapping; Game must use production objects, never read this author receipt"}
    if sidecar.exists():
        sidecar = reference_path(resources, sidecar_name)
        metadata = read_json(sidecar)
        fields = {"version", "kind", "tileX", "tileY", "tileGrid", "width", "height",
                  "layerCount", "layerKeys", "featherTexels", "contentSha256"}
        if not fields <= metadata.keys():
            raise ValueError("Paint sidecar incomplete native metadata fields")
        if metadata.get("version") != 2 or metadata.get("kind") != "terrainLayerWeights" or (
                metadata.get("width") != width or metadata.get("height") != height or
                metadata.get("layerCount") != layers or metadata.get("layerKeys") != keys):
            raise ValueError("Paint sidecar shape/layer identity differs")
        if any(type(metadata[field]) is not int for field in ("tileX", "tileY", "tileGrid")) or (
                metadata["tileGrid"] < 1 or not 0 <= metadata["tileX"] < metadata["tileGrid"] or
                not 0 <= metadata["tileY"] < metadata["tileGrid"]):
            raise ValueError("Paint sidecar tile identity is invalid")
        feather = metadata["featherTexels"]
        if type(feather) not in (int, float) or not math.isfinite(feather) or feather < 0:
            raise ValueError("Paint sidecar feather metadata is invalid")
        if metadata.get("contentSha256") not in ("", digest):
            raise ValueError("Paint sidecar contentSha256 differs from exact current bytes")
        record.update(status="paint-and-sidecar-verified",
                      sidecarFields=metadata,
                      sidecarSha256=hashlib.sha256(sidecar.read_bytes()).hexdigest())
    else:
        record.update(status="legacy-author-without-sidecar",
                      sidecarSha256=None, featherTexels="not-authored; not synthesized")
    return record
