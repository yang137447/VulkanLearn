"""Create the deterministic signed-RG PFM asset for VL scene validation."""

import argparse
import struct
from pathlib import Path


def generate(resource_root: Path):
    path = resource_root / "Generated/OfflineRenderer/Validation/groom.pfm"
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"Refusing to replace existing validation asset: {path}")
    # PFM 的首行对应 v=0，RG 是 UV 平面中的有符号单位方向；B 纱线朝向
    # 由双线性归一化与独立旋转角共同决定，不从颜色纹理的 sRGB 管线读取。
    directions = (
        (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (0.0, 1.0, 0.0), (1.0, 0.0, 0.0),
    )
    with path.open("xb") as stream:
        stream.write(b"PF\n2 2\n-1.0\n")
        for direction in directions:
            stream.write(struct.pack("<3f", *direction))
    print(path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("resource_root", type=Path)
    generate(parser.parse_args().resource_root)
