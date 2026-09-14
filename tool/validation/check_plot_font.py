"""探针字体表的形状核对（`M_brdfPlot.surface.glsl` 的 PLOT_FONT_CODES / PLOT_FONT_ROWS）。

存在的理由是一个真实事故：为写 `Beckmann` 新增的 `B` 字形被插在 `C` 之后，而编码顺序是
66='B'、67='C'，于是两个字形互换——mode 0 的说明行把 "Charlie" 画成了 "Bharlie"。

为什么**回读工具抓不到它**：`read_plot_text.py` 用同一张表解码，编码与解码自洽，
错位后依然"逐字一致"。唯一能抓住它的是下面这两件事，本脚本把其中一件自动化：

  1. 把每个 code 的字形画成 ASCII art，人工扫一眼（本脚本的默认输出）；
  2. 与改动前的截图逐像素对比（属于回归，不属于本脚本）。

因此本脚本的检查分两层：
  - **结构层（自动判定）**：声明数量 == 实际条数、行数 == 数量 x 7、code 严格升序且不重复、
    除空格外没有全零字形、没有两个不同 code 共用同一字形、每行不超过 5 位；
  - **形状层（输出给人看）**：ASCII art。字形是否"长得像那个字母"只有人能判。

退出码：结构层任一检查失败即 1。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SURFACE_SHADER_PATH = (
    Path(__file__).resolve().parents[2] / "shader" / "glsl" / "M_brdfPlot.surface.glsl"
)

GLYPH_ROWS = 7
GLYPH_COLUMNS = 5


def load_font(path: Path) -> tuple[int, list[int], list[list[int]]]:
    source = path.read_text(encoding="utf-8")
    declared = re.search(r"PLOT_FONT_GLYPH_COUNT = (\d+)", source)
    codes_block = re.search(r"PLOT_FONT_CODES\[\d+\] = int\[\d+\]\((.*?)\);", source, re.S)
    rows_block = re.search(r"PLOT_FONT_ROWS\[\d+\] = uint\[\d+\]\((.*?)\n\);", source, re.S)
    if declared is None or codes_block is None or rows_block is None:
        raise RuntimeError(f"字体表没解析出来：{path.name} 结构变了？")

    codes = [int(token) for token in re.findall(r"\d+", codes_block.group(1))]
    rows = [int(token, 16) for token in re.findall(r"0x([0-9A-Fa-f]+)u", rows_block.group(1))]
    if len(rows) != len(codes) * GLYPH_ROWS:
        raise RuntimeError(f"字形行数 {len(rows)} != {len(codes)} x {GLYPH_ROWS}")
    return int(declared.group(1)), codes, [
        rows[i * GLYPH_ROWS:(i + 1) * GLYPH_ROWS] for i in range(len(codes))
    ]


def glyph_art(glyph: list[int]) -> list[str]:
    return [
        "".join(
            "#" if (row >> (GLYPH_COLUMNS - 1 - column)) & 1 else "."
            for column in range(GLYPH_COLUMNS)
        )
        for row in glyph
    ]


def check_structure(declared: int, codes: list[int], glyphs: list[list[int]]) -> list[str]:
    problems: list[str] = []
    if len(codes) != declared:
        problems.append(f"PLOT_FONT_GLYPH_COUNT = {declared}，实际 {len(codes)} 个 code")
    if codes != sorted(codes):
        problems.append("code 未按升序排列（插字形时最常犯的错）")
    if len(set(codes)) != len(codes):
        duplicates = sorted({code for code in codes if codes.count(code) > 1})
        problems.append(f"code 重复：{duplicates}")

    seen: dict[tuple[int, ...], int] = {}
    for code, glyph in zip(codes, glyphs):
        if code != 32 and not any(glyph):
            problems.append(f"code {code}（'{chr(code)}'）字形全空——不是空格却是空白")
        for row in glyph:
            if row >> GLYPH_COLUMNS:
                problems.append(f"code {code}（'{chr(code)}'）有一行超出 5 位：{row:#07x}")
                break
        key = tuple(glyph)
        if key in seen:
            problems.append(
                f"code {code}（'{chr(code)}'）与 code {seen[key]}"
                f"（'{chr(seen[key])}'）字形完全相同——多半是复制粘贴后忘了改"
            )
        seen[key] = code
    return problems


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--shader", default=str(SURFACE_SHADER_PATH))
    parser.add_argument(
        "--codes", nargs="*", type=int, default=None,
        help="只画这些 code 的字形（默认全部）",
    )
    args = parser.parse_args(argv)

    declared, codes, glyphs = load_font(Path(args.shader))
    problems = check_structure(declared, codes, glyphs)

    print(f"{args.shader}")
    print(f"声明字形数 {declared}，实际 {len(codes)}，行数 {len(codes) * GLYPH_ROWS}")
    print("\n字形表（每行 7 行位图；'#' = 墨点）：")
    for code, glyph in zip(codes, glyphs):
        if args.codes and code not in args.codes:
            continue
        label = chr(code) if 32 <= code < 127 else "?"
        print(f"  {code:4d} '{label}'  " + "   ".join(glyph_art(glyph)))

    if problems:
        print("\n结构检查: 不通过")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("\n结构检查: 通过（形状是否像那个字母仍需人眼扫一遍上面的 art）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
