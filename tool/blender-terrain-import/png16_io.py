"""Minimal dependency-free PNG I/O for terrain height fields.

Only the Python standard library (``struct`` + ``zlib``) and numpy are used.
The writer always emits non-interlaced, filter-0 16-bit grayscale PNGs.  The
reader accepts the common non-interlaced grayscale/RGB/RGBA layouts at both
8-bit and 16-bit depth and always returns a ``uint16`` 2-D array.

Terrain masks in this repository are stored as 16-bit grayscale with the
row-0-at-top convention, so no vertical flip is performed here.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# PNG color type -> channel count.
_CHANNELS = {
    0: 1,  # grayscale
    2: 3,  # RGB
    3: 1,  # palette index (unsupported; kept out of the accepted table)
    4: 2,  # grayscale + alpha
    6: 4,  # RGBA
}


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def write_gray16(path: str | Path, values: np.ndarray) -> None:
    """Write a 16-bit grayscale PNG from a 2-D array.

    Values are clipped to 0..65535 and rounded to the nearest integer, which
    keeps a decode/encode round trip stable for quantized height fields.
    """

    array = np.asarray(values)
    if array.ndim != 2:
        raise ValueError(f"expected a 2-D array, got shape {array.shape}")
    height, width = array.shape
    if height == 0 or width == 0:
        raise ValueError("cannot write an empty PNG")

    quantized = np.rint(np.clip(array, 0.0, 65535.0)).astype(np.uint16)
    rows = np.zeros((height, 1 + width * 2), dtype=np.uint8)
    rows[:, 0] = 0  # filter type None
    rows[:, 1:] = quantized.astype(">u2", copy=False).view(np.uint8).reshape(
        height, width * 2
    )
    header = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    payload = (
        PNG_MAGIC
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(rows.tobytes(), 6))
        + _png_chunk(b"IEND", b"")
    )

    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(payload)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter_rows(
    raw: bytes, height: int, width: int, bpp: int
) -> list[bytearray]:
    """Undo PNG scanline filters.  ``bpp`` is bytes per complete pixel."""

    stride = width * bpp
    if len(raw) < height * (stride + 1):
        raise ValueError("truncated PNG image data")

    rows: list[bytearray] = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        row = bytearray(raw[offset : offset + stride])
        offset += stride

        if filter_type == 0:
            pass
        elif filter_type == 1:  # Sub
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + previous[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                upper_left = previous[i - bpp] if i >= bpp else 0
                row[i] = (
                    row[i] + _paeth(left, previous[i], upper_left)
                ) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter type {filter_type}")

        rows.append(row)
        previous = row
    return rows


def read_gray16(path: str | Path) -> np.ndarray:
    """Read a PNG and return its first channel as ``uint16``.

    Supported layouts are non-interlaced grayscale (0), RGB (2),
    grayscale+alpha (4) and RGBA (6) at 8 or 16 bits per sample.  For color
    images the red channel is returned.  Palette and interlaced images raise a
    ``ValueError`` instead of silently producing wrong terrain.
    """

    data = Path(path).read_bytes()
    if not data.startswith(PNG_MAGIC):
        raise ValueError(f"{path} is not a PNG file")

    offset = len(PNG_MAGIC)
    header: tuple[int, int, int, int, int, int, int] | None = None
    idat = bytearray()
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        tag = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        if len(payload) != length:
            raise ValueError(f"truncated PNG chunk {tag!r}")
        offset += 12 + length

        if tag == b"IHDR":
            if length != 13:
                raise ValueError("invalid PNG IHDR length")
            header = struct.unpack(">IIBBBBB", payload)
        elif tag == b"IDAT":
            idat.extend(payload)
        elif tag == b"IEND":
            break
        elif tag == b"PLTE":
            raise ValueError("palette PNGs are not supported")

    if header is None:
        raise ValueError(f"{path} has no IHDR chunk")

    width, height, bit_depth, color_type, compression, filter_method, interlace = header
    if width == 0 or height == 0:
        raise ValueError("PNG has zero width or height")
    if compression != 0 or filter_method != 0:
        raise ValueError("unsupported PNG compression/filter method")
    if interlace != 0:
        raise ValueError("interlaced PNGs are not supported")
    if color_type not in _CHANNELS or color_type == 3:
        raise ValueError(f"unsupported PNG color type {color_type}")
    if bit_depth not in (8, 16):
        raise ValueError(f"unsupported PNG bit depth {bit_depth}")

    channels = _CHANNELS[color_type]
    bpp = channels * (bit_depth // 8)
    raw = zlib.decompress(bytes(idat))
    rows = _unfilter_rows(raw, height, width, bpp)

    packed = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(
        height, width, bpp
    )
    if bit_depth == 16:
        samples = packed.reshape(height, width, channels, 2)
        channel0 = (
            samples[:, :, 0, 0].astype(np.uint16) << 8
        ) | samples[:, :, 0, 1].astype(np.uint16)
    else:
        channel0 = packed[:, :, 0].astype(np.uint16)

    return np.ascontiguousarray(channel0)


__all__ = ["read_gray16", "write_gray16"]
