"""Retired legacy direct/composite runtime comparison entry point.

Existing S1 evidence is historical. This command cannot start a renderer or
Cook a retired production path. Use validate_composited_bake_gpu.py for the
independent numerical bake oracle.
"""

import argparse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_known_args()
    parser.error("Legacy production-path execution is prohibited; no renderer or Cook was started.")


if __name__ == "__main__":
    main()
