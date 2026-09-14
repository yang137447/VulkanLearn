"""从曲线探针截图里**反向识别图上文字**（M_brdfPlot 的图头 / 图例）。

为什么需要它：`M_brdfPlot` 自带 5x7 点阵字体，说明行里的结论数字是 shader 现算的。
"图上写的数 == 独立复算的数"这类判定，前提是**图里真的写了那串字符**——只靠读 shader
源码无法排除"字形表错位 / 数字没画上去 / 标签写错"。本工具把截图里的字形逐格解回来，
与预期字符串逐字比对，是 M-08 起沿用的核对方法（见 case 记录）。

做法：字体表从 `shader/glsl/M_brdfPlot.surface.glsl` 里解析（不在这里再抄一份）；
文字框位置按材质 `u_plotRectPixels` / `u_plotTextPixels` 与 shader 里同一套版式公式算出；
每个字形按"字模单元（cell）中心像素是不是文字色"还原成 5x7 位图，再与字体表反查。

用法：

    py -3 tool/validation/read_plot_text.py <shot.bmp> --row caption
    py -3 tool/validation/read_plot_text.py <shot.bmp> --row legend0 --expect "Schlick (direct) k=(r+1)^2/8"

退出码：解出的字符串与 `--expect` 不一致即 1；无法判定（字形表对不上）也是 1。
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

from bmp_reader import BmpImage, linear_to_srgb, srgb_to_linear
from measure_plot_curve import is_ui_overlay_pixel, set_ui_overlay_enabled

MATERIAL_JSON_PATH = Path(__file__).resolve().parents[2] / "shader" / "glsl" / "M_brdfPlot.json"
SURFACE_SHADER_PATH = Path(__file__).resolve().parents[2] / "shader" / "glsl" / "M_brdfPlot.surface.glsl"

# 与 shader 常量一致
PLOT_TEXT_COLOR = (0.72, 0.74, 0.78)
PLOT_BASE_COLOR = (0.13, 0.14, 0.17)


def load_font(path: Path = SURFACE_SHADER_PATH) -> tuple[list[int], list[list[int]]]:
    """解析 shader 里的字体表，返回 (codes, rows_by_glyph)。"""
    source = path.read_text(encoding="utf-8")
    codes_block = re.search(
        r"PLOT_FONT_CODES\[\d+\] = int\[\d+\]\((.*?)\);", source, re.S
    )
    rows_block = re.search(
        r"PLOT_FONT_ROWS\[\d+\] = uint\[\d+\]\((.*?)\n\);", source, re.S
    )
    if codes_block is None or rows_block is None:
        raise RuntimeError("字体表没解析出来：M_brdfPlot.surface.glsl 结构变了？")
    codes = [int(token) for token in re.findall(r"\d+", codes_block.group(1))]
    rows = [int(token, 16) for token in re.findall(r"0x([0-9A-Fa-f]+)u", rows_block.group(1))]
    if len(rows) != len(codes) * 7:
        raise RuntimeError(f"字形行数 {len(rows)} != {len(codes)} x 7")
    return codes, [rows[i * 7:(i + 1) * 7] for i in range(len(codes))]


def load_layout(path: Path = MATERIAL_JSON_PATH):
    import json

    parameters = json.loads(path.read_text(encoding="utf-8"))["parameters"]
    rect = [float(v) for v in parameters["u_plotRectPixels"]["default"]]
    text = [float(v) for v in parameters["u_plotTextPixels"]["default"]]
    return rect, text


def glyph_bitmap(
    image: BmpImage, origin_x: float, baseline_y: float, cell: float, index: int
) -> list[int] | None:
    """读出第 index 个字形单元的 5x7 位图；任一字模像素被运行时 UI 遮挡时返回 None。

    UI 遮挡是实测约束：`--no-dev-ui` 只关 ImGui，RmlUi 仍会在左上角画一块
    `x[31..216] y[32..45]` 的控件，它正好压在图例第 1~2 行上。被压住的字模像素
    会以"接近文字色"的亮度被读成墨点（实测让 'c' 的最后一行多出一个 bit），
    所以这里不能猜，只能标成不可读。
    """
    advance = cell * 6.0
    glyph_x = origin_x + advance * index
    glyph_top_screen = image.height - (baseline_y + cell * 7.0)
    # 文字色经 sRGB 编码后的 8-bit 期望值（三通道各自换算）
    expected = tuple(round(255.0 * linear_to_srgb(channel)) for channel in PLOT_TEXT_COLOR)
    # 半像素相位：shader 里的 `pixel = uv * panelSize` 是**像素中心**坐标（uv 取自
    # 像素中心），所以字模单元 [origin + c*cell, ...+cell) 在屏幕整数像素上整体偏 -0.5。
    # 采样点必须取"单元中心再减回半像素"，否则 cell=2 时会踩到相邻字模行的边界，
    # 把下一行的墨点混进当前行（实测让 legend0 的 'S' 第 5 行多出两个 bit）。
    sample_offset = cell / 2.0 - 0.5
    rows: list[int] = []
    occluded = False
    for row in range(7):
        bits = 0
        for column in range(5):
            x = int(glyph_x + column * cell + sample_offset)
            y = int(glyph_top_screen + row * cell + sample_offset)
            if x < 0 or y < 0 or x >= image.width or y >= image.height:
                continue
            if is_ui_overlay_pixel(x, y):
                occluded = True
                continue
            pixel = image.rgb(x, y)
            if all(abs(pixel[i] - expected[i]) <= 24 for i in range(3)):
                bits |= 1 << (4 - column)
        rows.append(bits)
    return None if occluded else rows


def decode_text(
    image: BmpImage,
    codes: list[int],
    rows_by_glyph: list[list[int]],
    origin_x: float,
    baseline_y: float,
    cell: float,
    max_chars: int = 48,
) -> tuple[str, int]:
    """返回 (解出的字符串, 被 UI 遮挡的字形数)；被遮挡的字形写成 '?'。"""
    by_bitmap = {tuple(rows): code for code, rows in zip(codes, rows_by_glyph)}
    characters: list[str] = []
    occluded_count = 0
    for index in range(max_chars):
        bitmap = glyph_bitmap(image, origin_x, baseline_y, cell, index)
        if bitmap is None:
            characters.append("?")
            occluded_count += 1
            continue
        if all(bits == 0 for bits in bitmap):
            # 全空 = 空格，或是文字结束（后面还有字形就说明是空格）
            following = [
                glyph_bitmap(image, origin_x, baseline_y, cell, index + offset + 1)
                for offset in range(2)
            ]
            if any(b is not None and any(bits != 0 for bits in b) for b in following):
                characters.append(" ")
            else:
                break
            continue
        code = by_bitmap.get(tuple(bitmap))
        if code is None:
            raise RuntimeError(f"第 {index} 个字形的位图不在字体表里: {bitmap}")
        characters.append(chr(code))
    return "".join(characters).rstrip(), occluded_count


def matches(decoded: str, expect: str) -> bool:
    """被 UI 遮挡的字形（'?'）对任何字符都算匹配，其余必须逐字一致。"""
    if len(decoded) != len(expect):
        return False
    return all(d == "?" or d == e for d, e in zip(decoded, expect))


def main(argv: list[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("bmp")
    parser.add_argument(
        "--row", required=True,
        help="caption = 说明行；annotation = 右上角参数注记（需配合 --expect）；"
             "legend0 / legend1 / ... = 图例第 1..N 条（N 由 --legend-count 给出）",
    )
    parser.add_argument("--expect", default=None, help="预期字符串；不一致即非零退出")
    parser.add_argument(
        "--legend-count", type=int, default=3,
        help="图例条数。shader 在 >= 4 条时把行距收紧到 cell*8（85 px 上留白只装得下 4 行），"
             "所以基线必须按同一条规则算，否则 legend2/legend3 会读错行",
    )
    parser.add_argument(
        "--no-ui-overlay",
        action="store_true",
        help="不要跳过运行时 UI 包围盒。配合「关掉 ui.enabled 后采的图」使用："
             "短图例整行都在盒子里，不开这个开关就只能解出 '?'",
    )
    parser.add_argument("--material-json", default=str(MATERIAL_JSON_PATH))
    parser.add_argument("--max-chars", type=int, default=48)
    args = parser.parse_args(argv)

    if args.no_ui_overlay:
        set_ui_overlay_enabled(False)

    image = BmpImage.load(args.bmp)
    codes, rows_by_glyph = load_font()
    rect, text_metrics = load_layout(Path(args.material_json))
    cell, gap = text_metrics[0], text_metrics[3]

    pixel_min_x = rect[0]
    pixel_max_x = image.width - rect[2]
    pixel_max_y = image.height - rect[3]
    caption_baseline = pixel_max_y + rect[3] - gap - cell * 7.0
    # 行距必须与 shader 的 PlotDrawLegend 同步：>= 4 条图例时收紧到 cell*8。
    row_step = cell * 8.0 if args.legend_count >= 4 else cell * 7.0 + gap

    if args.row == "caption":
        origin_x = pixel_min_x
        baseline = caption_baseline
    elif args.row == "annotation":
        # 参数注记是**右对齐**在说明行上的，起点只能由字符串长度反推：
        # 没有 --expect 就没有长度，也就没有起点，这里不猜。
        if args.expect is None:
            raise SystemExit("--row annotation 需要 --expect：注记右对齐，起点要由字符串长度反推")
        origin_x = pixel_max_x - (cell * 6.0 * len(args.expect) - cell)
        baseline = caption_baseline
    elif args.row.startswith("legend"):
        index = int(args.row[len("legend"):])
        if not 0 <= index < args.legend_count:
            raise SystemExit(f"--row legend{index} 超出图例条数 {args.legend_count}")
        # 图例项：色线宽 cell*5，间隔 cell*2，然后才是文字
        origin_x = pixel_min_x + cell * 5.0 + cell * 2.0
        baseline = caption_baseline - row_step - row_step * index
    else:
        raise SystemExit(f"未知的 --row: {args.row}")

    decoded, occluded = decode_text(
        image, codes, rows_by_glyph, origin_x, baseline, cell, args.max_chars
    )
    print(f"图片: {args.bmp}  row={args.row}")
    print(f"字模单元: {cell:g} px  说明行基线(y 向上): {caption_baseline:g}")
    print(f"解出文字: {decoded!r}")
    if occluded:
        print(f"  其中 {occluded} 个字模被运行时 UI 遮挡（'?'），只判其余字符")

    if args.expect is None:
        return 0
    print(f"预期文字: {args.expect!r}")
    if not matches(decoded, args.expect):
        print("判定: 不通过（与预期字符串不一致）")
        return 1
    print("判定: 通过（逐字一致，遮挡处按通配处理）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
