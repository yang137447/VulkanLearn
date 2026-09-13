"""24-bit BMP 读取与像素采样（VulkanLearn 论文 case 验证工具的底层模块）。

只处理 `rendererBackendVulkan.cpp -> WriteScreenshotBmp()` 产出的那一种形态：

    54 字节头 + 24bpp + 负高度（行序 top-down）+ 每行按 4 字节对齐 + 通道序 BGR

约定：对外暴露的坐标一律是**屏幕坐标**——(0, 0) 是画面左上角，y 向下增大。
文件里既可能是 top-down 也可能是 bottom-up（正高度），读取时统一成屏幕坐标，
调用方不需要知道文件行序。

颜色空间：swapchain 是 sRGB 格式（`vulkanManager.cpp` 优先选 eR8G8B8A8Srgb），
tone mapping 的最终写入会被硬件做一次 linear -> sRGB 编码。因此
`linear_rgb()` 给出的才是 shader 真正写出的线性值；`rgb()` 给的是 8-bit 原始值。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


def srgb_to_linear(value: float) -> float:
    """sRGB 电光转换的逆：8-bit 显示值 -> shader 写出的线性值。"""
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def linear_to_srgb(value: float) -> float:
    """线性值 -> sRGB 显示值，用于把解析期望值换算成"应该读到的 8-bit 值"。"""
    if value <= 0.0031308:
        return value * 12.92
    return 1.055 * (value ** (1.0 / 2.4)) - 0.055


@dataclass
class BmpImage:
    width: int
    height: int
    top_down: bool
    row_bytes: int
    data: bytes

    @classmethod
    def load(cls, path: str | Path) -> "BmpImage":
        raw = Path(path).read_bytes()
        if len(raw) < 54 or raw[0:2] != b"BM":
            raise ValueError(f"not a BMP file: {path}")

        pixel_offset = struct.unpack_from("<I", raw, 10)[0]
        header_size = struct.unpack_from("<I", raw, 14)[0]
        if header_size < 40:
            raise ValueError(f"unsupported BMP header size {header_size}: {path}")

        width = struct.unpack_from("<i", raw, 18)[0]
        raw_height = struct.unpack_from("<i", raw, 22)[0]
        bit_count = struct.unpack_from("<H", raw, 28)[0]
        if bit_count != 24:
            raise ValueError(f"only 24bpp BMP is supported, got {bit_count}: {path}")

        height = abs(raw_height)
        row_bytes = (width * 3 + 3) & ~3
        expected = pixel_offset + row_bytes * height
        if len(raw) < expected:
            raise ValueError(
                f"truncated BMP: need {expected} bytes, file has {len(raw)}: {path}"
            )

        return cls(
            width=width,
            height=height,
            top_down=raw_height < 0,
            row_bytes=row_bytes,
            data=raw[pixel_offset:expected],
        )

    def _file_row(self, screen_y: int) -> int:
        """屏幕行号 -> 文件行号（bottom-up 文件需要翻转）。"""
        return screen_y if self.top_down else self.height - 1 - screen_y

    def bgr(self, x: int, y: int) -> tuple[int, int, int]:
        """屏幕坐标处的原始 8-bit (B, G, R)。"""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"pixel ({x}, {y}) outside {self.width}x{self.height}")
        offset = self._file_row(y) * self.row_bytes + x * 3
        blue, green, red = self.data[offset], self.data[offset + 1], self.data[offset + 2]
        return blue, green, red

    def rgb(self, x: int, y: int) -> tuple[int, int, int]:
        blue, green, red = self.bgr(x, y)
        return red, green, blue

    def linear_rgb(self, x: int, y: int) -> tuple[float, float, float]:
        """sRGB 解码后的线性值 [0, 1]，可与 shader 内部数值直接比较。"""
        red, green, blue = self.rgb(x, y)
        return (
            srgb_to_linear(red / 255.0),
            srgb_to_linear(green / 255.0),
            srgb_to_linear(blue / 255.0),
        )

    def row(self, y: int) -> list[tuple[int, int, int]]:
        return [self.rgb(x, y) for x in range(self.width)]

    def column(self, x: int) -> list[tuple[int, int, int]]:
        return [self.rgb(x, y) for y in range(self.height)]

    def bbox(self) -> tuple[int, int, int, int]:
        """整幅画面的包围盒 (left, top, right, bottom)，右/下为闭区间。"""
        return 0, 0, self.width - 1, self.height - 1
