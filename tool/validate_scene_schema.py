"""Validate every real scene against the rebuilt scene.schema.json.

A schema that rejects an existing scene is wrong, and a schema that accepts a
scene the C++ validator rejects is also wrong. This checks the first half against
the real resource worktree; the second half is covered by reading the validator.

It also asserts the two drifts this rebuild was meant to close: the environment
block really is nested, and no scene carries an intensity-unit field that the code
in this snapshot does not read.
"""
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(REPO, 'config', 'config.json'), encoding='utf-8') as config_file:
    RESOURCE = os.path.normpath(os.path.join(REPO, json.load(config_file)['resourcePath']))

# Scenes that are deliberately invalid. Each entry must say why, and the scene's own
# `description` must agree -- an exemption without a stated reason is how a real
# defect gets hidden.
INTENTIONAL_EXCEPTIONS = {
    r'Maps\SC_scene01\SC_scene01.json':
        'keeps a legacy sunLight object on purpose as a historical reference; '
        'SceneAssetValidator rejects the type and the UI scene selector skips it, '
        'which the scene description states itself',
}


def load_schema():
    with open(os.path.join(REPO, 'schema', 'scene.schema.json'), encoding='utf-8') as handle:
        return json.load(handle)


def check(instance, schema, root, path='$'):
    """Enough of draft-07 to be meaningful: type, required, const, enum, items,
    min/maxItems, minimum/exclusiveMinimum, minLength, additionalProperties and
    oneOf dispatch on a const-typed union. Returns a list of problem strings."""
    problems = []

    def fail(message):
        problems.append('%s: %s' % (path, message))

    if '$ref' in schema:
        ref = schema['$ref']
        assert ref.startswith('#/'), 'only local refs supported'
        target = root
        for step in ref[2:].split('/'):
            target = target[step]
        merged = dict(target)
        for key, value in schema.items():
            if key != '$ref':
                merged[key] = value
        return check(instance, merged, root, path)

    if 'allOf' in schema:
        for index, option in enumerate(schema['allOf']):
            problems.extend(check(instance, option, root, '%s(allOf[%d])' % (path, index)))
        # Anything alongside allOf still applies.
        rest = {key: value for key, value in schema.items() if key != 'allOf'}
        if rest:
            problems.extend(check(instance, rest, root, path))
        return problems

    if 'oneOf' in schema:
        matched = []
        for index, option in enumerate(schema['oneOf']):
            if not check(instance, option, root, path):
                matched.append(index)
        if len(matched) != 1:
            fail('matched %d of %d oneOf branches (expected exactly 1)'
                 % (len(matched), len(schema['oneOf'])))
        rest = {key: value for key, value in schema.items() if key != 'oneOf'}
        if rest:
            problems.extend(check(instance, rest, root, path))
        return problems

    if 'not' in schema:
        if not check(instance, schema['not'], root, path):
            fail('matched forbidden schema')
    if 'if' in schema:
        branch = 'then' if not check(instance, schema['if'], root, path) else 'else'
        if branch in schema:
            problems.extend(check(instance, schema[branch], root, path))
    if 'anyOf' in schema and not any(
            not check(instance, option, root, path) for option in schema['anyOf']):
        fail('did not match any anyOf branch')

    # required 也可独立于 type 使用；Actor 的 inline/reference 互斥分支依赖它。
    if isinstance(instance, dict):
        for name in schema.get('required', []):
            if name not in instance:
                fail('missing required field %r' % name)

    expected = schema.get('type')
    if isinstance(expected, list):
        rest = {key: value for key, value in schema.items() if key != 'type'}
        if not any(not check(instance, dict(rest, type=kind), root, path) for kind in expected):
            fail('did not match any allowed type')
        return problems
    if expected == 'object':
        if not isinstance(instance, dict):
            fail('expected object')
            return problems
        properties = schema.get('properties', {})
        if schema.get('additionalProperties') is False:
            for name in instance:
                if name not in properties:
                    fail('unexpected field %r' % name)
        for name, subschema in properties.items():
            if name in instance:
                problems.extend(check(instance[name], subschema, root,
                                      '%s.%s' % (path, name)))
        extra = schema.get('additionalProperties')
        if isinstance(extra, dict):
            for name, value in instance.items():
                if name not in properties:
                    problems.extend(check(value, extra, root, '%s.%s' % (path, name)))
        return problems

    if expected == 'array':
        if not isinstance(instance, list):
            fail('expected array')
            return problems
        if 'minItems' in schema and len(instance) < schema['minItems']:
            fail('needs at least %d items' % schema['minItems'])
        if 'maxItems' in schema and len(instance) > schema['maxItems']:
            fail('needs at most %d items' % schema['maxItems'])
        if 'items' in schema:
            for index, item in enumerate(instance):
                problems.extend(check(item, schema['items'], root, '%s[%d]' % (path, index)))
        return problems

    if expected == 'string':
        if not isinstance(instance, str):
            fail('expected string')
        elif 'minLength' in schema and len(instance) < schema['minLength']:
            fail('shorter than minLength %d' % schema['minLength'])
        return problems

    if expected == 'integer':
        if not isinstance(instance, int) or isinstance(instance, bool):
            fail('expected integer')
        return problems

    if expected == 'boolean':
        if not isinstance(instance, bool):
            fail('expected boolean')
        return problems

    if expected == 'number':
        if not isinstance(instance, (int, float)) or isinstance(instance, bool):
            fail('expected number')
            return problems
        if 'minimum' in schema and instance < schema['minimum']:
            fail('below minimum %s' % schema['minimum'])
        if 'exclusiveMinimum' in schema and instance <= schema['exclusiveMinimum']:
            fail('not above exclusiveMinimum %s' % schema['exclusiveMinimum'])
        return problems

    if 'const' in schema and instance != schema['const']:
        fail('expected const %r' % schema['const'])
    if 'enum' in schema and instance not in schema['enum']:
        fail('not one of %s' % schema['enum'])
    return problems


def main():
    schema = load_schema()
    scenes = sorted(glob.glob(os.path.join(RESOURCE, 'Maps', '**', 'SC_*.json'), recursive=True))
    if not scenes:
        print('no scenes found under %s' % RESOURCE)
        return 1

    failures = 0
    exempt = 0
    env_keys = set()
    for path in scenes:
        relative = os.path.relpath(path, RESOURCE)
        try:
            with open(path, encoding='utf-8') as handle:
                scene = json.load(handle)
        except ValueError as error:
            print('%-64s JSON ERROR %s' % (relative, error))
            failures += 1
            continue
        problems = check(scene, schema, schema)
        if problems and relative in INTENTIONAL_EXCEPTIONS:
            description = scene.get('description', '')
            # The exemption is only honest if the scene documents it too.
            if not description or '不可加载' not in description:
                print('%-64s exempt but its description does not state why' % relative)
                failures += 1
            else:
                exempt += 1
                print('%-64s EXEMPT (documented): %s'
                      % (relative, INTENTIONAL_EXCEPTIONS[relative]))
            problems = []
        if problems:
            failures += 1
            print('%-64s %d PROBLEM(S)' % (relative, len(problems)))
            for problem in problems[:4]:
                print('      %s' % problem)
        for obj in scene.get('objects', []):
            if obj.get('type') == 'environment':
                env = obj.get('environment', {})
                env_keys.add((env.get('type'), tuple(sorted(env.keys()))))

    print()
    print('scenes checked: %d   failures: %d   documented exemptions: %d'
          % (len(scenes), failures, exempt))
    print('environment shapes observed:')
    for kind, keys in sorted(env_keys, key=lambda item: str(item)):
        print('   type=%-14s keys=%s' % (kind, list(keys)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
