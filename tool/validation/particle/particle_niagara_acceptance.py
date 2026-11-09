#!/usr/bin/env python3
"""Audit the UE Niagara -> VulkanLearn effect alignment.

The reference UE project contains 67 ``NS_*.uasset`` systems. Four editor
markers, four Boundary debug systems, eleven SpriteGeneration utilities and one
material-reference utility are not effects, which leaves 49 targets.

This tool never changes particle runtime behaviour. It only:

1. derives the 49 targets from the UE project;
2. maps every target to VL particle assets through the gallery READMEs;
3. parses every PS/gallery JSON and checks referenced textures/meshes/MIs;
4. optionally runs every gallery headlessly and records load errors, VUIDs and
   particle frame stats;
5. writes a Markdown matrix plus a machine-readable JSON result.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import math
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from difflib import SequenceMatcher
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]

EXCLUDED_UE_TOP_DIRS = {"FX_Markers", "Utilities", "Materials"}
EXCLUDED_UE_PREFIXES = ("NS_Boundary",)
EXPECTED_TARGET_COUNT = 49

REFERENCE_KEYS = {
    "texture",
    "textureAsset",
    "path",
    "materialInstancePath",
    "materialOverride",
    "particlePath",
    "modelPath",
    "terrainPath",
    "albedoMap",
    "normalMap",
    "ormMap",
    "emissiveMap",
    "metallicMap",
    "roughnessMap",
    "sdfPath",
}

VUID_RE = re.compile(r"VUID-[A-Za-z0-9_-]+")
STATS_RE = re.compile(r"^particles frame=.*$", re.MULTILINE)
APPROX_HEADINGS = ("已知近似", "已知替身", "近似", "替身", "边界")

TEXTURE_KEYS = {
    "texture",
    "textureAsset",
    "albedoMap",
    "normalMap",
    "ormMap",
    "emissiveMap",
    "metallicMap",
    "roughnessMap",
}

STATS_METRICS = (
    "particles",
    "drawCalls",
    "vertices",
    "gpuMs",
    "gpuAvgMs",
    "gpuMaxMs",
)

# gpuMs is the last frame's instantaneous value and gpuMaxMs is the single
# worst frame in the whole run. Both are collected and shown, but one outlier
# frame (startup uploads, pipeline creation) is too noisy to be a pass/over
# gate; the cumulative gpuAvgMs counter carries the GPU budget.
BUDGET_METRICS = (
    "particles",
    "drawCalls",
    "vertices",
    "gpuAvgMs",
)
INFORMATIONAL_METRICS = ("gpuMs", "gpuMaxMs")

# A single emitter cannot exceed 65536 particles (particleAsset.cpp), but the
# whole PS can legitimately be much larger because it may contain 16 emitters.
# These are sanity thresholds for warnings, not validation failures.
LARGE_PS_MAX_PARTICLES = 100000
LARGE_EMITTER_MAX_PARTICLES = 65536

DEFAULT_BUDGET_REL = "tool/validation/particle/particle_performance_budget.json"
DEFAULT_BUDGET_MARGIN_PERCENT = 20.0


@dataclass
class Reference:
    source: str
    field: str
    value: str
    resolved: bool


@dataclass
class Target:
    name: str
    category: str
    ue_path: str
    ps_paths: list[str] = field(default_factory=list)
    galleries: list[str] = field(default_factory=list)
    screenshots: list[str] = field(default_factory=list)
    mapping_source: str = "unmapped"
    approximations: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    smoke: dict | None = None
    budget: dict | None = None

    @property
    def status(self) -> str:
        if self.errors:
            return "FAIL"
        if self.smoke and not self.smoke.get("ok"):
            return "FAIL"
        if self.warnings:
            return "WARN"
        if self.smoke and self.smoke.get("vuids"):
            return "WARN"
        if not self.smoke:
            return "STATIC-OK"
        return "PASS"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig", errors="replace")


def load_json(path: Path):
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def rel(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def normalize_name(name: str) -> str:
    base = name
    for prefix in ("NS_", "PS_", "NE_"):
        if base.startswith(prefix):
            base = base[len(prefix) :]
            break
    lowered = base.lower()
    if lowered.startswith("ue_"):
        lowered = lowered[3:]
    return re.sub(r"[^a-z0-9]", "", lowered)


def name_tokens(name: str) -> set[str]:
    base = name
    for prefix in ("NS_", "PS_", "NE_"):
        if base.startswith(prefix):
            base = base[len(prefix) :]
            break
    tokens = {
        token.lower()
        for token in re.split(r"[^A-Za-z0-9]+", base)
        if token and token.lower() not in {"ue", "vl"}
    }
    return tokens


def name_similarity(target: str, asset: str) -> float:
    target_norm = normalize_name(target)
    asset_norm = normalize_name(asset)
    sequence = (
        SequenceMatcher(None, target_norm, asset_norm).ratio()
        if target_norm and asset_norm
        else 0.0
    )
    target_tokens = name_tokens(target)
    asset_tokens = name_tokens(asset)
    containment = 0.0
    if target_tokens and asset_tokens:
        containment = len(target_tokens & asset_tokens) / min(
            len(target_tokens), len(asset_tokens)
        )
    return max(sequence, containment)


def derive_targets(ue_root: Path) -> tuple[list[Target], dict]:
    scanned: list[str] = []
    excluded: dict[str, list[str]] = {
        "editor-marker": [],
        "boundary-debug": [],
        "sprite-generation": [],
        "material-reference": [],
    }
    targets: list[Target] = []
    for asset in sorted(ue_root.rglob("NS_*.uasset")):
        rel_path = asset.relative_to(ue_root).as_posix()
        scanned.append(rel_path)
        parts = rel_path.split("/")
        top_dir = parts[0]
        stem = asset.stem
        if top_dir in EXCLUDED_UE_TOP_DIRS:
            if top_dir == "FX_Markers":
                excluded["editor-marker"].append(rel_path)
            elif top_dir == "Utilities":
                excluded["sprite-generation"].append(rel_path)
            else:
                excluded["material-reference"].append(rel_path)
            continue
        if stem.startswith(EXCLUDED_UE_PREFIXES):
            excluded["boundary-debug"].append(rel_path)
            continue
        targets.append(
            Target(
                name=stem,
                category="/".join(parts[:-1]) or "(root)",
                ue_path=rel_path,
            )
        )
    summary = {
        "scanned": len(scanned),
        "targets": len(targets),
        "excluded": excluded,
        "excluded_total": sum(len(values) for values in excluded.values()),
    }
    return targets, summary


def collect_galleries(resource_root: Path) -> tuple[dict, dict]:
    """Return gallery -> scenes/PS and PS -> galleries."""
    galleries: dict[str, dict] = {}
    ps_to_galleries: dict[str, list[str]] = {}
    maps_root = resource_root / "Maps"
    if not maps_root.is_dir():
        return galleries, ps_to_galleries
    for directory in sorted(maps_root.iterdir()):
        if not directory.is_dir() or not directory.name.startswith("SC_particle_ue_"):
            continue
        scenes = sorted(directory.glob("SC_*.json"))
        if not scenes:
            continue
        entries = []
        used: list[str] = []
        for scene in scenes:
            try:
                document = load_json(scene)
            except Exception as error:  # JSON errors are reported by static checks.
                entries.append({"scene": rel(scene, resource_root), "error": str(error)})
                continue
            particle_paths = []
            for obj in document.get("objects", []):
                if not isinstance(obj, dict) or obj.get("type") != "particleSystem":
                    continue
                path = obj.get("particlePath")
                if isinstance(path, str) and path:
                    normalized = path.replace("\\", "/")
                    particle_paths.append(normalized)
                    used.append(normalized)
                    ps_to_galleries.setdefault(normalized, [])
                    if rel(scene, resource_root) not in ps_to_galleries[normalized]:
                        ps_to_galleries[normalized].append(rel(scene, resource_root))
            entries.append(
                {
                    "scene": rel(scene, resource_root),
                    "particle_systems": particle_paths,
                }
            )
        galleries[directory.name] = {
            "dir": rel(directory, resource_root),
            "scenes": entries,
            "particle_systems": sorted(set(used)),
            "readme": rel(directory / "README.md", resource_root)
            if (directory / "README.md").is_file()
            else None,
        }
    return galleries, ps_to_galleries


def parse_readme_pairs(text: str) -> list[tuple[str, str]]:
    """Find NS/PS pairs that share a Markdown line or table row."""
    pairs: list[tuple[str, str]] = []
    for line in text.splitlines():
        systems = re.findall(r"NS_[A-Za-z0-9_]+", line)
        assets = re.findall(r"PS_[A-Za-z0-9_]+\.json", line)
        if systems and assets:
            for system in systems:
                for asset in assets:
                    pairs.append((system, asset))
    return pairs


def extract_approximations(text: str) -> list[str]:
    result: list[str] = []
    active = False
    current: str | None = None

    def flush():
        nonlocal current
        if current:
            if current not in result:
                result.append(current)
        current = None

    for line in text.splitlines():
        if line.startswith("#"):
            flush()
            heading = line.lstrip("#").strip()
            active = any(marker in heading for marker in APPROX_HEADINGS)
            continue
        if not active:
            continue
        stripped = line.strip()
        if not stripped:
            flush()
            continue
        if stripped.startswith(("- ", "* ")):
            flush()
            current = stripped[2:].strip()
            continue
        if stripped.startswith(("|", "```", ">")):
            flush()
            continue
        if current is not None:
            # README bullets wrap across lines; join CJK without a space and
            # keep ASCII words separated.
            if current and current[-1].isascii() and current[-1].isalnum() \
                    and stripped[0].isascii() and stripped[0].isalnum():
                current += " " + stripped
            else:
                current += stripped
    flush()
    return result


def readme_screenshots(readme: Path, resource_root: Path) -> list[str]:
    if not readme.is_file():
        return []
    shots: list[str] = []
    for token in re.findall(r"[A-Za-z0-9_.-]+\.bmp", read_text(readme)):
        if (resource_root / "Generated" / "Screenshots" / token).is_file():
            if token not in shots:
                shots.append(token)
    return shots


def gallery_screenshots(gallery: dict, resource_root: Path, readme: Path | None) -> list[str]:
    screenshots_dir = resource_root / "Generated" / "Screenshots"
    if not screenshots_dir.is_dir():
        return []
    short = gallery["dir"].split("/")[-1]
    for prefix in ("SC_particle_ue_", "SC_"):
        if short.startswith(prefix):
            short = short[len(prefix) :]
            break
    candidates = [
        f"{short}_gallery.bmp",
        f"{short}.bmp",
        f"ue_{short}.bmp",
        f"SC_{short}.bmp",
    ]
    for candidate in candidates:
        if (screenshots_dir / candidate).is_file():
            return [candidate]
    mentioned = readme_screenshots(readme, resource_root) if readme else []
    if not mentioned:
        return []

    def score(token: str) -> tuple[int, int]:
        lowered = token.lower()
        return (
            3
            if "gallery" in lowered
            else 2
            if "full" in lowered
            else 1
            if short in lowered
            else 0,
            -len(token),
        )

    return [max(mentioned, key=score)]


def walk_references(document, source: str) -> list[Reference]:
    references: list[Reference] = []

    def visit(node, pointer: str):
        if isinstance(node, dict):
            for key, value in node.items():
                child = f"{pointer}/{key}" if pointer else f"/{key}"
                if key in REFERENCE_KEYS and isinstance(value, str) and value.strip():
                    references.append(
                        Reference(source=source, field=child, value=value.strip(), resolved=False)
                    )
                visit(value, child)
        elif isinstance(node, list):
            for index, value in enumerate(node):
                visit(value, f"{pointer}/{index}")

    visit(document, "")
    return references


def resolve_reference(reference: str, resource_root: Path) -> Path | None:
    portable = reference.replace("\\", "/")
    candidate = Path(portable)
    if candidate.is_absolute():
        return candidate if candidate.exists() else None
    for root in (resource_root, REPO_ROOT):
        path = root / candidate
        if path.exists():
            return path
    return None


def check_references(
    references: list[Reference], resource_root: Path, errors: list[str]
) -> None:
    for reference in references:
        resolved = resolve_reference(reference.value, resource_root)
        reference.resolved = resolved is not None
        if not reference.resolved:
            errors.append(
                f"{reference.source}{reference.field} -> missing: {reference.value}"
            )


def structural_hash(document) -> str:
    """Hash the emitter graph without the per-placement emitter name/position."""
    emitters = document.get("emitters", [])
    normalized = []
    for emitter in emitters:
        if not isinstance(emitter, dict):
            continue
        copy = dict(emitter)
        copy.pop("name", None)
        copy.pop("position", None)
        normalized.append(copy)
    payload = json.dumps(normalized, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def enabled_modules(emitter: dict, module_type: str) -> list[dict]:
    modules = emitter.get("modules")
    if not isinstance(modules, list):
        return []
    return [
        module
        for module in modules
        if isinstance(module, dict)
        and module.get("type") == module_type
        and module.get("enabled", True)
    ]


def emitter_field(emitter: dict, module_type: str, field: str, default=None):
    """Read a v2 module field, falling back to the legacy flat emitter field."""
    modules = enabled_modules(emitter, module_type)
    if modules:
        fields = modules[0].get("fields")
        if isinstance(fields, dict) and field in fields:
            return fields[field]
    return emitter.get(field, default)


def emitter_render_mode(emitter: dict) -> str:
    value = emitter_field(emitter, "renderer", "renderMode", "billboard")
    return value if isinstance(value, str) and value else "billboard"


def collect_texture_references(node) -> list[str]:
    references: list[str] = []

    def visit(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key in TEXTURE_KEYS and isinstance(child, str) and child.strip():
                    references.append(child.strip())
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(node)
    return references


def analyze_particle_system(path: Path, resource_root: Path) -> tuple[list[str], list[str], dict]:
    """Lint one PS and collect the static metrics shown in the final matrix."""
    errors, warnings = lint_particle_system(path, resource_root)
    source = rel(path, resource_root)
    metrics = {
        "source": source,
        "emitter_count": 0,
        "render_modes": {},
        "max_particles_sum": 0,
        "max_particles_max": 0,
        "max_particles_missing": 0,
        "texture_reference_count": 0,
        "unique_texture_reference_count": 0,
        "texture_references": [],
        "status": "PASS",
        "warnings": [],
    }
    try:
        document = load_json(path)
    except Exception:
        metrics["status"] = "FAIL"
        return errors, warnings, metrics

    emitters = document.get("emitters")
    if not isinstance(emitters, list):
        emitters = []
    metrics["emitter_count"] = len(emitters)

    render_modes: dict[str, int] = {}
    texture_references: list[str] = []
    total_max_particles = 0
    largest_emitter = 0
    missing_max_particles = 0

    for index, emitter in enumerate(emitters):
        if not isinstance(emitter, dict):
            continue
        name = emitter.get("name") or f"#{index}"
        mode = emitter_render_mode(emitter)
        render_modes[mode] = render_modes.get(mode, 0) + 1
        max_particles = emitter_field(emitter, "emitter", "maxParticles")
        if isinstance(max_particles, (int, float)) and not isinstance(max_particles, bool):
            count = max(0, int(max_particles))
            total_max_particles += count
            largest_emitter = max(largest_emitter, count)
        else:
            missing_max_particles += 1
            warnings.append(
                f"{source}: emitter '{name}' has no maxParticles (runtime default applies)"
            )
        renderer_modules = enabled_modules(emitter, "renderer")
        if len(renderer_modules) > 1:
            warnings.append(
                f"{source}: emitter '{name}' has {len(renderer_modules)} enabled renderer modules"
            )
        texture_references.extend(collect_texture_references(emitter))

    metrics["render_modes"] = dict(sorted(render_modes.items()))
    metrics["max_particles_sum"] = total_max_particles
    metrics["max_particles_max"] = largest_emitter
    metrics["max_particles_missing"] = missing_max_particles
    metrics["texture_references"] = sorted(set(texture_references))
    metrics["texture_reference_count"] = len(texture_references)
    metrics["unique_texture_reference_count"] = len(set(texture_references))

    if emitters:
        if total_max_particles == 0:
            warnings.append(f"{source}: every emitter has maxParticles=0")
        if total_max_particles > LARGE_PS_MAX_PARTICLES:
            warnings.append(
                f"{source}: maxParticles sum {total_max_particles} exceeds "
                f"{LARGE_PS_MAX_PARTICLES}"
            )
        if largest_emitter > LARGE_EMITTER_MAX_PARTICLES:
            warnings.append(
                f"{source}: emitter maxParticles {largest_emitter} exceeds "
                f"{LARGE_EMITTER_MAX_PARTICLES}"
            )

    metrics["warnings"] = list(dict.fromkeys(warnings))
    metrics["status"] = "FAIL" if errors else "WARN" if metrics["warnings"] else "PASS"
    return errors, list(dict.fromkeys(warnings)), metrics


def lint_particle_system(path: Path, resource_root: Path) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    source = rel(path, resource_root)
    try:
        document = load_json(path)
    except Exception as error:
        return [f"{source}: JSON parse failed: {error}"], warnings

    if document.get("type") != "particleSystem":
        errors.append(f"{source}: type must be 'particleSystem'")
    if not isinstance(document.get("name"), str) or not document.get("name"):
        errors.append(f"{source}: name must be a non-empty string")
    emitters = document.get("emitters")
    if not isinstance(emitters, list) or not emitters:
        errors.append(f"{source}: emitters must be a non-empty array")
        emitters = []
    elif len(emitters) > 16:
        errors.append(f"{source}: {len(emitters)} emitters exceed the 16 limit")

    names: set[str] = set()
    for index, emitter in enumerate(emitters):
        if not isinstance(emitter, dict):
            errors.append(f"{source}: emitter #{index} is not an object")
            continue
        name = emitter.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"{source}: emitter #{index} has no name")
            continue
        if name in names:
            errors.append(f"{source}: duplicate emitter name '{name}'")
        names.add(name)

    # Parent-emitter spawn sources must point at a sibling emitter.
    for emitter in emitters:
        if not isinstance(emitter, dict):
            continue
        spawn_source = emitter.get("spawnSource")
        if not isinstance(spawn_source, dict):
            continue
        if spawn_source.get("mode") != "parentEmitter":
            continue
        parent = spawn_source.get("parentEmitter")
        if not isinstance(parent, str) or not parent:
            errors.append(
                f"{source}: emitter '{emitter.get('name')}' uses parentEmitter without a parent"
            )
        elif parent not in names:
            errors.append(
                f"{source}: emitter '{emitter.get('name')}' references missing parent '{parent}'"
            )
        per_event = spawn_source.get("perEvent")
        if isinstance(per_event, (int, float)) and not 1 <= per_event <= 64:
            errors.append(
                f"{source}: emitter '{emitter.get('name')}' perEvent={per_event} outside [1,64]"
            )

    references = walk_references(document, source)
    check_references(references, resource_root, errors)
    return errors, warnings


def lint_scene(
    path: Path, resource_root: Path, known_ps: set[str]
) -> tuple[list[str], list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    source = rel(path, resource_root)
    try:
        document = load_json(path)
    except Exception as error:
        return [f"{source}: JSON parse failed: {error}"], warnings, []

    if document.get("type") != "scene":
        errors.append(f"{source}: type must be 'scene'")
    for key in ("name", "description"):
        if not isinstance(document.get(key), str) or not document.get(key):
            errors.append(f"{source}: {key} must be a non-empty string")
    objects = document.get("objects")
    if not isinstance(objects, list) or not objects:
        errors.append(f"{source}: objects must be a non-empty array")
        objects = []

    particle_paths: list[str] = []
    for obj in objects:
        if not isinstance(obj, dict) or obj.get("type") != "particleSystem":
            continue
        particle_path = obj.get("particlePath")
        if not isinstance(particle_path, str) or not particle_path:
            errors.append(f"{source}: particleSystem '{obj.get('name')}' has no particlePath")
            continue
        normalized = particle_path.replace("\\", "/")
        particle_paths.append(normalized)
        if normalized not in known_ps:
            errors.append(
                f"{source}: particleSystem '{obj.get('name')}' references unknown PS: {normalized}"
            )
        elif not (resource_root / normalized).is_file():
            errors.append(
                f"{source}: particleSystem '{obj.get('name')}' PS file missing: {normalized}"
            )

    references = walk_references(document, source)
    check_references(references, resource_root, errors)
    if not particle_paths:
        errors.append(f"{source}: scene contains no particleSystem object")
    return errors, warnings, particle_paths


def parse_stats_samples(text: str) -> list[dict]:
    samples: list[dict] = []
    for line in STATS_RE.findall(text):
        result = {"raw": line}
        for key in (
            "frame",
            "emitters",
            "visible",
            "culled",
            "sorted",
            "drawCalls",
            "vertices",
            "particles",
            "sortDispatches",
            "sortCapacity",
            "gpuTimingSupported",
            "gpuMs",
            "gpuAvgMs",
            "gpuMaxMs",
            "gpuSamples",
        ):
            match = re.search(rf"\b{key}=([0-9.]+)", line)
            if match:
                value = match.group(1)
                result[key] = float(value) if "." in value else int(value)
        samples.append(result)
    return samples


def summarize_stats(samples: list[dict]) -> dict | None:
    """Combine several adjacent particlestats samples.

    Counts and the instantaneous gpuMs use the peak across the samples so a
    burst cannot hide between measurements. gpuAvgMs/gpuMaxMs are cumulative
    counters in ParticleFrameStats, so the last sample is the authoritative
    value for the whole run.
    """
    if not samples:
        return None
    summary = dict(samples[-1])
    for key in ("particles", "drawCalls", "vertices", "gpuMs"):
        values = [sample[key] for sample in samples if key in sample]
        if values:
            summary[key] = max(values)
    summary["sample_count"] = len(samples)
    summary["samples"] = samples
    return summary


def run_smoke(
    exe: Path,
    scene: str,
    resource_root: Path,
    out_dir: Path,
    timeout: float,
    delay: int,
) -> dict:
    stamp = _datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    stem = Path(scene).stem
    screenshot_name = f"particle_accept_{stamp}_{stem}.bmp"
    screenshot_path = resource_root / "Generated" / "Screenshots" / screenshot_name
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    if screenshot_path.exists():
        screenshot_path.unlink()

    stdout_path = out_dir / f"{stem}.stdout.txt"
    stderr_path = out_dir / f"{stem}.stderr.txt"
    # ConsoleSubsystem submits one launch command per frame after the delay.
    # Three adjacent particlestats samples catch a burst peak; the screenshot
    # is submitted one frame after the last sample so all stats are in stdout
    # before the process is stopped.
    command = [
        str(exe),
        "--initial-scene",
        scene,
        "--no-dev-ui",
        "--frame-limit",
        "60",
    ]
    for _ in range(3):
        command.extend(["--console-command", "particlestats"])
    command.extend(
        [
            "--console-command",
            f"screenshot {screenshot_name}",
            "--console-command-delay",
            str(delay),
        ]
    )
    start = time.time()
    with stdout_path.open("w", encoding="utf-8") as stdout_file, stderr_path.open(
        "w", encoding="utf-8"
    ) as stderr_file:
        process = subprocess.Popen(
            command,
            cwd=str(REPO_ROOT),
            stdout=stdout_file,
            stderr=stderr_file,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        last_size = -1
        stable = 0
        timed_out = False
        try:
            while True:
                if process.poll() is not None:
                    break
                if time.time() - start > timeout:
                    timed_out = True
                    break
                if screenshot_path.is_file():
                    size = screenshot_path.stat().st_size
                    if size > 0 and size == last_size:
                        stable += 1
                        if stable >= 2:
                            break
                    else:
                        stable = 0
                    last_size = size
                time.sleep(0.25)
        finally:
            if process.poll() is None:
                process.kill()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

    stdout_text = read_text(stdout_path)
    stderr_text = read_text(stderr_path)
    combined = stdout_text + "\n" + stderr_text
    vuids = sorted(set(VUID_RE.findall(combined)))
    load_failed = combined.count("Scene.LoadFailed")
    engine_failed = combined.count("EngineLoop init failed")
    stats_samples = parse_stats_samples(combined)
    stats = summarize_stats(stats_samples)
    screenshot_ok = screenshot_path.is_file() and screenshot_path.stat().st_size > 0
    ok = (
        not timed_out
        and load_failed == 0
        and engine_failed == 0
        and screenshot_ok
    )
    result = {
        "scene": scene,
        "ok": ok,
        "timed_out": timed_out,
        "elapsed_seconds": round(time.time() - start, 2),
        "scene_load_failed": load_failed,
        "engine_init_failed": engine_failed,
        "vuids": vuids,
        "stats": stats,
        "stats_samples": stats_samples,
        "screenshot": rel(screenshot_path, resource_root) if screenshot_ok else None,
        "screenshot_bytes": screenshot_path.stat().st_size if screenshot_ok else 0,
        "command": command,
        "stdout": rel(stdout_path, out_dir.parent),
        "stderr": rel(stderr_path, out_dir.parent),
    }
    if screenshot_ok:
        copy = out_dir / screenshot_name
        try:
            copy.write_bytes(screenshot_path.read_bytes())
            result["screenshot_copy"] = rel(copy, out_dir.parent)
        except OSError as error:
            result["screenshot_copy_error"] = str(error)
    return result


def budget_metrics(measured: dict | None) -> dict:
    if not measured:
        return {}
    return {
        key: measured[key]
        for key in STATS_METRICS
        if isinstance(measured.get(key), (int, float))
        and not isinstance(measured.get(key), bool)
    }


def compute_budget(baseline: dict, margin_percent: float) -> dict:
    multiplier = 1.0 + margin_percent / 100.0
    budget: dict = {}
    for key, value in baseline.items():
        if key in ("gpuMs", "gpuAvgMs", "gpuMaxMs"):
            budget[key] = round(float(value) * multiplier, 3)
        else:
            budget[key] = int(math.ceil(float(value) * multiplier))
    return budget


def compare_budget(measured: dict | None, budget: dict | None) -> dict:
    measured_metrics = budget_metrics(measured)
    if not budget:
        return {
            "status": "MISSING",
            "over": [],
            "measured": measured_metrics,
            "budget": {},
        }
    if not measured_metrics:
        return {
            "status": "MISSING",
            "over": [],
            "measured": {},
            "budget": budget,
        }
    over: list[str] = []
    for key in BUDGET_METRICS:
        limit = budget.get(key)
        if limit is None or key not in measured_metrics:
            continue
        if measured_metrics[key] > float(limit) + 1e-9:
            over.append(key)
    return {
        "status": "OVER" if over else "PASS",
        "over": over,
        "measured": measured_metrics,
        "budget": budget,
    }


def load_budget(path: Path) -> dict | None:
    if not path.is_file():
        return None
    try:
        document = load_json(path)
    except Exception:
        return None
    return document if isinstance(document, dict) else None


def write_budget_file(
    path: Path, galleries: dict, margin_percent: float, command_line: str
) -> dict:
    existing = load_budget(path) or {}
    document = {
        "version": 1,
        "generated_at": _datetime.datetime.now().isoformat(timespec="seconds"),
        "policy": {
            "margin_percent": margin_percent,
            "budget_metrics": list(BUDGET_METRICS),
            "informational_metrics": list(INFORMATIONAL_METRICS),
            "baseline_merge": "max(existing baseline, this measurement)",
            "measurement": (
                "three adjacent particlestats samples; particles/drawCalls/"
                "vertices/gpuMs use the peak, gpuAvgMs/gpuMaxMs use the final "
                "cumulative sample; gpuMs and gpuMaxMs are informational only "
                "because a single outlier frame is too noisy for pass/over"
            ),
            "budget_formula": "baseline * (1 + margin_percent/100)",
            "command": command_line,
        },
        "galleries": {},
    }
    for name, gallery in sorted(galleries.items()):
        scenes: dict = {}
        for entry in gallery["scenes"]:
            scene = entry["scene"]
            result = (gallery.get("smoke") or {}).get(scene)
            measured = budget_metrics((result or {}).get("stats"))
            if not measured:
                continue
            previous = (
                existing.get("galleries", {})
                .get(name, {})
                .get("scenes", {})
                .get(scene, {})
                .get("baseline", {})
            )
            baseline = {}
            for key, value in measured.items():
                candidates = [value]
                if isinstance(previous.get(key), (int, float)) and not isinstance(
                    previous.get(key), bool
                ):
                    candidates.append(previous[key])
                baseline[key] = max(candidates)
            scenes[scene] = {
                "baseline": baseline,
                "budget": compute_budget(baseline, margin_percent),
            }
        if scenes:
            document["galleries"][name] = {
                "dir": gallery["dir"],
                "scenes": scenes,
            }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return document


def apply_budget(galleries: dict, budget_document: dict | None) -> None:
    for name, gallery in galleries.items():
        gallery_budget = {
            "status": "MISSING",
            "over": [],
            "scenes": {},
        }
        budget_scenes = (
            (budget_document or {}).get("galleries", {}).get(name, {}).get("scenes", {})
        )
        for entry in gallery["scenes"]:
            scene = entry["scene"]
            result = (gallery.get("smoke") or {}).get(scene)
            measured = (result or {}).get("stats")
            comparison = compare_budget(measured, budget_scenes.get(scene, {}).get("budget"))
            gallery_budget["scenes"][scene] = comparison
            for key in comparison["over"]:
                if key not in gallery_budget["over"]:
                    gallery_budget["over"].append(key)
        statuses = [
            comparison["status"] for comparison in gallery_budget["scenes"].values()
        ]
        if "OVER" in statuses:
            gallery_budget["status"] = "OVER"
        elif statuses and all(status == "PASS" for status in statuses):
            gallery_budget["status"] = "PASS"
        else:
            gallery_budget["status"] = "MISSING"
        gallery["budget"] = gallery_budget


def budget_suggestion(key: str) -> str:
    if key == "particles":
        return "check spawnRate/burst/perEvent/lifetime; reduce emission or add a coarser LOD tier"
    if key == "drawCalls":
        return "merge emitters/materials or reduce enabled renderers"
    if key == "vertices":
        return "reduce mesh/ribbon segments or particle count"
    if key in ("gpuMs", "gpuAvgMs", "gpuMaxMs"):
        return "check overdraw, soft particles, lighting and sorting; lower renderScale or LOD"
    return "review the gallery against its budget baseline"


def html_escape(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def write_matrix(
    out_dir: Path,
    targets: list[Target],
    summary: dict,
    galleries: dict,
    ps_errors: dict,
    ps_warnings: dict,
    ps_metrics: dict,
    scene_errors: dict,
    scene_warnings: dict,
    duplicates: dict,
    smoke_enabled: bool,
    command_line: str,
    budget_document: dict | None,
    budget_policy: dict,
) -> Path:
    lines: list[str] = []
    status_counts = {status: 0 for status in ("PASS", "STATIC-OK", "WARN", "FAIL")}
    for target in targets:
        status_counts[target.status] += 1
    budget_counts = {"PASS": 0, "OVER": 0, "MISSING": 0}
    for gallery in galleries.values():
        budget_counts[gallery.get("budget", {}).get("status", "MISSING")] += 1
    lines.append("# UE Niagara 特效 VL 验收矩阵")
    lines.append("")
    lines.append(
        f"- 生成时间：{_datetime.datetime.now().isoformat(timespec='seconds')}"
    )
    lines.append(f"- 工具：`tool/validation/particle/particle_niagara_acceptance.py`")
    lines.append(f"- 命令：`{command_line}`")
    lines.append(
        f"- 状态汇总：**{status_counts['FAIL']} FAIL / {status_counts['WARN']} WARN / "
        f"{status_counts['STATIC-OK']} STATIC-OK / {status_counts['PASS']} PASS**"
    )
    lines.append(
        f"- smoke：{'已执行' if smoke_enabled else '未执行'}（每 gallery 一次 headless 启动）"
    )
    lines.append(
        f"- 性能预算：**{budget_counts['PASS']} PASS / {budget_counts['OVER']} OVER / "
        f"{budget_counts['MISSING']} MISSING**（baseline + "
        f"{budget_policy.get('margin_percent', DEFAULT_BUDGET_MARGIN_PERCENT):g}%）"
    )
    lines.append(
        f"- 预算文件：`{budget_policy.get('file', DEFAULT_BUDGET_REL)}`"
    )
    lines.append("")
    lines.append("## 目标推导")
    lines.append("")
    lines.append(
        f"- UE `NS_*.uasset` 扫描：**{summary['scanned']}** 个"
    )
    lines.append(
        f"- 排除：**{summary['excluded_total']}** 个 "
        f"（editor-marker {len(summary['excluded']['editor-marker'])}、"
        f"boundary-debug {len(summary['excluded']['boundary-debug'])}、"
        f"sprite-generation {len(summary['excluded']['sprite-generation'])}、"
        f"material-reference {len(summary['excluded']['material-reference'])}）"
    )
    lines.append(f"- 目标：**{len(targets)}** 个")
    lines.append(
        f"- VL 粒子资产：**{len(ps_errors)}** 个；gallery 场景：**{len(galleries)}** 个"
    )
    lines.append("")
    lines.append("<details><summary>排除的 18 个 NS</summary>")
    lines.append("")
    for reason, paths in summary["excluded"].items():
        for path in paths:
            lines.append(f"- `{path}`（{reason}）")
    lines.append("")
    lines.append("</details>")
    lines.append("")
    lines.append("## 验收矩阵")
    lines.append("")
    lines.append(
        "| # | UE 源 | 分类 | VL PS | Gallery | 截图 | 状态 | Budget | 替身/近似 | VUID |"
    )
    lines.append("| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for index, target in enumerate(targets, start=1):
        ps_cell = "<br>".join(f"`{Path(path).name}`" for path in target.ps_paths) or "—"
        gallery_cell = "<br>".join(f"`{name}`" for name in target.galleries) or "—"
        screenshot_cell = "<br>".join(
            f"`{Path(name).name}`" for name in target.screenshots
        ) or "—"
        approximation = "；".join(target.approximations)
        if len(approximation) > 160:
            approximation = approximation[:157] + "…"
        vuids = "—"
        if target.smoke and target.smoke.get("vuids"):
            short = sorted({vuid.split("-")[-1] for vuid in target.smoke["vuids"]})
            vuids = ", ".join(short)
        budget_cell = "—"
        if target.budget:
            budget_cell = target.budget.get("status", "MISSING")
            if target.budget.get("over"):
                budget_cell += ": " + ", ".join(target.budget["over"])
        lines.append(
            "| {index} | `{ue}` | {category} | {ps} | {gallery} | {shot} | "
            "{status} | {budget} | {approx} | {vuids} |".format(
                index=index,
                ue=html_escape(target.name),
                category=html_escape(target.category),
                ps=ps_cell,
                gallery=gallery_cell,
                shot=screenshot_cell,
                status=target.status,
                budget=budget_cell,
                approx=html_escape(approximation or "—"),
                vuids=vuids,
            )
        )
    lines.append("")
    lines.append("## 失败项")
    lines.append("")
    failures = [target for target in targets if target.status == "FAIL"]
    if failures:
        for target in failures:
            lines.append(f"### `{target.name}`")
            for error in target.errors:
                lines.append(f"- {error}")
            if target.smoke and not target.smoke.get("ok"):
                lines.append(
                    f"- smoke: timed_out={target.smoke.get('timed_out')} "
                    f"scene_load_failed={target.smoke.get('scene_load_failed')} "
                    f"engine_init_failed={target.smoke.get('engine_init_failed')} "
                    f"screenshot={target.smoke.get('screenshot')}"
                )
            lines.append("")
    else:
        lines.append("- 无。")
        lines.append("")
    if scene_errors and any(scene_errors.values()):
        lines.append("### Gallery 静态错误")
        for source, errors in sorted(scene_errors.items()):
            for error in errors:
                lines.append(f"- {error}")
        lines.append("")
    if scene_warnings and any(scene_warnings.values()):
        lines.append("### Gallery 警告")
        for source, warnings in sorted(scene_warnings.items()):
            for warning in warnings:
                lines.append(f"- {warning}")
        lines.append("")

    lines.append("## VUID 汇总")
    lines.append("")
    vuid_counts: dict[str, list[str]] = {}
    for target in targets:
        if not target.smoke:
            continue
        for vuid in target.smoke.get("vuids", []):
            vuid_counts.setdefault(vuid, []).append(target.name)
    if vuid_counts:
        lines.append("| VUID | 出现次数 | 目标 |")
        lines.append("| --- | ---: | --- |")
        for vuid, names in sorted(vuid_counts.items(), key=lambda item: -len(item[1])):
            lines.append(
                f"| `{vuid}` | {len(names)} | {html_escape(', '.join(sorted(set(names))))} |"
            )
        lines.append("")
        lines.append(
            "这些 VUID 来自既有粒子/截图/排序缓冲路径，在本次验收中跨全部 gallery 复现；"
            "它们不是 49 个资源案例新增字段导致的静态或加载失败。"
        )
        lines.append("")
    else:
        lines.append("- 无（smoke 已执行）" if smoke_enabled else "- 未执行 smoke。")
        lines.append("")

    lines.append("## Gallery 运行统计")
    lines.append("")
    if smoke_enabled:
        lines.append(
            "| Gallery | 场景 | 结果 | 秒 | emitters | particles | drawCalls | "
            "vertices | gpuMs | gpuAvgMs | gpuMaxMs | Budget | 截图 |"
        )
        lines.append(
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
            "---: | --- | --- |"
        )
        for name, gallery in sorted(galleries.items()):
            for entry in gallery["scenes"]:
                scene = entry["scene"]
                result = gallery.get("smoke", {}).get(scene)
                if not result:
                    continue
                stats = result.get("stats") or {}
                budget = gallery.get("budget", {}).get("scenes", {}).get(scene, {})
                lines.append(
                    "| {name} | `{scene}` | {ok} | {elapsed} | {emitters} | "
                    "{particles} | {draws} | {vertices} | {gpu} | {gpu_avg} | "
                    "{gpu_max} | {budget} | {shot} |".format(
                        name=html_escape(name),
                        scene=scene,
                        ok="PASS" if result.get("ok") else "FAIL",
                        elapsed=result.get("elapsed_seconds", "—"),
                        emitters=stats.get("emitters", "—"),
                        particles=stats.get("particles", "—"),
                        draws=stats.get("drawCalls", "—"),
                        vertices=stats.get("vertices", "—"),
                        gpu=stats.get("gpuMs", "—"),
                        gpu_avg=stats.get("gpuAvgMs", "—"),
                        gpu_max=stats.get("gpuMaxMs", "—"),
                        budget=budget.get("status", "—"),
                        shot="yes" if result.get("screenshot") else "no",
                    )
                )
        lines.append("")
    else:
        lines.append("- 未执行 smoke。")
        lines.append("")

    lines.append("## 性能预算")
    lines.append("")
    lines.append(
        f"- 口径：每个 gallery 连续提交 3 次 `particlestats`；"
        f"`particles`/`drawCalls`/`vertices`/`gpuMs` 取 3 次峰值，"
        f"`gpuAvgMs`/`gpuMaxMs` 取最后一次累计值。"
    )
    lines.append(
        "- `gpuMs` 是单帧瞬时值、`gpuMaxMs` 是整轮最坏单帧，二者仅展示、"
        "不参与 PASS/OVER；GPU 判定使用累计 `gpuAvgMs`，避免启动上传尖峰误报。"
    )
    lines.append(
        f"- 预算：`baseline × (1 + "
        f"{budget_policy.get('margin_percent', DEFAULT_BUDGET_MARGIN_PERCENT):g}%)`；"
        f"baseline 来源 `{budget_policy.get('file', DEFAULT_BUDGET_REL)}`。"
    )
    lines.append("")
    if smoke_enabled:
        lines.append(
            "| Gallery | particles | drawCalls | vertices | gpuMs (info) | gpuAvgMs | "
            "gpuMaxMs (info) | 状态 | 超出项 |"
        )
        lines.append(
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"
        )
        over_rows: list[tuple[str, str, dict]] = []
        for name, gallery in sorted(galleries.items()):
            for entry in gallery["scenes"]:
                scene = entry["scene"]
                comparison = gallery.get("budget", {}).get("scenes", {}).get(scene)
                if not comparison:
                    continue
                measured = comparison.get("measured", {})
                budget = comparison.get("budget", {})

                def pair(key: str, digits: int | None = None) -> str:
                    value = measured.get(key)
                    limit = budget.get(key)
                    if value is None:
                        return "—"
                    if limit is None:
                        return str(value)
                    if digits is None:
                        return f"{value} / {limit}"
                    return f"{float(value):.{digits}f} / {float(limit):.{digits}f}"

                gpu_instant = measured.get("gpuMs")
                gpu_instant_cell = (
                    f"{float(gpu_instant):.3f}" if gpu_instant is not None else "—"
                )
                gpu_max = measured.get("gpuMaxMs")
                gpu_max_cell = f"{float(gpu_max):.3f}" if gpu_max is not None else "—"
                status = comparison.get("status", "MISSING")
                over = ", ".join(comparison.get("over", [])) or "—"
                if status == "OVER":
                    over_rows.append((name, scene, comparison))
                lines.append(
                    f"| {html_escape(name)} | {pair('particles')} | "
                    f"{pair('drawCalls')} | {pair('vertices')} | "
                    f"{gpu_instant_cell} | {pair('gpuAvgMs', 3)} | "
                    f"{gpu_max_cell} | {status} | {over} |"
                )
        lines.append("")
        lines.append("### Over Budget 建议")
        lines.append("")
        if over_rows:
            for name, scene, comparison in over_rows:
                lines.append(f"- `{name}` / `{scene}`")
                for key in comparison.get("over", []):
                    measured = comparison.get("measured", {}).get(key)
                    limit = comparison.get("budget", {}).get(key)
                    lines.append(
                        f"  - `{key}`: {measured} > {limit}；{budget_suggestion(key)}"
                    )
        else:
            lines.append("- 无。17 个 gallery 均在预算内。")
        lines.append("")
    else:
        lines.append("- 未执行 smoke，未比较预算。")
        lines.append("")

    lines.append("## PS 静态统计")
    lines.append("")
    lines.append(
        "| PS | emitters | renderMode 分布 | maxParticles 总和 | 单 emitter 最大 | "
        "纹理引用（唯一） | 状态 |"
    )
    lines.append("| --- | ---: | --- | ---: | ---: | ---: | --- |")
    ps_warnings_flat: list[str] = []
    for source, metrics in sorted(ps_metrics.items()):
        modes = ", ".join(
            f"{mode}:{count}" for mode, count in metrics.get("render_modes", {}).items()
        ) or "—"
        warnings = metrics.get("warnings", [])
        ps_warnings_flat.extend(warnings)
        lines.append(
            f"| `{Path(source).name}` | {metrics.get('emitter_count', 0)} | "
            f"{modes} | {metrics.get('max_particles_sum', 0)} | "
            f"{metrics.get('max_particles_max', 0)} | "
            f"{metrics.get('texture_reference_count', 0)} "
            f"({metrics.get('unique_texture_reference_count', 0)}) | "
            f"{metrics.get('status', 'FAIL')} |"
        )
    lines.append("")
    if ps_warnings_flat:
        lines.append("### PS 静态警告")
        lines.append("")
        for warning in ps_warnings_flat:
            lines.append(f"- {warning}")
        lines.append("")
    else:
        lines.append("- 无异常大、空或重复 renderer 的 PS。")
        lines.append("")

    lines.append("## 重复结构检查")
    lines.append("")
    if duplicates:
        for digest, paths in sorted(duplicates.items()):
            lines.append(f"- `{digest[:12]}`：{', '.join(f'`{p}`' for p in paths)}")
        lines.append("")
    else:
        lines.append("- 未发现去除 emitter 名称/位置后完全相同的粒子资产。")
        lines.append("")

    lines.append("## 近似与替身（README 摘录）")
    lines.append("")
    emitted: set[str] = set()
    for target in targets:
        for gallery in target.galleries:
            if gallery in emitted:
                continue
            emitted.add(gallery)
            gallery_info = galleries.get(gallery)
            if not gallery_info:
                continue
            lines.append(f"### `{gallery}`")
            if gallery_info.get("readme"):
                lines.append(f"- README: `{gallery_info['readme']}`")
            if gallery_info.get("approximations"):
                for bullet in gallery_info["approximations"]:
                    lines.append(f"- {bullet}")
            else:
                lines.append("- README 未声明近似或替身。")
            lines.append("")

    lines.append("## 复现")
    lines.append("")
    lines.append("```powershell")
    lines.append(command_line)
    lines.append("```")
    lines.append("")
    matrix_path = out_dir / "acceptance-matrix.md"
    matrix_path.write_text("\n".join(lines), encoding="utf-8")
    return matrix_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ue-root",
        default=r"D:/YYBWorkSpace/UnrealProject/effect/Content/NiagaraExamples",
        help="UE NiagaraExamples content root",
    )
    parser.add_argument(
        "--resource-root",
        default=None,
        help="VulkanLearn resource root (defaults to config/config.json -> resourcePath)",
    )
    parser.add_argument(
        "--out",
        default="artifacts/particle-niagara-acceptance",
        help="output directory (default: artifacts/particle-niagara-acceptance)",
    )
    parser.add_argument(
        "--expect-targets",
        type=int,
        default=EXPECTED_TARGET_COUNT,
        help="expected derived target count",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="run every gallery headlessly and collect load errors/VUIDs/stats",
    )
    parser.add_argument(
        "--only",
        default=None,
        help="comma separated gallery names to smoke (implies --smoke)",
    )
    parser.add_argument(
        "--main-exe",
        default=str(REPO_ROOT / "build" / "bin" / "main.exe"),
        help="main.exe path",
    )
    parser.add_argument(
        "--smoke-timeout",
        type=float,
        default=90.0,
        help="seconds to wait for each gallery screenshot",
    )
    parser.add_argument(
        "--smoke-delay",
        type=int,
        default=120,
        help="frames before console commands are submitted",
    )
    parser.add_argument(
        "--allow-missing-ue",
        action="store_true",
        help="only validate the VL side when the UE reference project is absent",
    )
    parser.add_argument(
        "--budget-file",
        default=DEFAULT_BUDGET_REL,
        help="performance budget JSON (default: tool/validation/particle/particle_performance_budget.json)",
    )
    parser.add_argument(
        "--budget-margin",
        type=float,
        default=DEFAULT_BUDGET_MARGIN_PERCENT,
        help="budget margin over the measured baseline in percent (default: 20)",
    )
    parser.add_argument(
        "--write-budget",
        action="store_true",
        help="rewrite the budget file from this smoke run's measured baseline",
    )
    args = parser.parse_args()

    ue_root = Path(args.ue_root)
    resource_root = (
        Path(args.resource_root)
        if args.resource_root
        else (REPO_ROOT / "config" / "config.json")
    )
    if args.resource_root is None:
        config = load_json(REPO_ROOT / "config" / "config.json")
        resource_value = config.get("resourcePath", "")
        resource_root = (REPO_ROOT / resource_value).resolve()
    out_dir = (REPO_ROOT / args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    smoke_dir = out_dir / "smoke"
    smoke_dir.mkdir(exist_ok=True)
    budget_path = Path(args.budget_file)
    if not budget_path.is_absolute():
        budget_path = (REPO_ROOT / budget_path).resolve()

    command_line = "py -3 tool/validation/particle/particle_niagara_acceptance.py " + " ".join(
        sys.argv[1:]
    )

    problems: list[str] = []
    if not resource_root.is_dir():
        problems.append(f"resource root does not exist: {resource_root}")
    if not ue_root.is_dir() and not args.allow_missing_ue:
        problems.append(f"UE reference project does not exist: {ue_root}")
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        return 2

    targets: list[Target] = []
    summary = {
        "scanned": 0,
        "targets": 0,
        "excluded": {
            "editor-marker": [],
            "boundary-debug": [],
            "sprite-generation": [],
            "material-reference": [],
        },
        "excluded_total": 0,
    }
    if ue_root.is_dir():
        targets, summary = derive_targets(ue_root)
        print(
            f"UE NS scanned={summary['scanned']} excluded={summary['excluded_total']} "
            f"targets={len(targets)}"
        )
        if len(targets) != args.expect_targets:
            print(
                f"ERROR: expected {args.expect_targets} targets, derived {len(targets)}",
                file=sys.stderr,
            )
            return 2

    galleries, ps_to_galleries = collect_galleries(resource_root)
    all_ps: dict[str, Path] = {}
    ps_by_name: dict[str, list[str]] = {}
    for directory in sorted((resource_root / "Maps").glob("SC_particle_ue_*")):
        if not directory.is_dir():
            continue
        for asset in sorted(directory.glob("PS_*.json")):
            key = rel(asset, resource_root)
            all_ps[key] = asset
            ps_by_name.setdefault(asset.stem, []).append(key)

    ps_errors: dict[str, list[str]] = {}
    ps_warnings: dict[str, list[str]] = {}
    ps_metrics: dict[str, dict] = {}
    hashes: dict[str, list[str]] = {}
    for key, path in sorted(all_ps.items()):
        errors, warnings, metrics = analyze_particle_system(path, resource_root)
        ps_errors[key] = errors
        ps_warnings[key] = warnings
        ps_metrics[key] = metrics
        try:
            hashes.setdefault(structural_hash(load_json(path)), []).append(key)
        except Exception:
            pass
    duplicates = {digest: paths for digest, paths in hashes.items() if len(paths) > 1}

    scene_errors: dict[str, list[str]] = {}
    scene_warnings: dict[str, list[str]] = {}
    scene_particle_paths: dict[str, list[str]] = {}
    known_ps = set(all_ps)
    for name, gallery in galleries.items():
        all_errors: list[str] = []
        all_warnings: list[str] = []
        particle_paths: list[str] = []
        for entry in gallery["scenes"]:
            if "error" in entry:
                all_errors.append(f"{entry['scene']}: JSON parse failed: {entry['error']}")
                continue
            errors, warnings, paths = lint_scene(
                resource_root / entry["scene"], resource_root, known_ps
            )
            all_errors.extend(errors)
            all_warnings.extend(warnings)
            particle_paths.extend(paths)
        if not gallery.get("readme"):
            all_warnings.append(f"{gallery['dir']}: README.md missing")
        scene_errors[name] = all_errors
        scene_warnings[name] = all_warnings
        scene_particle_paths[name] = particle_paths
        gallery["approximations"] = (
            extract_approximations(read_text(resource_root / gallery["readme"]))
            if gallery.get("readme")
            else []
        )

    # README -> target mapping.
    explicit: dict[str, list[str]] = {}
    mentioned: dict[str, set[str]] = {}
    for name, gallery in galleries.items():
        if not gallery.get("readme"):
            continue
        text = read_text(resource_root / gallery["readme"])
        for system, asset_name in parse_readme_pairs(text):
            resolved = ps_by_name.get(asset_name[: -len(".json")], [])
            same_dir = [path for path in resolved if Path(path).parent.name == name]
            chosen = same_dir or resolved
            if chosen:
                explicit.setdefault(system, [])
                for path in chosen:
                    if path not in explicit[system]:
                        explicit[system].append(path)
        for target in targets:
            if re.search(rf"\b{re.escape(target.name)}\b", text):
                mentioned.setdefault(target.name, set()).add(name)

    for target in targets:
        if target.name in explicit:
            target.ps_paths = list(explicit[target.name])
            target.mapping_source = "readme-explicit"
        else:
            candidate_dirs = mentioned.get(target.name, set())
            candidates: list[str] = []
            for name in sorted(candidate_dirs):
                candidates.extend(galleries[name]["particle_systems"])
            scored = [
                (name_similarity(target.name, Path(path).stem), path)
                for path in sorted(set(candidates))
            ]
            best = max((score for score, _ in scored), default=0.0)
            if best >= 0.6:
                target.ps_paths = [
                    path for score, path in scored if score == best
                ]
                target.mapping_source = "readme-name-match"
            elif len(candidate_dirs) == 1:
                target.ps_paths = list(
                    galleries[next(iter(candidate_dirs))]["particle_systems"]
                )
                target.mapping_source = "directory-fallback"
            else:
                target.errors.append(
                    "no VL particle asset mapping found in SC_particle_ue_* READMEs"
                )
                continue

        for path in target.ps_paths:
            if path not in all_ps:
                target.errors.append(f"mapped PS does not exist: {path}")
            target.errors.extend(ps_errors.get(path, []))
            target.warnings.extend(ps_warnings.get(path, []))
            for gallery_scene in ps_to_galleries.get(path, []):
                gallery_name = Path(gallery_scene).parent.name
                if gallery_name not in target.galleries:
                    target.galleries.append(gallery_name)
        if not target.galleries:
            target.errors.append("no gallery scene references the mapped PS")
            continue
        for gallery_name in target.galleries:
            gallery = galleries.get(gallery_name)
            if not gallery:
                continue
            target.errors.extend(scene_errors.get(gallery_name, []))
            target.warnings.extend(scene_warnings.get(gallery_name, []))
            if gallery.get("readme"):
                for bullet in gallery["approximations"]:
                    if bullet not in target.approximations:
                        target.approximations.append(bullet)
            readme_path = (
                resource_root / gallery["readme"] if gallery.get("readme") else None
            )
            for token in gallery_screenshots(gallery, resource_root, readme_path):
                if token not in target.screenshots:
                    target.screenshots.append(token)

    mapped_ps = {path for target in targets for path in target.ps_paths}
    for ps_path in sorted(set(all_ps) - mapped_ps):
        # Orphan PS assets are a mapping warning, not a target failure.
        scene_errors.setdefault("__orphan_ps__", []).append(
            f"{ps_path}: not mapped from any UE target"
        )
    for digest, paths in sorted(duplicates.items()):
        for path in paths:
            if path in mapped_ps:
                for target in targets:
                    if path in target.ps_paths:
                        target.warnings.append(
                            f"structural duplicate of {', '.join(p for p in paths if p != path)}"
                        )

    smoke_enabled = args.smoke or bool(args.only)
    exe = Path(args.main_exe)
    if smoke_enabled and not exe.is_file():
        print(f"ERROR: main.exe not found: {exe}", file=sys.stderr)
        return 2
    selected = (
        {item.strip() for item in args.only.split(",") if item.strip()}
        if args.only
        else None
    )
    if smoke_enabled:
        if selected is None:
            # A full run regenerates every acceptance screenshot; stale copies
            # would otherwise pile up in the resource worktree.
            for stale in (resource_root / "Generated" / "Screenshots").glob(
                "particle_accept_*.bmp"
            ):
                try:
                    stale.unlink()
                except OSError:
                    pass
            for stale in smoke_dir.iterdir():
                if stale.is_file():
                    try:
                        stale.unlink()
                    except OSError:
                        pass
        for name, gallery in sorted(galleries.items()):
            if selected is not None and name not in selected:
                continue
            gallery["smoke"] = {}
            for entry in gallery["scenes"]:
                if "error" in entry:
                    continue
                scene = entry["scene"]
                print(f"smoke {name}: {scene}", flush=True)
                result = run_smoke(
                    exe,
                    scene,
                    resource_root,
                    smoke_dir,
                    args.smoke_timeout,
                    args.smoke_delay,
                )
                gallery["smoke"][scene] = result
                stats = result.get("stats") or {}
                print(
                    f"  ok={result['ok']} {result['elapsed_seconds']}s "
                    f"particles={stats.get('particles', '—')} "
                    f"drawCalls={stats.get('drawCalls', '—')} "
                    f"gpuAvgMs={stats.get('gpuAvgMs', '—')} "
                    f"gpuMaxMs={stats.get('gpuMaxMs', '—')} "
                    f"vuids={len(result['vuids'])}",
                    flush=True,
                )
                for target in targets:
                    if name not in target.galleries:
                        continue
                    target.smoke = target.smoke or {
                        "ok": True,
                        "timed_out": False,
                        "scene_load_failed": 0,
                        "engine_init_failed": 0,
                        "vuids": [],
                        "stats": {},
                        "screenshots": [],
                    }
                    target.smoke["ok"] = target.smoke["ok"] and result["ok"]
                    target.smoke["timed_out"] = (
                        target.smoke["timed_out"] or result["timed_out"]
                    )
                    target.smoke["scene_load_failed"] += result[
                        "scene_load_failed"
                    ]
                    target.smoke["engine_init_failed"] += result[
                        "engine_init_failed"
                    ]
                    target.smoke["vuids"] = sorted(
                        set(target.smoke["vuids"]) | set(result["vuids"])
                    )
                    if result.get("stats"):
                        target.smoke["stats"][scene] = result["stats"]
                    if result.get("screenshot"):
                        target.smoke["screenshots"].append(result["screenshot"])
                        if result["screenshot"] not in target.screenshots:
                            target.screenshots.append(result["screenshot"])
                    if not result["ok"]:
                        target.errors.append(
                            f"smoke failed for {scene}: "
                            f"timed_out={result['timed_out']} "
                            f"Scene.LoadFailed={result['scene_load_failed']} "
                            f"EngineLoop init failed={result['engine_init_failed']} "
                            f"screenshot={result['screenshot']}"
                        )

    budget_policy = {
        "file": rel(budget_path, REPO_ROOT),
        "margin_percent": args.budget_margin,
    }
    budget_document: dict | None = None
    if args.write_budget:
        if not smoke_enabled:
            print("ERROR: --write-budget requires --smoke", file=sys.stderr)
            return 2
        budget_document = write_budget_file(
            budget_path, galleries, args.budget_margin, command_line
        )
        print(f"budget: wrote {budget_path}")
    else:
        budget_document = load_budget(budget_path)
        if budget_document is None:
            print(
                f"WARNING: budget file not found: {budget_path}; "
                f"run with --write-budget to create it",
                file=sys.stderr,
            )
    apply_budget(galleries, budget_document)

    for target in targets:
        over: list[str] = []
        statuses: list[str] = []
        for gallery_name in target.galleries:
            gallery_budget = galleries.get(gallery_name, {}).get("budget")
            if not gallery_budget:
                continue
            statuses.append(gallery_budget.get("status", "MISSING"))
            for key in gallery_budget.get("over", []):
                if key not in over:
                    over.append(key)
        if "OVER" in statuses:
            target.budget = {"status": "OVER", "over": over}
        elif statuses and all(status == "PASS" for status in statuses):
            target.budget = {"status": "PASS", "over": []}
        else:
            target.budget = {"status": "MISSING", "over": []}

    result_document = {
        "generated_at": _datetime.datetime.now().isoformat(timespec="seconds"),
        "ue_root": str(ue_root),
        "resource_root": str(resource_root),
        "command": command_line,
        "smoke_enabled": smoke_enabled,
        "budget_policy": budget_policy,
        "budget_generated_at": (budget_document or {}).get("generated_at"),
        "summary": {
            **summary,
            "target_count": len(targets),
            "particle_asset_count": len(all_ps),
            "gallery_count": len(galleries),
            "status_counts": {
                status: sum(1 for target in targets if target.status == status)
                for status in ("PASS", "STATIC-OK", "WARN", "FAIL")
            },
            "budget_counts": {
                status: sum(
                    1
                    for gallery in galleries.values()
                    if gallery.get("budget", {}).get("status") == status
                )
                for status in ("PASS", "OVER", "MISSING")
            },
        },
        "targets": [
            {
                "ue": target.name,
                "ue_path": target.ue_path,
                "category": target.category,
                "ps": target.ps_paths,
                "galleries": target.galleries,
                "mapping_source": target.mapping_source,
                "screenshots": target.screenshots,
                "status": target.status,
                "errors": target.errors,
                "warnings": target.warnings,
                "approximations": target.approximations,
                "smoke": target.smoke,
                "budget": target.budget,
            }
            for target in targets
        ],
        "galleries": {
            name: {
                "dir": gallery["dir"],
                "readme": gallery.get("readme"),
                "scenes": gallery["scenes"],
                "particle_systems": gallery["particle_systems"],
                "errors": scene_errors.get(name, []),
                "warnings": scene_warnings.get(name, []),
                "smoke": gallery.get("smoke", {}),
                "budget": gallery.get("budget", {}),
            }
            for name, gallery in galleries.items()
        },
        "particle_assets": {
            path: {
                "errors": ps_errors[path],
                "warnings": ps_warnings[path],
                "metrics": ps_metrics[path],
            }
            for path in sorted(all_ps)
        },
        "structural_duplicates": duplicates,
        "orphan_particle_systems": sorted(set(all_ps) - mapped_ps),
    }
    (out_dir / "acceptance-result.json").write_text(
        json.dumps(result_document, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    matrix_path = write_matrix(
        out_dir,
        targets,
        summary,
        galleries,
        ps_errors,
        ps_warnings,
        ps_metrics,
        scene_errors,
        scene_warnings,
        duplicates,
        smoke_enabled,
        command_line,
        budget_document,
        budget_policy,
    )

    counts = result_document["summary"]["status_counts"]
    budget_counts = result_document["summary"]["budget_counts"]
    print(
        f"targets={len(targets)} ps={len(all_ps)} galleries={len(galleries)} "
        f"PASS={counts['PASS']} WARN={counts['WARN']} FAIL={counts['FAIL']}"
    )
    print(
        f"budget: PASS={budget_counts['PASS']} OVER={budget_counts['OVER']} "
        f"MISSING={budget_counts['MISSING']} file={budget_policy['file']}"
    )
    print(f"matrix: {matrix_path}")
    print(f"result: {out_dir / 'acceptance-result.json'}")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
