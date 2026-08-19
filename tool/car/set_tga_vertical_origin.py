import argparse
from pathlib import Path


TGA_HEADER_SIZE = 18
TGA_TOP_ORIGIN_BIT = 0x20


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Set the vertical-origin flag in TGA headers without "
            "changing pixel payloads."
        )
    )
    parser.add_argument(
        "origin",
        choices=("top", "bottom"),
    )
    parser.add_argument(
        "images",
        nargs="+",
        type=Path,
    )
    return parser.parse_args()


def set_vertical_origin(path, origin):
    data = bytearray(path.read_bytes())
    if len(data) < TGA_HEADER_SIZE:
        raise RuntimeError(
            f"TGA header is truncated: {path}"
        )

    previous_descriptor = data[17]
    if origin == "top":
        data[17] |= TGA_TOP_ORIGIN_BIT
    else:
        data[17] &= ~TGA_TOP_ORIGIN_BIT

    path.write_bytes(data)
    return {
        "path": str(path),
        "previousDescriptor": (
            f"0x{previous_descriptor:02X}"
        ),
        "descriptor": f"0x{data[17]:02X}",
        "fileLength": len(data),
    }


def main():
    arguments = parse_arguments()
    for image_path in arguments.images:
        result = set_vertical_origin(
            image_path.resolve(),
            arguments.origin,
        )
        print(result)


if __name__ == "__main__":
    main()
