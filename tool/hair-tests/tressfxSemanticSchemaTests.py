import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_scene_schema import RESOURCE, check, load_schema


class TressfxSemanticSchemaTests(unittest.TestCase):
    def setUp(self):
        self.schema = load_schema("model_descriptor.schema.json")
        self.document = {
            "name": "semantic test",
            "type": "tressfx",
            "modelDataPath": "test.tfx",
            "strandRadius": 0.012,
            "simulationGuideCount": 2000,
            "materialSlots": [{"name": "hair", "materialInstancePath": "MI_test.json"}],
        }

    def problems(self, values):
        return check(dict(self.document, simulationDefaults=values), self.schema, self.schema)

    def test_legacy_and_versioned_profile(self):
        self.assertFalse(self.problems({"windGain": 0.01, "bendStiffness": 0.3}))
        self.assertFalse(self.problems({
            "solver": "cosseratXpbdV1", "solverProfile": "effectiveBundleV1",
            "backend": "gpu"
        }))
        self.assertFalse(self.problems({
            "solver": "cosseratImplicitV3", "solverProfile": "effectiveBundleV1",
            "backend": "gpu"
        }))
        for values in [
            {"wind": [0, 0, 1]}, {"solver": "rod"},
            {"solver": "cosseratXpbdV1", "solverProfile": "balanced"},
            {"solverProfile": "effectiveBundleV1"},
            {"solver": "tressfx", "solverProfile": "effectiveBundleV1"},
            {"solver": "cosseratXpbdV1", "misspelled": 1},
            {"solver": "cosseratXpbdV1", "backend": "cpu"},
            {"solver": "cosseratXpbdV1"},
            {"solver": "cosseratImplicitV3", "backend": "cpu"},
            {"solver": "cosseratImplicitV3"},
            {"solver": "cosseratImplicitV3", "backend": "gpu", "solverProfile": "unknown"},
            {"solver": "cosseratImplicitV2", "backend": "gpu"},
        ]:
            with self.subTest(values=values):
                self.assertTrue(self.problems(values))

    def test_raw_rod_fields_rejected(self):
        for field, value in {
            "damping": 0.1, "iterations": 3, "rootFixedPoints": 2,
            "maxDt": 0.01, "bendStiffness": 0.3, "localShapeStiffness": [0.2],
            "globalShapeStiffness": 0.1, "globalShapeEffectiveRange": 0.5,
            "vspCoefficient": 0.2, "vspAccelThresholdMax": 140,
            "vspStiffness": [0.2], "windGain": 0.01,
        }.items():
            with self.subTest(field=field):
                for solver in ["cosseratXpbdV1", "cosseratImplicitV3"]:
                    self.assertTrue(self.problems({
                        "solver": solver, "backend": "gpu", field: value}))

    def test_group_field_scope(self):
        for overrides, valid in [
            ({"solverProfile": "effectiveBundleV1"}, True),
            ({"bendStiffness": 0.3}, True), ({"backend": "gpu"}, False),
            ({"solver": "cosseratXpbdV1"}, False), ({"unknown": 1}, False),
        ]:
            document = dict(self.document, groups=[{
                "name": "fringe", "guideRanges": [[0, 2]],
                "simulationOverrides": overrides,
            }])
            with self.subTest(overrides=overrides):
                self.assertEqual(not check(document, self.schema, self.schema), valid)

    def test_formal_tf_descriptors(self):
        paths = sorted(Path(RESOURCE).glob("Maps/SC_*/Meshes/**/TF_*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path):
                document = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(check(document, self.schema, self.schema), [])

    def test_rod_group_rejects_raw_override(self):
        for solver in ["cosseratXpbdV1", "cosseratImplicitV3"]:
            document = dict(self.document,
                simulationDefaults={"solver": solver, "backend": "gpu"},
                groups=[{"name": "fringe", "guideRanges": [[0, 2]],
                    "simulationOverrides": {"bendStiffness": 0.3}}])
            self.assertTrue(check(document, self.schema, self.schema))

    def test_scene_wind_matches_current_authoring_boundary(self):
        schema = load_schema("scene.schema.json")
        wind_schema = schema["$defs"]["windObject"]
        wind = {"name": "MainWind", "type": "wind", "direction": [1, 0, 0], "strength": 0}
        self.assertFalse(check(wind, wind_schema, schema))
        self.assertFalse(check(dict(wind, strength=2000,
            gust={"amplitude": 0.2, "frequency": 0.5, "turbulence": 0.3}),
            wind_schema, schema))
        for invalid in [
            dict(wind, direction=[0, 0, 0]), dict(wind, direction=[1, 0]),
            dict(wind, strength=-1), dict(wind, position=[0, 0, 0]),
            dict(wind, gust={"amplitude": 0.2}),
            dict(wind, gust={"amplitude": 2, "frequency": 0.5, "turbulence": 0.3}),
        ]:
            with self.subTest(invalid=invalid):
                self.assertTrue(check(invalid, wind_schema, schema))


if __name__ == "__main__":
    unittest.main()
