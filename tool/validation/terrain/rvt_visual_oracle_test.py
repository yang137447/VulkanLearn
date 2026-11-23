import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import rvt_visual_oracle as oracle


class VisualOracleTests(unittest.TestCase):
    def test_byte_metrics_exact_constant(self):
        reference = np.zeros((2, 3, 3), np.uint8)
        result = oracle.image_metrics(reference + 2, reference, np.ones((2, 3), bool))
        self.assertEqual((result["maeBytes"], result["rmseBytes"], result["p99ErrorBytes"]), (2, 2, 2))

    def test_fixed_mask_excludes_only_preselected_pixels(self):
        reference = np.zeros((1, 2, 3), np.uint8)
        candidate = np.array([[[2, 2, 2], [255, 255, 255]]], np.uint8)
        result = oracle.image_metrics(candidate, reference, np.array([[True, False]]))
        self.assertEqual(result["maeBytes"], 2)
        self.assertEqual(result["pixels"], 1)

    def test_rmse_and_p99_are_not_mae(self):
        reference = np.zeros((10, 10, 3), np.uint8)
        candidate = np.repeat(np.arange(100, dtype=np.uint8).reshape(10, 10, 1), 3, axis=2)
        result = oracle.image_metrics(candidate, reference, np.ones((10, 10), bool))
        self.assertAlmostEqual(result["maeBytes"], 49.5)
        self.assertAlmostEqual(result["rmseBytes"], (sum(value * value for value in range(100))/100)**.5)
        self.assertAlmostEqual(result["p99ErrorBytes"], 98.01)

    def test_no_float_rgb_rescale(self):
        with self.assertRaisesRegex(ValueError, "display byte"):
            oracle.image_metrics(np.zeros((1, 1, 3)), np.zeros((1, 1, 3)), np.ones((1, 1), bool))

    def test_empty_roi_is_missing_evidence(self):
        with self.assertRaisesRegex(ValueError, "Empty"):
            oracle.image_metrics(np.zeros((1, 1, 3), np.uint8), np.zeros((1, 1, 3), np.uint8), np.zeros((1, 1), bool))

    def test_silhouette_xor_counts_missing_geometry(self):
        reference = np.ones((10, 100), bool)
        candidate = reference.copy()
        candidate[0, 0] = False
        result = oracle.silhouette_metrics(candidate, reference, np.ones((10, 100), bool))
        self.assertEqual(result["xorFraction"], .001)
        candidate[0, 1] = False
        self.assertGreater(oracle.silhouette_metrics(candidate, reference, np.ones((10, 100), bool))["xorFraction"], .001)

    def test_fixed_frozen_limits(self):
        limits, frozen = oracle.thresholds()
        self.assertEqual([limits[key] for key in ("litTerrainRoiMaxMaeBytes", "litTerrainRoiMaxRmseBytes",
                                                 "litTerrainRoiMaxP99ErrorBytes", "maxSilhouetteXorFraction",
                                                 "tesMaxHeightErrorR16Units")], [2, 3, 8, .001, 2])
        self.assertEqual(frozen, "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd")

    def test_tes_counter_overflow_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tes.bin"
            path.write_bytes(struct.pack("<16I", 1, 1, 0, 1, *([0]*12)) + bytes(192))
            with self.assertRaisesRegex(ValueError, "overflowed"):
                oracle.decode_tes(path)

    def test_tes_header_and_record_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tes.bin"
            path.write_bytes(struct.pack("<16I", 1, 0, 8, 16, *range(12)) + np.arange(48, dtype="<f4").tobytes())
            header, records = oracle.decode_tes(path)
            self.assertEqual(header[0].tolist(), [1, 0, 8, 16])
            self.assertEqual(records.shape, (1, 12, 4))
            self.assertEqual(records[0, 7, 3], 31)

    def test_nonfinite_tes_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tes.bin"
            values = np.zeros(48, "<f4")
            values[31] = np.nan
            path.write_bytes(struct.pack("<16I", 1, 0, 0, 1, *([0]*12)) + values.tobytes())
            with self.assertRaisesRegex(ValueError, "Nonfinite"):
                oracle.decode_tes(path)

    def test_same_camera_different_epoch_rejected(self):
        candidate = {"binding": {"pose": 2, "submissionEpoch": 10}}
        reference = {"binding": {"pose": 2, "submissionEpoch": 11}}
        with patch.object(oracle, "load_packet", side_effect=[(candidate, {}), (reference, {})]):
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                oracle.compare_packets("candidate", "reference")

    def test_feedback_domain_not_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            payload = directory / "payload.npz"
            np.savez(payload, litRgb=np.zeros((1, 1, 3), np.uint8))
            metadata = {"version": 1, "role": "candidate",
                        "payload": {"path": payload.name, "sha256": oracle.digest(payload)},
                        "binding": {"samplingDomain": "feedback-visible-terrain"}}
            path = directory / "capture.json"
            path.write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, "Feedback domain"):
                oracle.load_packet(path, "candidate")

    def test_actual_frozen_mip2_not_mip0_or_transposed(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            values = np.arange(2 * (16 + 4 + 1), dtype="<u2")
            (directory / "height.bin").write_bytes(values.tobytes())
            manifest = {"production": {"surfaces": [{"surfaceId": "terrain|test", "inputs": [""]*8 + ["height"]}],
                                      "objects": [{"objectId": "height", "path": "height.bin",
                                                   "image": {"format": "r16-unorm", "width": 4, "height": 4,
                                                             "layers": 2, "mipLevels": 3}}]}}
            path = directory / "manifest.json"
            path.write_text(json.dumps(manifest))
            _, planes, width, height, _ = oracle.source_mip2(path, "terrain|test")
            self.assertEqual((width, height), (1, 1))
            self.assertEqual([plane.tolist() for plane in planes], [[20], [41]])

    def tes_fixture(self, directory, error_units):
        directory = Path(directory)
        record = np.zeros((12, 4), "<f4")
        record[1, 3] = 1
        record[2] = [0, -12.5, 0, 1000]
        record[3] = [0, 1, 0, 1]
        record[6, 0] = 1
        record[7, 0] = error_units / 65535
        record[8, 0] = record[9, 0] = .25
        record[10, 0] = .25
        record[11, 2] = 6000
        header = [1, 0, 0, 1] + [1, 0, 2, 0] + [3, 0, 4, 0] + [0, 0, 0, 10]
        capture = directory / "tes.bin"
        capture.write_bytes(struct.pack("<16I", *header)+record.tobytes())
        manifest = directory / "publication.json"
        manifest.write_text("{}")
        metadata = {"stage": "tessellation-evaluation", "gpuExecuted": True, "primitiveTopology": "patch-list",
                    "actualLod": 0, "completedEpoch": 2, "submissionEpoch": 2, "baseFineResidentPages": 0,
                    "captureSha256": oracle.digest(capture), "drawWords": [0, 0, 0, 10],
                    "frameEpochWords": [1, 0, 2, 0], "worldPoseWords": [3, 0, 4, 0],
                    "publicationSha256": oracle.digest(manifest), "surfaceId": "terrain|fixture"}
        meta = directory / "metadata.json"
        meta.write_text(json.dumps(metadata))
        source = ({"globalToSourceSlice": [0]}, [np.array([0, 65535, 65535, 0])], 2, 2, {"scope": "unit-fixture-only"})
        return capture, meta, manifest, source

    def test_tes_independent_r16_gate_pass_and_fail(self):
        for error_units, expected in ((2, True), (3, False)):
            with tempfile.TemporaryDirectory() as directory:
                capture, meta, manifest, source = self.tes_fixture(directory, error_units)
                with patch.object(oracle, "source_mip2", return_value=source):
                    result = oracle.compare_tes(capture, meta, manifest)
                self.assertEqual(result["heightGatePassed"], expected)
                self.assertEqual(result["predictedWorldMaxAbsCm"], 0)

    def test_tes_compute_only_or_synthetic_does_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            capture, meta, manifest, source = self.tes_fixture(directory, 0)
            data = json.loads(meta.read_text())
            data["stage"] = "compute"
            meta.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "Actual TES"):
                oracle.compare_tes(capture, meta, manifest)

    def test_tes_displacement_off_is_missing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            capture, meta, manifest, source = self.tes_fixture(directory, 0)
            data = bytearray(capture.read_bytes())
            struct.pack_into("<f", data, 64+15*4, 0)
            capture.write_bytes(data)
            metadata = json.loads(meta.read_text())
            metadata["captureSha256"] = oracle.digest(capture)
            meta.write_text(json.dumps(metadata))
            with patch.object(oracle, "source_mip2", return_value=source):
                with self.assertRaisesRegex(ValueError, "No active"):
                    oracle.compare_tes(capture, meta, manifest)


if __name__ == "__main__":
    unittest.main()
