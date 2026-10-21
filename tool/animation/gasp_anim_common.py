"""Shared helpers for the GASP Skeleton/Sequence/AnimMontage exporters.

This module is deliberately dependency-free: the engine checkout does not ship a
Python BLAKE3 package, and the three asset JSONs must carry the same BLAKE3-256
digests the native loader uses.  The implementation below is the portable
BLAKE3 hash (no key, 32-byte output) and is checked against the bundled C
implementation by ``--self-test`` in ``export_gasp_skeleton.py``.

JSON policy: UTF-8, two-space indent, LF newlines, insertion-ordered keys and a
trailing newline.  Callers build dicts in schema order so repeated runs produce
byte-identical files.
"""

from __future__ import annotations

import json
import os
import struct
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# BLAKE3 (portable, unkeyed, 32-byte digest)
# ---------------------------------------------------------------------------

_IV = (
    0x6A09E667,
    0xBB67AE85,
    0x3C6EF372,
    0xA54FF53A,
    0x510E527F,
    0x9B05688C,
    0x1F83D9AB,
    0x5BE0CD19,
)
_MSG_PERMUTATION = (2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8)
_CHUNK_START = 1
_CHUNK_END = 2
_PARENT = 4
_ROOT = 8
_MASK32 = 0xFFFFFFFF


def _rotr(value: int, shift: int) -> int:
    return ((value >> shift) | (value << (32 - shift))) & _MASK32


def _g(state: list[int], a: int, b: int, c: int, d: int, mx: int, my: int) -> None:
    state[a] = (state[a] + state[b] + mx) & _MASK32
    state[d] = _rotr(state[d] ^ state[a], 16)
    state[c] = (state[c] + state[d]) & _MASK32
    state[b] = _rotr(state[b] ^ state[c], 12)
    state[a] = (state[a] + state[b] + my) & _MASK32
    state[d] = _rotr(state[d] ^ state[a], 8)
    state[c] = (state[c] + state[d]) & _MASK32
    state[b] = _rotr(state[b] ^ state[c], 7)


def _round(state: list[int], message: list[int]) -> None:
    _g(state, 0, 4, 8, 12, message[0], message[1])
    _g(state, 1, 5, 9, 13, message[2], message[3])
    _g(state, 2, 6, 10, 14, message[4], message[5])
    _g(state, 3, 7, 11, 15, message[6], message[7])
    _g(state, 0, 5, 10, 15, message[8], message[9])
    _g(state, 1, 6, 11, 12, message[10], message[11])
    _g(state, 2, 7, 8, 13, message[12], message[13])
    _g(state, 3, 4, 9, 14, message[14], message[15])


def _compress(
    chaining_value: tuple[int, ...] | list[int],
    block_words: list[int],
    counter: int,
    block_len: int,
    flags: int,
) -> list[int]:
    state = (
        list(chaining_value)
        + list(_IV[:4])
        + [counter & _MASK32, (counter >> 32) & _MASK32, block_len, flags]
    )
    message = list(block_words)
    for round_index in range(7):
        _round(state, message)
        if round_index < 6:
            message = [message[index] for index in _MSG_PERMUTATION]
    for index in range(8):
        state[index] ^= state[index + 8]
        state[index + 8] ^= chaining_value[index]
    return state


class _Output:
    """The final block of a chunk or a parent node, before ROOT finalisation."""

    __slots__ = ("chaining_value_in", "block_words", "counter", "block_len", "flags")

    def __init__(
        self,
        chaining_value_in: tuple[int, ...] | list[int],
        block_words: list[int],
        counter: int,
        block_len: int,
        flags: int,
    ) -> None:
        self.chaining_value_in = tuple(chaining_value_in)
        self.block_words = block_words
        self.counter = counter
        self.block_len = block_len
        self.flags = flags

    def chaining_value(self) -> list[int]:
        return _compress(
            self.chaining_value_in,
            self.block_words,
            self.counter,
            self.block_len,
            self.flags,
        )[:8]

    def root_bytes(self) -> bytes:
        words = _compress(
            self.chaining_value_in,
            self.block_words,
            0,
            self.block_len,
            self.flags | _ROOT,
        )
        return b"".join(word.to_bytes(4, "little") for word in words[:8])


def _block_words(block: bytes) -> list[int]:
    if len(block) != 64:
        raise ValueError("BLAKE3 block must be 64 bytes")
    return list(struct.unpack("<16I", block))


def _chunk_output(chunk: bytes, chunk_counter: int) -> _Output:
    block_count = max(1, (len(chunk) + 63) // 64)
    chaining_value: tuple[int, ...] | list[int] = _IV
    output: _Output | None = None
    for block_index in range(block_count):
        start = block_index * 64
        block = chunk[start:start + 64]
        flags = 0
        if block_index == 0:
            flags |= _CHUNK_START
        if block_index == block_count - 1:
            flags |= _CHUNK_END
        output = _Output(chaining_value, _block_words(block.ljust(64, b"\x00")),
                         chunk_counter, len(block), flags)
        if block_index != block_count - 1:
            chaining_value = output.chaining_value()
    assert output is not None
    return output


def _parent_output(left: list[int], right: list[int]) -> _Output:
    return _Output(_IV, list(left) + list(right), 0, 64, _PARENT)


def blake3_bytes(data: bytes) -> bytes:
    """Return the 32-byte BLAKE3-256 digest of *data*."""
    chunks = [data[index:index + 1024] for index in range(0, len(data), 1024)] or [b""]
    stack: list[list[int]] = []
    for chunk_index, chunk in enumerate(chunks[:-1]):
        cv = _chunk_output(chunk, chunk_index).chaining_value()
        total_chunks = chunk_index + 1
        while (total_chunks & 1) == 0:
            cv = _parent_output(stack.pop(), cv).chaining_value()
            total_chunks >>= 1
        stack.append(cv)
    output = _chunk_output(chunks[-1], len(chunks) - 1)
    while stack:
        output = _parent_output(stack.pop(), output.chaining_value())
    return output.root_bytes()


def blake3_hex(data: bytes) -> str:
    return blake3_bytes(data).hex()


def blake3_file(path: Path) -> str:
    return blake3_hex(path.read_bytes())


# ---------------------------------------------------------------------------
# JSON / path helpers
# ---------------------------------------------------------------------------

def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def json_bytes(document: Any) -> bytes:
    """Canonical asset bytes: UTF-8, 2-space indent, stable key order, LF, final newline."""
    text = json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False)
    return (text + "\n").encode("utf-8")


def preflight_targets(paths: list[Path], force: bool) -> None:
    """Reject the whole batch before any write when a target already exists."""
    if force:
        return
    existing = [path for path in paths if path.exists()]
    if existing:
        raise FileExistsError(
            "Refusing to overwrite existing file(s); pass --force to replace:\n  "
            + "\n  ".join(str(path) for path in existing)
        )


def write_json(path: Path, document: Any, force: bool) -> int:
    """Write one canonical JSON file and return its byte size.

    Without force the create is exclusive (``x``); with force the bytes go to a
    sibling temp file first and replace the target atomically.  In both cases a
    failed write never leaves a half-written asset under the target name.
    """
    data = json_bytes(document)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not force:
        with path.open("xb") as stream:
            stream.write(data)
        return len(data)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
    os.replace(temporary, path)
    return len(data)


def resource_relative(path: Path, resource_root: Path) -> str:
    return path.resolve().relative_to(resource_root.resolve()).as_posix()


def enum_token(value: Any) -> str:
    """Return the bare UE enum token from a Python string or enum-like value.

    UE 5.8 Python renders ``get_editor_property`` enums as
    ``<RootMotionRootLock.REF_POSE: 0>``; plain strings are returned unchanged.
    """
    text = str(value)
    if text.startswith("<") and text.endswith(">"):
        body = text[1:-1]
        if "." in body:
            text = body.rsplit(".", 1)[1]
        if ":" in text:
            text = text.split(":", 1)[0]
    return text.strip()
