"""Validate an Euclid-owned immutable binary/shader snapshot before stage copy."""

import hashlib
import json
from pathlib import Path

from rvt_delivery import require
from rvt_artifact_binding import bind_artifact


def validate_snapshot(path):
    receipt = json.loads(path.read_text(encoding="utf-8"))
    require(receipt.get("kind") == "rvt-stage-snapshot" and receipt.get("version") == 1
            and receipt.get("status") == "locked" and receipt.get("owner") == "Euclid",
            "Copy requires Euclid's locked binary/shader snapshot receipt")
    root = Path(receipt["root"]).resolve(strict=True)
    require(root.is_dir() and receipt.get("configuration") == "Release",
            "Snapshot must name an isolated Release directory")
    entries = receipt["files"]
    require(isinstance(entries, list) and entries, "Snapshot needs exact file inventory")
    indexed = {}
    for entry in entries:
        require(entry["path"] not in indexed, "Duplicate snapshot file")
        indexed[entry["path"]] = bind_artifact(root, entry, maximum=1024 * 1024 * 1024)
    for directory in ("bin", "shader/glsl", "shader/spv"):
        directory_path = root / directory
        require(directory_path.is_dir(), f"Missing snapshot directory: {directory}")
        actual = {item.relative_to(root).as_posix() for item in directory_path.rglob("*") if item.is_file()}
        expected = {name for name in indexed if name.startswith(directory + "/")}
        require(actual == expected, f"Snapshot tree differs from locked inventory: {directory}")
    for name in ("main_game.exe", "main_editor.exe", "vl_export_game.exe", "vl_build_flavor.main_game.json"):
        require("bin/" + name in indexed, f"Snapshot binary input missing: {name}")
    require("shader/spv/shader-build-cache.json" in indexed, "Snapshot shader build cache missing")
    marker = json.loads(indexed["bin/vl_build_flavor.main_game.json"].read_text())
    require(marker.get("flavor") == "game" and marker.get("editorEnabled") is False,
            "Snapshot game marker differs")
    return root, receipt
