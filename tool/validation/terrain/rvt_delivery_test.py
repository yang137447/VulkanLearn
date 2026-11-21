"""CPU-only delivery evidence tests; synthetic receipts are not GPU acceptance."""

import argparse
from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import rvt_delivery as delivery


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage, self.package, self.resources = [self.root / name for name in ("stage", "package", "resources")]
        for path in (self.stage, self.package, self.resources):
            path.mkdir()
        self.scene_name = "Maps/full.json"
        self.freeze_path = self.root / "freeze.json"
        self.write(self.freeze_path, {"originalP0Schema": "adapter-owned"})
        self.measurement = self.root / "baseline.json"
        self.write(self.measurement, {"measurement": "synthetic CPU fixture"})
        self.frozen = {
            "status": "frozen", "baseline_valid": True, "measurement_run_id": "synthetic-cpu-fixture",
            "hardware": {"gpu": "fixture"}, "display": {"width": 1},
            "build": {"mode": "fixture"}, "cache_scope": {"deleted": ["fixture"]},
            "quality": dict.fromkeys(("target_mip", "effective_mip", "first_frame", "convergence"), "fixture"),
            "comparison_requirements": {"fixture": "adapter must check"},
            "baseline_receipt": self.relative_artifact(self.measurement),
            "measurement_artifacts": [self.relative_artifact(self.measurement)],
            "budgets": dict.fromkeys(delivery.BUDGETS, 10),
            "performance": {name: {"maximum": 10, "min_samples": 2, "statistic": "max",
                                   "sampling": delivery.SAMPLING[name], "unit": "fixture", "cases": ["cold"]}
                            for name in delivery.METRICS},
        }
        for name, rule in self.frozen["performance"].items():
            path = self.root / f"baseline_{name}.json"
            self.write(path, {"sampling": rule["sampling"], "unit": rule["unit"],
                              "aggregation": "raw", "values": [1, 2],
                              "sample_ids": ["baseline1", "baseline2"]})
            rule["baseline_samples"] = self.relative_artifact(path)
        for name in ("bin/main_game.exe", "bin/vl_export_game.exe", "bin/vl_build_flavor.json",
                     "shader/spv/shader-build-cache.json"):
            self.write(self.stage / name, {"fixture": name})
        for name in ("main_game.exe", "vl_build_flavor.json", "game-export-manifest.json",
                     "shader/runtime-shader.manifest.json", "shader/spv/shader-build-cache.json"):
            self.write(self.package / name, {"fixture": name})
        self.write(self.stage / "config/config.json",
                   {"resourcePath": str(self.resources), "initScene": self.scene_name})
        self.write(self.package / "config/config.json", {"resourcePath": "resources"})
        self.scene = {"objects": [
            {"type": "terrain", "name": f"tile_{column}_{row}", "position": [column * 10, 0, row * 10],
             "terrainPath": f"Maps/tr_{column}_{row}.json"}
            for row in range(4) for column in range(4)]}
        for placement in self.scene["objects"]:
            terrain = {"heightmapPath": "Maps/height.png", "gridResolution": 2, "heightmapMargin": 1,
                       "size": [10, 10], "heightScale": 65535, "heightOffset": 0}
            self.write(self.resources / placement["terrainPath"], terrain)
        self.write(self.resources / "Maps/height.png", {"fixture": "mock PNG decoder"})
        self.write(self.resources / self.scene_name, self.scene)
        self.capture = self.resources / "Maps/capture_plan.json"
        self.write(self.capture, {"shots": [
            {"name": name, "position": [0, height, height], "look_at": [0, 0, 0]}
            for name, height in (("far", 100), ("mid", 20), ("near", 2))]})
        self.options = argparse.Namespace(
            stage=self.stage, package=self.package, scene=self.scene_name,
            worker=[1, 2], weight_source=list(delivery.WEIGHT_SOURCES),
            p0_freeze=self.freeze_path, adapter=self.root / "adapter.py", capture_plan=None,
            exporter=None)
        self.write(self.options.adapter, {"fixture": "mock adapter"})
        self.adapter = SimpleNamespace(normalize_p0=lambda path: copy.deepcopy(self.frozen),
                                       inspect_package=self.inspect_package, verify_case=self.verify_case)
        self.png_patch = patch.object(delivery, "png16", return_value=[[5] * 5 for _ in range(5)])
        self.png_patch.start()
        self.addCleanup(self.png_patch.stop)

    def write(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def relative_artifact(self, path):
        result = delivery.artifact(path)
        result["path"] = path.relative_to(self.root).as_posix()
        return result

    def inspect_package(self, package, scene, mode):
        return {"version": 2, "weight_source": mode, "content_identity": "a" * 64,
                "closure_sha256": "b" * 64,
                "surface_ids": sorted("terrain|" + item["name"] for item in self.scene["objects"])}

    def verify_case(self, case, evidence, frozen, summary):
        return {"status": "passed", "checks": dict.fromkeys(case["gates"], True)}

    def plan(self):
        with patch.object(delivery, "load_adapter", return_value=self.adapter):
            return delivery.build_plan(self.options)

    def receipt(self, plan):
        metrics = {}
        entries = []
        for name, rule in plan["frozen"]["performance"].items():
            raw = {"sampling": rule["sampling"], "unit": rule["unit"], "aggregation": "raw",
                   "values": [1, 2], "sample_ids": ["run1", "run2"]}
            path = self.root / f"raw_{name}.json"
            self.write(path, raw)
            entry = self.relative_artifact(path)
            entries.append(entry)
            metrics[name] = {"artifact": entry, "samples": raw}
        cases = []
        for case in plan["matrix"]:
            cases.append({"id": case["id"], "status": "passed", "artifacts": entries,
                          "identity_sha256": plan["identity_sha256"], "worker": case["worker"],
                          "weight_source": case["weight_source"], "scene": plan["identity"]["scene"],
                          "metrics": metrics if case["case"] == "cold" else {}})
        receipt = {"identity_sha256": plan["identity_sha256"], "cases": cases}
        path = self.root / "receipt.json"
        self.write(path, receipt)
        return path, receipt

    def test_geometry_counts_and_real_height(self):
        plan = self.plan()
        self.assertEqual(plan["camera_plan"]["counts"],
                         {"center": 16, "edge": 24, "junction": 9, "outer": 4})
        self.assertTrue(all(shot["look_at"][1] == 5 for shot in plan["camera_plan"]["shots"]))
        self.assertEqual(len({shot["name"] for shot in plan["camera_plan"]["shots"]}), 53)
        self.assertTrue(all(len(shot["surfaces"]) == 4 for shot in plan["camera_plan"]["shots"]
                            if shot["category"] == "junction"))

    def test_matrix_has_both_modes_workers_and_all_cases(self):
        matrix = self.plan()["matrix"]
        self.assertEqual(len(matrix), len(delivery.CASE_GATES) * 4)
        self.assertEqual(len({case["id"] for case in matrix}), len(matrix))

    def test_dry_run_is_never_acceptance(self):
        plan = self.plan()
        self.assertEqual(plan["status"], "planned")
        self.assertFalse(plan["acceptance_passed"])
        self.assertFalse(plan["renderer_started"])

    def test_missing_adapter_blocks(self):
        with patch.object(delivery, "load_adapter", side_effect=delivery.IncompleteEvidence("required")):
            plan = delivery.build_plan(self.options)
        self.assertEqual(plan["status"], "incomplete")
        with self.assertRaises(delivery.IncompleteEvidence):
            delivery.verify_receipt(plan, self.root / "absent.json", self.adapter)

    def test_missing_identity_blocks(self):
        (self.stage / "bin/main_game.exe").unlink()
        self.assertEqual(self.plan()["status"], "incomplete")

    def test_missing_freeze_blocks(self):
        self.freeze_path.unlink()
        self.assertEqual(self.plan()["status"], "incomplete")

    def test_p0_missing_each_metric_or_budget_fails(self):
        for section, names in (("performance", delivery.METRICS), ("budgets", delivery.BUDGETS)):
            for name in names:
                with self.subTest(section=section, name=name):
                    frozen = copy.deepcopy(self.frozen)
                    del frozen[section][name]
                    with self.assertRaises(ValueError):
                        delivery.validate_frozen(frozen, self.root)

    def test_p0_unfrozen_or_empty_measurement_fails(self):
        self.frozen["status"] = "draft"
        with self.assertRaises(ValueError):
            delivery.validate_frozen(self.frozen, self.root)
        self.frozen["status"] = "frozen"
        self.measurement.write_bytes(b"")
        entry = self.relative_artifact(self.measurement)
        self.frozen.update(baseline_receipt=entry, measurement_artifacts=[entry])
        with self.assertRaisesRegex(ValueError, "Empty P0"):
            delivery.validate_frozen(self.frozen, self.root)

    def test_p0_rejects_missing_samples_and_snapshot_percentiles(self):
        for patch_rule in ({"min_samples": 0}, {"statistic": "p99", "sampling": "per-run"},
                           {"maximum": float("nan")}):
            frozen = copy.deepcopy(self.frozen)
            frozen["performance"]["cpu_frame_ms"].update(patch_rule)
            with self.assertRaises(ValueError):
                delivery.validate_frozen(frozen, self.root)

    def test_p0_requires_actual_baseline_samples(self):
        for change in ("missing", "empty", "insufficient", "averaged"):
            frozen = copy.deepcopy(self.frozen)
            rule = frozen["performance"]["cpu_frame_ms"]
            if change == "missing":
                del rule["baseline_samples"]
            else:
                path = self.root / "broken-baseline.json"
                original = json.loads((self.root / rule["baseline_samples"]["path"]).read_text())
                if change == "empty":
                    original["values"] = []
                elif change == "insufficient":
                    original["values"] = [1]
                    original["sample_ids"] = ["frame1"]
                else:
                    original["aggregation"] = "rolling-average"
                self.write(path, original)
                rule["baseline_samples"] = self.relative_artifact(path)
            with self.subTest(change=change), self.assertRaises(ValueError):
                delivery.validate_frozen(frozen, self.root)

    def test_invalidated_or_unidentified_p0_baseline_is_rejected(self):
        for field, value in (("baseline_valid", False), ("baseline_valid", 1),
                             ("baseline_valid", None), ("measurement_run_id", ""),
                             ("measurement_run_id", None)):
            frozen = copy.deepcopy(self.frozen)
            frozen[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                delivery.validate_frozen(frozen, self.root)

    def test_v1_or_wrong_weight_adapter_cannot_pass(self):
        for field, value in (("version", 1), ("weight_source", "wrong"), ("surface_ids", [])):
            def inspect(package, scene, mode):
                result = self.inspect_package(package, scene, mode)
                result[field] = value
                return result
            self.adapter.inspect_package = inspect
            self.assertEqual(self.plan()["status"], "incomplete")

    def test_geometry_rejects_missing_duplicate_transform_or_gap(self):
        original = copy.deepcopy(self.scene)
        for change in ("missing", "duplicate", "rotation", "gap"):
            self.scene = copy.deepcopy(original)
            if change == "missing":
                self.scene["objects"].pop()
            elif change == "duplicate":
                self.scene["objects"][1] = self.scene["objects"][0]
            elif change == "rotation":
                self.scene["objects"][0]["rotation"] = [0, 1, 0]
            else:
                for item in self.scene["objects"]:
                    if item["position"][0] == 30:
                        item["position"][0] = 31
            self.write(self.resources / self.scene_name, self.scene)
            with self.assertRaises(ValueError):
                self.plan()

    def test_identity_changes_with_binary_freeze_adapter_and_camera(self):
        original = self.plan()["identity_sha256"]
        for path in (self.stage / "bin/main_game.exe", self.freeze_path, self.options.adapter, self.capture):
            before = path.read_bytes()
            path.write_bytes(before + b" ")
            self.assertNotEqual(self.plan()["identity_sha256"], original)
            path.write_bytes(before)

    def test_verify_complete_synthetic_receipt(self):
        plan = self.plan()
        path, _ = self.receipt(plan)
        result = delivery.verify_receipt(plan, path, self.adapter)
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["renderer_started"])

    def test_receipt_missing_duplicate_skipped_or_mismatched_identity_fails(self):
        plan = self.plan()
        path, original = self.receipt(plan)
        for change in ("missing", "duplicate", "skipped", "identity", "artifact"):
            receipt = copy.deepcopy(original)
            if change == "missing":
                receipt["cases"].pop()
            elif change == "duplicate":
                receipt["cases"].append(receipt["cases"][0])
            elif change == "skipped":
                receipt["cases"][0]["status"] = "skipped"
            elif change == "identity":
                receipt["identity_sha256"] = "0" * 64
            else:
                receipt["cases"][0]["artifacts"][0]["sha256"] = "0" * 64
            self.write(path, receipt)
            with self.assertRaises(ValueError):
                delivery.verify_receipt(plan, path, self.adapter)

    def test_semantic_check_missing_or_nonboolean_fails(self):
        plan = self.plan()
        path, _ = self.receipt(plan)
        gates = [gate for values in delivery.CASE_GATES.values() for gate in values]
        for status in ({}, {"status": "passed", "checks": dict.fromkeys(gates, 1)}):
            self.adapter.verify_case = lambda *args: status
            with self.assertRaises(ValueError):
                delivery.verify_receipt(plan, path, self.adapter)

    def test_cross_worker_weight_scene_or_case_identity_receipt_rejected(self):
        plan = self.plan()
        path, original = self.receipt(plan)
        for name, value in (("worker", 99), ("weight_source", "wrong"), ("scene", "other.json"),
                            ("identity_sha256", "0" * 64)):
            receipt = copy.deepcopy(original)
            receipt["cases"][0][name] = value
            self.write(path, receipt)
            with self.subTest(field=name), self.assertRaisesRegex(ValueError, "execution identity"):
                delivery.verify_receipt(plan, path, self.adapter)

    def test_partial_matrix_cannot_certify_delivery(self):
        self.options.worker = [2]
        plan = self.plan()
        path, _ = self.receipt(plan)
        with self.assertRaisesRegex(ValueError, "both workers"):
            delivery.verify_receipt(plan, path, self.adapter)

    def test_raw_p99_uses_frames_and_rejects_missing_or_averaged_samples(self):
        rule = {"sampling": "per-frame", "unit": "ms", "min_samples": 100,
                "statistic": "p99", "maximum": 100}
        samples = {"sampling": "per-frame", "unit": "ms", "aggregation": "raw",
                   "sample_ids": [str(index) for index in range(100)], "values": list(range(100))}
        self.assertAlmostEqual(delivery.sample_statistic(samples, rule), 98.01)
        for alteration in ({"aggregation": "rolling-average"}, {"values": [1]},
                           {"sampling": "pid-memory"}, {"values": [float("nan")] * 100},
                           {"sample_ids": ["same"] * 100}):
            with self.assertRaises(ValueError):
                delivery.sample_statistic({**samples, **alteration}, rule)

    def test_edited_inline_samples_or_over_threshold_fails(self):
        plan = self.plan()
        path, receipt = self.receipt(plan)
        cold = next(case for case in receipt["cases"] if case["id"].endswith("/cold"))
        cold["metrics"]["cpu_frame_ms"]["samples"] = {
            **cold["metrics"]["cpu_frame_ms"]["samples"], "values": [20, 20]}
        self.write(path, receipt)
        with self.assertRaisesRegex(ValueError, "Inline samples"):
            delivery.verify_receipt(plan, path, self.adapter)
        plan["frozen"]["performance"]["cpu_frame_ms"]["maximum"] = 1
        path, _ = self.receipt(plan)
        with self.assertRaisesRegex(ValueError, "Frozen performance"):
            delivery.verify_receipt(plan, path, self.adapter)

    def test_path_escape_and_missing_adapter_methods_fail(self):
        for name in ("../outside.json", "C:/outside.json", "/outside.json", "Maps\\a.json"):
            with self.assertRaises(ValueError):
                delivery.logical_path(name)
        path = self.root / "empty_adapter.py"
        path.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "normalize_p0"):
            delivery.load_adapter(path)

    def test_cli_defaults_to_plan_and_never_starts_process(self):
        output = self.root / "plan.json"
        argv = ["--stage", str(self.stage), "--package", str(self.package), "--scene", self.scene_name,
                "--worker", "1", "2", "--weightSource", *delivery.WEIGHT_SOURCES,
                "--p0-freeze", str(self.freeze_path), "--adapter", str(self.options.adapter),
                "--output", str(output)]
        with patch.object(delivery, "load_adapter", return_value=self.adapter), \
                patch.object(delivery.subprocess, "run", side_effect=AssertionError("Plan launched a process")), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(delivery.cli(argv), 0)
        self.assertEqual(json.loads(output.read_text())["status"], "planned")
        self.assertFalse(json.loads(output.read_text())["acceptance_passed"])
        with self.assertRaisesRegex(ValueError, "overwrite"):
            delivery.cli(argv)

    def test_installation_calls_only_bound_unified_validator(self):
        plan = self.plan()
        response = SimpleNamespace(returncode=0, stdout=b"verified", stderr=b"")
        with patch.object(delivery.subprocess, "run", return_value=response) as run:
            result = delivery.verify_installation(plan, self.package, 1)
        self.assertEqual(run.call_args.args[0],
                         [str((self.stage / "bin/vl_export_game.exe").resolve()),
                          "--verify-package", str(self.package.resolve())])
        self.assertFalse(result["renderer_started"])
        self.assertEqual(result["exit_code"], 0)

    def test_installation_rejects_failed_changed_missing_or_wrong_validator(self):
        plan = self.plan()
        response = SimpleNamespace(returncode=1, stdout=b"", stderr=b"V1 rejected")
        with patch.object(delivery.subprocess, "run", return_value=response), \
                self.assertRaisesRegex(ValueError, "validator failed"):
            delivery.verify_installation(plan, self.package, 1)
        (self.stage / "bin/vl_export_game.exe").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed after"):
            delivery.verify_installation(plan, self.package, 1)
        plan["identity"]["files"]["exporter"] = delivery.artifact(self.stage / "bin/main_game.exe")
        with self.assertRaisesRegex(ValueError, "requires vl_export_game"):
            delivery.verify_installation(plan, self.package, 1)
        del plan["identity"]["files"]["exporter"]
        with self.assertRaises(delivery.IncompleteEvidence):
            delivery.verify_installation(plan, self.package, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
