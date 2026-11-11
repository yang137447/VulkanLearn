"""Erode a baked Pegasus shape heightmap with the CPU landscape-evolution solver.

This is the fast iteration path: the shape stage (noise + distort) is baked
once by ``build_pegasus_library_global.py`` with erosion disabled, and every
erosion parameter change re-runs only this script.

默认算法按主流 LEM 论文取值，而不是拟合某个商用软件的输出：

* 洼地 / 汇流：priority-flood (Barnes et al. 2014) + D8 (O'Callaghan & Mark 1984)
* 河道下切：隐式 stream power ``E = K A^m S^n`` (Braun & Willett 2013)，
  ``m = 0.5, n = 1``
* 坡面：临界坡松弛 (Roering et al. 1999 的离散形式)

Houdini 的 ``sop_pe_landscape`` 只是产业流程的一种实现，不是基准；我们保证
阶段顺序与物理意义一致。需要贴近它的形态时才用 ``--passes two``。

The drainage network (fill + accumulation + levels) is cached on disk keyed by
the input heightmap content, cell size, fill epsilon and flow method.  Changing
``k`` / ``m`` / ``n`` / ``iterations`` / thermal settings therefore skips the
only expensive stage.

Usage:

```powershell
python erode_pegasus_global.py ^
  --input  <res>\\Generated\\BlenderPegasusGN\\Global\\Source\\heightmap_gn.png ^
  --manifest <res>\\Generated\\BlenderPegasusGN\\Global\\blender_gn_import.json ^
  --cache-dir <res>\\Generated\\BlenderPegasusGN\\Global\\Cache ^
  --erodibility 3e-6 --dt-years 1000 --steps 12

物理量形式：``∂h/∂t = U - K A^m S^n``。``K`` 单位 1/yr（m=0.5、A 以 m² 计），
``U`` 用 mm/kyr 给出，总模拟时间 = ``dt-years × steps``。
```
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import terrain_erosion as erosion  # noqa: E402
from png16_io import read_gray16, write_gray16  # noqa: E402

# Houdini `sop_pe_landscape.1.0` 的四次侵蚀（解码值见
# `decoded/_erode_parameters.md` D.3）。solver 没有水层/bank 模型，
# 因此按下表映射到现有旋钮：
#   K 倍率      = erodability × erosionrate / 1.1（以 erode1 为基准）
#   thermal     = thermal_erodability
#   talus       = thermal_cutangle
#   deposition  = 1 - removalrate（removalrate 是"被带走"的比例）
#   flow        = bankangle ≤ 30 → MFD（宽谷），否则 D8（窄谷）
PEGASUS_PASSES: tuple[dict, ...] = (
    {"name": "erode1", "k": 1.1 * 1.0 / 1.1, "thermal": 1.0,
     "talus": 50.0, "deposition": 0.9, "flow": "mfd",
     "houdini": "erodability 1.1, rate 1.0, bank 20, spread 140, removal 0.1"},
    {"name": "erode2", "k": 1.5 * 1.0 / 1.1, "thermal": 1.0,
     "talus": 35.0, "deposition": 0.0, "flow": "mfd",
     "houdini": "erodability 1.5, rate 1.0, bank 25, spread 40, removal 1.0"},
    {"name": "erode3", "k": 1.0 * 0.4 / 1.1, "thermal": 1.0,
     "talus": 35.0, "deposition": 0.4, "flow": "d8",
     "houdini": "erodability 1.0, rate 0.4, bank 70, spread 40, removal 0.6"},
    {"name": "erode4", "k": 1.0 * 0.4 / 1.1, "thermal": 0.0,
     "talus": 35.0, "deposition": 0.9, "flow": "d8",
     "houdini": "erodability 1.0, rate 0.4, bank 70, thermal off, "
                "water_addtoheight on"},
)


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=None,
                        help="造型高度场；默认 <manifest 目录>/heightmap_shape.png")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=None,
                        help="默认 <input 目录>/heightmap_gn.png")
    parser.add_argument("--cache-dir", type=Path, default=None)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--k", type=float, default=None,
                        help="直接给可蚀性 K [1/yr]；省略时用 --erodibility")
    parser.add_argument("--erodibility", type=float, default=3.0e-6,
                        help="K [1/yr]（m=0.5、A 以 m² 计）。"
                             "3e-6 ≈ 每 1 kyr 的 k=0.003")
    parser.add_argument("--dt-years", type=float, default=1000.0,
                        help="每个时间步代表的年数")
    parser.add_argument("--steps", type=int, default=None,
                        help="时间步数；省略时用 --iterations")
    parser.add_argument("--uplift-mm-per-kyr", type=float, default=0.0,
                        help="抬升速率 U [mm/kyr]；0 = 纯下切")
    parser.add_argument("--coarse-k", type=float, default=2.0e-5,
                        help="第一段（D8，开宽谷）的 k")
    parser.add_argument("--fine-k", type=float, default=2.5e-5,
                        help="第二段（MFD，刻细沟）的 k")
    parser.add_argument("--passes", choices=("two", "one"), default="one",
                        help="one = 标准单次隐式求解；two = 宽谷+细沟两段"
                             "（为贴近 Houdini 形态加的，不属于标准流程）")
    parser.add_argument("--chain", choices=("single", "pegasus"), default="single",
                        help="pegasus = 按 Houdini 的四次侵蚀链（erode1..4）"
                             "依次求解，参数见 PEGASUS_PASSES")
    parser.add_argument("--chain-k", type=float, default=1.0,
                        help="四次链的 K 总倍率（相对 --erodibility）")
    parser.add_argument(
        "--chain-resample", action=argparse.BooleanOptionalAction, default=False,
        help="四次链按 Houdini 分辨率：erode1/2 512，erode3/4 256，再升回输入分辨率")
    parser.add_argument(
        "--chain-warp", action=argparse.BooleanOptionalAction, default=True,
        help="在 erode2 后和 erode4 前插入 domain warp（仅 chain-resample 生效）")
    parser.add_argument(
        "--chain-warp-mask", type=float, default=0.75,
        help="domain warp 的低频 mask 强度 0..1")
    parser.add_argument(
        "--chain-detail-restore", type=float, default=0.0,
        help="升采样后回填 shape 高频残差的比例 0..1")
    parser.add_argument("--warp-seed", type=int, default=17)
    parser.add_argument("--m", type=float, default=0.5,
                        help="汇水面积指数（SPL 常用 0.5）")
    parser.add_argument("--n", type=float, default=1.0)
    parser.add_argument("--iterations", type=int, default=12)
    parser.add_argument("--thermal-iterations", type=int, default=6)
    parser.add_argument("--thermal-rate", type=float, default=0.5)
    parser.add_argument("--talus", type=float, default=36.0)
    parser.add_argument("--deposition", type=float, default=0.5,
                        help="沉积比例 0..1（对应 1 - hydro_removalrate）；"
                             ">0 时同时产出 debris/sediment 掩膜")
    parser.add_argument("--flow-method", choices=("mfd", "d8"), default="d8")
    parser.add_argument("--fill-sinks", action="store_true",
                        help="路由前填洼（河网更连续，但洼地不会下切）")
    parser.add_argument("--network-refresh", type=int, default=0,
                        help="每 N 次迭代重建汇流网络；0 = 只建一次")
    parser.add_argument("--force-network", action="store_true",
                        help="忽略缓存，重建汇流网络")
    return parser.parse_args(argv)


def _network_key(image: np.ndarray, cell: float, epsilon: float,
                 method: str, fill_sinks: bool) -> str:
    digest = hashlib.blake2b(digest_size=16)
    digest.update(np.ascontiguousarray(image, dtype=np.float32).tobytes())
    digest.update(f"|{cell:.6f}|{epsilon:.6g}|{method}|{int(fill_sinks)}"
                  .encode("ascii"))
    return digest.hexdigest()


def _resize_bilinear(values: np.ndarray, target: int) -> np.ndarray:
    source = np.asarray(values, dtype=np.float64)
    if source.shape == (target, target):
        return source
    rows, columns = source.shape
    x = np.linspace(0.0, columns - 1.0, target)
    y = np.linspace(0.0, rows - 1.0, target)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, columns - 1)
    y1 = np.minimum(y0 + 1, rows - 1)
    fx = x - x0
    fy = y - y0
    top = (source[y0[:, None], x0[None, :]] * (1.0 - fx)[None, :]
           + source[y0[:, None], x1[None, :]] * fx[None, :])
    bottom = (source[y1[:, None], x0[None, :]] * (1.0 - fx)[None, :]
              + source[y1[:, None], x1[None, :]] * fx[None, :])
    return top * (1.0 - fy)[:, None] + bottom * fy[:, None]


def _smooth_field(shape: tuple[int, int], cell: float, element: float,
                  rng: np.random.Generator) -> np.ndarray:
    side = shape[0]
    coarse = int(round(max(1.0, (side - 1) * cell / max(element, cell))))
    coarse = min(side, max(2, coarse))
    values = rng.random((coarse, coarse))
    field = _resize_bilinear(values, side)
    low, high = float(field.min()), float(field.max())
    return (field - low) / max(high - low, 1e-9)


def _warp_surface(surface: np.ndarray, cell: float, amplitude_m: float,
                  element_m: float, rng: np.random.Generator,
                  mask_strength: float) -> np.ndarray:
    """Deterministic domain warp, approximating Houdini distort2/3."""
    side = surface.shape[0]
    dx_field = _smooth_field(surface.shape, cell, element_m, rng) * 2.0 - 1.0
    dy_field = _smooth_field(surface.shape, cell, element_m, rng) * 2.0 - 1.0
    mask_field = _smooth_field(surface.shape, cell, element_m * 2.0, rng)
    mask = 1.0 - np.clip(mask_strength, 0.0, 1.0) * (1.0 - mask_field)
    grid_y, grid_x = np.mgrid[0:side, 0:side]
    dx = dx_field * (amplitude_m / max(cell, 1e-9)) * mask
    dy = dy_field * (amplitude_m / max(cell, 1e-9)) * mask
    sample_x = np.clip(grid_x + dx, 0.0, side - 1.0)
    sample_y = np.clip(grid_y + dy, 0.0, side - 1.0)
    x0 = np.floor(sample_x).astype(np.int64)
    y0 = np.floor(sample_y).astype(np.int64)
    x1 = np.minimum(x0 + 1, side - 1)
    y1 = np.minimum(y0 + 1, side - 1)
    fx = sample_x - x0
    fy = sample_y - y0
    top = (surface[y0, x0] * (1.0 - fx) + surface[y0, x1] * fx)
    bottom = (surface[y1, x0] * (1.0 - fx) + surface[y1, x1] * fx)
    return top * (1.0 - fy) + bottom * fy


def main() -> int:
    args = _parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    started = time.perf_counter()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    input_path = args.input or (
        args.manifest.parent / "Source" / "heightmap_shape.png"
    )
    shape_meta = input_path.with_suffix(".json")
    if shape_meta.exists():
        shape_range = json.loads(shape_meta.read_text(encoding="utf-8"))
        low = float(shape_range["low"])
        high = float(shape_range["high"])
    else:
        low = float(manifest["sourceLow"])
        high = float(manifest["sourceHigh"])
    if not input_path.exists():
        raise SystemExit(
            f"shape heightmap not found: {input_path}\n"
            "run build_pegasus_library_global.py --no-erosion first"
        )
    raw = read_gray16(input_path).astype(np.float64) / 65535.0
    height = low + raw * (high - low)
    side = height.shape[0]
    cell = args.size_m / max(side - 1, 1)
    print(f"input {input_path.name} {side}x{side} cell={cell:.3f} m "
          f"range={low:.2f}..{high:.2f}")

    fill_epsilon = erosion.ErosionParams().fill_epsilon
    if args.cache_dir is not None:
        args.cache_dir.mkdir(parents=True, exist_ok=True)

    # --- 物理参数换算 ---------------------------------------------------
    #
    # SPL:  ∂h/∂t = U - K A^m S^n
    #   K [1/yr]（m=0.5、A 以 m² 计），U [m/yr]，dt [yr]
    # solver 内部已经乘了 dt（coefficient = k·dt·A^m），所以这里传的 k 就是 K。
    steps = int(args.steps if args.steps is not None else args.iterations)
    dt_years = float(args.dt_years)
    k = float(args.k) if args.k is not None else float(args.erodibility)
    uplift_m_per_year = float(args.uplift_mm_per_kyr) * 1e-3 / 1000.0
    # 有抬升时地表每步都在变，网络必须跟着重建，否则上下游关系会过期。
    network_refresh = args.network_refresh
    if uplift_m_per_year > 0.0 and network_refresh == 0:
        network_refresh = 1
    print(f"SPL: K={k:.3g}/yr  dt={dt_years:g} yr  steps={steps}  "
          f"U={args.uplift_mm_per_kyr:g} mm/kyr "
          f"-> total uplift {args.uplift_mm_per_kyr * 1e-3 * dt_years * steps / 1000:.2f} m")

    def get_network(surface: np.ndarray, method: str, cell_m: float,
                    fill_sinks: bool) -> erosion.FlowNetwork:
        cache_path = None
        if args.cache_dir is not None:
            key = _network_key(surface, cell_m, fill_epsilon, method, fill_sinks)
            cache_path = args.cache_dir / f"network_{key}.npz"
            if cache_path.exists() and not args.force_network:
                print(f"network cache hit {cache_path.name}")
                return erosion.load_flow_network(cache_path)
            print(f"network cache miss {cache_path.name}")
        started = time.perf_counter()
        network = erosion.build_flow_network(
            surface, cell_m, fill_epsilon, method, fill_sinks
        )
        print(f"network built in {time.perf_counter() - started:.2f} s")
        if cache_path is not None:
            erosion.save_flow_network(network, cache_path)
        return network

    def run_pass(surface: np.ndarray, method: str, k: float, cell_m: float, *,
                 thermal_rate: float | None = None,
                 talus: float | None = None,
                 deposition: float = 0.0,
                 label: str = ""):
        effective_thermal = (args.thermal_rate if thermal_rate is None
                             else thermal_rate)
        params = erosion.ErosionParams(
            method="implicit",
            flow_method=method,
            fill_sinks=args.fill_sinks,
            network_refresh=network_refresh,
            iterations=steps,
            k=k,
            m=args.m,
            n=args.n,
            dt=dt_years,
            uplift_m=uplift_m_per_year,
            deposition_rate=deposition,
            # thermal_erodability=0（Houdini erode4）就是关闭坡面松弛，
            # 用 0 次迭代表达；solver 的 rate 校验仍要求 > 0。
            thermal_iterations=(args.thermal_iterations
                                if effective_thermal > 0.0 else 0),
            thermal_rate=max(effective_thermal, 1.0e-6),
            talus_deg=(args.talus if talus is None else talus),
        )
        network = get_network(surface, method, cell_m, args.fill_sinks)
        result = erosion.erode(
            surface, cell_m, params, diagnostics,
            network=network, compute_final_flow=False,
            layers=layers,
        )
        if label:
            print(f"{label}: rms={np.sqrt(((result[0] - surface) ** 2).mean()):.4f} m")
        return result

    diagnostics = erosion.ErosionDiagnostics()
    layers: dict[str, np.ndarray] = {}
    build_start = time.perf_counter()
    if args.chain == "pegasus":
        if args.chain_resample:
            original_side = height.shape[0]
            work_side = max(2, original_side // 2)
            work = _resize_bilinear(height, work_side)
            work_cell = args.size_m / max(work_side - 1, 1)
            rng = np.random.default_rng(args.warp_seed)
            flow_area = None
            for entry in PEGASUS_PASSES[:2]:
                work, flow_area, _ = run_pass(
                    work, entry["flow"], k * entry["k"] * args.chain_k,
                    work_cell,
                    thermal_rate=args.thermal_rate * entry["thermal"],
                    talus=entry["talus"],
                    deposition=entry["deposition"],
                    label=f"{entry['name']} ({entry['houdini']}) @{work_side}",
                )
            work_side = max(2, work_side // 2)
            work = _resize_bilinear(work, work_side)
            work_cell = args.size_m / max(work_side - 1, 1)
            for name, values in list(layers.items()):
                if values.shape != work.shape:
                    layers[name] = _resize_bilinear(values, work_side)
            if args.chain_warp:
                work = _warp_surface(
                    work, work_cell, 60.0, 1000.0, rng,
                    args.chain_warp_mask)
            for entry in PEGASUS_PASSES[2:3]:
                work, flow_area, _ = run_pass(
                    work, entry["flow"], k * entry["k"] * args.chain_k,
                    work_cell,
                    thermal_rate=args.thermal_rate * entry["thermal"],
                    talus=entry["talus"],
                    deposition=entry["deposition"],
                    label=f"{entry['name']} ({entry['houdini']}) @{work_side}",
                )
            if args.chain_warp:
                work = _warp_surface(
                    work, work_cell, 6.0, 80.0, rng,
                    args.chain_warp_mask)
            for entry in PEGASUS_PASSES[3:]:
                work, flow_area, _ = run_pass(
                    work, entry["flow"], k * entry["k"] * args.chain_k,
                    work_cell,
                    thermal_rate=args.thermal_rate * entry["thermal"],
                    talus=entry["talus"],
                    deposition=entry["deposition"],
                    label=f"{entry['name']} ({entry['houdini']}) @{work_side}",
                )
            eroded = _resize_bilinear(work, original_side)
            if args.chain_detail_restore > 0.0:
                smooth_side = max(2, original_side // 4)
                smooth = _resize_bilinear(
                    _resize_bilinear(height, smooth_side), original_side)
                eroded = (eroded + args.chain_detail_restore
                          * (height - smooth))
            for name, values in list(layers.items()):
                if values.shape != (original_side, original_side):
                    layers[name] = _resize_bilinear(values, original_side)
            if (flow_area is not None
                    and flow_area.shape != (original_side, original_side)):
                flow_area = _resize_bilinear(flow_area, original_side)
            print("chain: 4 passes done (512/256 resample + domain warp)")
        else:
            eroded = height
            flow_area = None
            for entry in PEGASUS_PASSES:
                eroded, flow_area, _ = run_pass(
                    eroded, entry["flow"], k * entry["k"] * args.chain_k,
                    cell,
                    thermal_rate=args.thermal_rate * entry["thermal"],
                    talus=entry["talus"],
                    deposition=entry["deposition"],
                    label=f"{entry['name']} ({entry['houdini']})",
                )
            print(f"chain: {len(PEGASUS_PASSES)} passes done")
    elif args.passes == "two":
        coarse, _, _ = run_pass(height, "d8", args.coarse_k, cell,
                                deposition=args.deposition)
        print(f"pass 1 (d8 k={args.coarse_k:g}) done")
        eroded, flow_area, _ = run_pass(coarse, "mfd", args.fine_k, cell,
                                        deposition=args.deposition)
        print(f"pass 2 (mfd k={args.fine_k:g}) done")
    else:
        eroded, flow_area, _ = run_pass(height, args.flow_method, k, cell,
                                        deposition=args.deposition)
    solve_seconds = time.perf_counter() - build_start
    print(f"erode {solve_seconds:.2f} s  steps={steps} passes={args.passes}")

    out_low = float(eroded.min())
    out_high = float(eroded.max())
    normalized = (eroded - out_low) / max(out_high - out_low, 1e-9)
    output = args.output or (input_path.parent / "heightmap_gn.png")
    write_gray16(output, (normalized * 65535.0).astype(np.uint16))
    manifest["sourceLow"] = out_low
    manifest["sourceHigh"] = out_high
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=4),
                             encoding="utf-8")
    print(f"wrote {output.name} range={out_low:.2f}..{out_high:.2f} "
          f"std={eroded.std():.2f}")

    # 汇流掩膜：把 area 归一化到 0..1（对数压缩），供引擎做河网着色/植被剔除。
    flow = flow_area / max(float(flow_area.mean()), 1e-9)
    flow_mask = np.clip(np.log1p(flow * 8.0) / np.log1p(512.0), 0.0, 1.0)
    flow_path = output.parent / "mask_gn_flow.png"
    write_gray16(flow_path, (flow_mask * 65535.0).astype(np.uint16))
    print(f"wrote {flow_path.name} (flow p50={np.percentile(flow, 50):.1f}x, "
          f"p99={np.percentile(flow, 99):.1f}x)")

    # --- S4 派生层 -----------------------------------------------------
    #
    # water：河道填充模型。水深与归一化汇水面积成正比、上限 WATER_MAX_M。
    # Houdini 的 water 层是 hydro solver 的输出，rms/mean 比纯 A^p 更集中
    # （实测 p≤0.6 只能到 2.17，参考是 3.44），简单幂律复刻不了；这里用
    # 「汇水面积线性 + 1.5 m 上限」这个物理上更直白的近似，掩膜写 0..1。
    water_sheet_m = 0.015
    water_channel_m = 1.5
    area_norm = flow_area / max(float(flow_area.max()), 1e-9)
    water_depth = water_sheet_m + water_channel_m * area_norm
    water_peak_m = water_sheet_m + water_channel_m
    water_path = output.parent / "mask_gn_water.png"
    write_gray16(water_path,
                 (np.clip(water_depth / water_peak_m, 0.0, 1.0)
                  * 65535.0).astype(np.uint16))
    print(f"wrote {water_path.name} (depth mean={water_depth.mean():.4f} "
          f"rms={np.sqrt((water_depth ** 2).mean()):.4f} "
          f"max={water_depth.max():.2f} m)")

    # bedrock：4 km 参考的 height_after_bedrock 是高度层（不是常量 1），
    # 范围接近侵蚀前 shape；按 Houdini 的 layer 语义导出归一化高度。
    bedrock_normalized = (height - low) / max(high - low, 1e-9)
    bedrock_path = output.parent / "mask_gn_bedrock.png"
    write_gray16(bedrock_path,
                 (np.clip(bedrock_normalized, 0.0, 1.0)
                  * 65535.0).astype(np.uint16))
    print("wrote mask_gn_bedrock.png (height layer)")

    # debris / sediment：来自求解器的沉积台账（--deposition > 0 才有）。
    # debris = 累计沉积；sediment = 累计被带走（净侵蚀 - 沉积）。
    # Houdini 的 remap_debris/remap_sediment 是 0..1 clamp，不做按峰值归一化；
    # 这里保持绝对米制后再 clamp，才能和参考的 max≈1 m 对齐。
    for name in ("debris", "sediment"):
        values = layers.get(name)
        if values is None:
            values = np.zeros_like(area_norm)
        write_gray16(output.parent / f"mask_gn_{name}.png",
                     (np.clip(values, 0.0, 1.0)
                      * 65535.0).astype(np.uint16))
        print(f"wrote mask_gn_{name}.png "
              f"(rms={np.sqrt((values ** 2).mean()):.4f} "
              f"max={float(values.max()):.4f} m, clamped to 1)")

    print(f"total {time.perf_counter() - started:.2f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
