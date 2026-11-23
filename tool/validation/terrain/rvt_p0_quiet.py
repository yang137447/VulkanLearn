"""Repeat the raw V1 camera route while auditing Windows compiler processes."""

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import threading
import time


def compiler_processes():
    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError("Process audit snapshot unavailable")
    result = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        names = {"cmake.exe", "ninja.exe", "mingw32-make.exe", "g++.exe", "c++.exe",
                 "cc1plus.exe", "ld.exe", "ar.exe", "cl.exe", "msbuild.exe", "dotnet.exe"}
        while valid:
            if entry.szExeFile.lower() in names:
                result.append({"pid": entry.th32ProcessID, "name": entry.szExeFile})
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    output = options.output.resolve()
    audit = output.with_suffix(".host-audit.jsonl")
    stop = threading.Event()
    started = time.monotonic()
    errors = []

    def observe():
        with audit.open("w", encoding="utf-8") as stream:
            while not stop.is_set():
                try:
                    row = {"elapsedSeconds": time.monotonic() - started,
                           "compilerProcesses": compiler_processes(), "available": True}
                except OSError as error:
                    row = {"elapsedSeconds": time.monotonic() - started,
                           "available": False, "error": str(error)}
                    errors.append(row)
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                stop.wait(0.5)

    watcher = threading.Thread(target=observe, daemon=True)
    watcher.start()
    try:
        command = [sys.executable, str(Path(__file__).with_name("run_full_runtime.py")),
                   "--stage", str(options.stage.resolve()), "--output", str(output),
                   "--execute", "--no-validation", "--no-world-reload"]
        result = subprocess.run(command, cwd=Path(__file__).resolve().parents[3], check=False)
    finally:
        stop.set()
        watcher.join(timeout=3)
    rows = [json.loads(line) for line in audit.read_text().splitlines()]
    busy = [row for row in rows if row.get("compilerProcesses")]
    summary = {"auditSamples": len(rows), "busySamples": busy, "errors": errors,
               "requestedIntervalSeconds": 0.5, "audit": str(audit),
               "scope": "Named Windows compiler/build process inventory; can miss processes shorter than sampling cadence; unrelated background workload is not proven absent",
               "quietAtObservedSamples": not busy and not errors, "driverExitCode": result.returncode}
    output.with_suffix(".host-audit-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    if result.returncode or busy or errors:
        raise RuntimeError("Quiet P0 run failed or compiler activity was observed")


if __name__ == "__main__":
    main()
