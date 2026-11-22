"""Offline read-only Game driver: real NTFS WRITE-denied install + file-open trace.

记录 ACL 保护/探测、包内只读运行、运行后 inventory 不变、文件读取轨迹 footer/分类，
以及本次 R15 起要求记录的 dedicated runtime-evidence footer/integrity。
不修改包内容；不依赖尚未实现的 [RVT_EVIDENCE] 事件族。

R15 工具参数化：--package/--ownership/--resources/--output 均可覆盖，
默认值仍指向 R14 的旧路径以保持向后兼容。
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PKG = Path(r'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\p5g\C010r14')
RESOURCES = Path(r'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources')
OWNERSHIP = REPO / 'artifacts/terrain-rvt-production/p5/C010r14-package/export/package-ownership.json'
OUT = REPO / 'artifacts/terrain-rvt-production/final-delivery/offline-game'

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rvt_readonly_install import readonly_package  # noqa: E402


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): {'bytes': p.stat().st_size, 'sha256': sha(p)}
            for p in sorted(root.rglob('*')) if p.is_file()}


def evidence_footer(path):
    """Validate the dedicated RuntimeEvidence JSONL: sequence, last footer, byte accounting."""
    result = {'path': str(path), 'present': path.is_file()}
    if not result['present']:
        result.update(complete=False, reason='runtime evidence file absent (sink never initialized)')
        return result
    raw = path.read_bytes()
    result.update(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
    if not raw.endswith(b'\n'):
        result.update(complete=False, reason='file does not end with newline')
        return result
    lines = raw.splitlines()
    records, sequence_ok, first_error = [], True, None
    for index, line in enumerate(lines):
        try:
            record = json.loads(line.decode('utf-8'))
        except Exception as error:
            sequence_ok, first_error = False, f'line {index + 1}: {error}'
            break
        if record.get('sequence') != index + 1:
            sequence_ok, first_error = False, f"line {index + 1}: sequence={record.get('sequence')}"
            break
        records.append(record)
    result['lines'] = len(lines)
    result['sequenceContiguous'] = sequence_ok
    result['sequenceError'] = first_error
    if not records:
        result.update(complete=False, reason='empty evidence file')
        return result
    footer = records[-1]
    footer_bytes = len(lines[-1]) + 1
    result['footer'] = footer
    result['footerIsLast'] = footer.get('eventClass') == 'runtimeEvidenceEnd'
    payload = footer.get('payload') or {}
    result['recordsBeforeTerminal'] = payload.get('recordsBeforeTerminal')
    result['bytesBeforeTerminal'] = payload.get('bytesBeforeTerminal')
    result['byteAccountingMatches'] = (
        payload.get('bytesBeforeTerminal') is not None and
        payload['bytesBeforeTerminal'] + footer_bytes == len(raw))
    result['recordAccountingMatches'] = (
        payload.get('recordsBeforeTerminal') is not None and
        payload['recordsBeforeTerminal'] + 1 == len(lines))
    result['incomplete'] = payload.get('incomplete')
    result['complete'] = bool(sequence_ok and result['footerIsLast'] and
                              result['byteAccountingMatches'] and
                              result['recordAccountingMatches'] and result['incomplete'] is False)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, default=PKG)
    parser.add_argument('--ownership', type=Path, default=OWNERSHIP)
    parser.add_argument('--resources', type=Path, default=RESOURCES)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--worker', type=int, default=2)
    parser.add_argument('--no-runtime-evidence', action='store_true',
                        help='Skip the dedicated RuntimeEvidence sink (R14-compatible fallback)')
    options = parser.parse_args()
    package = options.package.resolve(strict=True)
    ownership = options.ownership.resolve(strict=True)
    resources = options.resources.resolve(strict=True)
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    before = inventory(package)
    config = json.loads((package / 'config/config.json').read_text(encoding='utf-8'))
    trace = output / 'file-trace.jsonl'
    evidence = output / 'runtime-events.jsonl'
    if trace.exists():
        raise SystemExit('refuse to overwrite existing trace; use a fresh evidence dir')
    if evidence.exists():
        raise SystemExit('refuse to overwrite existing runtime evidence; use a fresh evidence dir')
    environment = {k: v for k, v in os.environ.items() if not k.startswith('VL_')
                   and not k.startswith('VULKANLEARN_')}
    if not options.no_runtime_evidence:
        environment['VULKANLEARN_RUNTIME_EVIDENCE'] = str(evidence)
    environment.update({
        'VULKANLEARN_RUNTIME_FILE_TRACE': str(trace),
        'VULKANLEARN_RUNTIME_FILE_TRACE_PACKAGE_ROOT': str(package),
        'VULKANLEARN_RUNTIME_FILE_TRACE_PROJECT_ROOT': str(REPO),
        'VULKANLEARN_RUNTIME_FILE_TRACE_SOURCEAUTHOR_ROOT': str(resources),
        'VULKANLEARN_RUNTIME_FILE_TRACE_CACHE_ROOT': str(output / 'cache'),
        'VULKANLEARN_RUNTIME_FILE_TRACE_SYSTEM_ROOT': r'C:\Windows',
        'VULKANLEARN_RUNTIME_FILE_TRACE_EXPLICITOUTPUT_ROOT': str(output),
    })
    args = [str(package / 'main_game.exe'), '--initial-scene', config['initScene'],
            '--worker-thread-count', str(options.worker), '--frame-limit', '30',
            '--present-mode', 'immediate', '--no-dev-ui', '--validation',
            '--exit-after-frames', str(options.frames), '--console-command-delay', '1',
            '--console-command', 'camera pose 0.0 480057.952692 611781.472602 0.0 21221.848241 0.0',
            '--console-command', 'terrainvt stats']
    log = output / 'runtime.log'
    receipt = {'package': str(package), 'mainGameSha256': sha(package / 'main_game.exe'),
               'packageFiles': len(before),
               'packageBytes': sum(entry['bytes'] for entry in before.values()),
               'gameExportManifestSha256': sha(package / 'game-export-manifest.json'),
               'ownership': str(ownership), 'argv': args,
               'runtimeEvidenceEnabled': not options.no_runtime_evidence}
    started = time.monotonic()
    with readonly_package(package, ownership, output, REPO, resources) as probe:
        receipt['aclProbe'] = probe
        receipt['readonlyProtectSeconds'] = round(time.monotonic() - started, 1)
        with log.open('wb') as handle:
            process = subprocess.Popen(args, cwd=package, stdout=handle, stderr=subprocess.STDOUT,
                                       env=environment)
            try:
                code = process.wait(timeout=1500)
                timed_out = False
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
                timed_out = True
    receipt.update({'exitCode': code, 'timedOut': timed_out,
                    'elapsedSeconds': round(time.monotonic() - started, 1)})
    text = log.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines()
    receipt['worldCommitted'] = any('World/graph transaction committed' in line for line in lines)
    receipt['cleanTeardown'] = any('EngineLoop teardown complete' in line for line in lines)
    receipt['shaderCompileLines'] = [line for line in lines if 'Shader build' in line or 'Cache miss' in line]
    receipt['errors'] = [line for line in lines if ('[Error]' in line or 'VUID-' in line
                                                   or 'Validation Error' in line)]
    after = inventory(package)
    receipt['packageUnchanged'] = before == after
    receipt['changedFiles'] = sorted(set(before) ^ set(after))
    receipt['runtimeEvidence'] = evidence_footer(evidence)
    if trace.is_file():
        raw = trace.read_text(encoding='utf-8', errors='replace').splitlines()
        receipt['trace'] = {'path': str(trace), 'bytes': trace.stat().st_size, 'sha256': sha(trace),
                            'lines': len(raw), 'first': raw[0] if raw else None,
                            'last': raw[-1] if raw else None}
        roots = {}
        outside = []
        for line in raw:
            try:
                row = json.loads(line)
            except Exception:
                continue
            scope = row.get('rootScope') or row.get('root') or 'unknown'
            roots[scope] = roots.get(scope, 0) + 1
            if scope in ('sourceauthor', 'project'):
                outside.append(row)
        receipt['trace']['rootScopeCounts'] = roots
        receipt['trace']['externalAuthorOrProjectOpens'] = outside[:200]
        receipt['trace']['externalAuthorOrProjectOpenCount'] = len(outside)
    else:
        receipt['trace'] = {'missing': True}
    (output / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=1), encoding='utf-8')
    print(json.dumps({k: receipt[k] for k in ('exitCode', 'timedOut', 'worldCommitted', 'cleanTeardown',
                                              'packageUnchanged', 'elapsedSeconds')}, ensure_ascii=False))
    print(json.dumps(receipt.get('trace', {}).get('rootScopeCounts'), ensure_ascii=False))
    print(json.dumps({k: receipt['runtimeEvidence'].get(k) for k in (
        'present', 'complete', 'sequenceContiguous', 'footerIsLast', 'byteAccountingMatches',
        'recordAccountingMatches', 'incomplete', 'lines')}, ensure_ascii=False))
    evidence_ok = options.no_runtime_evidence or receipt['runtimeEvidence'].get('complete') is True
    return 0 if (code == 0 and receipt['worldCommitted'] and receipt['cleanTeardown']
                 and receipt['packageUnchanged'] and evidence_ok) else 3


if __name__ == '__main__':
    sys.exit(main())
