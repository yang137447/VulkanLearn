"""Shared, CPU-only evidence helpers for the lit ROI gate.

Everything here reads real repository/artifact bytes: the C010r14 publication
manifest, the TVQ1 quality captures, the p3 original-evaluator page set, the
C008 capture plan and the r11 expanded ROI fragments. Nothing renders, nothing
fabricates a slot or a page byte.
"""

import hashlib
import json
import math
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[3]
ASSET_ROOT = ROOT.parent / "p5g" / "C010r14"
MANIFEST = ASSET_ROOT / "resources" / "VT" / "00af1195dc7dfbb0" / "manifest.json"
CAPTURE_PLAN = (ROOT / "artifacts/terrain-rvt-production/p5/"
                "tour-final-runtime-evidence-matrix-cpu-r2/capture-and-oracle-checklist.json")
SMOKE_PLAN = (ROOT / "artifacts/terrain-rvt-production/final-delivery/"
              "s1-smoke-c010r14-w2b/plan.json")
FRAGMENT_DIR = ROOT / "artifacts/terrain-rvt-production/r11-lit-roi-private-r1"
PAGE_SOURCE = (ROOT / "artifacts/terrain-rvt-production/p3-real-numeric-c008/"
               "runs/run-001/mode-0/original")
HASH_BUILD = ROOT / "artifacts/terrain-rvt-production/native-tes-capture-private-r2/build.json"
LIT_EXE = ROOT / "build-vt-release/bin/native_terrain_lit_capture_game.exe"
DELIVERY = ROOT / "artifacts/terrain-rvt-production/final-delivery/lit"
QUALITY_ROOT = ROOT / "artifacts/terrain-rvt-production/final-delivery"

# Original independent evaluator identity, hardcoded in
# native-capture/terrainReferenceAtlas.cpp (Require(...originalEvaluatorSha256...)).
ORIGINAL_EVALUATOR_SHA256 = ("d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b")
RECT = [576, 296, 128, 128]
FULL_VIEWPORT = [1280, 720]
# P0Frozen steadyRvtCaps.physicalSlots; the TVQ1 owner table also has 512 rows.
ACTUAL_SLOT_CAPACITY = 512
# channels ["rgba8-srgb", "rgba8-unorm", "rgba8-unorm"] -> vk::Format
VK_FORMATS = {"rgba8-srgb": 43, "rgba8-unorm": 37}
ATLAS_CHANNELS = ["rgba8-srgb", "rgba8-unorm", "rgba8-unorm"]

TVQ1_MAGIC = 0x31515654
TVQ1_CAPACITY = 16384
TVQ1_SAMPLE_WORD = 64
TVQ1_RESULT_WORD = 131136
TVQ1_OWNER_WORD = 393280
TVQ1_SURFACE_WORD = 401472
PAGE_BYTES = 3 * 136 * 136 * 4 + 4


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write(path, document):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical(document):
    return json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def digest_document(document):
    return hashlib.sha256(canonical(document).encode()).hexdigest()


class Blake3(object):
    """BLAKE3 through the already-shipped native CPU test binary (--hash)."""

    def __init__(self):
        build = read(HASH_BUILD)
        self.executable = Path(build["executable"])
        if sha256(self.executable) != build["executableSha256"]:
            raise ValueError("BLAKE3 hash binary changed; refusing to hash inputs")
        self.environment = os.environ.copy()
        self.environment["PATH"] = str(ROOT / "build-vt-release/bin") + ";" + self.environment.get("PATH", "")
        self._cache = {}

    def hash(self, path):
        path = str(Path(path).resolve())
        if path not in self._cache:
            result = subprocess.run([str(self.executable), "--hash", path], env=self.environment,
                                    capture_output=True, text=True, check=True)
            text = result.stdout.strip()
            if len(text) != 64:
                raise ValueError("Unexpected BLAKE3 output for " + path)
            self._cache[path] = text
        return self._cache[path]


def resolve_pose(name):
    """Return (position, look_at) doubles from a real capture plan."""
    for plan in (CAPTURE_PLAN, SMOKE_PLAN):
        document = read(plan)
        for shot in document.get("shots", document.get("poses", [])):
            if shot.get("name") == name:
                return shot["position"], shot["look_at"], plan
    raise ValueError("Pose " + name + " not found in the capture plan")


def pose_command(position, look_at):
    values = list(position) + list(look_at)
    return "camera pose " + " ".join(format(float(value), ".12g") for value in values)


# ---------------------------------------------------------------- TVQ1 decode
class QualityCapture(object):
    """Bounded TVQ1 v1/v2 reader; only actual recorded results are used."""

    def __init__(self, binary_path, metadata_path):
        self.binary_path = Path(binary_path)
        self.metadata_path = Path(metadata_path)
        self.data = self.binary_path.read_bytes()
        self.metadata = read(self.metadata_path)
        version = struct.unpack_from("<I", self.data, 4)[0]
        expected = 1606912 + (12288 if version == 2 else 0)
        if version not in (1, 2) or len(self.data) != expected:
            raise ValueError("Unknown TVQ1 byte count/version")
        self.header = struct.unpack_from("<64I", self.data)
        if self.header[:3] != (TVQ1_MAGIC, version, TVQ1_CAPACITY):
            raise ValueError("Unknown TVQ1 magic/version/capacity")
        if self.metadata.get("binaryAbi") != "TVQ1-final-results-v%d" % version:
            raise ValueError("TVQ1 metadata ABI differs")
        if self.metadata.get("source") != "gpu-page-table-readback":
            raise ValueError("TVQ1 requires an actual GPU page-table readback")
        self.names = self.metadata["surfaceIds"]
        if self.header[18] != len(self.names) or len(self.names) != len(set(self.names)):
            raise ValueError("TVQ1 surface dictionary differs")
        self._owners = {}
        self._samples = {}
        self._decode()

    @staticmethod
    def _join(low, high):
        return low | high << 32

    def _decode(self):
        for pixel in range(TVQ1_CAPACITY):
            values = struct.unpack_from("<6f2I", self.data, (TVQ1_SAMPLE_WORD + pixel * 8) * 4)
            if values[7] == 0:
                continue
            if values[7] != 1:
                raise ValueError("TVQ1 duplicate visible pixel")
            surface_index, taps = values[6] & 65535, values[6] >> 16
            if surface_index >= len(self.names) or not 1 <= taps <= 8:
                raise ValueError("TVQ1 surface/tap count")
            record = {"surfaceIndex": surface_index, "surfaceId": self.names[surface_index],
                      "uv": list(values[:2]), "targetMip": values[4], "bias": values[5],
                      "taps": []}
            for tap in range(taps):
                pair = {}
                for branch, label in enumerate(("low", "high")):
                    packed = struct.unpack_from(
                        "<I", self.data,
                        (TVQ1_RESULT_WORD + pixel * 16 + tap * 2 + branch) * 4)[0]
                    entry, mip = packed & 1023, packed >> 10 & 15
                    page_x, page_y = packed >> 14 & 63, packed >> 20 & 63
                    if not packed & (1 << 26) or packed >> 28 != 0 or entry > 512:
                        raise ValueError("TVQ1 malformed resolved result")
                    pair[label] = {"entry": entry, "slot": entry - 1 if entry else None,
                                   "mip": mip, "x": page_x, "y": page_y,
                                   "anchored": bool(packed & (1 << 27))}
                record["taps"].append(pair)
            self._samples[(pixel % 128, pixel // 128)] = record
        if len(self._samples) != self.header[3] or self.header[3] != self.header[6]:
            raise ValueError("TVQ1 visible/resolved counter differs")
        if any(self.header[index] for index in (4, 5, 21, 22)):
            raise ValueError("TVQ1 duplicate/invalid/dropped/overflow header")

    def owner(self, entry):
        if not entry:
            return None
        if entry not in self._owners:
            offset = (TVQ1_OWNER_WORD + (entry - 1) * 16) * 4
            row = struct.unpack_from("<16I", self.data, offset)
            if row[0] == 0:
                self._owners[entry] = None
            else:
                self._owners[entry] = {
                    "slot": entry - 1, "surfaceIndex": row[1],
                    "surfaceId": self.names[row[1]], "mip": row[2],
                    "x": row[3] & 65535, "y": row[3] >> 16,
                    "worldGeneration": self._join(row[4], row[5]),
                    "revision": self._join(row[6], row[7]),
                    "contentIdentity": self.data[offset + 32:offset + 64].hex()}
        return self._owners[entry]

    def sample(self, fx, fy):
        return self._samples.get((fx, fy))

    def owner_matches(self, record, result, surface):
        owner = self.owner(result["entry"])
        if owner is None:
            return False
        return (owner["surfaceId"] == record["surfaceId"]
                and owner["mip"] == result["mip"]
                and owner["x"] == result["x"] and owner["y"] == result["y"]
                and owner["contentIdentity"] == surface["expectedContentIdentity"]
                and owner["revision"] == surface["expectedRevision"]
                and owner["worldGeneration"] == self.metadata["worldGeneration"])

    def surface(self, index):
        offset = (TVQ1_SURFACE_WORD + index * 16) * 4
        row = struct.unpack_from("<16I", self.data, offset)
        maximum = row[3] & 255
        if row[0] != index or not 1 <= row[1] <= 8192 or not 1 <= row[2] <= 8192:
            raise ValueError("TVQ1 surface layout")
        if maximum != max(row[1], row[2]).bit_length() - 1 or row[3] & ~511:
            raise ValueError("TVQ1 surface max mip")
        return {"surfaceId": self.metadata["surfaceIds"][index], "width": row[1],
                "height": row[2], "maxMip": maximum,
                "expectedRevision": self._join(row[6], row[7]),
                "expectedContentIdentity": self.data[offset + 32:offset + 64].hex()}

    def valid_pixel(self, fx, fy):
        record = self.sample(fx, fy)
        if record is None:
            return False, record
        surface = self.surface(record["surfaceIndex"])
        for tap in record["taps"]:
            for result in tap.values():
                if not self.owner_matches(record, result, surface):
                    return False, record
        return True, record

    def render_extent(self):
        return int(self.header[16]), int(self.header[17])

    def pose_id(self):
        return self._join(self.header[10], self.header[11])

    def counts(self):
        return {"submissionEpoch": self._join(self.header[14], self.header[15]),
                "frame": self._join(self.header[8], self.header[9]),
                "worldGeneration": self._join(self.header[12], self.header[13]),
                "visibleSamples": self.header[3], "resolvedSamples": self.header[6],
                "uninitializedDraws": self.header[7], "surfaceCount": self.header[18]}


def capture_direction(metadata):
    """TVQ1 `cameraLookAt` is position + one-centimetre forward, not a target."""
    position, look_at = metadata.get("cameraPosition"), metadata.get("cameraLookAt")
    if not position or not look_at:
        return None
    return _normalize([look_at[i] - position[i] for i in range(3)])


def find_quality_captures(position, direction=None, tolerance=5.0, direction_tolerance=0.02):
    """Every real TVQ1 capture in final-delivery whose pose (position + view direction) matches.

    The direction tolerance is loose because TVQ1 stores `cameraLookAt` as
    position + one centimetre of forward: at |position| ~ 1e5 cm float32
    quantisation of that delta perturbs the observed direction by ~2e-3.
    """
    matches = []
    for metadata_path in sorted(QUALITY_ROOT.rglob("quality/capture-*.json")):
        try:
            metadata = read(metadata_path)
        except (ValueError, OSError):
            continue
        camera = metadata.get("cameraPosition")
        if not camera:
            continue
        if math.dist(camera, position) > tolerance:
            continue
        if direction is not None:
            observed = capture_direction(metadata)
            if observed is None or max(abs(observed[i] - direction[i]) for i in range(3)) > direction_tolerance:
                continue
        binary = metadata_path.with_suffix(".bin")
        if binary.exists():
            matches.append((metadata_path, binary, metadata))
    return matches


def select_capture(position, direction=None, require_complete=True):
    """Latest completed capture at the pose with zero unresolved/incorrect identity."""
    best = None
    for metadata_path, binary_path, metadata in find_quality_captures(position, direction):
        if require_complete and metadata.get("completedEpoch", 0) < metadata.get("submissionEpoch", 1):
            continue
        captures = QualityCapture(binary_path, metadata_path)
        counts = captures.counts()
        if metadata.get("uninitializedDraws", 0) != 0:
            continue
        if counts["visibleSamples"] == 0:
            continue
        key = (counts["submissionEpoch"], counts["frame"])
        if best is None or key > best[0]:
            best = (key, metadata_path, binary_path, captures)
    if best is None:
        raise ValueError("No completed quality capture at the requested pose")
    return best[1], best[2], best[3]


# ------------------------------------------------------------ ROI mapping
def roi_feedback_texels(mapping, render_extent=None):
    """Map the fixed lit ROI rect onto the fixed 128x128 terrainFeedback domain.

    terrainFeedback is a fixed 128x128 target rendered with the actual scene
    camera and a viewport/scissor equal to the target extent (PassRuntime::
    BuildPassViewportState uses renderPass.width/height), so NDC is preserved
    and render pixel p maps to feedback texel floor(p * 128 / renderExtent).
    """
    width, height = render_extent or FULL_VIEWPORT
    x0, y0, w, h = RECT
    selected = []
    for fy in range(128):
        for fx in range(128):
            center_x = (fx + 0.5) * width / 128.0
            center_y = (fy + 0.5) * height / 128.0
            inside = (x0 <= center_x < x0 + w) and (y0 <= center_y < y0 + h)
            if mapping == "roi-center":
                if inside:
                    selected.append((fx, fy))
            elif mapping == "roi-intersect":
                low_x, high_x = x0 * 128.0 / width, (x0 + w) * 128.0 / width
                low_y, high_y = y0 * 128.0 / height, (y0 + h) * 128.0 / height
                if fx + 1 > low_x and fx < high_x and fy + 1 > low_y and fy < high_y:
                    selected.append((fx, fy))
            elif mapping == "identity":
                selected.append((fx, fy))
            else:
                raise ValueError("Unknown footprint mapping: " + mapping)
    return selected


def roi_pixel_texel(x, y, render_extent=None):
    width, height = render_extent or FULL_VIEWPORT
    return (int((RECT[0] + x + 0.5) * 128.0 / width),
            int((RECT[1] + y + 0.5) * 128.0 / height))


def footprint_from_capture(capture, mapping):
    texels = roi_feedback_texels(mapping, capture.render_extent())
    slots = {}
    for fx, fy in texels:
        record = capture.sample(fx, fy)
        if record is None:
            continue
        for tap in record["taps"]:
            for result in tap.values():
                if not result["entry"]:
                    continue
                owner = capture.owner(result["entry"])
                if owner is None:
                    continue
                slots[owner["slot"]] = {"surfaceId": owner["surfaceId"], "mip": owner["mip"],
                                        "x": owner["x"], "y": owner["y"]}
    return slots, texels


def roi_visible_surfaces(capture, mapping="roi-center"):
    """Surface ids that the ROI-mapped feedback texels actually claim."""
    _, texels = footprint_from_capture(capture, mapping)
    surfaces = set()
    for fx, fy in texels:
        record = capture.sample(fx, fy)
        if record is not None:
            surfaces.add(record["surfaceId"])
    return surfaces


def footprint_from_owners(capture, surface_ids=None):
    """Every page the live pool claims for the selected surfaces.

    The TVQ1 owner dictionary is the epoch-bound committed pool snapshot, so this
    is the exact residency of the frame being replayed: every page the ROI can
    sample (directly or through coarse-mip fallback) is in this set.
    """
    slots = {}
    for entry in range(1, 513):
        owner = capture.owner(entry)
        if owner is None:
            continue
        if surface_ids is not None and owner["surfaceId"] not in surface_ids:
            continue
        slots[owner["slot"]] = {"surfaceId": owner["surfaceId"], "mip": owner["mip"],
                                "x": owner["x"], "y": owner["y"]}
    return slots


# ---------------------------------------------- reference page set (p3 C008)
def load_page_report(report_path=None):
    report = read(report_path or (PAGE_SOURCE / "report.json"))
    pages = {}
    for entry in report["pages"]:
        key = (entry["surfaceId"], entry["mip"], entry["x"], entry["y"])
        if key in pages:
            raise ValueError("Duplicate page request in original page report")
        pages[key] = entry
    return report, pages


def publication_equivalence(candidate_manifest, reference_manifest):
    """Prove the C008 page bytes apply to the C010r14 publication.

    The numeric page producer baked from the C008 closure. Compare the two
    closures: surfaces (ignoring authorDependencies, which only carries
    authoring source paths), every kind-0 image object referenced by a
    surface, the generator and the adjacency must be identical. The only
    differing objects are shader-source provenance rows (kinds 1/2), which the
    independent original evaluator never reads as arithmetic inputs (its own
    compiled shader is taken from the calibration closure).
    """
    left, right = read(candidate_manifest), read(reference_manifest)
    a, b = left["production"], right["production"]
    report = {"candidateManifest": str(candidate_manifest),
              "candidateManifestSha256": sha256(candidate_manifest),
              "referenceManifest": str(reference_manifest),
              "referenceManifestSha256": sha256(reference_manifest)}
    if a["generator"] != b["generator"]:
        raise ValueError("Publication generator differs")
    if a["adjacency"] != b["adjacency"]:
        raise ValueError("Publication adjacency differs")
    report["generatorEqual"] = True
    report["adjacencyEqual"] = True
    for index, (first, second) in enumerate(zip(a["surfaces"], b["surfaces"])):
        left_surface = {k: v for k, v in first.items() if k != "authorDependencies"}
        right_surface = {k: v for k, v in second.items() if k != "authorDependencies"}
        if left_surface != right_surface:
            raise ValueError("Surface row differs at index %d" % index)
    if len(a["surfaces"]) != len(b["surfaces"]):
        raise ValueError("Surface count differs")
    report["surfacesEqualIgnoringAuthorDependencies"] = True
    objects_a = {row["objectId"]: row for row in a["objects"]}
    objects_b = {row["objectId"]: row for row in b["objects"]}
    if sorted(objects_a) != sorted(objects_b):
        raise ValueError("Publication object ids differ")
    differing = [key for key in objects_a if objects_a[key] != objects_b[key]]
    report["differingObjectIds"] = differing
    referenced = set()
    for surface in a["surfaces"]:
        referenced.update(surface["inputs"])
    for object_id in sorted(referenced):
        if not object_id:
            continue
        if object_id not in objects_a:
            raise ValueError("Referenced publication object missing: " + object_id)
        if objects_a[object_id] != objects_b[object_id]:
            raise ValueError("Bake input object differs between publications: " + object_id)
    report["surfaceReferencedObjectCount"] = len(referenced)
    report["surfaceReferencedObjectsIdentical"] = True
    for object_id in differing:
        row = objects_a[object_id]
        if row["kind"] not in (1, 2) or not row["path"].startswith("inputs/"):
            raise ValueError("Unexpected non-provenance publication difference: " + object_id)
    report["nonProvenanceDifferences"] = 0
    report["conclusion"] = ("C008 page bytes are computed from identical bake inputs; only "
                            "shader-source provenance rows differ, so the independent "
                            "original-evaluator pages are valid for the C010r14 publication")
    return report


def page_set_sha256(entries):
    identity = [[int(entry["actualPhysicalSlot"]), entry["surfaceId"], int(entry["mip"]),
                 int(entry["x"]), int(entry["y"]), entry["blake3"]]
                for entry in sorted(entries, key=lambda item: int(item["actualPhysicalSlot"]))]
    return digest_document(identity)


# ------------------------------------------------- camera view-projection
def _f32(value):
    return float(struct.unpack("<f", struct.pack("<f", float(value)))[0])


def _normalize(vector):
    x, y, z = (_f32(component) for component in vector)
    squared = _f32(_f32(_f32(x * x) + _f32(y * y)) + _f32(z * z))
    length = _f32(math.sqrt(squared))
    return [_f32(x / length), _f32(y / length), _f32(z / length)]


def _cross(a, b):
    return [_f32(_f32(a[1] * b[2]) - _f32(a[2] * b[1])),
            _f32(_f32(a[2] * b[0]) - _f32(a[0] * b[2])),
            _f32(_f32(a[0] * b[1]) - _f32(a[1] * b[0]))]


def _matmul(left, right):
    """4x4 float32 product with Eigen's sequential k accumulation order."""
    result = [[0.0] * 4 for _ in range(4)]
    for row in range(4):
        for column in range(4):
            value = 0.0
            for k in range(4):
                value = _f32(value + _f32(left[row][k] * right[k][column]))
            result[row][column] = value
    return result


def _quaternion_from_matrix(matrix):
    """Eigen Quaternionf(Matrix3f)::fromRotationMatrix (float32)."""
    m00, m01, m02 = matrix[0]
    m10, m11, m12 = matrix[1]
    m20, m21, m22 = matrix[2]
    trace = _f32(_f32(m00 + m11) + m22)
    if trace > 0.0:
        t = _f32(math.sqrt(_f32(trace + 1.0)))
        w = _f32(0.5 * t)
        t = _f32(0.5 / t)
        x = _f32(_f32(m21 - m12) * t)
        y = _f32(_f32(m02 - m20) * t)
        z = _f32(_f32(m10 - m01) * t)
    elif m00 > m11 and m00 > m22:
        t = _f32(math.sqrt(_f32(_f32(1.0 + m00) - m11 - m22)))
        x = _f32(0.5 * t)
        t = _f32(0.5 / t)
        y = _f32(_f32(m01 + m10) * t)
        z = _f32(_f32(m02 + m20) * t)
        w = _f32(_f32(m21 - m12) * t)
    elif m11 > m22:
        t = _f32(math.sqrt(_f32(_f32(1.0 + m11) - m00 - m22)))
        y = _f32(0.5 * t)
        t = _f32(0.5 / t)
        x = _f32(_f32(m01 + m10) * t)
        z = _f32(_f32(m12 + m21) * t)
        w = _f32(_f32(m02 - m20) * t)
    else:
        t = _f32(math.sqrt(_f32(_f32(1.0 + m22) - m00 - m11)))
        z = _f32(0.5 * t)
        t = _f32(0.5 / t)
        x = _f32(_f32(m02 + m20) * t)
        y = _f32(_f32(m12 + m21) * t)
        w = _f32(_f32(m10 - m01) * t)
    return [x, y, z, w]


def _matrix_from_quaternion(quaternion):
    x, y, z, w = (float(component) for component in quaternion)
    tx, ty, tz = _f32(2.0 * x), _f32(2.0 * y), _f32(2.0 * z)
    twx, twy, twz = _f32(tx * w), _f32(ty * w), _f32(tz * w)
    txx, txy, txz = _f32(tx * x), _f32(ty * x), _f32(tz * x)
    tyy, tyz, tzz = _f32(ty * y), _f32(tz * y), _f32(tz * z)
    return [[_f32(1.0 - _f32(tyy + tzz)), _f32(txy - twz), _f32(txz + twy)],
            [_f32(txy + twz), _f32(1.0 - _f32(txx + tzz)), _f32(tyz - twx)],
            [_f32(txz - twy), _f32(tyz + twx), _f32(1.0 - _f32(txx + tyy))]]


def engine_view_projection(position, look_at, fov, aspect, near, far):
    """Mirror Camera::SetCamera/SetProjection/GetViewMatrix/CameraSnapshot.

    The engine stores the view through a quaternion round-trip
    (SetRotation(Quaternion(matrix01)) -> GetViewMatrix -> QuatToMatrix), so the
    round-trip is reproduced here; returning both raw and round-tripped matrices
    lets the caller report sensitivity.
    """
    position = [_f32(component) for component in position]
    look_at = [_f32(component) for component in look_at]
    forward = _normalize([-_f32(look_at[i] - position[i]) for i in range(3)])
    right = _normalize(_cross([0.0, 1.0, 0.0], forward))
    up = _normalize(_cross(forward, right))
    matrix01 = [[right[0], right[1], right[2], 0.0],
                [up[0], up[1], up[2], 0.0],
                [forward[0], forward[1], forward[2], 0.0],
                [0.0, 0.0, 0.0, 1.0]]
    quaternion = _quaternion_from_matrix([[matrix01[0][0], matrix01[1][0], matrix01[2][0]],
                                         [matrix01[0][1], matrix01[1][1], matrix01[2][1]],
                                         [matrix01[0][2], matrix01[1][2], matrix01[2][2]]])
    rotation = _matrix_from_quaternion(quaternion)
    # QuatToMatrix(...)^T * Translate(-position); Eigen accumulates the
    # translation column over k = 0,1,2,3 (the 4th product is exactly +0).
    view = [[0.0] * 4 for _ in range(4)]
    for row in range(3):
        for column in range(3):
            view[row][column] = _f32(rotation[column][row])
        value = 0.0
        for k in range(3):
            value = _f32(value + _f32(rotation[k][row] * _f32(-position[k])))
        view[row][3] = _f32(value + 0.0)
    view[3] = [0.0, 0.0, 0.0, 1.0]
    projection = _projection_matrix(fov, aspect, near, far)
    ndc = [[1.0, 0.0, 0.0, 0.0],
           [0.0, -1.0, 0.0, 0.0],
           [0.0, 0.0, -0.5, 0.5],
           [0.0, 0.0, 0.0, 1.0]]
    projection = _matmul(ndc, projection)
    view_projection = _matmul(projection, view)
    flat = [view_projection[row][column] for column in range(4) for row in range(4)]
    return {"viewProjectionColumnMajor": flat,
            "viewProjection": view_projection,
            "view": view,
            "projection": projection,
            "quaternion": quaternion}


def _projection_matrix(fov, aspect, near, far):
    n = _f32(-1.0 * near)
    f = _f32(-1.0 * far)
    fov_rad = _f32(_f32(fov * math.pi) / 180.0)
    k = _f32(-1.0 / _f32(math.tan(_f32(fov_rad / 2.0))))
    aspect = _f32(aspect)
    return [[_f32(-k), 0.0, 0.0, 0.0],
            [0.0, _f32(-aspect * k), 0.0, 0.0],
            [0.0, 0.0, _f32(-_f32(n + f) / _f32(n - f)), _f32(_f32(2.0 * n * f) / _f32(n - f))],
            [0.0, 0.0, -1.0, 0.0]]


def blake3_of_float32(hasher, values):
    """BLAKE3 over the 16 float32 words the engine hashes."""
    import struct as _struct
    payload = b"".join(_struct.pack("<f", _f32(value)) for value in values)
    temporary = Path(os.environ.get("TEMP", ".")) / ("lit-vp-%d.bin" % os.getpid())
    temporary.write_bytes(payload)
    try:
        return hasher.hash(temporary)
    finally:
        temporary.unlink(missing_ok=True)
