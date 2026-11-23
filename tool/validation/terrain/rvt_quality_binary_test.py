"""Independent decoder tests for the actual TerrainQualityAbi v1 header."""

import struct
import unittest

from rvt_quality_binary import (
    BYTES, MAGIC, CAPACITY, SAMPLE_OFFSET, RESULT_OFFSET, OWNER_OFFSET, SURFACE_OFFSET,
    quality_binary_counts, decode_bound_capture,
)


class QualityBinaryTests(unittest.TestCase):
    def fixture(self):
        raw = bytearray(BYTES)
        words = [0] * 64
        words[0:4] = [MAGIC, 1, CAPACITY, 1]
        words[6:21] = [1, 0, 4, 0, 7, 0, 1, 0, 2, 0, 1280, 720, 1, 1, 0]
        struct.pack_into("<64I", raw, 0, *words)
        struct.pack_into("<6f2I", raw, SAMPLE_OFFSET, 0.5, 0.5, 0.0, 0.0, 0.2, 0.0, 1 << 16, 1)
        struct.pack_into("<2I", raw, RESULT_OFFSET, 1 | 1 << 26, 2 | 1 << 10 | 1 << 26)
        identity = bytes(range(32))
        for slot, mip in ((0, 0), (1, 1)):
            offset = OWNER_OFFSET + slot * 64
            struct.pack_into("<8I", raw, offset, 1, 0, mip, 0, 1, 0, 1, 0)
            raw[offset + 32:offset + 64] = identity
        struct.pack_into("<8I", raw, SURFACE_OFFSET, 0, 8192, 8192, 13, 1, 0, 1, 0)
        raw[SURFACE_OFFSET + 32:SURFACE_OFFSET + 64] = identity
        event = {"surfaceIds": ["terrain|Landscape_00"], "frame": 4, "poseId": "7",
                 "worldGeneration": 1, "submissionEpoch": 2, "completedEpoch": 2,
                 "renderExtent": [1280, 720], "captureSequence": 1,
                 "samplingDomain": "feedback-visible-terrain", "feedbackExtent": [128, 128],
                 "captureScope": "all-fragment-taps", "visibleSamples": 1,
                 "validAtTargetPlusBias": 1, "effectiveMipBias": 0, "unresolved": 0,
                 "incorrectIdentity": 0, "rootOnlySamples": 0, "uninitializedDraws": 0}
        surfaces = {"terrain|Landscape_00": {"width": 8192, "height": 8192, "contentHash": identity.hex()}}
        return raw, event, surfaces

    def test_actual_fixed_abi_counts_without_expanding_json(self):
        raw, event, surfaces = self.fixture()
        self.assertEqual(quality_binary_counts(raw, event, surfaces)["validAtTargetPlusBias"], 1)

    def test_changed_owner_identity_cannot_be_hidden_by_count(self):
        raw, event, surfaces = self.fixture()
        raw[OWNER_OFFSET + 32] ^= 1
        with self.assertRaisesRegex(ValueError, "independent raw decode"):
            quality_binary_counts(raw, event, surfaces)

    def test_unknown_version_and_truncated_buffer_rejected(self):
        raw, event, surfaces = self.fixture()
        with self.assertRaises(ValueError):
            quality_binary_counts(raw[:-1], event, surfaces)
        struct.pack_into("<I", raw, 4, 2)
        with self.assertRaisesRegex(ValueError, "Unknown"):
            quality_binary_counts(raw, event, surfaces)

    def test_duplicate_capture_and_pending_epoch_rejected(self):
        raw, event, surfaces = self.fixture()
        struct.pack_into("<I", raw, 4 * 4, 1)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            quality_binary_counts(raw, event, surfaces)
        struct.pack_into("<I", raw, 4 * 4, 0)
        event["completedEpoch"] = 1
        with self.assertRaisesRegex(ValueError, "epoch"):
            quality_binary_counts(raw, event, surfaces)

    def test_bad_tap_completion_bit_rejected(self):
        raw, event, surfaces = self.fixture()
        struct.pack_into("<I", raw, RESULT_OFFSET, 1)
        with self.assertRaisesRegex(ValueError, "resolved address"):
            quality_binary_counts(raw, event, surfaces)

    def test_expected_surface_digest_bound_to_installed_manifest(self):
        raw, event, surfaces = self.fixture()
        surfaces["terrain|Landscape_00"]["contentHash"] = "f" * 64
        with self.assertRaisesRegex(ValueError, "expected surface identity"):
            quality_binary_counts(raw, event, surfaces)

    def test_bias_one_total_raw_plus_one_is_accepted(self):
        raw, event, surfaces = self.fixture()
        struct.pack_into("<f", raw, SAMPLE_OFFSET + 20, 1.0)
        struct.pack_into("<2I", raw, RESULT_OFFSET,
                         1 | 1 << 10 | 1 << 26, 2 | 2 << 10 | 1 << 26)
        struct.pack_into("<I", raw, OWNER_OFFSET + 8, 1)
        struct.pack_into("<I", raw, OWNER_OFFSET + 64 + 8, 2)
        event["effectiveMipBias"] = 1
        self.assertEqual(quality_binary_counts(raw, event, surfaces)["validAtTargetPlusBias"], 1)

    def test_bias_one_raw_plus_two_is_not_good_even_with_valid_identity(self):
        raw, event, surfaces = self.fixture()
        struct.pack_into("<f", raw, SAMPLE_OFFSET + 20, 1.0)
        struct.pack_into("<2I", raw, RESULT_OFFSET,
                         1 | 2 << 10 | 1 << 26, 2 | 2 << 10 | 1 << 26)
        struct.pack_into("<I", raw, OWNER_OFFSET + 8, 2)
        struct.pack_into("<I", raw, OWNER_OFFSET + 64 + 8, 2)
        event["effectiveMipBias"] = 1
        event["validAtTargetPlusBias"] = 0
        counts = quality_binary_counts(raw, event, surfaces)
        self.assertEqual(counts["validAtTargetPlusBias"], 0)
        self.assertEqual(counts["incorrectIdentity"], 0)
        self.assertEqual(counts["unresolved"], 0)

    def test_actual_noether_write_metadata_binds_binary_and_log(self):
        raw, event, surfaces = self.fixture()
        metadata = {field: event[field] for field in (
            "frame", "worldGeneration", "submissionEpoch", "completedEpoch", "surfaceIds")}
        metadata.update(protocol=1, source="gpu-page-table-readback",
                        binaryAbi="TVQ1-final-results-v1", poseId=7)
        self.assertEqual(decode_bound_capture(raw, metadata, event, surfaces)["visibleSamples"], 1)
        metadata["frame"] += 1
        with self.assertRaisesRegex(ValueError, "log/metadata"):
            decode_bound_capture(raw, metadata, event, surfaces)

    def test_independent_noether_decoder_agrees_on_observed_words(self):
        from noether_quality_decode import Capture
        raw, event, surfaces = self.fixture()
        metadata = dict(event, protocol=1, source="gpu-page-table-readback", poseId=7)
        self.assertEqual(Capture(raw, metadata, surfaces).counts(),
                         quality_binary_counts(raw, event, surfaces))

    def test_dropped_and_overflow_header_fields_cannot_pass(self):
        for word in (21, 22):
            raw, event, surfaces = self.fixture()
            struct.pack_into("<I", raw, word * 4, 1)
            with self.assertRaisesRegex(ValueError, "dropped/overflow"):
                quality_binary_counts(raw, event, surfaces)


if __name__ == "__main__":
    unittest.main()
