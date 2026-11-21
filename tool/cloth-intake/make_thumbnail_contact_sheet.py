import sys
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    argv = sys.argv[1:]
    if len(argv) != 2:
        raise SystemExit("usage: make_thumbnail_contact_sheet.py <dir> <output>")
    directory = Path(argv[0])
    output = Path(argv[1])
    files = sorted(directory.glob("*.png"))
    tile = 256
    columns = 8
    rows = (len(files) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * tile, rows * tile), (30, 30, 30))
    draw = ImageDraw.Draw(sheet)
    for index, path in enumerate(files):
        with Image.open(path) as image:
            image = image.convert("RGB")
            image.thumbnail((tile, tile - 24))
            x = (index % columns) * tile + (tile - image.width) // 2
            y = (index // columns) * tile + 24
            sheet.paste(image, (x, y))
        draw.text(
            ((index % columns) * tile + 6, (index // columns) * tile + 6),
            path.stem,
            fill=(255, 255, 255),
        )
    sheet.save(output)
    print(output)


if __name__ == "__main__":
    main()
