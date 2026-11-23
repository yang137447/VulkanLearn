import ast
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

from assemble_pegasus_tiles import _load_converter, _prepare_composited_materials
from png16_io import write_gray16


class CompositedAuthoringTests(unittest.TestCase):
    def test_global_producer_manifest_does_not_author_retired_mode(self):
        producer = Path(__file__).with_name("build_pegasus_library_global.py")
        syntax = ast.parse(producer.read_text(encoding="utf-8"))
        manifests = [
            node.value for node in ast.walk(syntax)
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict)
            and any(isinstance(target, ast.Name) and target.id == "manifest"
                    for target in node.targets)]
        self.assertEqual(len(manifests), 1)
        keys = {key.value for key in manifests[0].keys if isinstance(key, ast.Constant)}
        self.assertTrue({"heightmapPath", "size", "layers"} <= keys)
        self.assertNotIn("terrainLayerMode", keys)
        self.assertNotIn("terrain_layer_mode", keys)

    def test_actual_split_manifests_import_as_composited_and_drop_legacy_source_mode(self):
        converter = _load_converter()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            height = source / "height.png"
            mask = source / "weight.png"
            write_gray16(height, np.arange(49, dtype=np.uint16).reshape(7, 7) * 1000)
            write_gray16(mask, np.full((7, 7), 65535, dtype=np.uint16))
            textures = {}
            for name, colour in (
                ("albedo", (80, 120, 160, 255)),
                ("normal", (128, 128, 255, 255)),
                ("pbr", (255, 180, 0, 255))):
                path = source / f"{name}.png"
                Image.new("RGBA", (8, 8), colour).save(path)
                textures[name] = str(path)
            textures["height"] = str(height)
            manifest = {
                "heightmapPath": str(height), "size": [2000, 2000],
                "heightScale": 100, "materialUvScale": 4,
                "layers": [{"name": "Ground", "weightmapPath": str(mask),
                            "textures": textures, "heightRange": [0, 1],
                            "slopeRange": [0, 90]}],
            }
            source_path = source / "manifest.json"
            for legacy_mode in (None, 1.0):
                current = dict(manifest)
                if legacy_mode is not None:
                    current["terrainLayerMode"] = legacy_mode
                source_path.write_text(json.dumps(current), encoding="utf-8")
                resource_root = root / ("standard" if legacy_mode is None else "legacy_source")
                output_root = resource_root / "Generated/Tiles"
                result = subprocess.run([
                    sys.executable, str(Path(__file__).with_name("split_pegasus_tiles.py")),
                    "--source-manifest", str(source_path),
                    "--resource-root", str(resource_root),
                    "--output-root", str(output_root),
                    "--tile-grid", "2", "--tile-resolution", "2",
                    "--tile-size-m", "10", "--lod-levels", "1",
                    "--texture-resolution", "512", "--paint-resolution", "4",
                ], capture_output=True, text=True, encoding="utf-8", errors="replace")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                tiles = sorted(output_root.glob("tile_*/blender_gn_import.json"))
                self.assertEqual(len(tiles), 4)
                for tile in tiles:
                    authored = json.loads(tile.read_text(encoding="utf-8"))
                    self.assertNotIn("terrainLayerMode", authored)
                    self.assertNotIn("terrain_layer_mode", authored)
                    converter._load_manifest(tile, None, None)
                summary = converter._convert(tiles[0], resource_root, None, None, False)
                self.assertFalse(summary["renderReady"])
                self.assertEqual(summary["requiredCookIdentity"], {
                    "generatorVersion": 3,
                    "filterRule": "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3",
                    "encoding": "gpu-rvt-adjacency-area4-rgba8-v3"})
                self.assertIn("--terrain-vt-cook", summary["cookCommand"])
                terrain_paths = list(Path(summary["outputRoot"]).glob("TR_*.json"))
                self.assertEqual(len(terrain_paths), 1)
                terrain = json.loads(terrain_paths[0].read_text(encoding="utf-8"))
                self.assertIs(terrain["compositedVT"]["enabled"], True)
                self.assertTrue(terrain["compositedVT"]["packagePath"])
                self.assertEqual(set(terrain["compositedVT"]), {
                    "enabled", "packagePath", "productionSourcePath", "virtualResolution"})
                recipe = json.loads((resource_root / terrain["compositedVT"]["productionSourcePath"]).read_text())
                self.assertEqual(recipe["surfaces"], ["terrain|Landscape_00"])
                self.assertEqual(recipe["adjacency"], [])
                material = json.loads(
                    (resource_root / terrain["materialInstancePath"]).read_text(encoding="utf-8"))
                self.assertNotIn("u_terrainLayerMode", material["parameters"])

    def test_assembly_preserves_independent_materials_and_package_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tiles = []
            originals = {}
            for index in range(2):
                relative = f"Generated/tile{index}"
                directory = root / relative
                directory.mkdir(parents=True)
                terrain = {
                    "compositedVT": {
                        "enabled": True,
                        "packagePath": f"{relative}/Cooked/manifest.json"},
                    "materialInstancePath": f"{relative}/MI_tile{index}.json"}
                terrain_path = directory / f"TR_terrain_tile{index}.json"
                terrain_path.write_text(json.dumps(terrain), encoding="utf-8")
                originals[terrain_path] = terrain_path.read_bytes()
                (root / terrain["materialInstancePath"]).write_text(
                    json.dumps({"textures": {"layerNormal": "Source/normal.json"}}),
                    encoding="utf-8")
                tiles.append({"outputRelative": relative, "assetName": f"tile{index}"})
            _prepare_composited_materials(root, tiles, False)
            for path, original in originals.items():
                self.assertEqual(path.read_bytes(), original)
                terrain = json.loads(path.read_text(encoding="utf-8"))
                material = json.loads((root / terrain["materialInstancePath"]).read_text())
                self.assertEqual(material["textures"]["layerNormal"], "Source/normal.json")
                self.assertIn("globalColour", material["textures"])

    def test_assembly_rejects_legacy_terrain(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "TR_terrain_tile.json").write_text(
                json.dumps({"materialInstancePath": "MI_shared.json"}))
            with self.assertRaisesRegex(RuntimeError, "compositedVT.enabled=true"):
                _prepare_composited_materials(
                    root, [{"outputRelative": ".", "assetName": "tile"}], False)


if __name__ == "__main__":
    unittest.main()
