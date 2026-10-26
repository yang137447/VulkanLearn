"""Validate SM_ and TF_ descriptors using the repository's shared schema checker."""

import argparse
import json
from pathlib import Path

from validate_scene_schema import RESOURCE, check, load_schema


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resource-root', type=Path, default=Path(RESOURCE))
    options = parser.parse_args()
    root = options.resource_root.resolve()
    schema = load_schema('model_descriptor.schema.json')
    descriptors = set()
    for pattern in ('Common/Meshes/**/SM_*.json', 'Common/Meshes/**/TF_*.json',
                    'Maps/SC_*/Meshes/**/SM_*.json', 'Maps/SC_*/Meshes/**/TF_*.json'):
        descriptors.update(root.glob(pattern))
    failures = 0
    tressfx = 0
    for path in sorted(descriptors):
        try:
            instance = json.loads(path.read_text(encoding='utf-8'))
            tressfx += instance.get('type') == 'tressfx'
            problems = check(instance, schema, schema)
        except (ValueError, OSError) as error:
            problems = ['JSON error: %s' % error]
        if problems:
            failures += 1
            print('%s: %d problem(s)' % (path.relative_to(root), len(problems)))
            for problem in problems[:4]:
                print('    %s' % problem)
    print('model descriptors checked: %d   TressFX: %d   failures: %d' %
          (len(descriptors), tressfx, failures))
    return 1 if failures or not descriptors else 0


if __name__ == '__main__':
    raise SystemExit(main())
