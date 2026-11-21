"""Offline read-only Game driver: real NTFS WRITE-denied install + file-open trace.

记录 ACL 保护/探测、包内只读运行、运行后 inventory 不变、文件读取轨迹 footer/分类。
不修改包内容；不依赖尚未实现的 [RVT_EVIDENCE] 事件族。
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--worker', type=int, default=2)
    options = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    before = inventory(PKG)
    config = json.loads((PKG / 'config/config.json').read_text(encoding='utf-8'))
    trace = OUT / 'file-trace.jsonl'
    if trace.exists():
        raise SystemExit('refuse to overwrite existing trace; use a fresh evidence dir')
    environment = {k: v for k, v in os.environ.items() if not k.startswith('VL_')
                   and not k.startswith('VULKANLEARN_')}
    environment.update({
        'VULKANLEARN_RUNTIME_FILE_TRACE': str(trace),
        'VULKANLEARN_RUNTIME_FILE_TRACE_PACKAGE_ROOT': str(PKG),
        'VULKANLEARN_RUNTIME_FILE_TRACE_PROJECT_ROOT': str(REPO),
        'VULKANLEARN_RUNTIME_FILE_TRACE_SOURCEAUTHOR_ROOT': str(RESOURCES),
        'VULKANLEARN_RUNTIME_FILE_TRACE_CACHE_ROOT': str(OUT / 'cache'),
        'VULKANLEARN_RUNTIME_FILE_TRACE_SYSTEM_ROOT': r'C:\Windows',
        'VULKANLEARN_RUNTIME_FILE_TRACE_EXPLICITOUTPUT_ROOT': str(OUT),
    })
    args = [str(PKG / 'main_game.exe'), '--initial-scene', config['initScene'],
            '--worker-thread-count', str(options.worker), '--frame-limit', '30',
            '--present-mode', 'immediate', '--no-dev-ui', '--validation',
            '--exit-after-frames', str(options.frames), '--console-command-delay', '1',
            '--console-command', 'camera pose 0.0 480057.952692 611781.472602 0.0 21221.848241 0.0',
            '--console-command', 'terrainvt stats']
    log = OUT / 'runtime.log'
    receipt = {'package': str(PKG), 'mainGameSha256': sha(PKG / 'main_game.exe'),
               'ownership': str(OWNERSHIP), 'argv': args}
    started = time.monotonic()
    with readonly_package(PKG, OWNERSHIP, OUT, REPO, RESOURCES) as probe:
        receipt['aclProbe'] = probe
        receipt['readonlyProtectSeconds'] = round(time.monotonic() - started, 1)
        with log.open('wb') as handle:
            process = subprocess.Popen(args, cwd=PKG, stdout=handle, stderr=subprocess.STDOUT,
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
    after = inventory(PKG)
    receipt['packageUnchanged'] = before == after
    receipt['changedFiles'] = sorted(set(before) ^ set(after))
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
    (OUT / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=1), encoding='utf-8')
    print(json.dumps({k: receipt[k] for k in ('exitCode', 'timedOut', 'worldCommitted', 'cleanTeardown',
                                              'packageUnchanged', 'elapsedSeconds')}, ensure_ascii=False))
    print(json.dumps(receipt.get('trace', {}).get('rootScopeCounts'), ensure_ascii=False))
    return 0 if (code == 0 and receipt['worldCommitted'] and receipt['cleanTeardown']
                 and receipt['packageUnchanged']) else 3


if __name__ == '__main__':
    sys.exit(main())
