import struct
import unittest

from noether_quality_decode import Capture
from noether_quality_decode_test import fixture
from quality_duplicate_decode import BASE_BYTES, BYTES, decode


class DuplicateDiagnostics(unittest.TestCase):
    def diagnostic(self):
        data, metadata = fixture()
        data.extend(bytes(BYTES - len(data)))
        struct.pack_into("<I", data, 4, 2)
        struct.pack_into("<I", data, 16, 1)
        struct.pack_into("<I", data, 92, 1)
        struct.pack_into("<I", data, 284, 2)
        first = struct.unpack_from("<7I", data, 256)
        struct.pack_into("<12I", data, BASE_BYTES, 0, 1, *first, 0xffffffff,
                         0x3f000000, 0x3f000000)
        return data, metadata

    def test_identical_payload_is_observed_but_not_accepted(self):
        data, metadata = self.diagnostic()
        report = decode(data, metadata)
        self.assertTrue(report["allSamplingPayloadsBitIdentical"])
        self.assertTrue(report["witnessesComplete"])
        self.assertFalse(report["deduplicationAuthorized"])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            Capture(data, metadata).counts()

    def test_different_uv_is_preserved(self):
        data, metadata = self.diagnostic()
        struct.pack_into("<f", data, BASE_BYTES + 8, 0.75)
        report = decode(data, metadata)
        self.assertFalse(report["allSamplingPayloadsBitIdentical"])
        self.assertEqual(report["witnesses"][0]["additional"]["uv"][0], 0.75)

    def test_different_surface_is_preserved(self):
        data, metadata = self.diagnostic()
        struct.pack_into("<I", data, BASE_BYTES + 32, (8 << 16) | 1)
        metadata["surfaceIds"].append("terrain|other")
        report = decode(data, metadata)
        self.assertEqual(report["witnesses"][0]["additional"]["surfaceId"], "terrain|other")
        self.assertFalse(report["allSamplingPayloadsBitIdentical"])

    def test_unfinished_readback_is_rejected(self):
        data, metadata = self.diagnostic()
        metadata["completedEpoch"] -= 1
        with self.assertRaisesRegex(ValueError, "completed"):
            decode(data, metadata)

    def test_claim_accounting_is_required(self):
        data, metadata = self.diagnostic()
        struct.pack_into("<I", data, 92, 2)
        with self.assertRaisesRegex(ValueError, "accounting"):
            decode(data, metadata)

    def test_v2_clean_capture_keeps_original_quality_gate(self):
        data, metadata = fixture()
        data.extend(bytes(BYTES - len(data)))
        struct.pack_into("<I", data, 4, 2)
        self.assertEqual(Capture(data, metadata).counts()["validAtTargetPlusBias"], 1)


if __name__ == "__main__":
    unittest.main()
