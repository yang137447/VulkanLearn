r"""Regression tests for the pure-numpy terrain erosion model.

Run with the Blender-bundled Python (it ships numpy):

    <blender>\5.2\python\bin\python.exe terrain_erosion_tests.py -v

The tests are intentionally small; the 1024² producer run is the visual/scale
acceptance, while this file pins the numerical invariants that are easy to
break when routing or the sediment ledger changes.
"""

from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

import split_pegasus_tiles
from png16_io import read_gray16, write_gray16
from terrain_erosion import (
    ErosionDiagnostics,
    ErosionParams,
    border_fade,
    erode,
    flow_accumulation,
    implicit_stream_power_erode,
    river_mask,
    taper_to_sea_level,
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

    def test_border_taper_reaches_sea_level_on_every_edge(self) -> None:
        height = np.full((40, 40), 100.0, dtype=np.float64)
        tapered = taper_to_sea_level(height, 10.0, 50.0)
        self.assertEqual(float(tapered[0, 20]), 0.0)
        self.assertEqual(float(tapered[-1, 20]), 0.0)
        self.assertEqual(float(tapered[20, 0]), 0.0)
        self.assertEqual(float(tapered[20, -1]), 0.0)
        self.assertAlmostEqual(float(tapered[20, 20]), 100.0)
        self.assertLess(float(tapered[4, 20]), float(tapered[5, 20]))
        self.assertFalse(np.any(border_fade((40, 40), 10.0, 0.0) < 1.0))

    def test_river_mask_marks_the_drainage_channel(self) -> None:
        flow_area = np.full((32, 32), 1.0, dtype=np.float64)
        flow_area[:, 15:17] = 1.0e6
        flow_area[16, 15:17] = 5.0e6
        land = np.ones((32, 32), dtype=bool)
        land[0, :] = False

        mask = river_mask(flow_area, 10.0, land, 90.0)
        self.assertGreater(float(mask[16, 16]), 0.5)
        self.assertEqual(float(mask[16, 0]), 0.0)
        self.assertEqual(float(mask[0, 16]), 0.0)
        self.assertTrue(np.all(np.isfinite(mask)))
        self.assertTrue(np.all((mask >= 0.0) & (mask <= 1.0)))

        with self.assertRaises(ValueError):
            river_mask(flow_area, 10.0, land, 100.0)


class TerrainTilingTests(unittest.TestCase):
    def test_splitter_shares_boundary_samples_and_page_origins(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vl-pegasus-tiles-") as temp:
            root = Path(temp)
            source_dir = root / "Global"
            source_dir.mkdir()
            resource_root = root / "Resources"
            resource_root.mkdir()

            samples = np.arange(81, dtype=np.float64).reshape(9, 9) * 800.0
            height_path = source_dir / "heightmap.png"
            write_gray16(height_path, samples)
            layers = []
            for index, name in enumerate(("Lowland", "Rock")):
                mask_path = source_dir / f"mask_{index}.png"
                write_gray16(mask_path, np.full((9, 9), 32767.5))
                layers.append({
                    "name": name,
                    "weightmapPath": str(mask_path),
                    "uvScale": 8.0,
                    "heightRange": [0.0, 1.0],
                    "slopeRange": [0.0, 90.0],
                })
            manifest = {
                "assetName": "tiling_test",
                "heightmapPath": str(height_path),
                "size": [200000.0, 200000.0],
                "heightScale": 80000.0,
                "heightOffset": 0.0,
                "materialUvScale": 8.0,
                "layers": layers,
            }
            manifest_path = source_dir / "blender_gn_import.json"
            manifest_path.write_text(
                json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

            output_root = resource_root / "Generated/Tiles"
            old_argv = sys.argv
            try:
                sys.argv = [
                    "split_pegasus_tiles.py",
                    "--source-manifest", str(manifest_path),
                    "--resource-root", str(resource_root),
                    "--output-root", str(output_root),
                    "--tile-grid", "2",
                    "--tile-resolution", "16",
                    "--tile-size-m", "1000",
                    "--page-tiles-per-side", "2",
                ]
                split_pegasus_tiles.main()
            finally:
                sys.argv = old_argv

            layout = json.loads(
                (output_root / "tile_layout.json").read_text(encoding="utf-8"))
            self.assertEqual(layout["tileGrid"], 2)
            self.assertEqual(len(layout["tiles"]), 4)
            self.assertEqual(layout["tileSamples"], 19)
            self.assertEqual(layout["heightmapMargin"], 1)
            self.assertEqual(layout["lodLevels"], 3)
            self.assertEqual(layout["lodDistancesCm"], [125000.0, 250000.0])

            tile0 = read_gray16(output_root / "tile_00/Source/heightmap_gn_tile.png")
            tile1 = read_gray16(output_root / "tile_01/Source/heightmap_gn_tile.png")
            tile2 = read_gray16(output_root / "tile_02/Source/heightmap_gn_tile.png")
            self.assertEqual(tile0.shape, (19, 19))
            np.testing.assert_array_equal(tile0[:, 16:19], tile1[:, 0:3])
            np.testing.assert_array_equal(tile0[16:19, :], tile2[0:3, :])
            np.testing.assert_array_equal(tile0[:, 17], tile1[:, 1])
            np.testing.assert_array_equal(tile0[17, :], tile2[1, :])

            tile1_manifest = json.loads(
                (output_root / "tile_01/blender_gn_import.json").read_text(
                    encoding="utf-8"))
            self.assertEqual(tile1_manifest["pageOrigin"], [2, 0])
            self.assertEqual(tile1_manifest["pageTilesPerSide"], 2)
            self.assertEqual(tile1_manifest["heightmapMargin"], 1)
            self.assertEqual(tile1_manifest["lodLevels"], 3)
            self.assertEqual(
                tile1_manifest["lodDistances"], [125000.0, 250000.0])
            self.assertAlmostEqual(
                tile1_manifest["materialUvScale"], 4.0)
            self.assertAlmostEqual(
                tile1_manifest["layers"][0]["uvScale"], 4.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
