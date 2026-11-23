import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_frozen(compiler, flags, source, target, cwd):
    source, target, cwd = Path(source).resolve(), Path(target).resolve(), Path(cwd).resolve()
    dependency_file = target.with_suffix(".dependencies")
    preprocessed = target.with_suffix(".ii")
    subprocess.run([str(compiler), *flags, "-M", "-MT", "frozenInput",
                    "-MF", str(dependency_file), str(source)], cwd=cwd, check=True)
    dependencies = dependency_file.read_text(encoding="utf-8").replace("\\\n", "")
    dependencies = dependencies.split(":", 1)[1]
    paths = set()
    for token in re.findall(r"(?:\\.|[^\s])+", dependencies):
        token = re.sub(r"\\([ #:\\])", r"\1", token)
        path = Path(token)
        paths.add((path if path.is_absolute() else cwd / path).resolve())
    for argument in flags:
        if argument.startswith("@"):
            paths.add(Path(argument[1:]).resolve())
    paths.add(source)
    before = {str(path): digest(path) for path in sorted(paths)}
    subprocess.run([str(compiler), *flags, "-E", str(source), "-o", str(preprocessed)],
                   cwd=cwd, check=True)
    after = {str(path): digest(path) for path in sorted(paths)}
    if before != after:
        raise RuntimeError("C++ source/header closure changed while freezing preprocessing input")
    frozen_digest = digest(preprocessed)
    subprocess.run([str(compiler), *flags, "-c", str(preprocessed), "-o", str(target)],
                   cwd=cwd, check=True)
    if digest(preprocessed) != frozen_digest:
        raise RuntimeError("Frozen C++ preprocessing input changed during compilation")
    record = {
        "scope": "new compilation only; no retrospective claim for earlier binaries",
        "source": str(source), "compiler": str(compiler), "flags": flags,
        "filesBeforeAndAfterPreprocessing": before,
        "sourceClosureSha256": hashlib.sha256(
            json.dumps(before, sort_keys=True).encode("utf-8")).hexdigest(),
        "compiledInput": str(preprocessed), "compiledInputSha256": frozen_digest,
        "object": str(target), "objectSha256": digest(target),
    }
    target.with_suffix(".compile-closure.json").write_text(json.dumps(record, indent=2))
    return record
