"""读取 Houdini .hip/.hda 的 ``INDX`` 容器格式（只读，不依赖 Houdini）。

Houdini 把 .hip/.hda 存成一组以 ``INDX`` 为 magic 的记录。文件结构（实测定于
2026-11-05，Pegasus LandscapeWorkflow 全部 hda/hip 可读）：

```text
INDX <8 bytes zero> <u32 record count> <index 条目>
                                     <record 0 payload>
INDX <8 bytes zero> <u32 record count> <index 条目>
                                     <record 1 payload>
...
```

* record payload 是一个 CPIO 风格的成员流：每个成员以
  ``0707070000010000...`` 标记开头，成员内容结束处有 ``TRAILER!!!``。
* 成员内容可以是纯文本（DialogScript、VEX），也可以是 gzip 压缩的
  （SOP 网络、Contents）；gzip 头可能出现在成员内的任意偏移。
* 因此按 ``TRAILER!!!`` 切成员、对每个成员做 ``gunzip``，即可拿到可读内容。

本模块只负责"把字节挖出来"，语义解析（VEX、参数、节点网络）交给调用方。
"""

from __future__ import annotations

import re
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

INDX_MAGIC = b"INDX"
GZIP_MAGIC = b"\x1f\x8b\x08"
MEMBER_MARKER = b"0707070000010000"
TRAILER = b"TRAILER!!!"
_TEXT_RUN = re.compile(rb"[\x20-\x7e\t\r\n]{24,}")
_VEX_SNIPPET = re.compile(
    r'name\s+"snippet"\s*.*?default\s*\{\s*"(.*?)"\s*\}', re.S)


class ArchiveError(RuntimeError):
    pass


@dataclass
class ArchiveMember:
    """一个解包出来的成员块。"""

    source: str
    index: int
    data: bytes
    name: str = ""

    @property
    def text(self) -> str:
        return self.data.decode("utf-8", errors="replace")

    @property
    def kind(self) -> str:
        text = self.text
        if self.name.endswith(".parm"):
            return "node_parm"
        if self.name.endswith(".def"):
            return "node_def"
        if self.name.endswith(".init"):
            return "node_init"
        if self.name.endswith(".chn"):
            return "node_channels"
        if self.name.endswith(".order"):
            return "node_order"
        if self.name.endswith(".inp"):
            return "node_inputs"
        if "# Dialog script" in text or "dialogscript" in text.lower():
            return "dialog_script"
        if _VEX_SNIPPET.search(text):
            return "vex"
        if "Contents" in text[:64]:
            return "contents"
        if text and all(ch.isprintable() or ch in "\t\r\n" for ch in text):
            return "text"
        return "binary"


@dataclass
class Archive:
    source: Path
    members: list[ArchiveMember] = field(default_factory=list)

    def by_kind(self, kind: str) -> list[ArchiveMember]:
        return [member for member in self.members if member.kind == kind]

    def find(self, needle: str) -> list[ArchiveMember]:
        return [member for member in self.members if needle in member.text]

    @property
    def vex_snippets(self) -> list[str]:
        snippets: list[str] = []
        for member in self.members:
            snippets.extend(_extract_snippets(member.text))
        return snippets


def _gunzip_all(data: bytes) -> bytes:
    """把一段连续 gzip 成员解压后拼接；不是 gzip 就原样返回。"""
    if GZIP_MAGIC not in data:
        return data
    out = bytearray()
    cursor = 0
    saw_member = False
    while cursor < len(data):
        start = data.find(GZIP_MAGIC, cursor)
        if start < 0:
            break
        try:
            stream = zlib.decompressobj(31)
            out += stream.decompress(data[start:])
            out += stream.flush()
        except zlib.error:
            cursor = start + 3
            continue
        if not stream.eof:
            cursor = start + 3
            continue
        saw_member = True
        cursor = len(data) - len(stream.unused_data)
    return bytes(out) if saw_member else data


def _iter_member_bodies(data: bytes) -> Iterable[bytes]:
    """按 ``TRAILER!!!`` 切出成员体（未解压）。

    单个成员体内可能夹着 gzip 流，gzip 流里也可能出现 ``TRAILER`` 字样，
    所以这里只做粗粒度分组；真正的子记录由 ``_split_subrecords``
    在整文件解压后的数据上按 ``070707`` 头扫描。
    """
    position = 0
    while position < len(data):
        end = data.find(TRAILER, position)
        if end < 0:
            return
        yield data[position:end]
        position = end + len(TRAILER) + 1


def _split_members(raw: bytes) -> list[tuple[str, bytes]]:
    """返回所有 (成员名, 内容)。

    实测有两种容器排布，必须两种都试、取成员更多的那一种：

    * 明文成员：整文件直接按 ``070707`` 头切（Pegasus 的 hda）；
    * gzip 成员：成员体被压缩，``TRAILER`` 也在压缩流内部，所以整文件
      先解压再按头切。

    只试其中一种会让另一类文件得到 0 个成员。
    """
    plain = _split_subrecords(raw)
    expanded = _split_subrecords(_gunzip_all(raw))
    return expanded if len(expanded) > len(plain) else plain


def _split_subrecords(decoded: bytes) -> list[tuple[str, bytes]]:
    """按 ``070707`` 子记录头切块。

    子记录 = ``070707`` 头（含名字）到下一个子记录头。
    名字为空的子记录（索引段包装块）跳过。
    """
    heads = [m.start() for m in re.finditer(rb"0707070000", decoded)]
    records: list[tuple[str, bytes]] = []
    for order, start in enumerate(heads):
        end = heads[order + 1] if order + 1 < len(heads) else len(decoded)
        chunk = decoded[start:end]
        name = _member_name(chunk)
        if not name:
            continue
        # 成员体里如果有 gzip 流（真实 hda 的 .def/.parm），解压后再取内容。
        payload = _gunzip_all(_strip_subrecord(chunk))
        if len(payload) < 4:
            continue
        records.append((name, payload))
    return records


# 子记录头（实测 76 字节）：
#   "070707" + 70 位十进制（序号/时间戳/长度等字段） + 名字 + NUL。
#   字段边界没有可靠的公开资料，且长度字段与名字长度并不总是相等，
#   所以不拆字段，直接按 HEAD_NAME_OFFSET 取名字，内容从名字后的 NUL 开始。
# 索引段里的是 "070707" + 16 位数字、没有名字，不会匹配。
HEAD_NAME_OFFSET = 76


def _member_name(chunk: bytes) -> str:
    """从子记录头取成员名；无名字返回空串。

    .hda 的成员名是 ``hdaroot/...``（偏移 76）；.hip 的段名如 ``.start`` /
    ``.OPfallbacks`` 偏移不固定，所以名字前不是数字就按"到 NUL 为止的
    ASCII 名字"处理。
    """
    if chunk[:6] != b"070707":
        return ""
    nul = chunk.find(b"\x00", HEAD_NAME_OFFSET)
    if nul > HEAD_NAME_OFFSET:
        name = chunk[HEAD_NAME_OFFSET:nul]
        if name[:1].isalnum() or name[:1] == b".":
            return name.decode("latin1")
    # .hip 的段名：头部数字之后直接是 ".name"。
    start = 6
    while start < len(chunk) and chunk[start:start + 1] in b"0123456789":
        start += 1
    if chunk[start:start + 1] in (b".",) or chunk[start:start + 1].isalnum():
        nul = chunk.find(b"\x00", start)
        if 0 < nul <= start + 80:
            name = chunk[start:nul]
            if name.isascii():
                return name.decode("ascii")
    return ""


def _strip_subrecord(payload: bytes) -> bytes:
    """去掉子记录头（含名字与 NUL），只留内容。"""
    if payload[:6] != b"070707":
        return payload
    nul = payload.find(b"\x00", HEAD_NAME_OFFSET)
    return payload[nul + 1:] if nul >= 0 else payload


def _extract_snippets(text: str) -> list[str]:
    snippets: list[str] = []
    for match in _VEX_SNIPPET.finditer(text):
        raw = match.group(1)
        try:
            snippets.append(raw.encode("latin1").decode("unicode_escape"))
        except UnicodeError:
            snippets.append(raw)
    return snippets


def _iter_text_runs(data: bytes, min_length: int = 24) -> Iterable[bytes]:
    for match in _TEXT_RUN.finditer(data):
        if len(match.group(0)) >= min_length:
            yield match.group(0)


def open_archive(path: Path) -> Archive:
    """解包一个 .hip/.hda，返回全部成员块。"""
    raw = path.read_bytes()
    archive = Archive(source=path)

    if raw[:4] == GZIP_MAGIC:
        raw = _gunzip_all(raw)

    # .hip 不以 INDX 开头，而是直接以 ".start" / ".OPfallbacks" 这类子记录
    # 排列；.hda 则以 INDX+索引段开头。两种情况都走同一套子记录扫描即可，
    # 不存在子记录时再退回纯文本。
    for index, (name, payload) in enumerate(_split_members(raw)):
        archive.members.append(ArchiveMember(
            source=str(path), index=index, data=payload, name=name))

    if not archive.members:
        for run_index, run in enumerate(_iter_text_runs(raw)):
            archive.members.append(ArchiveMember(
                source=str(path), index=run_index, data=run))
    return archive


def main(argv: list[str] | None = None) -> int:
    import argparse
    import json

    parser = argparse.ArgumentParser(
        description="Unpack Houdini .hip/.hda archives into readable members.")
    parser.add_argument("path", type=Path)
    parser.add_argument("--json", action="store_true",
                        help="machine-readable summary")
    parser.add_argument("--dump", type=int, metavar="INDEX",
                        help="print the full text of one member")
    parser.add_argument("--grep", help="only report members containing this text")
    args = parser.parse_args(argv)

    archive = open_archive(args.path.resolve())
    if args.dump is not None:
        member = archive.members[args.dump]
        print(member.text)
        return 0

    rows = []
    for member in archive.members:
        if args.grep and args.grep not in member.text:
            continue
        rows.append({
            "index": member.index,
            "kind": member.kind,
            "bytes": len(member.data),
            "vexSnippets": len(_extract_snippets(member.text)),
            "preview": member.text.strip().splitlines()[0][:96]
            if member.text.strip() else "",
        })
    if args.json:
        print(json.dumps({"source": str(archive.source), "members": rows},
                         ensure_ascii=False, indent=2))
        return 0
    for row in rows:
        # 预览可能含非文本字节；Windows 控制台是 GBK，先折成 ASCII 再打印。
        preview = row["preview"].encode("ascii", "replace").decode("ascii")
        print(f"[{row['index']:3d}] {row['bytes']:9d} {row['kind']:<14} "
              f"vex={row['vexSnippets']:<3} {preview}")
    print(f"total {len(rows)} members from {archive.source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
