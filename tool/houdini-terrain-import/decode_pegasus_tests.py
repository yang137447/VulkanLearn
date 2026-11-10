"""Pegasus 解码回归测试。

覆盖 ``houdini_archive`` 的容器解析与 ``decode_pegasus`` 的配方还原。
Pegasus 原始资产不在仓库内（授权限制），所以这里用合成的最小容器，
回归的是"字节格式理解"而不是某一份具体文件。

运行：

    py -3 tool\\houdini-terrain-import\\decode_pegasus_tests.py -v
"""

from __future__ import annotations

import gzip
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import decode_pegasus  # noqa: E402
import houdini_archive as ha  # noqa: E402


def _member(name: str, payload: bytes) -> bytes:
    """构造一个 76 字节头的子记录。

    头 = "070707" + 70 位十进制，名字从第 76 字节开始并以 NUL 结束。
    真实文件里成员体可能还被 gzip 包一层，但那种情况必须带 ``TRAILER!!!``
    边界才能被解析器识别（见 ``test_compressed_member_is_expanded``）。
    """
    name_bytes = name.encode("latin1")
    head = b"070707" + b"0" * (76 - 6)
    return head + name_bytes + b"\x00" + payload


def _container(members: list[bytes]) -> bytes:
    """把子记录串成一个 INDX 容器。

    真实文件里每个成员体后面都跟着 ``070707...TRAILER!!!``，解析器按
    "成员体 = 前一 TRAILER 之后到下一 TRAILER" 分组，所以这里补上收尾标记。
    """
    trailer = b"070707" + b"0" * 15 + b"TRAILER!!!\x00"
    body = b"".join(member + trailer for member in members)
    return (b"INDX" + b"\x00" * 8 + (3).to_bytes(4, "big")
            + os_style_index() + body + trailer)


def os_style_index() -> bytes:
    """最小索引段：名字表 + 4 字节偏移，不含 gzip 魔数。"""
    return b"\x00\x00\x00\x08Contents\x00\x00\x00\x00\x00\x00\x00\x10"


def _node(name: str, kind: str, parms: str = "", def_text: str = "",
          compress: bool = False) -> list[bytes]:
    trailer = b"070707" + b"0" * 15 + b"TRAILER!!!\x00"

    def wrap(payload: bytes) -> bytes:
        # 压缩成员自己带 TRAILER，由 _container 负责补齐外层。
        return gzip.compress(payload, mtime=0) if compress else payload
    return [
        _member(f"hdaroot/{name}.init", wrap(f"type = {kind}\nmatchesdef = 1\n".encode())),
        _member(f"hdaroot/{name}.def",
                wrap((def_text or "sopflags sopflags = \ninputs\n{\n}\n").encode())),
        _member(f"hdaroot/{name}.parm", wrap(parms.encode())),
    ]


class ArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def _write(self, name: str, data: bytes) -> Path:
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_reads_node_name_and_type(self) -> None:
        path = self._write("sop_test.hda", _container(
            _node("heightfield_noise1", "heightfield_noise")))
        members = ha.open_archive(path).members
        self.assertTrue(members, hex(len(path.read_bytes())))
        init = [m for m in members if m.name == "hdaroot/heightfield_noise1.init"]
        self.assertEqual(len(init), 1)
        self.assertIn("heightfield_noise", init[0].text)

    def test_child_network_paths_are_kept(self) -> None:
        path = self._write("sop_nested.hda", _container(
            _node("erode1/solve/d/s/add_mask", "null")))
        names = {m.name for m in ha.open_archive(path).members}
        self.assertIn("hdaroot/erode1/solve/d/s/add_mask.init", names)

    def test_compressed_member_is_expanded(self) -> None:
        compressed = gzip.compress(b'{\nversion 0.8\namp\t[ 0\tlocks=0 ]\t(\t5\t)\n}\n',
                                   mtime=0)
        path = self._write("sop_zip.hda", _container([
            _member("hdaroot/a.parm", compressed)]))
        member = next(m for m in ha.open_archive(path).members
                      if m.name.endswith(".parm"))
        self.assertIn("amp", member.text)

    def test_index_section_is_not_a_member(self) -> None:
        index_blob = b"INDX" + b"\x00" * 8 + b"INDEX_SECTION" + b"\x00" * 48
        path = self._write("sop_idx.hda", _container([
            _member("hdaroot/x.init", index_blob),
            _member("hdaroot/x.def", b"sopflags sopflags = \ninputs\n{\n}\n"),
        ]))
        init = [m for m in ha.open_archive(path).members
                if m.name == "hdaroot/x.init"]
        self.assertEqual(len(init), 1)
        self.assertTrue(init[0].text.startswith("INDX"))


def os_style_index() -> bytes:
    return b"\x00" * 40 + b"\x00\x00\x00\x04\x00\x01\x02\x03"


class DecodeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.graph = decode_pegasus.decode_asset_from_tmp(
            [_member("hdaroot/heightfield_noise1.init",
                     b"type = heightfield_noise\nmatchesdef = 1\n"),
             _member("hdaroot/heightfield_remap1.init",
                     b"type = heightfield_remap\nmatchesdef = 1\n"),
             _member("hdaroot/heightfield_remap1.def", b"""sopflags sopflags =
outputsNamed3
{
2 "output1"
}
inputs
{
0 	heightfield_noise1 1 1
}
"""),
             _member("hdaroot/heightfield_remap1.parm", b"""{
version 0.8
snippet	[ 0	locks=0 ]	(	"float h = 2;
float k = (h + 1) * 0.5;"
	)
}
""")])

    def test_links_follow_def_inputs(self) -> None:
        upstream = {target: source for source, target in self.graph.links}
        self.assertIn("heightfield_remap1:0", upstream)
        self.assertEqual(upstream["heightfield_remap1:0"], "heightfield_noise1.output1")

    def test_multiline_snippet_is_preserved(self) -> None:
        node = self.graph.nodes["heightfield_remap1"]
        self.assertIn("float k = (h + 1) * 0.5;", node.snippet)
        self.assertEqual(self.graph.nodes["heightfield_remap1"].parms["snippet"],
                         node.snippet)

    def test_terrain_nodes_are_classified(self) -> None:
        kinds = {node.kind for node in self.graph.terrain_nodes()}
        self.assertEqual(kinds, {"heightfield_noise", "heightfield_remap"})

    def test_to_dict_round_trips_through_json(self) -> None:
        payload = json.dumps(self.graph.to_dict(), ensure_ascii=False)
        self.assertIn("heightfield_remap1", payload)


if __name__ == "__main__":
    unittest.main()
