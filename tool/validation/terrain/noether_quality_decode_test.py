import struct
import unittest

from noether_quality_decode import (
    CAPACITY, MAGIC, OWNER_WORD, PAYLOAD_BYTES, RESULT_WORD, SAMPLE_WORD, SURFACE_WORD, Capture,
)


def fixture():
    payload = bytearray(PAYLOAD_BYTES)
    header = [MAGIC, 1, CAPACITY, 1, 0, 0, 1, 0, 17, 0, 18, 0, 19, 0, 20, 0,
              1920, 1080, 1, 1, 0]
    struct.pack_into("<21I", payload, 0, *header)
    struct.pack_into("<6f2I", payload, SAMPLE_WORD * 4, .5, .5, .01, 0, 2.25, 0, 8 << 16, 1)
    identity = list(range(8))
    struct.pack_into("<16I", payload, SURFACE_WORD * 4, 0, 8192, 8192, 13, 19, 0, 7, 0, *identity)
    for branch in range(2):
        struct.pack_into("<16I", payload, (OWNER_WORD + branch * 16) * 4,
                         1, 0, 2 + branch, 0, 19, 0, 7, 0, *identity)
    for tap in range(8):
        for branch in range(2):
            struct.pack_into("<I", payload, (RESULT_WORD + tap * 2 + branch) * 4,
                             branch + 1 | (2 + branch) << 10 | 1 << 26)
    metadata = {"protocol": 1, "source": "gpu-page-table-readback", "frame": 17, "poseId": 18,
                "worldGeneration": 19, "submissionEpoch": 20, "completedEpoch": 20,
                "surfaceIds": ["terrain|fixture"]}
    return payload, metadata


def word(payload, offset, value):
    struct.pack_into("<I", payload, offset * 4, value)


class QualityDecoderTests(unittest.TestCase):
    def test_all_eight_taps(self):
        payload, metadata = fixture()
        capture = Capture(payload, metadata)
        self.assertEqual(len(next(capture.samples())["taps"]), 8)
        self.assertEqual(capture.counts()["validAtTargetPlusBias"], 1)

    def test_last_tap_invalid_identity_is_not_hidden(self):
        payload, metadata = fixture()
        struct.pack_into("<16I", payload, (OWNER_WORD + 32) * 4,
                         1, 0, 3, 0, 19, 0, 7, 0, 999, 1, 2, 3, 4, 5, 6, 7)
        word(payload, RESULT_WORD + 15, 3 | 3 << 10 | 1 << 26)
        counts = Capture(payload, metadata).counts()
        self.assertEqual(counts["incorrectIdentity"], 1)
        self.assertEqual(counts["validAtTargetPlusBias"], 0)

    def test_unknown_owner(self):
        payload, metadata = fixture()
        word(payload, OWNER_WORD, 0)
        self.assertEqual(Capture(payload, metadata).counts()["unresolved"], 1)

    def test_invalid_owner_state(self):
        payload, metadata = fixture()
        word(payload, OWNER_WORD, 2)
        with self.assertRaisesRegex(ValueError, "owner state"):
            Capture(payload, metadata).counts()

    def test_wrong_owner_key_generation_revision(self):
        for offset in (1, 2, 3, 4, 6):
            with self.subTest(offset=offset):
                payload, metadata = fixture()
                word(payload, OWNER_WORD + offset, 999)
                self.assertEqual(Capture(payload, metadata).counts()["incorrectIdentity"], 1)

    def test_missing_final_result(self):
        payload, metadata = fixture()
        word(payload, RESULT_WORD + 15, 0)
        with self.assertRaisesRegex(ValueError, "resolved"):
            Capture(payload, metadata).counts()

    def test_invalid_entry(self):
        payload, metadata = fixture()
        word(payload, RESULT_WORD, 513 | 2 << 10 | 1 << 26)
        with self.assertRaisesRegex(ValueError, "entry"):
            Capture(payload, metadata).counts()

    def test_absent_page_remains_in_denominator(self):
        payload, metadata = fixture()
        word(payload, RESULT_WORD, 13 << 10 | 1 << 26)
        counts = Capture(payload, metadata).counts()
        self.assertEqual(counts["visibleSamples"], 1)
        self.assertEqual(counts["unresolved"], 1)

    def test_duplicate_invalid_drop_overflow(self):
        for offset in (4, 5, 21, 22):
            with self.subTest(offset=offset):
                payload, metadata = fixture()
                word(payload, offset, 1)
                with self.assertRaises(ValueError):
                    Capture(payload, metadata).counts()

    def test_duplicate_claim(self):
        payload, metadata = fixture()
        word(payload, SAMPLE_WORD + 7, 2)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            Capture(payload, metadata).counts()

    def test_missing_sample(self):
        payload, metadata = fixture()
        word(payload, SAMPLE_WORD + 7, 0)
        with self.assertRaisesRegex(ValueError, "missing"):
            Capture(payload, metadata).counts()

    def test_target_bias_and_tap_count(self):
        for value in (-1, .5, 2, float("nan"), float("inf")):
            with self.subTest(bias=value):
                payload, metadata = fixture()
                struct.pack_into("<f", payload, (SAMPLE_WORD + 5) * 4, value)
                with self.assertRaises(ValueError):
                    Capture(payload, metadata).counts()
        for taps in (0, 9, 65535):
            payload, metadata = fixture()
            word(payload, SAMPLE_WORD + 6, taps << 16)
            with self.assertRaises(ValueError):
                Capture(payload, metadata).counts()

    def test_bias_cannot_relax_original_target_plus_one(self):
        payload, metadata = fixture()
        struct.pack_into("<f", payload, (SAMPLE_WORD + 5) * 4, 1)
        for branch in range(2):
            word(payload, OWNER_WORD + branch * 16 + 2, 4 + branch)
        for tap in range(8):
            for branch in range(2):
                word(payload, RESULT_WORD + tap * 2 + branch,
                     branch + 1 | (4 + branch) << 10 | 1 << 26)
        counts = Capture(payload, metadata).counts()
        self.assertEqual(counts["effectiveMipBias"], 1)
        self.assertEqual(counts["validAtTargetPlusBias"], 0)
        self.assertEqual(next(Capture(payload, metadata).samples())["taps"][0]["low"]["requestedMip"], 3)

    def test_raw_target_zero_keeps_fine_zero_and_rejects_coarse_two(self):
        payload, metadata = fixture()
        struct.pack_into("<f", payload, (SAMPLE_WORD + 4) * 4, .25)
        for branch in range(2):
            word(payload, OWNER_WORD + branch * 16 + 2, branch)
        for tap in range(8):
            for branch in range(2):
                word(payload, RESULT_WORD + tap * 2 + branch,
                     branch + 1 | branch << 10 | 1 << 26)
        capture = Capture(payload, metadata)
        self.assertEqual(next(capture.samples())["taps"][0]["low"]["requestedMip"], 0)
        self.assertEqual(capture.counts()["effectiveMipBias"], 0)
        self.assertEqual(capture.counts()["validAtTargetPlusBias"], 1)
        word(payload, OWNER_WORD + 2, 2)
        for tap in range(8):
            word(payload, RESULT_WORD + tap * 2, 1 | 2 << 10 | 1 << 26)
        self.assertEqual(Capture(payload, metadata).counts()["validAtTargetPlusBias"], 0)

    def test_epoch_frame_pose_generation(self):
        for field in ("submissionEpoch", "completedEpoch", "frame", "poseId", "worldGeneration"):
            payload, metadata = fixture()
            metadata[field] = 0
            with self.subTest(field=field), self.assertRaises(ValueError):
                Capture(payload, metadata)

    def test_verified_publication_identity(self):
        payload, metadata = fixture()
        verified = {"terrain|fixture": {"width": 8192, "height": 8192, "contentHash": "00" * 32}}
        with self.assertRaisesRegex(ValueError, "publication"):
            Capture(payload, metadata, verified)

    def test_root_only_and_uninitialized(self):
        payload, metadata = fixture()
        for branch in range(2):
            word(payload, OWNER_WORD + branch * 16 + 2, 13)
        for tap in range(8):
            for branch in range(2):
                word(payload, RESULT_WORD + tap * 2 + branch, branch + 1 | 13 << 10 | 1 << 26)
        word(payload, 7, 1)
        counts = Capture(payload, metadata).counts()
        self.assertEqual(counts["rootOnlySamples"], 1)
        self.assertEqual(counts["uninitializedDraws"], 1)

    def test_size_and_reserved_bits(self):
        payload, metadata = fixture()
        with self.assertRaises(ValueError):
            Capture(payload[:-1], metadata)
        word(payload, RESULT_WORD, 1 | 2 << 10 | 1 << 26 | 1 << 31)
        with self.assertRaises(ValueError):
            Capture(payload, metadata).counts()


if __name__ == "__main__":
    unittest.main()
