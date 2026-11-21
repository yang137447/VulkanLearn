"""Bound external run artifacts by canonical path, size and actual SHA256."""

import hashlib
import json
from pathlib import Path

from rvt_delivery import require

MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_RAW_BYTES = 4 * 1024 * 1024


def bind_artifact(root, entry, maximum=MAX_RAW_BYTES):
    require(isinstance(entry, dict), "Artifact reference must be an object")
    name = entry.get("path")
    require(isinstance(name, str) and name and "\\" not in name and ":" not in name,
            "Artifact path must be relative forward-slash")
    logical = Path(name)
    require(not logical.is_absolute() and not logical.root and
            all(part not in ("", ".", "..") for part in name.split("/")),
            "Unsafe artifact path")
    root = root.resolve(strict=True)
    path = root
    for component in logical.parts:
        path /= component
        require(not path.is_symlink(), "Artifact cannot be a symlink")
    path = path.resolve(strict=True)
    require(path.is_relative_to(root) and path.is_file(), "Artifact escaped run directory")
    size = path.stat().st_size
    require(type(entry.get("bytes")) is int and size == entry["bytes"] and 0 < size <= maximum,
            "Artifact byte identity or bounded size differs")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    require(entry.get("sha256") == digest, "Artifact SHA256 identity differs")
    return path


def load_quality_json(root, entry):
    path = bind_artifact(root, entry, MAX_JSON_BYTES)
    require(path.suffix == ".json", "Binary QualityGpu ABI requires its pinned decoder; cannot parse as JSON")
    document = json.loads(read_bound_bytes(root, entry, MAX_JSON_BYTES).decode("utf-8"))
    require(type(document) is dict and type(document.get("samples")) is list
            and len(document["samples"]) <= 16384, "Bounded quality sample array required")
    return document


def read_bound_bytes(root, entry, maximum=MAX_RAW_BYTES):
    path = bind_artifact(root, entry, maximum)
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    require(len(data) == entry["bytes"] and len(data) <= maximum
            and hashlib.sha256(data).hexdigest() == entry["sha256"],
            "Artifact changed between binding and decoding")
    return data


def event_artifacts(event):
    references = []
    if "samplesArtifact" in event:
        references.append(("gpu_samples", event["samplesArtifact"]))
    if "metadataArtifact" in event:
        references.append(("gpu_metadata", event["metadataArtifact"]))
    if "artifacts" in event:
        require(isinstance(event["artifacts"], list), "Event artifact list required")
        references.extend(("native_raw", entry) for entry in event["artifacts"])
    return references
