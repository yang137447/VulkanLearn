"""Direct native negative-case runner.

与 run_rvt_case 不同：本脚本只做真实安装副本的文件级负例与真实 console fault 注入，
记录原始日志/退出码/关键行，不依赖尚未实现的 [RVT_EVIDENCE] 事件族，也不改冻结门槛。
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PKG = Path(r'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\p5g\C010r14')
PRIVATE = Path(r'D:\rvt-final')
SCEN = REPO / 'artifacts/terrain-rvt-production/final-delivery/scenarios'
OUT = REPO / 'artifacts/terrain-rvt-production/final-delivery/direct-cases'
KEY_MARKERS = ('Runtime error', 'Error]', 'LoadWorld transaction failed', 'World/graph transaction committed',
               'fault-injection', 'production-failure', 'submit-', 'candidate-budget-admission',
               'rvtfault', 'TerrainRvtFrozen', 'terrainvt stats')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def copy_package(name):
    target = PRIVATE / ('pkg-' + name)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(PKG, target)
    return target


def manifest_of(package):
    config = read_json(package / 'config/config.json')
    scene = read_json(package / 'resources' / config['initScene'])
    placement = next(o for o in scene['objects'] if o.get('type') == 'terrain')
    terrain_path = package / 'resources' / placement['terrainPath']
    terrain = read_json(terrain_path)
    manifest = package / 'resources' / terrain['compositedVT']['packagePath']
    return config, terrain_path, terrain, manifest


def mutate(package, case):
    """Return a record of the byte/JSON mutation applied."""
    config, terrain_path, terrain, manifest = manifest_of(package)
    document = read_json(manifest)
    if case == 'root_corrupt':
        root = document['roots'][0]
        page = next(p for p in document['pages']
                    if all(p[f] == root[f] for f in ('surfaceId', 'mip', 'x', 'y')))
        payload = manifest.parent / document['package']
        data = bytearray(payload.read_bytes())
        offset = page['offset']
        data[offset] ^= 1
        payload.write_bytes(bytes(data))
        return {'kind': 'corrupt-root', 'path': str(payload), 'offset': offset,
                'surfaceId': root['surfaceId'], 'mip': root['mip']}
    if case == 'input_corrupt':
        inputs = document['production']['surfaces'][0]['inputs']
        source = inputs.get('layerAlbedo') if isinstance(inputs, dict) else inputs[0]
        entry = next(o for o in document['production']['objects'] if o['objectId'] == source)
        payload = manifest.parent / entry['path']
        data = bytearray(payload.read_bytes())
        data[0] ^= 1
        payload.write_bytes(bytes(data))
        return {'kind': 'corrupt-production-input', 'objectId': source, 'path': str(payload)}
    if case == 'v1_reject':
        document['version'] = 1
        manifest.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding='utf-8')
        return {'kind': 'wrong-version', 'path': str(manifest), 'version': 1}
    if case == 'legacy_switch_reject':
        terrain['compositedVT']['residentPages'] = 192
        terrain_path.write_text(json.dumps(terrain, ensure_ascii=False, indent=2), encoding='utf-8')
        return {'kind': 'tr-resident-pages', 'path': str(terrain_path), 'residentPages': 192}
    raise SystemExit('unknown mutation ' + case)


def commands_for(case):
    if case in ('production_failure', 'over_budget'):
        return read_json(SCEN / (case + '.json'))['commands']
    return ["camera pose 0.0 480057.952692 611781.472602 0.0 21221.848241 0.0", "terrainvt stats"]


def run_case(case, frames, expect_failure):
    output = OUT / case
    if output.exists():
        print('SKIP', case)
        return None
    output.mkdir(parents=True)
    package = copy_package(case)
    mutation = mutate(package, case) if case in (
        'root_corrupt', 'input_corrupt', 'v1_reject', 'legacy_switch_reject') else None
    config = read_json(package / 'config/config.json')
    args = [str(package / 'main_game.exe'), '--initial-scene', config['initScene'],
            '--worker-thread-count', '2', '--frame-limit', '30', '--present-mode', 'immediate',
            '--no-dev-ui', '--validation', '--exit-after-frames', str(frames),
            '--console-command-delay', '1']
    for command in commands_for(case):
        args.extend(['--console-command', command])
    log = output / 'runtime.log'
    started = time.monotonic()
    with log.open('wb') as handle:
        process = subprocess.Popen(args, cwd=package, stdout=handle, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=1500)
            timed_out = False
        except subprocess.TimeoutExpired:
            process.kill()
            code = process.wait()
            timed_out = True
    elapsed = time.monotonic() - started
    text = log.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines()
    markers = [line for line in lines if any(m in line for m in KEY_MARKERS)]
    committed = any('World/graph transaction committed' in line for line in lines)
    init_failed = any(('EngineLoop init failed' in line) or ('LoadWorld transaction failed' in line)
                      or ('WorldTransition.LoadFailed' in line) for line in lines)
    faults = [line for line in lines if ('fault-injection' in line or 'production-failure' in line
                                         or 'submit-' in line or 'candidate-budget-admission' in line)]
    errors = [line for line in lines if ('[Error]' in line or 'VUID-' in line or 'Validation Error' in line)]
    verdict_failure = expect_failure and not committed and (init_failed or code != 0)
    verdict_continue = (not expect_failure) and committed and code == 0 and not timed_out
    record = {
        'case': case, 'expectFailure': expect_failure, 'exitCode': code, 'timedOut': timed_out,
        'elapsedSeconds': round(elapsed, 1), 'worldCommitted': committed, 'initFailed': init_failed,
        'mutation': mutation, 'package': str(package),
        'packageMainSha256': sha(package / 'main_game.exe'),
        'runtimeLog': {'path': str(log), 'bytes': log.stat().st_size, 'sha256': sha(log)},
        'errorLines': errors[:40], 'faultTelemetryLines': faults[:60],
        'keyLines': markers[:80],
        'verdict': ('failed-as-expected' if verdict_failure else
                    'continued-as-expected' if verdict_continue else 'unexpected-outcome'),
    }
    (output / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=1), encoding='utf-8')
    print(json.dumps({'case': case, 'exit': code, 'committed': committed, 'initFailed': init_failed,
                      'verdict': record['verdict'], 'elapsed': record['elapsedSeconds']}, ensure_ascii=False))
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cases', nargs='*', default=['closure'])
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--expect-failure', nargs='*', default=[])
    options = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    records = []
    for case in options.cases:
        record = run_case(case, options.frames, case in options.expect_failure)
        if record:
            records.append(record)
    (OUT / 'summary.json').write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
