"""Retired legacy direct/composite authoring entry point.

Historical comparisons remain in existing evidence and Git history. Terrain
authoring is composite-only. The independent GPU bake oracle remains available
in validate_composited_bake_gpu.py.
"""

import argparse


def prepare(*args, **kwargs):
    raise RuntimeError("Legacy direct scene generation is retired; use composite-only fixtures and the independent GPU bake oracle.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_known_args()
    parser.error("Legacy scene generation is prohibited; no resources were modified.")


if __name__ == "__main__":
    main()
