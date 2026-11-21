import tempfile
import unittest
from pathlib import Path

import numpy as np
from compare_native_lit_roi import decode_channel
from rvt_visual_oracle import image_metrics, silhouette_metrics


class NativeLitReaderTests(unittest.TestCase):
    def test_bgra_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bytes.bin"
            np.tile(np.array([3, 5, 7, 255], np.uint8), (128, 128, 1)).tofile(path)
            values = decode_channel(path, {"width": 128, "height": 128, "bytes": 65536,
                "format": 44, "channel": "lit"})
            np.testing.assert_array_equal(values[0, 0], [7, 5, 3])

    def test_raw_depth_rejects_nonfinite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bytes.bin"
            np.full((128, 128), np.nan, "<f4").tofile(path)
            with self.assertRaises(ValueError):
                decode_channel(path, {"width": 128, "height": 128, "bytes": 65536,
                    "format": 126, "channel": "deviceDepth"})

    def test_fixed_error_math(self):
        candidate = np.zeros((2, 2, 3), np.uint8)
        reference = np.full((2, 2, 3), 3, np.uint8)
        domain = np.ones((2, 2), np.bool_)
        error = image_metrics(candidate, reference, domain)
        self.assertEqual((error["maeBytes"], error["rmseBytes"], error["p99ErrorBytes"]), (3, 3, 3))
        actual = domain.copy()
        actual[0, 0] = False
        self.assertEqual(silhouette_metrics(actual, domain, domain)["xorFraction"], .25)


if __name__ == "__main__":
    unittest.main()
