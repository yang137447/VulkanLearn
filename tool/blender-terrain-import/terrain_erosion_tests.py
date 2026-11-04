r"""Regression tests for the pure-numpy terrain erosion model.

Run with the Blender-bundled Python (it ships numpy):

    <blender>\5.2\python\bin\python.exe terrain_erosion_tests.py -v

The tests are intentionally small; the 1024² producer run is the visual/scale
acceptance, while this file pins the numerical invariants that are easy to
break when routing or the sediment ledger changes.
"""

from __future__ import annotations

import math
import unittest

import numpy as np

from terrain_erosion import (
    ErosionDiagnostics,
    ErosionParams,
    erode,
    flow_accumulation,
    implicit_stream_power_erode,
)


def _synthetic_cone(resolution: int = 72) -> tuple[np.ndarray, float]:
    cell_size = 40.0
    coordinates = np.arange(resolution, dtype=np.float64) * cell_size
    xx, yy = np.meshgrid(coordinates, coordinates)
    centre = 0.5 * (resolution - 1) * cell_size
    radius = np.hypot(xx - centre, yy - centre)
    cone = 140.0 * np.clip(
        1.0 - radius / (0.42 * resolution * cell_size), 0.0, 1.0
    ) ** 1.25
    rng = np.random.default_rng(20261105)
    cone += rng.normal(0.0, 0.35, cone.shape)
    cone[26:32, 26:32] -= 25.0
    return cone, cell_size


class TerrainErosionTests(unittest.TestCase):
    def test_mfd_spreads_flow_more_than_d8(self) -> None:
        cone, cell_size = _synthetic_cone()
        mfd = flow_accumulation(cone, cell_size, "mfd")
        d8 = flow_accumulation(cone, cell_size, "d8")
        self.assertTrue(np.all(np.isfinite(mfd)) and np.all(mfd > 0.0))
        self.assertTrue(np.all(np.isfinite(d8)) and np.all(d8 > 0.0))
        mfd_ratio = float(np.percentile(mfd, 99) / np.median(mfd))
        d8_ratio = float(np.percentile(d8, 99) / np.median(d8))
        self.assertLess(
            mfd_ratio,
            d8_ratio,
            "MFD should concentrate less flow than the single-receiver D8",
        )

    def test_deposition_preserves_sediment_mass(self) -> None:
        cone, cell_size = _synthetic_cone()
        diagnostics = ErosionDiagnostics()
        deposited, _, _ = erode(
            cone,
            cell_size,
            ErosionParams(
                iterations=5,
                thermal_iterations=0,
                deposition_rate=1.0,
                deposition_length_m=500.0,
            ),
            diagnostics,
        )
        produced = diagnostics.eroded_volume_m3
        deposited_volume = diagnostics.deposited_volume_m3
        exported = diagnostics.exported_sediment_m3
        self.assertGreater(produced, 0.0)
        self.assertGreater(deposited_volume, 0.0)
        self.assertGreater(exported, 0.0)
        self.assertLess(
            abs(produced - deposited_volume - exported)
            / max(produced, 1.0),
            1.0e-9,
        )
        self.assertGreaterEqual(float(deposited.mean()), float(cone.mean()))

    def test_uplift_is_coupled_to_the_implicit_solve(self) -> None:
        cone, cell_size = _synthetic_cone()
        raised = implicit_stream_power_erode(
            cone,
            cell_size,
            iterations=2,
            k=0.0,
            uplift=0.25,
            flow_method="mfd",
        )
        self.assertGreater(float(raised.mean()), float(cone.mean()))
        self.assertGreater(
            float(raised.mean() - cone.mean()),
            0.0,
        )

    def test_deposition_length_is_resolution_independent(self) -> None:
        # The per-cell fraction must shrink with the cell size; otherwise a
        # 2048² run would deposit far more sediment than a 512² run.
        from terrain_erosion import _deposit_sediment

        surface = np.zeros((4, 4), dtype=np.float64)
        incision = np.ones((4, 4), dtype=np.float64)
        edges = None
        level = np.zeros((4, 4), dtype=np.int32)
        slope = np.zeros((4, 4), dtype=np.float64)
        coarse, _ = _deposit_sediment(
            surface,
            incision,
            edges,
            level,
            slope,
            8.0,
            1.0,
            15.0,
            1000.0,
        )
        fine, _ = _deposit_sediment(
            surface,
            incision,
            edges,
            level,
            slope,
            4.0,
            1.0,
            15.0,
            1000.0,
        )
        coarse_fraction = float(coarse.mean())
        fine_fraction = float(fine.mean())
        self.assertGreater(coarse_fraction, fine_fraction)
        self.assertAlmostEqual(
            coarse_fraction,
            1.0 - math.exp(-8.0 / 1000.0),
            places=12,
        )
        self.assertAlmostEqual(
            fine_fraction,
            1.0 - math.exp(-4.0 / 1000.0),
            places=12,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
