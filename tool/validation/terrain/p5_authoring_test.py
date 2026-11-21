"""CPU authoring/publication failure tests; no GPU or large stage."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from build_composited_fixture import validate_author_world, world_author_recipe
from publish_full_cook import publish_files
from rvt_native_adapter import sha256, normalize_p0, audit_p0
from rvt_author_semantics import author_parameter, remove_default_parameters, paint_semantics
from prepare_full_delivery_stage import author_sidecars
import struct


class MaterialAuthoringDefaultTests(unittest.TestCase):
    def test_coverage_omits_default_zero_and_local_preserves_one(self):
        schema = {"parameters": {"u_terrainWeightMapMode": {"default": 0.0}}}
        for desired, expected in ((0, {}), (1, {"u_terrainWeightMapMode": 1})):
            material = {"parameters": {"u_terrainWeightMapMode": 0}}
            author_parameter(material, schema, "u_terrainWeightMapMode", desired)
            self.assertEqual(material["parameters"], expected)

    def test_current_schema_default_one_reverses_omission(self):
        schema = {"parameters": {"u_terrainWeightMapMode": {"default": 1.0}}}
        material = {}
        author_parameter(material, schema, "u_terrainWeightMapMode", 1)
        self.assertEqual(material["parameters"], {})
        author_parameter(material, schema, "u_terrainWeightMapMode", 0)
        self.assertEqual(material["parameters"], {"u_terrainWeightMapMode": 0})

    def test_boolean_cannot_be_hidden_as_numeric_default(self):
        material = {"parameters": {"mode": False, "scale": 4}}
        self.assertEqual(remove_default_parameters(material, {"parameters": {
            "mode": {"default": 0.0}, "scale": {"default": 64}}}), {})
        self.assertIs(material["parameters"]["mode"], False)


class PaintSidecarStateTests(unittest.TestCase):
    def fixture(self, root):
        (root / "paint.vlpaint").write_bytes(struct.pack("<4s5I", b"VLP1", 2, 2, 1, 0, 0)
            + struct.pack("<4f", 0.25, 0.5, 0.75, 1) + bytes([0, 64, 128, 255]))
        return {"paintPath": "paint.vlpaint", "layers": [{"name": "Ground", "key": "ground"}]}

    def test_legacy_absence_is_recorded_without_synthetic_sidecar(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            terrain = self.fixture(root)
            result = paint_semantics(root, terrain, "TR.json")
            self.assertEqual(result["status"], "legacy-author-without-sidecar")
            self.assertEqual(result["layerKeys"], ["ground"])
            self.assertFalse((root / "paint.vlpaint.meta.json").exists())

    def test_existing_sidecar_identity_and_current_content_verified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            terrain = self.fixture(root)
            metadata = {"version": 2, "kind": "terrainLayerWeights", "width": 2, "height": 2,
                        "layerCount": 1, "layerKeys": ["ground"],
                        "tileX": 0, "tileY": 0, "tileGrid": 1, "featherTexels": 0,
                        "contentSha256": sha256(root / "paint.vlpaint")}
            sidecar = root / "paint.vlpaint.meta.json"
            sidecar.write_text(json.dumps(metadata))
            self.assertEqual(paint_semantics(root, terrain, "TR.json")["status"], "paint-and-sidecar-verified")
            metadata["layerKeys"] = ["different"]
            sidecar.write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, "layer identity"):
                paint_semantics(root, terrain, "TR.json")

    def test_truncated_opacity_is_not_complete_paint_semantics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            terrain = self.fixture(root)
            paint = root / "paint.vlpaint"
            paint.write_bytes(paint.read_bytes()[:-1])
            with self.assertRaisesRegex(ValueError, "weights\\+opacity"):
                paint_semantics(root, terrain, "TR.json")

    def test_stage_copy_discovers_existing_sidecar_without_inventing_missing_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            terrain = self.fixture(root)
            self.assertEqual(author_sidecars(root, terrain), [])
            (root / "paint.vlpaint.meta.json").write_text("{}")
            self.assertEqual(author_sidecars(root, terrain), ["paint.vlpaint.meta.json"])


class WorldAuthoringTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.ids = [f"terrain|Landscape_{index:02d}" for index in range(16)]
        self.recipe = world_author_recipe(self.ids, {
            (index % 4, index // 4): name for index, name in enumerate(self.ids)})
        self.scene = {"objects": [
            {"type": "terrain", "name": name.split("|")[1], "terrainPath": f"TR_{index}.json"}
            for index, name in enumerate(self.ids)]}
        self.terrain = {"compositedVT": {"enabled": True, "packagePath": "Published/manifest.json",
                        "productionSourcePath": "world.author.json", "virtualResolution": [8192, 8192]}}
        self.write("scene.json", self.scene)
        self.write("world.author.json", self.recipe)
        for index in range(16):
            self.write(f"TR_{index}.json", self.terrain)

    def write(self, name, value):
        (self.root / name).write_text(json.dumps(value), encoding="utf-8")

    def validate(self):
        return validate_author_world(self.root, "scene.json")

    def test_full16_world_has_48_directed_links(self):
        self.assertEqual(len(self.validate()["author"]["adjacency"]), 48)

    def test_retired_budgets_rejected(self):
        for field in ("residentPages", "heightBudgetBytes"):
            changed = copy.deepcopy(self.terrain)
            changed["compositedVT"][field] = 1
            self.write("TR_0.json", changed)
            with self.assertRaisesRegex(ValueError, "retired/unknown"):
                self.validate()

    def test_missing_inverse_rejected(self):
        self.recipe["adjacency"].pop()
        self.write("world.author.json", self.recipe)
        with self.assertRaisesRegex(ValueError, "inverse"):
            self.validate()

    def test_duplicate_link_rejected(self):
        self.recipe["adjacency"].append(copy.deepcopy(self.recipe["adjacency"][0]))
        self.write("world.author.json", self.recipe)
        with self.assertRaises(ValueError):
            self.validate()

    def test_partial_surface_recipe_rejected(self):
        self.recipe["surfaces"].pop()
        self.write("world.author.json", self.recipe)
        with self.assertRaisesRegex(ValueError, "surface"):
            self.validate()

    def test_multiple_world_manifests_rejected(self):
        self.terrain["compositedVT"]["packagePath"] = "Other/manifest.json"
        self.write("TR_0.json", self.terrain)
        with self.assertRaisesRegex(ValueError, "shared World"):
            self.validate()

    def test_singular_transform_rejected(self):
        self.recipe["adjacency"][0]["sourceUvToTargetUv"] = [0] * 6
        self.write("world.author.json", self.recipe)
        with self.assertRaisesRegex(ValueError, "Singular"):
            self.validate()

    def test_pending_p0_review_cannot_be_normalized_as_accepted(self):
        with patch("rvt_native_adapter.audit_p0", return_value={"independentReview": "pending"}):
            with self.assertRaisesRegex(ValueError, "independentReview is pending"):
                normalize_p0(self.root / "unused.json")

    def test_frozen_config_and_matching_sidecar_cannot_be_relaxed_together(self):
        path = self.root / "frozen.json"
        path.write_text(json.dumps({"schemaVersion": 1, "frozen": True, "status": "frozen"}))
        Path(str(path) + ".sha256").write_text(sha256(path))
        with self.assertRaisesRegex(ValueError, "independently approved original"):
            audit_p0(path)


class PublicationTests(unittest.TestCase):
    def test_failed_copy_preserves_previous_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "manifest.json"
            manifest.write_bytes(b"old")
            candidate = root / "candidate"
            candidate.write_bytes(b"new")
            payload = root / "payload"
            payload.write_bytes(b"payload")
            files = [(payload, root / "installed/payload", {"sha256": sha256(payload), "manifest": False}),
                     (candidate, manifest, {"sha256": sha256(candidate), "manifest": True})]
            with patch("publish_full_cook.shutil.copyfileobj", side_effect=OSError("injected")):
                with self.assertRaises(OSError):
                    publish_files(files)
            self.assertEqual(manifest.read_bytes(), b"old")

    def test_source_mutation_rejected_before_pointer_switch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "candidate"
            source.write_bytes(b"candidate")
            entry = {"sha256": sha256(source), "manifest": True}
            source.write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "Source changed"):
                publish_files([(source, root / "manifest.json", entry)])
            self.assertFalse((root / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
